// XamContentCreateEx and XamContentCreateEnumerator for saves and DLC packages: copies the root name before the
// work is deferred, opens content found under the common (xuid 0) root, reopens a package whose delete fails and
// keeps each local player's saves apart.

#include <cstdint>
#include <string>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_device.h>
#include <rex/system/xam/content_manager.h>
#include <rex/system/xenumerator.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "local_players.h"

// The runtime's implementation (exported from the runtime DLL).
namespace rex::kernel::xam {
rex::u32 XamContentCreateEnumerator_entry(rex::u32 user_index, rex::u32 device_id, rex::u32 content_type,
                                          rex::u32 content_flags, rex::u32 items_per_enumerate,
                                          mapped_u32 buffer_size_ptr, mapped_u32 handle_out);
}

namespace cc1 {
namespace {

using rex::u32;
using rex::u64;
using rex::X_HRESULT;
using rex::X_RESULT;
using rex::system::XContentType;
using rex::system::xam::XCONTENT_AGGREGATE_DATA;
using rex::system::xam::XCONTENT_DATA;

enum class Disposition : uint32_t { kUnknown = 0, kCreate = 1, kOpen = 2 };

u32 XamContentCreateEx(u32 user_index, mapped_string root_name, mapped_void content_data_ptr,
                       u32 flags, mapped_u32 disposition_ptr, mapped_u32 license_mask_ptr,
                       u32 cache_size, u64 content_size, mapped_void overlapped_ptr) {
  (void)cache_size;
  (void)content_size;
  if (!content_data_ptr || !root_name) return X_ERROR_INVALID_PARAMETER;

  auto* kernel_state = REX_KERNEL_STATE();
  auto* content_manager = kernel_state->content_manager();
  uint64_t xuid = LocalPlayer(user_index) ? LocalPlayerXuid(user_index) : kernel_state->user_profile()->xuid();

  XCONTENT_AGGREGATE_DATA content_data;
  content_data = *content_data_ptr.as<XCONTENT_DATA*>();
  if (content_data.content_type == XContentType::kMarketplaceContent) xuid = 0;
  if (xuid != 0 && !content_manager->ContentExists(xuid, content_data) &&
      content_manager->ContentExists(0, content_data)) {
    xuid = 0;
  }

  if (overlapped_ptr && disposition_ptr) *disposition_ptr = 0;

  auto run = [content_manager, xuid, root = std::string(root_name.value()), flags, content_data,
              disposition_ptr,
              license_mask_ptr](uint32_t& extended_error, uint32_t& length) -> X_RESULT {
    X_RESULT result = X_ERROR_INVALID_PARAMETER;
    Disposition disposition = Disposition::kUnknown;
    const bool exists = content_manager->ContentExists(xuid, content_data);
    // Deletes the package; a delete the host refuses leaves it to be reopened.
    auto replace = [&] {
      content_manager->CloseContent(root);
      if (content_manager->ContentExists(xuid, content_data)) {
        content_manager->DeleteContent(xuid, content_data);
      }
      return content_manager->ContentExists(xuid, content_data) ? Disposition::kOpen
                                                                 : Disposition::kCreate;
    };
    switch (flags & 0xF) {
      case 1:  // CREATE_NEW
        if (exists) {
          result = X_ERROR_ALREADY_EXISTS;
        } else {
          disposition = Disposition::kCreate;
        }
        break;
      case 2:  // CREATE_ALWAYS
        disposition = replace();
        break;
      case 3:  // OPEN_EXISTING
        if (exists) {
          disposition = Disposition::kOpen;
        } else {
          result = X_ERROR_PATH_NOT_FOUND;
        }
        break;
      case 4:  // OPEN_ALWAYS
        disposition = exists ? Disposition::kOpen : Disposition::kCreate;
        break;
      case 5:  // TRUNCATE_EXISTING
        if (exists) {
          disposition = replace();
        } else {
          result = X_ERROR_PATH_NOT_FOUND;
        }
        break;
      default:
        break;
    }

    uint32_t license = 0;
    if (disposition == Disposition::kCreate) {
      result = content_manager->CreateContent(root, xuid, content_data);
      if (XSUCCEEDED(result)) content_manager->WriteContentHeaderFile(xuid, content_data);
    } else if (disposition == Disposition::kOpen) {
      result = content_manager->OpenContent(root, xuid, content_data, license);
    }
    if (license_mask_ptr && XSUCCEEDED(result)) *license_mask_ptr = license;
    if (disposition_ptr) *disposition_ptr = static_cast<uint32_t>(disposition);

    extended_error = X_HRESULT_FROM_WIN32(result);
    length = static_cast<uint32_t>(disposition);
    REXKRNL_DEBUG("[content] XamContentCreateEx '{}' file='{}' flags={:X} -> result={:08X} disp={}",
                  root, content_data.file_name(), flags, uint32_t(result), uint32_t(disposition));
    return result;
  };

  if (!overlapped_ptr) {
    uint32_t extended_error, length;
    return run(extended_error, length);
  }
  kernel_state->CompleteOverlappedDeferredEx(run, overlapped_ptr.guest_address());
  return X_ERROR_IO_PENDING;
}

// Replaces the runtime's, which lists player 1's content for any user: a player of local_players.h gets their own
// content plus the common (xuid 0) content such as DLC.
u32 XamContentCreateEnumerator(u32 user_index, u32 device_id, u32 content_type, u32 content_flags,
                               u32 items_per_enumerate, mapped_u32 buffer_size_ptr, mapped_u32 handle_out) {
  constexpr uint32_t kHdd = static_cast<uint32_t>(rex::system::xam::DummyDeviceId::HDD);
  if (!LocalPlayer(user_index) || !handle_out || (device_id && device_id != kHdd)) {
    return rex::kernel::xam::XamContentCreateEnumerator_entry(user_index, device_id, content_type, content_flags,
                                                              items_per_enumerate, buffer_size_ptr, handle_out);
  }
  auto* kernel_state = REX_KERNEL_STATE();
  if (buffer_size_ptr) *buffer_size_ptr = uint32_t(sizeof(XCONTENT_DATA) * items_per_enumerate);
  auto list = rex::system::make_object<rex::system::XStaticEnumerator<XCONTENT_DATA>>(kernel_state,
                                                                                     items_per_enumerate);
  const X_HRESULT result = list->Initialize(0xFF, 0xFE, 0x20005, 0x20007, 0);
  if (XFAILED(result)) return result;
  for (const uint64_t xuid : {LocalPlayerXuid(user_index), uint64_t(0)}) {
    for (const auto& content : kernel_state->content_manager()->ListContent(
             kHdd, xuid, XContentType(uint32_t(content_type)))) {
      *list->AppendItem() = content;
    }
  }
  *handle_out = list->handle();
  return X_ERROR_SUCCESS;
}

}  // namespace
}  // namespace cc1

REX_HOOK(__imp__XamContentCreateEx, cc1::XamContentCreateEx)
REX_HOOK(__imp__XamContentCreateEnumerator, cc1::XamContentCreateEnumerator)
