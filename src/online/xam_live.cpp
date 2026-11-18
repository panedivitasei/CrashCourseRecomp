// XAM side of a LIVE session: who the player is, and the XSession and XLive messages, where a session is local data the
// lobby server passes between members and an invite is the lobby host's session.

#pragma GCC diagnostic ignored "-Wunused-parameter"

#include <algorithm>
#include <cstring>
#include <memory>

#include <rex/hook.h>
#include <rex/kernel/xam/apps/app.h>
#include <rex/kernel/xam/apps/xgi_app.h>
#include <rex/kernel/xam/apps/xlivebase_app.h>
#include <rex/kernel/xam/apps/xmp_app.h>
#include <rex/kernel/xam/module.h>
#include <rex/kernel/xbdm/module.h>
#include <rex/kernel/xboxkrnl/module.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/runtime.h>
#include <rex/string.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/system/xenumerator.h>
#include <rex/types.h>

#include "../local_players.h"
#include "../online.h"
#include "../spectator.h"
#include "leaderboards.h"
#include "relay.h"

// Declared in the Guide (jmlive/src/live.h): opens it on the game's achievements.
namespace rexglue_guide::live {
void RequestAchievements();
}

namespace cc1::online {
namespace {

using rex::u32;
using rex::X_HRESULT;

constexpr uint32_t kXSessionCreate = 0x000B0010, kXSessionDelete = 0x000B0011, kXSessionLeave = 0x000B0013;
constexpr uint32_t kXSessionMigrateHost = 0x000B001E, kXUserReadStats = 0x000B0021;
constexpr uint32_t kXUserWriteAchievements = 0x000B0008;
constexpr uint32_t kLogonGetHR = 0x00058003, kFriendsEnumerate = 0x00058020, kInviteGetAcceptedInfo = 0x00058023;
constexpr uint32_t kMuteListQuery = 0x0005800E;
constexpr uint32_t kSessionHost = 0x01, kSessionPeerNetwork = 0x20;  // XSESSION_CREATE_* flags
constexpr uint32_t kLogonEstablished = 0x001510F0;  // XONLINE_S_LOGON_CONNECTION_ESTABLISHED
constexpr uint32_t kSuccess = 0, kNoSuchUser = 0x80070525, kInvalidArg = 0x80070057, kFail = 0x80004005;
constexpr uint32_t kErrorNoSuchUser = 1317, kErrorInvalidParameter = 87;

template <typename T>
T* Guest(uint32_t address) {
  return REX_KERNEL_MEMORY()->TranslateVirtual<T*>(address);
}

struct StatsResults {  // XUSER_STATS_READ_RESULTS
  rex::be<uint32_t> num_views;
  rex::be<uint32_t> views_ptr;
};
struct StatsView {  // XUSER_STATS_VIEW
  rex::be<uint32_t> view_id;
  rex::be<uint32_t> total_rows;
  rex::be<uint32_t> num_rows;
  rex::be<uint32_t> rows_ptr;
};
#pragma pack(push, 4)
struct InviteInfo {  // XINVITE_INFO
  rex::be<uint64_t> xuid_invitee;
  rex::be<uint64_t> xuid_inviter;
  rex::be<uint32_t> title_id;
  uint8_t host[kSessionInfoSize];
  rex::be<uint32_t> from_game_invite;
};
#pragma pack(pop)
static_assert(sizeof(InviteInfo) == 0x54);

uint64_t SessionId(const uint8_t* info) {
  uint64_t id = 0;
  for (int i = 0; i < 8; ++i) id = (id << 8) | info[i];
  return id;
}

// ---- XSession ----------------------------------------------------------------------------------------------------

class SessionApp final : public rex::kernel::xam::apps::XgiApp {
 public:
  using XgiApp::XgiApp;

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length) override {
    if (!buffer_ptr) return XgiApp::DispatchMessageSync(message, buffer_ptr, buffer_length);
    const uint8_t* buffer = memory_->TranslateVirtual(buffer_ptr);
    auto word = [&](uint32_t offset) { return rex::memory::load_and_swap<uint32_t>(buffer + offset); };
    switch (message) {
      case kXSessionCreate: {
        const uint32_t flags = word(4), info_ptr = word(20), nonce_ptr = word(24);
        // Single player and local multiplayer make sessions too; those stay the runtime's.
        if (!(flags & kSessionPeerNetwork) || !info_ptr) break;
        const bool host = flags & kSessionHost;
        uint8_t info[kSessionInfoSize];
        if (host) {
          RandomBytes(info, 8);
          info[0] = uint8_t((info[0] & 0x0F) | 0x80);  // XNET_XNKID_ONLINE_PEER
          relay::FillLocalXnAddr(info + 8);
          RandomBytes(info + 44, 16);
          std::memcpy(memory_->TranslateVirtual(info_ptr), info, sizeof(info));
        } else {
          std::memcpy(info, memory_->TranslateVirtual(info_ptr), sizeof(info));
        }
        if (nonce_ptr) RandomBytes(memory_->TranslateVirtual(nonce_ptr), 8);
        REXLOG_INFO("[online] XSessionCreate(flags {:08X}, public {}, private {}): {} session {:016X}", flags, word(8),
                    word(12), host ? "hosts" : "joins", SessionId(info));
        live_session_ = word(0);
        OnSessionCreated(host, info);
        return kSuccess;
      }
      case kXUserWriteAchievements: {
        // An array of {user, achievement}, of which only player 1's are kept by this install.
        const uint32_t count = word(0), entries_ptr = word(4);
        if (buffer_length != 8 || !entries_ptr || !count || count > 64) break;
        const uint8_t* entries = memory_->TranslateVirtual(entries_ptr);
        bool others = false;
        for (uint32_t i = 0; i < count; ++i) {
          others |= LocalPlayer(rex::memory::load_and_swap<uint32_t>(entries + 8 * i));
        }
        if (!others) break;
        for (uint32_t i = 0; i < count; ++i) {
          const uint32_t user = rex::memory::load_and_swap<uint32_t>(entries + 8 * i);
          const uint32_t id = rex::memory::load_and_swap<uint32_t>(entries + 8 * i + 4);
          if (LocalPlayer(user)) {
            REXLOG_INFO("[players] achievement {} of player {} is not kept", id, user + 1);
          } else {
            kernel_state_->UnlockAchievement(id);
          }
        }
        return kSuccess;
      }
      case kXSessionDelete:
        if (!live_session_ || word(0) != live_session_) break;
        REXLOG_INFO("[online] XSessionDelete");
        live_session_ = 0;
        OnSessionDeleted();
        return kSuccess;
      case kXSessionLeave:
        return kSuccess;  // the runtime has no answer for this one
      case kXSessionMigrateHost: {
        // With a real user index this machine takes over and gets the info, otherwise the new host's info comes in.
        const uint32_t info_ptr = word(4), user = word(8);
        if (!info_ptr) return kSuccess;
        uint8_t* guest_info = memory_->TranslateVirtual(info_ptr);
        uint8_t info[kSessionInfoSize], local[36];
        std::memcpy(info, guest_info, sizeof(info));
        relay::FillLocalXnAddr(local);
        const bool takes_over = user < 4 || std::memcmp(info + 8, local, 4) == 0;
        if (takes_over) {
          uint8_t current[kSessionInfoSize];
          if (CurrentSession(current)) {
            std::memcpy(info, current, 8);             // same session id
            std::memcpy(info + 44, current + 44, 16);  // and key
          }
          std::memcpy(info + 8, local, sizeof(local));
          std::memcpy(guest_info, info, sizeof(info));
        }
        REXLOG_INFO("[online] XSessionMigrateHost(user {}): {} hosts session {:016X}", user,
                    takes_over ? "this machine" : "another member", SessionId(info));
        OnHostMigrated(takes_over, info);
        return kSuccess;
      }
      case kXUserReadStats:
        return ReadStats(buffer);
      default:
        break;
    }
    return XgiApp::DispatchMessageSync(message, buffer_ptr, buffer_length);
  }

