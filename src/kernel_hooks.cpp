// kernel_hooks.cpp: title-side overrides of runtime imports, each doing its CC1-specific
// part and otherwise calling the runtime's own implementation.

#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <deque>
#include <string>
#include <cstdio>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/input/input.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/thread.h>
#include <rex/types.h>

#include <rexglue_guide/guide.h>

#include "controls.h"
#include "online.h"
#include "spectator.h"

REXCVAR_DEFINE_INT32(cc1_poll_yield_every, 32, "CC1",
                     "A zero-timeout kernel wait that times out yields the host thread only on "
                     "every Nth poll of a rapid run (1 = every poll, 0 = never).");

// probe.cpp: true drops the keystroke (A down while the Saving popup owns input).
bool CC1_KeystrokeFilter(uint32_t user, uint16_t vk, uint16_t flags);

// The runtime's implementations (exported from the runtime DLL).
namespace rex::kernel::xam {
u32 XamInputGetState_entry(u32 user_index, u32 flags, ppc_ptr_t<rex::input::X_INPUT_STATE> input_state);
u32 XamInputGetKeystroke_entry(u32 user_index, u32 flags,
                               ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke);
u32 XamInputGetKeystrokeEx_entry(mapped_u32 user_index_ptr, u32 flags,
                                 ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke);
u32 XamInputSetState_entry(u32 user_index, u32 unk, ppc_ptr_t<rex::input::X_INPUT_VIBRATION> vibration);
}  // namespace rex::kernel::xam
namespace rex::kernel::xboxkrnl {
u32 KeWaitForSingleObject_entry(mapped_void object_ptr, u32 wait_reason, u32 processor_mode,
                                u32 alertable, mapped_u64 timeout_ptr);
u32 NtWaitForSingleObjectEx_entry(u32 object_handle, u32 wait_mode, u32 alertable,
                                  mapped_u64 timeout_ptr);
u32 KeWaitForMultipleObjects_entry(u32 count, mapped_u32 objects_ptr, u32 wait_type,
                                   u32 wait_reason, u32 processor_mode, u32 alertable,
                                   mapped_u64 timeout_ptr, mapped_void wait_block_array_ptr);
u32 NtWaitForMultipleObjectsEx_entry(u32 count, mapped_u32 handles, u32 wait_type, u32 wait_mode,
                                     u32 alertable, mapped_u64 timeout_ptr);
u32 NtSignalAndWaitForSingleObjectEx_entry(u32 signal_handle, u32 wait_handle, u32 alertable,
                                           u32 r6, mapped_u64 timeout_ptr);
}  // namespace rex::kernel::xboxkrnl

namespace cc1 {
using GamepadFilter = void (*)(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad);
void SetGamepadFilter(GamepadFilter filter);
void InjectKeystroke(uint16_t vk);
}  // namespace cc1

