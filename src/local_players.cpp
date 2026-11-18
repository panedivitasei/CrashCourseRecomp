// local_players.cpp: Guide sign-in for the other local players (see local_players.h).

#include "local_players.h"

#include <atomic>
#include <cstdlib>

#include <jmlive/jmlive.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/user_profile.h>
#include <rex/types.h>

#include "game_lua.h"
#include "online.h"

namespace cc1 {
namespace {

constexpr uint32_t kNotifyUi = 0x00000009, kNotifySigninChanged = 0x0000000A;

// Records the players IsCurrentUserLogged(player) reported as not signed in since the last poll, which is when the
// game shows its sign-in.
constexpr char kScript[] = R"lua(
if rawget(_G, "CC1P") then return "have" end
if not CC1_Hook then return "wait" end
local P = { asked = "" }
rawset(_G, "CC1P", P)
CC1_Hook("IsCurrentUserLogged", function(next, user, ...)
  local logged = next(user, ...)
  if not logged and type(user) == "number" then P.asked = P.asked .. user .. ";" end
  return logged
end)
rawset(_G, "CC1P_Poll", function()
  local asked = P.asked
  P.asked = ""
  return asked
end)
return "installed"
)lua";

std::atomic<bool> g_signin_shown{false};  // the game asked for a multi-pane sign-in since the last poll

// Each frame, turns the sign-in the game showed last frame into the Guide's for the player it was shown for.
void OnAsked(const std::string& asked) {
  const bool shown = g_signin_shown.exchange(false);
  if (!shown || asked.empty()) return;
  const size_t last = asked.find_last_of(';', asked.size() - 2);
  const uint32_t user = uint32_t(std::atoi(asked.c_str() + (last == std::string::npos ? 0 : last + 1)));
  uint8_t session[online::kSessionInfoSize];
  if (user < 1 || user >= kLocalPlayers || LocalPlayer(user) || online::CurrentSession(session)) return;
  REXLOG_INFO("[players] player {} signs in on the Guide", user + 1);
  jmlive::ShowSignIn(int(user));
}

// XShowSigninUI(panes, flags): the local lobby asks for four panes for the pad that pressed A, while the title
// screen's single pane is for the game's owner, which stays player 1.
rex::u32 ShowSigninUI(rex::u32 panes, rex::u32 flags) {
  (void)flags;
  if (panes > 1) g_signin_shown = true;
  uint32_t players = 1;
  for (uint32_t other = 1; other < kLocalPlayers; ++other) {
    if (LocalPlayer(other)) players |= 1u << other;
  }
  auto* kernel = REX_KERNEL_STATE();
  kernel->BroadcastNotification(kNotifySigninChanged, players);
  kernel->BroadcastNotification(kNotifyUi, 0);
  return 0;
}

}  // namespace

bool LocalPlayer(uint32_t user) {
  return user >= 1 && user < kLocalPlayers && !jmlive::Gamertag(int(user)).empty();
}

// Derived from the player's slot, not their account, so a save stays with the pad's player whoever signs in there.
uint64_t LocalPlayerXuid(uint32_t user) { return REX_KERNEL_STATE()->user_profile()->xuid() + user; }

std::string LocalPlayerName(uint32_t user) { return jmlive::Gamertag(int(user)); }

void InitializeLocalPlayers() { lua::Register({"players", kScript, "return CC1P_Poll()", nullptr, OnAsked}); }

}  // namespace cc1

REX_HOOK(__imp__XamShowSigninUI, cc1::ShowSigninUI)