 private:
  // One empty view per spec, with a rows pointer that is valid to read: the game sorts the rows without checking.
  X_HRESULT ReadStats(const uint8_t* buffer) {
    auto word = [&](uint32_t offset) { return rex::memory::load_and_swap<uint32_t>(buffer + offset); };
    const uint32_t specs = word(12), specs_ptr = word(16), size = word(20), results_ptr = word(24);
    if (!results_ptr || size < sizeof(StatsResults)) return kInvalidArg;
    if (boards::FriendsView(specs, specs_ptr, size, results_ptr)) return kSuccess;
    uint8_t* results = memory_->TranslateVirtual(results_ptr);
    std::memset(results, 0, size);
    const uint32_t views = std::min<uint32_t>(specs, (size - sizeof(StatsResults)) / sizeof(StatsView));
    const uint32_t views_ptr = results_ptr + uint32_t(sizeof(StatsResults));
    auto* header = reinterpret_cast<StatsResults*>(results);
    header->num_views = views;
    header->views_ptr = views_ptr;
    uint32_t rows_ptr = views_ptr + views * uint32_t(sizeof(StatsView));
    if (rows_ptr >= results_ptr + size) rows_ptr = views_ptr;
    auto* view = reinterpret_cast<StatsView*>(results + sizeof(StatsResults));
    for (uint32_t i = 0; i < views; ++i) {
      // A spec is 136 bytes and starts with its view id.
      view[i].view_id = specs_ptr ? rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(specs_ptr + i * 136)) : 0;
      view[i].rows_ptr = rows_ptr;
    }
    return kSuccess;
  }

