// Spectating a match: the lobby's spectator seats and the X toggle (spectator.h).

#include "spectator.h"

#include <atomic>
#include <cctype>
#include <string>
#include <vector>

#include <rex/logging.h>

#include "game_lua.h"
#include "online.h"
#include "spectator_lua.h"

namespace cc1::spectator {
namespace {

std::atomic<bool> g_lobby{false};  // the game's online lobby is on screen
std::atomic<bool> g_x{false};      // X was pressed
std::string g_pushed;              // what the script was last told

// The lobby as this player's game has it from the server.
struct Seats {
  bool marked = false;  // the lobby has spectator seats: its boxes say who watches
  bool can = false;     // this player may turn to watching, or back
  bool mine = false;    // this player watches
  std::vector<std::string> watching;
};

Seats Read() {
  Seats seats;
  int allowed = 0;
  const std::vector<online::Player> players = online::Players(&allowed);
  if (allowed <= 0) return seats;
  seats.marked = true;
  const online::Player* me = nullptr;
  for (const online::Player& player : players) {
    if (player.local) me = &player;
    if (player.spectator) seats.watching.push_back(player.name);
  }
  if (!me || !me->host) return seats;  // only the host may toggle watching
  const int taken = int(seats.watching.size());
  seats.mine = me->spectator;
  seats.can = seats.mine || (taken < allowed && int(players.size()) - taken >= 2);  // at least one racer remains
  return seats;
}

// A name as a Lua string; empty when it has anything a Game Jolt name does not.
std::string Quoted(const std::string& name) {
  for (const char c : name) {
    if (!std::isalnum(uint8_t(c)) && c != '_' && c != '-') return {};
  }
  return "'" + name + "'";
}

// Runs on the game's main thread every GUI frame.
void OnEvents(const std::string& events) {
  g_lobby = events.find("lobby;") != std::string::npos;
  for (size_t at = events.find("err,"); at != std::string::npos; at = events.find("err,", at + 1)) {
    lua::LogError("spectator script", events.substr(at + 4, events.find(';', at) - at - 4));
  }
  const Seats seats = Read();
  // X in the lobby toggles this player's watching, whichever box has the focus.
  if (g_x.exchange(false) && g_lobby && seats.can) online::Spectate(!seats.mine);
  std::string call = "CC1SP_Set(";
  call += !seats.can ? "nil" : seats.mine ? "'Race'" : "'Spectate'";
  call += ",{";
  for (const std::string& name : seats.watching) {
    if (const std::string quoted = Quoted(name); !quoted.empty()) call += quoted + ",";
  }
  call += seats.marked ? "},true," : "},false,";
  const std::string watched = Watched();
  call += watched.empty() || Quoted(watched).empty() ? "nil," : Quoted(watched) + ",";
  const int bars = Bars();
  call += bars < 0 ? "nil)" : std::to_string(bars) + ")";
  if (call == g_pushed) return;
  std::string out;
  if (!lua::Run(call, &out)) {
    lua::LogError("spectator", out);
    return;
  }
  g_pushed = call;
}

void OnInstalled() { g_pushed.clear(); }

}  // namespace

void Initialize() { lua::Register({"spectator", kScript, "return CC1SP_Poll()", OnInstalled, OnEvents}); }

bool OnGamercard() { return g_lobby && Read().marked; }

void PressX() { g_x = true; }

}  // namespace cc1::spectator