namespace cc1 {
using rex::u32;
using rex::X_RESULT;
using rex::X_STATUS;
using rex::system::object_ref;
using rex::system::XObject;
using rex::system::XThread;
using rex::thread::WaitResult;

// ---- Input -------------------------------------------------------------------

// probe.cpp watches the pad through a filter on XamInputGetState, and synthetic VK_PAD_*
// presses are queued for the game's GUI to read as keystrokes.

std::atomic<GamepadFilter> g_pad_filter{nullptr};

// One synthetic press at a time: KEYDOWN, about 120 ms held (the stream reports EMPTY),
// KEYUP, then a 150 ms gap, so a GUI draining keystrokes in a loop never sees both edges
// in one tick.
std::mutex g_inject_mutex;
std::deque<uint16_t> g_inject_pending;
uint16_t g_inject_active = 0;
std::chrono::steady_clock::time_point g_inject_up_at, g_inject_next_at;

enum class Injected { kNone, kDelivered, kHold };
static Injected PopInjectedKeystroke(rex::input::X_INPUT_KEYSTROKE* out) {
  std::lock_guard lock(g_inject_mutex);
  const auto now = std::chrono::steady_clock::now();
  auto fill = [out](uint16_t vk, uint16_t flags) {
    out->virtual_key = vk;
    out->unicode = 0;
    out->flags = flags;
    out->user_index = 0;
    out->hid_code = 0;
  };
  if (g_inject_active) {
    if (now < g_inject_up_at) return Injected::kHold;
    fill(g_inject_active, rex::input::X_INPUT_KEYSTROKE_KEYUP);
    g_inject_active = 0;
    g_inject_next_at = now + std::chrono::milliseconds(150);
    return Injected::kDelivered;
  }
  if (g_inject_pending.empty()) return Injected::kNone;
  if (now < g_inject_next_at) return Injected::kHold;
  g_inject_active = g_inject_pending.front();
  g_inject_pending.pop_front();
  fill(g_inject_active, rex::input::X_INPUT_KEYSTROKE_KEYDOWN);
  g_inject_up_at = now + std::chrono::milliseconds(120);
  return Injected::kDelivered;
}

// Players whose pad the Guide still owns: set while it is up, cleared once the pad is at rest after it closes, so
// the press that closed a Guide window is not also the game's.
static std::atomic<uint8_t> g_guide_pads{0};

static bool PadAtRest(const rex::input::X_INPUT_GAMEPAD& pad) {
  return uint16_t(pad.buttons) == 0 && pad.left_trigger < 30 && pad.right_trigger < 30;
}

// Keeps the pad's motors still for a spectator, whose fighter takes a racer's steps (puppet.cpp).
static u32 XamInputSetState(u32 user_index, u32 unk, ppc_ptr_t<rex::input::X_INPUT_VIBRATION> vibration) {
  if (vibration && !cc1::spectator::Watched().empty()) {
    vibration->left_motor_speed = 0;
    vibration->right_motor_speed = 0;
  }
  return rex::kernel::xam::XamInputSetState_entry(user_index, unk, vibration);
}

static u32 XamInputGetState(u32 user_index, u32 flags,
                            ppc_ptr_t<rex::input::X_INPUT_STATE> input_state) {
  const u32 result = rex::kernel::xam::XamInputGetState_entry(user_index, flags, input_state);
  if (!input_state) return result;
  if (result == X_ERROR_DEVICE_NOT_CONNECTED) {
    std::memset(static_cast<rex::input::X_INPUT_STATE*>(input_state), 0,
                sizeof(rex::input::X_INPUT_STATE));  // a defined state for disconnected pads
  } else if (result == X_ERROR_SUCCESS) {
    const bool any = (uint32_t(user_index) & 0xFF) == 0xFF || (flags & 0x40000000);  // ANY_USER
    const uint32_t user = any ? 0u : uint32_t(user_index);
    auto& pad = input_state->gamepad;
    // The Guide reads every pad, and while it or its sign-in page is up the game sees a pad
    // at rest.
    if (user < 4) {
      rexglue_guide::NoteGamePad(int(user), uint16_t(pad.buttons), pad.left_trigger,
                                 pad.right_trigger, int16_t(pad.thumb_lx), int16_t(pad.thumb_ly));
    }
    if (user < 4) cc1::controls::NotePad(user, uint16_t(pad.buttons), pad.left_trigger, pad.right_trigger);
    bool held_back = false;  // the Guide or a binding being set owns this read
    if (rexglue_guide::IsOpen() || cc1::controls::Capturing()) {
      g_guide_pads = 0xF;
      std::memset(&pad, 0, sizeof(pad));
      held_back = true;
    } else if (user < 4 && (g_guide_pads.load() & (1u << user))) {
      if (PadAtRest(pad)) {
        g_guide_pads &= uint8_t(~(1u << user));
      } else {
        std::memset(&pad, 0, sizeof(pad));
        held_back = true;
      }
    }
    if (!held_back) {
      cc1::controls::Remap(&pad);
      cc1::controls::ApplyKeyboard(user, &pad);
    }
    // The pad as the game sees it: at rest under the Guide, with the keyboard's buttons added.
    cc1::spectator::NotePad(user, uint16_t(pad.buttons));
    cc1::online::NotePad(user, uint16_t(pad.buttons), pad.left_trigger, pad.right_trigger);
    if (auto filter = g_pad_filter.load()) filter(user, &pad);
  }
  return result;
}

// True drops a keystroke the runtime delivered: all of them while the Guide is open (the
// game's menus navigate by keystroke), otherwise per probe.cpp's filter.
static bool DropKeystroke(const rex::input::X_INPUT_KEYSTROKE* ks) {
  if (rexglue_guide::IsOpen() || cc1::controls::Capturing()) return true;
  if (uint32_t(ks->user_index) < 4 && (g_guide_pads.load() & (1u << uint32_t(ks->user_index)))) return true;
  return CC1_KeystrokeFilter(uint32_t(ks->user_index), uint16_t(ks->virtual_key),
                             uint16_t(ks->flags));
}

// The runtime answers an any-player query with player 1's keystrokes alone, so each player is asked in turn.
constexpr uint32_t kAnyUser = 0x40000000;  // XINPUT_FLAG_ANYUSER
static bool AnyUser(uint32_t user_index, uint32_t flags) { return (user_index & 0xFF) == 0xFF || (flags & kAnyUser); }

static u32 XamInputGetKeystroke(u32 user_index, u32 flags,
                                ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  if (keystroke) {
    switch (PopInjectedKeystroke(keystroke)) {
      case Injected::kDelivered: return X_ERROR_SUCCESS;
      case Injected::kHold: return X_ERROR_EMPTY;
      case Injected::kNone: break;
    }
  }
  u32 result = AnyUser(user_index, flags) ? uint32_t(X_ERROR_EMPTY)
                                          : rex::kernel::xam::XamInputGetKeystroke_entry(user_index, flags, keystroke);
  if (AnyUser(user_index, flags)) {
    for (uint32_t user = 0; user < 4 && result != X_ERROR_SUCCESS; ++user) {
      result = rex::kernel::xam::XamInputGetKeystroke_entry(user, flags & ~kAnyUser, keystroke);
    }
    if (result != X_ERROR_SUCCESS) result = X_ERROR_EMPTY;
  }
  if (result == X_ERROR_SUCCESS && keystroke && DropKeystroke(keystroke)) return X_ERROR_EMPTY;
  return result;
}

static u32 XamInputGetKeystrokeEx(mapped_u32 user_index_ptr, u32 flags,
                                  ppc_ptr_t<rex::input::X_INPUT_KEYSTROKE> keystroke) {
  if (keystroke) {
    switch (PopInjectedKeystroke(keystroke)) {
      case Injected::kDelivered:
        if (user_index_ptr) *user_index_ptr = 0;
        return X_ERROR_SUCCESS;
      case Injected::kHold: return X_ERROR_EMPTY;
      case Injected::kNone: break;
    }
  }
  const uint32_t asked = user_index_ptr ? uint32_t(*user_index_ptr) : 0xFFu;
  u32 result = X_ERROR_EMPTY;
  if (user_index_ptr && AnyUser(asked, flags)) {
    for (uint32_t user = 0; user < 4 && result != X_ERROR_SUCCESS; ++user) {
      *user_index_ptr = user;
      result = rex::kernel::xam::XamInputGetKeystrokeEx_entry(user_index_ptr, flags & ~kAnyUser, keystroke);
    }
    if (result != X_ERROR_SUCCESS) {
      *user_index_ptr = asked;
      result = X_ERROR_EMPTY;
    }
  } else {
    result = rex::kernel::xam::XamInputGetKeystrokeEx_entry(user_index_ptr, flags, keystroke);
  }
  if (result == X_ERROR_SUCCESS && keystroke && DropKeystroke(keystroke)) return X_ERROR_EMPTY;
  return result;
}

// ---- Zero-timeout waits ------------------------------------------------------

// A poll is handled as XObject::Wait, WaitMultiple and SignalAndWait handle it (same host
// wait, success callback and status codes) except for the yield after a timeout.

// XObject's wait handle and success callback are protected members, reached through a
// member pointer formed via a derived class without changing the SDK.
struct XObjectAccess : XObject {
  static rex::thread::WaitHandle* WaitHandle(XObject* object) {
    return (object->*&XObjectAccess::GetWaitHandle)();
  }
  static void WaitSucceeded(XObject* object) { (object->*&XObjectAccess::WaitCallback)(); }
};

// Same timeout -> host milliseconds conversion as the runtime (XObject::Wait).
static bool IsPoll(const mapped_u64& timeout_ptr) {
  if (!timeout_ptr) return false;  // no timeout: waits forever
  const int64_t ticks = int64_t(uint64_t(*timeout_ptr));
  const uint32_t ms = ticks < 0 ? uint32_t(-ticks / 10000) : 0u;  // absolute times count as 0
  return rex::chrono::Clock::ScaleGuestDurationMillis(ms) == 0;
}

// Yields on every Nth poll that comes within 2 ms of the previous one (a spin loop
// waiting on another thread), while an isolated poll returns at once.
static void YieldInRapidRun() {
  const int32_t every = REXCVAR_GET(cc1_poll_yield_every);
  if (every <= 0) return;
  thread_local uint64_t last_poll = 0;
  thread_local int32_t run = 0;
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t gap_ticks = rex::chrono::Clock::QueryHostTickFrequency() / 500;
  run = (last_poll && now - last_poll < gap_ticks) ? run + 1 : 1;
  last_poll = now;
  if (run >= every) {
    run = 0;
    rex::thread::MaybeYield();
  }
}

static X_STATUS DeliverIfApc(X_STATUS result, u32 alertable) {
  if (alertable && result == X_STATUS_USER_APC) XThread::GetCurrentThread()->DeliverAPCs();
  return result;
}

static X_STATUS PollOne(XObject* object, u32 alertable) {
  auto* handle = XObjectAccess::WaitHandle(object);
  if (!handle) return X_STATUS_SUCCESS;  // the object does not support waiting
  switch (rex::thread::Wait(handle, alertable != 0, std::chrono::milliseconds(0))) {
    case WaitResult::kSuccess:
      XObjectAccess::WaitSucceeded(object);
      return X_STATUS_SUCCESS;
    case WaitResult::kUserCallback:
      return X_STATUS_USER_APC;
    case WaitResult::kTimeout:
      YieldInRapidRun();
      return X_STATUS_TIMEOUT;
    default:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

static X_STATUS PollMany(std::vector<object_ref<XObject>>& objects, u32 wait_type, u32 alertable) {
  std::vector<rex::thread::WaitHandle*> handles(objects.size());
  for (size_t i = 0; i < objects.size(); ++i) {
    handles[i] = XObjectAccess::WaitHandle(objects[i].get());
  }
  if (wait_type) {  // any
    auto [result, index] =
        rex::thread::WaitAny(std::move(handles), alertable != 0, std::chrono::milliseconds(0));
    switch (result) {
      case WaitResult::kSuccess:
        XObjectAccess::WaitSucceeded(objects[index].get());
        return X_STATUS(index);
      case WaitResult::kUserCallback:
        return X_STATUS_USER_APC;
      case WaitResult::kTimeout:
        YieldInRapidRun();
        return X_STATUS_TIMEOUT;
      case WaitResult::kFailed:
        return X_STATUS_UNSUCCESSFUL;
      default:
        return X_STATUS(X_STATUS_ABANDONED_WAIT_0 + index);
    }
  }
  switch (rex::thread::WaitAll(std::move(handles), alertable != 0, std::chrono::milliseconds(0))) {
    case WaitResult::kSuccess:
      for (auto& object : objects) XObjectAccess::WaitSucceeded(object.get());
      return X_STATUS_SUCCESS;
    case WaitResult::kUserCallback:
      return X_STATUS_USER_APC;
    case WaitResult::kTimeout:
      YieldInRapidRun();
      return X_STATUS_TIMEOUT;
    default:
      return X_STATUS_ABANDONED_WAIT_0;
  }
}

static u32 KeWaitForSingleObject(mapped_void object_ptr, u32 wait_reason, u32 processor_mode,
                                 u32 alertable, mapped_u64 timeout_ptr) {
  if (IsPoll(timeout_ptr)) {
    auto object = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(), object_ptr);
    if (!object) return X_STATUS_ABANDONED_WAIT_0;  // same as the runtime
    return DeliverIfApc(PollOne(object.get(), alertable), alertable);
  }
  return rex::kernel::xboxkrnl::KeWaitForSingleObject_entry(object_ptr, wait_reason,
                                                            processor_mode, alertable, timeout_ptr);
}

static u32 NtWaitForSingleObjectEx(u32 object_handle, u32 wait_mode, u32 alertable,
                                   mapped_u64 timeout_ptr) {
  if (IsPoll(timeout_ptr)) {
    auto object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(object_handle);
    if (!object) return X_STATUS_INVALID_HANDLE;
    return DeliverIfApc(PollOne(object.get(), alertable), alertable);
  }
  return rex::kernel::xboxkrnl::NtWaitForSingleObjectEx_entry(object_handle, wait_mode, alertable,
                                                              timeout_ptr);
}

static u32 KeWaitForMultipleObjects(u32 count, mapped_u32 objects_ptr, u32 wait_type,
                                    u32 wait_reason, u32 processor_mode, u32 alertable,
                                    mapped_u64 timeout_ptr, mapped_void wait_block_array_ptr) {
  if (IsPoll(timeout_ptr)) {
    std::vector<object_ref<XObject>> objects;
    for (uint32_t n = 0; n < count; n++) {
      auto native = REX_KERNEL_MEMORY()->TranslateVirtual(objects_ptr[n]);
      auto object = XObject::GetNativeObject<XObject>(REX_KERNEL_STATE(), native);
      if (!object) return X_STATUS_INVALID_PARAMETER;
      objects.push_back(std::move(object));
    }
    return DeliverIfApc(PollMany(objects, wait_type, alertable), alertable);
  }
  return rex::kernel::xboxkrnl::KeWaitForMultipleObjects_entry(
      count, objects_ptr, wait_type, wait_reason, processor_mode, alertable, timeout_ptr,
      wait_block_array_ptr);
}

static u32 NtWaitForMultipleObjectsEx(u32 count, mapped_u32 handles, u32 wait_type, u32 wait_mode,
                                      u32 alertable, mapped_u64 timeout_ptr) {
  if (IsPoll(timeout_ptr)) {
    std::vector<object_ref<XObject>> objects;
    for (uint32_t n = 0; n < count; n++) {
      auto object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(uint32_t(handles[n]));
      if (!object) return X_STATUS_INVALID_PARAMETER;
      objects.push_back(std::move(object));
    }
    return DeliverIfApc(PollMany(objects, wait_type, alertable), alertable);
  }
  return rex::kernel::xboxkrnl::NtWaitForMultipleObjectsEx_entry(count, handles, wait_type,
                                                                 wait_mode, alertable, timeout_ptr);
}

static u32 NtSignalAndWaitForSingleObjectEx(u32 signal_handle, u32 wait_handle, u32 alertable,
                                            u32 r6, mapped_u64 timeout_ptr) {
  if (IsPoll(timeout_ptr)) {
    auto signal_object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(signal_handle);
    auto wait_object = REX_KERNEL_OBJECTS()->LookupObject<XObject>(wait_handle);
    if (!signal_object || !wait_object) return DeliverIfApc(X_STATUS_INVALID_HANDLE, alertable);
    X_STATUS result;
    switch (rex::thread::SignalAndWait(XObjectAccess::WaitHandle(signal_object.get()),
                                       XObjectAccess::WaitHandle(wait_object.get()),
                                       alertable != 0, std::chrono::milliseconds(0))) {
      case WaitResult::kSuccess:
        XObjectAccess::WaitSucceeded(wait_object.get());
        result = X_STATUS_SUCCESS;
        break;
      case WaitResult::kUserCallback:
        result = X_STATUS_USER_APC;
        break;
      case WaitResult::kTimeout:
        YieldInRapidRun();
        result = X_STATUS_TIMEOUT;
        break;
      default:
        result = X_STATUS_ABANDONED_WAIT_0;
        break;
    }
    return DeliverIfApc(result, alertable);
  }
  return rex::kernel::xboxkrnl::NtSignalAndWaitForSingleObjectEx_entry(signal_handle, wait_handle,
                                                                       alertable, r6, timeout_ptr);
}
}  // namespace cc1

void cc1::SetGamepadFilter(GamepadFilter filter) { g_pad_filter.store(filter); }
void cc1::InjectKeystroke(uint16_t vk) {
  std::lock_guard lock(g_inject_mutex);
  g_inject_pending.push_back(vk);
}

REX_HOOK(__imp__XamInputGetState, cc1::XamInputGetState)
REX_HOOK(__imp__XamInputSetState, cc1::XamInputSetState)
REX_HOOK(__imp__XamInputGetKeystroke, cc1::XamInputGetKeystroke)
REX_HOOK(__imp__XamInputGetKeystrokeEx, cc1::XamInputGetKeystrokeEx)
REX_HOOK(__imp__KeWaitForSingleObject, cc1::KeWaitForSingleObject)
REX_HOOK(__imp__NtWaitForSingleObjectEx, cc1::NtWaitForSingleObjectEx)
REX_HOOK(__imp__KeWaitForMultipleObjects, cc1::KeWaitForMultipleObjects)
REX_HOOK(__imp__NtWaitForMultipleObjectsEx, cc1::NtWaitForMultipleObjectsEx)
REX_HOOK(__imp__NtSignalAndWaitForSingleObjectEx, cc1::NtSignalAndWaitForSingleObjectEx)