  uint32_t live_session_ = 0;  // the session object of the LIVE session, 0 when there is none
};

// ---- XLive -------------------------------------------------------------------------------------------------------

class LiveApp final : public rex::kernel::xam::apps::XLiveBaseApp {
 public:
  using XLiveBaseApp::XLiveBaseApp;

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr, uint32_t buffer_length) override {
    if (message == kLogonGetHR && Live()) return kLogonEstablished;
    if (message == kInviteGetAcceptedInfo) return AcceptedInvite(buffer_length);
    if (message == kFriendsEnumerate) return FriendsEnumerator(buffer_length);
    if (message == kMuteListQuery) return kSuccess;  // the caller's answer stays "not muted"
    return XLiveBaseApp::DispatchMessageSync(message, buffer_ptr, buffer_length);
  }

 private:
  // An empty friends list the game can enumerate; the arguments are {&user, &start, &count, &size out, &handle out}.
  X_HRESULT FriendsEnumerator(uint32_t args_ptr) {
    if (!args_ptr) return kInvalidArg;
    const uint8_t* args = memory_->TranslateVirtual(args_ptr);
    auto value = [&](int entry) { return rex::memory::load_and_swap<uint32_t>(args + entry * 16 + 12); };
    auto deref = [&](int entry) {
      return value(entry) ? rex::memory::load_and_swap<uint32_t>(memory_->TranslateVirtual(value(entry))) : 0;
    };
    const uint32_t user = deref(0), count = deref(2), size_ptr = value(3), handle_ptr = value(4);
    if (!handle_ptr) return kInvalidArg;
    rex::memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr), 0);
    if (!size_ptr || user >= 4 || count > 100) return kInvalidArg;
    constexpr size_t kFriendSize = 0xC4;  // XONLINE_FRIEND
    auto list = rex::system::object_ref<rex::system::XStaticUntypedEnumerator>(
        new rex::system::XStaticUntypedEnumerator(kernel_state_, count, kFriendSize));
    const X_HRESULT result = list->Initialize(user, app_id(), 0x58021, 0x58022, 0);
    if (XFAILED(result)) return result;
    rex::memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(size_ptr), uint32_t(count * kFriendSize));
    rex::memory::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr), list->handle());
    return kSuccess;
  }

  // The arguments are marshalled as {&user index, XINVITE_INFO*}: 16-byte entries, the value in the last 4 bytes.
  X_HRESULT AcceptedInvite(uint32_t args_ptr) {
    if (!args_ptr) return kInvalidArg;
    const uint8_t* args = memory_->TranslateVirtual(args_ptr);
    const uint32_t info_ptr = rex::memory::load_and_swap<uint32_t>(args + 16 + 12);
    if (rex::memory::load_and_swap<uint32_t>(args + 0x200) < 2 || !info_ptr) return kInvalidArg;
    uint8_t host[kSessionInfoSize];
    if (!HostSession(host)) {
      REXLOG_WARN("[online] XInviteGetAcceptedInfo without a lobby host session");
      return kFail;
    }
    auto* info = memory_->TranslateVirtual<InviteInfo*>(info_ptr);
    std::memset(info, 0, sizeof(*info));
    info->xuid_invitee = Xuid();
    // The game refuses an invite from a local profile, so the inviter is made up from the session id.
    info->xuid_inviter = 0x0009000000000000ull | (SessionId(host) & 0xFFFFFFFFull);
    info->title_id = kernel_state_->title_id();
    std::memcpy(info->host, host, sizeof(host));
    info->from_game_invite = 1;
    REXLOG_INFO("[online] XInviteGetAcceptedInfo: the lobby host's session {:016X}", SessionId(host));
    return kSuccess;
  }
};

// ---- The player --------------------------------------------------------------------------------------------------

// Player 1 is the Game Jolt account while one is signed in, and the local profile otherwise; players 2 to 4 are
// local profiles (local_players.h).
u32 GetSigninState(u32 user) { return user == 0 ? (Live() ? 2u : 1u) : (LocalPlayer(user) ? 1u : 0u); }

u32 GetXuid(u32 user, u32 type_mask, u32 xuid_ptr) {
  if (!xuid_ptr) return kInvalidArg;
  auto* out = Guest<rex::be<uint64_t>>(xuid_ptr);
  *out = 0;
  if (user >= 4) return kInvalidArg;
  if (LocalPlayer(user)) {
    *out = LocalPlayerXuid(user);
    return kSuccess;
  }
  if (user != 0) return kNoSuchUser;
  const uint64_t xuid = Xuid();
  *out = xuid ? xuid : REX_KERNEL_STATE()->user_profile()->xuid();
  return kSuccess;
}

