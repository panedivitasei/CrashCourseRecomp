// XamUserReadProfileSettings and XamUserWriteProfileSettings: gives a setting never written its type, which the
// game needs to find its settings without overrunning its buffer, and gives extra local players an empty profile
// that keeps nothing.

#include <cstdint>
#include <cstring>

#include <rex/hook.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xio.h>
#include <rex/system/xtypes.h>
#include <rex/types.h>

#include "local_players.h"

// The runtime's implementations (exported from the runtime DLL).
namespace rex::kernel::xam {
rex::u32 XamUserReadProfileSettings_entry(rex::u32 title_id, rex::u32 user_index, rex::u32 xuid_count,
                                          mapped_u64 xuids, rex::u32 setting_count, mapped_u32 setting_ids,
                                          mapped_u32 buffer_size_ptr, mapped_void buffer_ptr,
                                          ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped);
rex::u32 XamUserWriteProfileSettings_entry(rex::u32 title_id, rex::u32 user_index, rex::u32 setting_count,
                                           ppc_ptr_t<rex::system::xam::X_USER_PROFILE_SETTING> settings,
                                           ppc_ptr_t<rex::system::XAM_OVERLAPPED> overlapped);
}  // namespace rex::kernel::xam

namespace cc1 {
namespace {

using rex::X_RESULT;
using rex::system::XAM_OVERLAPPED;
using rex::system::xam::X_USER_PROFILE_SETTING;

// A setting's type is the top four bits of its id.
uint8_t TypeOf(uint32_t id) { return uint8_t(id >> 28); }

rex::u32 ReadProfileSettings(rex::u32 title_id, rex::u32 user_index, rex::u32 xuid_count, mapped_u64 xuids,
                             rex::u32 setting_count, mapped_u32 setting_ids, mapped_u32 buffer_size_ptr,
                             mapped_void buffer_ptr, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  const uint32_t user = user_index;
  // Another local player's answer takes its layout from player 1's.
  const bool other = LocalPlayer(user) && !xuid_count;
  const uint32_t result = rex::kernel::xam::XamUserReadProfileSettings_entry(
      title_id, other ? 0u : user, xuid_count, xuids, setting_count, setting_ids, buffer_size_ptr, buffer_ptr,
      overlapped);
  if ((result != X_ERROR_SUCCESS && result != X_ERROR_IO_PENDING) || !buffer_ptr) return result;
  if (overlapped && uint32_t(overlapped->result) != X_ERROR_SUCCESS) return result;
  auto* memory = REX_KERNEL_MEMORY();
  const uint8_t* header = memory->TranslateVirtual<const uint8_t*>(buffer_ptr.guest_address());
  const uint32_t count = rex::memory::load_and_swap<uint32_t>(header);
  const uint32_t settings_ptr = rex::memory::load_and_swap<uint32_t>(header + 4);
  if (!settings_ptr || count > 32) return result;
  auto* settings = memory->TranslateVirtual<X_USER_PROFILE_SETTING*>(settings_ptr);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = settings[i].setting_id;
    if (other) {
      std::memset(&settings[i], 0, sizeof(settings[i]));
      settings[i].user_index = user;
      settings[i].setting_id = id;
    }
    if (!settings[i].data.type) settings[i].data.type = TypeOf(id);
  }
  return result;
}

rex::u32 WriteProfileSettings(rex::u32 title_id, rex::u32 user_index, rex::u32 setting_count,
                              ppc_ptr_t<X_USER_PROFILE_SETTING> settings, ppc_ptr_t<XAM_OVERLAPPED> overlapped) {
  if (!LocalPlayer(user_index)) {
    return rex::kernel::xam::XamUserWriteProfileSettings_entry(title_id, user_index, setting_count, settings,
                                                               overlapped);
  }
  if (!overlapped) return X_ERROR_SUCCESS;
  REX_KERNEL_STATE()->CompleteOverlappedImmediate(overlapped.guest_address(), X_ERROR_SUCCESS);
  return X_ERROR_IO_PENDING;
}

}  // namespace
}  // namespace cc1

REX_HOOK(__imp__XamUserReadProfileSettings, cc1::ReadProfileSettings)
REX_HOOK(__imp__XamUserWriteProfileSettings, cc1::WriteProfileSettings)