u32 GetName(u32 user, u32 buffer_ptr, u32 buffer_len) {
  if (user >= 4) return kInvalidArg;
  if (user != 0 && !LocalPlayer(user)) return kNoSuchUser;
  const std::string name = user != 0 ? LocalPlayerName(user)
                           : Live()  ? Name()
                                     : std::string(REX_KERNEL_STATE()->user_profile()->name());
  if (buffer_ptr && buffer_len) {
    rex::string::copy_truncating(Guest<char>(buffer_ptr), name, std::min<uint32_t>(buffer_len, 16));
  }
  return kSuccess;
}

// XUSER_SIGNIN_INFO: xuid, info flags (1 LIVE enabled, 2 guest), sign-in state, guest number, sponsor, name[16].
u32 GetSigninInfo(u32 user, u32 flags, u32 info_ptr) {
  if (!info_ptr) return kInvalidArg;
  uint8_t* info = Guest<uint8_t>(info_ptr);
  std::memset(info, 0, 40);
  if (LocalPlayer(user)) {
    rex::memory::store_and_swap<uint64_t>(info, LocalPlayerXuid(user));
    rex::memory::store_and_swap<uint32_t>(info + 12, 1);
    rex::string::copy_truncating(reinterpret_cast<char*>(info + 24), LocalPlayerName(user), 16);
    return kSuccess;
  }
  if (user != 0) return kNoSuchUser;
  const bool live = Live();
  const auto* profile = REX_KERNEL_STATE()->user_profile();
  rex::memory::store_and_swap<uint64_t>(info, live ? Xuid() : profile->xuid());
  rex::memory::store_and_swap<uint32_t>(info + 8, live ? 1 : 0);
  rex::memory::store_and_swap<uint32_t>(info + 12, live ? 2 : 1);
  const std::string name = live ? Name() : std::string(profile->name());
  rex::string::copy_truncating(reinterpret_cast<char*>(info + 24), name, 16);
  return kSuccess;
}

// Every privilege with a Game Jolt account, none without (as the runtime answers).
u32 CheckPrivilege(u32 user, u32 privilege, u32 out_ptr) {
  if (user != 0xFF && user >= 4) return kErrorInvalidParameter;
  if (LocalPlayer(user)) {
    if (out_ptr) *Guest<rex::be<uint32_t>>(out_ptr) = 0;
    return 0;
  }
  if (user != 0xFF && user != 0) return kErrorNoSuchUser;
  if (out_ptr) *Guest<rex::be<uint32_t>>(out_ptr) = Live() ? 1 : 0;
  return 0;
}

}  // namespace

// Keep in step with the SDK's rex::kernel::InitializeKernel: the same apps and modules in the same order.
void InitializeKernel(rex::Runtime* runtime, rex::system::KernelState* kernel_state) {
  auto* apps = kernel_state->app_manager();
  apps->RegisterApp(std::make_unique<rex::kernel::xam::apps::XmpApp>(kernel_state));
  apps->RegisterApp(std::make_unique<SessionApp>(kernel_state));
  apps->RegisterApp(std::make_unique<LiveApp>(kernel_state));
  apps->RegisterApp(std::make_unique<rex::kernel::xam::apps::XamApp>(kernel_state));
  kernel_state->LoadKernelModule<rex::kernel::xboxkrnl::XboxkrnlModule>();
  kernel_state->LoadKernelModule<rex::kernel::xam::XamModule>();
  kernel_state->LoadKernelModule<rex::kernel::xbdm::XbdmModule>();
}

}  // namespace cc1::online

// XShowGamerCardUI(user, xuid) serves every Gamer Card button of the game's and shows no card, except that in a lobby
// with spectator seats the lobby's button toggles the player's watching (spectator_lua.h, online_menu_lua.h).
REX_HOOK_RAW(__imp__XamShowGamerCardUIForXUID) {
  cc1::spectator::OnGamercard();
  ctx.r3.u64 = 0;
}

// XShowAchievementsUI(user): the main and pause menus' Achievements.
REX_HOOK_RAW(__imp__XamShowAchievementsUI) {
  REXLOG_INFO("[online] the game asked for its achievements: opening the Guide on them");
  rexglue_guide::live::RequestAchievements();
  ctx.r3.u64 = 0;
}

REX_HOOK(__imp__XamUserGetSigninState, cc1::online::GetSigninState)
REX_HOOK(__imp__XamUserGetXUID, cc1::online::GetXuid)
REX_HOOK(__imp__XamUserGetName, cc1::online::GetName)
REX_HOOK(__imp__XamUserGetSigninInfo, cc1::online::GetSigninInfo)
REX_HOOK(__imp__XamUserCheckPrivilege, cc1::online::CheckPrivilege)

// The profile's Xbox LIVE storage sync (a number kept in title storage) would retry every frame with no storage server,
// so it never starts, as when the profile is local.
REX_HOOK_RAW(sub_8297AB88) {}
