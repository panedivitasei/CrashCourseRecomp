// Online Multiplayer (online.h): lists the server's lobbies in a Guide window, seats a joining player at the relay
// and hands the match to the game's own LIVE code, where the host publishes its session and the members join it as
// an accepted invite (xam_live.cpp).

#include "online.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <jmlive/jmlive.h>
#include <rexglue_guide/guide.h>

#include "game_lua.h"
#include "online/leaderboards.h"
#include "online/lobby_pose.h"
#include "online/relay.h"
#include "online_menu_lua.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>

namespace cc1::online {
namespace {

using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr char kMenuId[] = "cc1-lobbies";
constexpr auto kListEvery = std::chrono::seconds(3);
constexpr auto kHeartbeatEvery = std::chrono::seconds(10);  // the server allows 45 s of quiet
constexpr auto kWaitingEvery = std::chrono::seconds(2);     // while waiting for the host's session
constexpr auto kWatchedEvery = std::chrono::seconds(2);     // in a lobby whose members can turn to watching
constexpr auto kJoinDeadline = std::chrono::seconds(60);    // for the game to join once invited
constexpr uint32_t kNotifySigninChanged = 0x0000000A, kNotifyLiveConnection = 0x02000001;
constexpr uint32_t kNotifyInviteAccepted = 0x02000002, kLogonEstablished = 0x001510F0;

enum class Phase {
  kNone,       // in no lobby
  kHost,       // in a lobby as its host; the game is in its create-match screens
  kWaitHost,   // in a lobby, waiting for the host's game to publish a session
  kInvited,    // handed the host's session; the game is joining it
  kInSession,  // the game is in the lobby's session
};

struct Lobby {
  std::string id, name;
  int players = 0, max_players = 0;
  int spectators = 0;  // how many of its members may watch
};
struct Member {
  std::string name;
  bool host = false;
  bool spectator = false;
  int pid = 0;  // the member's seat at the relay
};
struct Session {
  bool set = false;
  uint8_t info[kSessionInfoSize] = {};
};

std::string g_server_url;
std::thread g_thread;

// The account, read on guest threads.
std::atomic<bool> g_live{false};
std::atomic<uint64_t> g_xuid{0};

std::mutex g_mutex;  // guards the rest
std::condition_variable g_wake;
bool g_stop = false;
std::string g_name;          // the account's name
std::vector<Lobby> g_lobbies;
bool g_listed = false;       // g_lobbies came from the server
std::string g_want;          // the lobby to join next
bool g_leave = false;        // leave the one joined
int g_spectate = -1;         // 0 or 1: ask the server to have this player race or watch
std::string g_kick;          // the member the host kicked in the game: the server is to take them out as well
std::string g_leave_notice;  // the reason shown to the player on leaving
Phase g_phase = Phase::kNone;
Lobby g_joined;              // id empty: in none
std::vector<Member> g_members;
std::string g_host_name;
Session g_host_session;      // what the lobby's host published
Session g_session;           // the session this game is in
bool g_publish = false;      // g_session is to be published: this game hosts
std::string g_notice;        // why the last request failed
Clock::time_point g_deadline;  // kInvited: when to give up

// What the game's main thread is to do next.
enum class Action { kNone, kHost, kInvite };
std::atomic<Action> g_action{Action::kNone};

// ---- The server ------------------------------------------------------------------

struct Reply {
  int status = 0;  // 0: no answer
  json body;
};

std::wstring Wide(const std::string& text) {
  std::wstring wide(text.size(), L'\0');
  const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), wide.data(),
                                         int(wide.size()));
  wide.resize(length > 0 ? length : 0);
  return wide;
}

Reply Request(const wchar_t* verb, const std::string& path, const std::string& session,
              const std::string& body = "{}", int timeout_ms = 8000) {
  Reply reply;
  const std::wstring url = Wide(g_server_url);
  wchar_t host[256];
  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.lpszHostName = host;
  parts.dwHostNameLength = DWORD(std::size(host));
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) return reply;

  static const HINTERNET http =
      WinHttpOpen(L"CrashCourse1/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                  WINHTTP_NO_PROXY_BYPASS, 0);
  const HINTERNET connection = http ? WinHttpConnect(http, host, parts.nPort, 0) : nullptr;
  const HINTERNET request =
      connection ? WinHttpOpenRequest(connection, verb, Wide(path).c_str(), nullptr,
                                      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0)
                 : nullptr;
  if (request) {
    WinHttpSetTimeouts(request, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    const std::wstring headers =
        L"Content-Type: application/json\r\nAuthorization: Bearer " + Wide(session) + L"\r\n";
    std::string sent = verb[0] == L'P' ? body : std::string();  // a GET carries nothing
    const DWORD size = DWORD(sent.size());
    if (WinHttpSendRequest(request, headers.c_str(), DWORD(-1), size ? sent.data() : nullptr, size, size, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
      DWORD code = 0, code_size = sizeof(code);
      WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &code_size, WINHTTP_NO_HEADER_INDEX);
      std::string text;
      char buffer[4096];
      DWORD read = 0;
      while (text.size() < (1u << 18) && WinHttpReadData(request, buffer, sizeof(buffer), &read) && read) {
        text.append(buffer, read);
      }
      reply.status = int(code);
      reply.body = json::parse(text, nullptr, false);
    }
    WinHttpCloseHandle(request);
  }
  if (connection) WinHttpCloseHandle(connection);
  return reply;
}

// A field of a reply; empty, zero or false when it's missing or of another type.
const json& At(const json& object, const char* key) {
  static const json none;
  if (!object.is_object()) return none;
  const auto at = object.find(key);
  return at != object.end() ? *at : none;
}
std::string Str(const json& object, const char* key) {
  const json& value = At(object, key);
  return value.is_string() ? value.get<std::string>() : std::string();
}
int Int(const json& object, const char* key) {
  const json& value = At(object, key);
  return value.is_number_integer() ? value.get<int>() : 0;
}
bool Flag(const json& object, const char* key) {
  const json& value = At(object, key);
  return value.is_boolean() && value.get<bool>();
}

// What to tell the player when a request didn't succeed.
std::string Failure(const Reply& reply) {
  const std::string error = Str(reply.body, "error");
  if (reply.status == 0) return "Can't reach the JMstudios server.";
  if (reply.status == 401) return "Your session ended. Sign in again from the Guide.";
  if (error == "lobby full") return "That lobby is full.";
  if (error == "not in lobby") return "You are no longer in the lobby.";
  if (error == "kicked") return "The host removed you from this lobby.";
  if (reply.status == 429) return "Too many requests. Try again in a moment.";
  return "The JMstudios server refused (" + std::to_string(reply.status) + ").";
}

std::string ToHex(const uint8_t* data, size_t size) {
  static const char digits[] = "0123456789abcdef";
  std::string hex;
  for (size_t i = 0; i < size; ++i) {
    hex += digits[data[i] >> 4];
    hex += digits[data[i] & 15];
  }
  return hex;
}

bool FromHex(const std::string& hex, uint8_t* out, size_t size) {
  if (hex.size() != size * 2) return false;
  auto nibble = [](char c) { return std::isdigit(uint8_t(c)) ? c - '0' : (std::tolower(uint8_t(c)) - 'a' + 10); };
  for (size_t i = 0; i < size; ++i) {
    if (!std::isxdigit(uint8_t(hex[i * 2])) || !std::isxdigit(uint8_t(hex[i * 2 + 1]))) return false;
    out[i] = uint8_t((nibble(hex[i * 2]) << 4) | nibble(hex[i * 2 + 1]));
  }
  return true;
}

// ---- The account -----------------------------------------------------------------

// An XUID in LIVE's format from the account's name, the same on every machine.
uint64_t XuidForName(const std::string& name) {
  uint64_t hash = 0xCBF29CE484222325ull;  // FNV-1a
  for (const char c : name) hash = (hash ^ uint8_t(std::tolower(uint8_t(c)))) * 0x100000001B3ull;
  return 0x0009000000000000ull | (hash & 0x0000FFFFFFFFFFFFull);
}

// Follows the Guide's Game Jolt sign-in and tells the game when it changes; worker thread, g_mutex not held.
void FollowAccount(const jmlive::Account& account) {
  const bool live = !account.session.empty() && !account.username.empty();
  {
    std::lock_guard lock(g_mutex);
    if (live == g_live.load() && (!live || account.username == g_name)) return;
    g_name = live ? account.username : std::string();
    g_xuid = live ? XuidForName(account.username) : 0;
    g_live = live;
  }
  REXLOG_INFO("[online] the game's profile is {}", live ? "signed in to LIVE as the Game Jolt account" : "local");
  if (auto* kernel = rex::system::kernel_state()) {
    kernel->BroadcastNotification(kNotifySigninChanged, 1);
    if (live) kernel->BroadcastNotification(kNotifyLiveConnection, kLogonEstablished);
  }
}

// ---- The worker ------------------------------------------------------------------

// Leaves the lobby locally; g_mutex held.
void Forget() {
  g_phase = Phase::kNone;
  g_joined = {};
  g_members.clear();
  g_host_name.clear();
  g_host_session = {};
  g_publish = false;
  g_spectate = -1;
}

// Takes a join or heartbeat reply, returning false when it was not one; g_mutex held.
bool TakeLobby(const Reply& reply) {
  if (reply.status != 200) return false;
  const json& lobby = At(reply.body, "lobby");
  g_members.clear();
  g_host_name.clear();
  const json& members = At(reply.body, "members");
  if (members.is_array()) {
    for (const json& member : members) {
      g_members.push_back(
          {Str(member, "username"), Flag(member, "host"), Flag(member, "spectator"), Int(member, "pid")});
      if (g_members.back().host) g_host_name = g_members.back().name;
    }
  }
  g_joined = {Str(lobby, "id"), Str(lobby, "name"), int(g_members.size()), Int(lobby, "max_players"),
              Int(lobby, "spectators")};
  g_host_session.set = FromHex(Str(reply.body, "session_info"), g_host_session.info, kSessionInfoSize);
  g_notice.clear();
  return true;
}

// Takes a list reply; g_mutex held.
void TakeList(const Reply& reply) {
  if (reply.status != 200) {
    g_notice = Failure(reply);
    return;
  }
  g_lobbies.clear();
  const json& lobbies = At(reply.body, "lobbies");
  if (lobbies.is_array()) {
    for (const json& lobby : lobbies) {
      g_lobbies.push_back(
          {Str(lobby, "id"), Str(lobby, "name"), Int(lobby, "players"), Int(lobby, "max_players")});
    }
  }
  g_listed = true;
  g_notice.clear();
}

// Moves a member's phase on once the server has answered; g_mutex held.
void Advance(const Reply& reply) {
  const bool hosts = Int(reply.body, "pid") != 0 && Int(reply.body, "pid") == Int(reply.body, "host_pid");
  if (g_phase == Phase::kNone || g_phase == Phase::kWaitHost) {
    if (hosts) {
      g_phase = Phase::kHost;
      g_action = Action::kHost;
    } else if (g_host_session.set) {
      g_phase = Phase::kInvited;
      g_deadline = Clock::now() + kJoinDeadline;
      g_action = Action::kInvite;
    } else {
      g_phase = Phase::kWaitHost;
    }
  }
}

void Worker() {
  const std::string lobbies = "/v1/lobbies";
  auto next_list = Clock::now(), next_heartbeat = Clock::now();
  bool at_once = false;  // skip the wait: there is more to do
  std::unique_lock lock(g_mutex);
  while (!g_stop) {
    if (!at_once) g_wake.wait_for(lock, std::chrono::milliseconds(250));
    at_once = false;
    if (g_stop) break;
    lock.unlock();

    // The Guide has locks of its own, so it is asked without g_mutex held.
    const bool shown = rexglue_guide::MenuShown(kMenuId);
    const jmlive::Account account = jmlive::GameJolt(0);
    FollowAccount(account);
    const std::string& session = account.session;
    const auto now = Clock::now();

    lock.lock();
    const std::string want = std::exchange(g_want, {});
    const std::string joined = g_joined.id;
    const Phase phase = g_phase;
    bool leave = std::exchange(g_leave, false);
    std::string leave_notice = std::exchange(g_leave_notice, {});
    // Until the game takes over the window is the lobby, so closing it leaves.
    if (phase == Phase::kWaitHost && !shown) leave = true;
    if (phase == Phase::kInvited && now >= g_deadline) {
      leave = true;
      leave_notice = "Couldn't join the match.";
    }
    const int spectate = std::exchange(g_spectate, -1);
    const std::string kick = std::exchange(g_kick, {});
    const bool watched = g_joined.spectators > 0;
    const bool publish = std::exchange(g_publish, false);
    const std::string published = publish ? ToHex(g_session.info, kSessionInfoSize) : std::string();
    lock.unlock();

    if (session.empty()) {  // signed out
      if (!joined.empty()) relay::Stop();
      lock.lock();
      Forget();
      g_lobbies.clear();
      g_listed = false;
    } else if (!joined.empty() && (leave || (!want.empty() && want != joined))) {
      relay::Stop();
      const Reply reply = Request(L"POST", lobbies + "/" + joined + "/leave", session);
      REXLOG_INFO("[online] left {}: {}", joined, reply.status);
      lock.lock();
      Forget();
      if (!leave_notice.empty()) g_notice = leave_notice;
      if (!want.empty() && want != joined && g_want.empty()) g_want = want;  // the join is next
      next_list = now;
      at_once = true;
    } else if (!want.empty() && joined.empty()) {
      const Reply reply = Request(L"POST", lobbies + "/" + want + "/join", session);
      const std::string relay_addr = Str(reply.body, "relay"), ticket = Str(reply.body, "relay_token");
      const int pid = Int(reply.body, "pid");
      REXLOG_INFO("[online] join {}: {} (seat {}, host seat {})", want, reply.status, pid,
                  Int(reply.body, "host_pid"));
      if (reply.status == 200 && pid && !relay_addr.empty() && !ticket.empty()) {
        relay::Start(relay_addr, ticket, pid);
      }
      lock.lock();
      if (!TakeLobby(reply)) {
        g_notice = Failure(reply);
      } else if (!relay::Pid()) {
        g_leave = true;
        g_leave_notice = "The lobby has no relay.";
      } else {
        Advance(reply);
      }
      next_heartbeat = Clock::now() + kWaitingEvery;
      at_once = true;
    } else if (!joined.empty() && publish) {
      const Reply reply =
          Request(L"POST", lobbies + "/" + joined + "/session", session, "{\"session_info\":\"" + published + "\"}");
      REXLOG_INFO("[online] published this game's session: {}", reply.status);
      lock.lock();
      if (reply.status != 200) g_publish = g_session.set;  // retried on the next round
    } else if (!joined.empty() && !kick.empty()) {
      const Reply reply = Request(L"POST", lobbies + "/" + joined + "/kick", session,
                                  json{{"username", kick}}.dump());
      REXLOG_INFO("[online] kicked a member from the lobby: {}", reply.status);
      lock.lock();
      TakeLobby(reply);
    } else if (!joined.empty() && spectate >= 0) {
      const Reply reply = Request(L"POST", lobbies + "/" + joined + "/spectate", session,
                                  spectate ? "{\"spectator\":true}" : "{\"spectator\":false}");
      REXLOG_INFO("[online] {}: {}", spectate ? "watching the match" : "racing in the match", reply.status);
      lock.lock();
      TakeLobby(reply);  // refused: the next heartbeat has the lobby as it stands
    } else if (!joined.empty() && now >= next_heartbeat) {
      const Reply reply = Request(L"POST", lobbies + "/" + joined + "/heartbeat", session);
      lock.lock();
      if (reply.status == 0) {
        g_notice = Failure(reply);  // one lost request: the next may get through
      } else if (!TakeLobby(reply)) {
        g_leave = true;  // the server dropped this member
        g_leave_notice = Failure(reply);
      } else {
        Advance(reply);
      }
      next_heartbeat = Clock::now() + (g_phase == Phase::kWaitHost ? kWaitingEvery
                                       : watched                    ? kWatchedEvery
                                                                    : kHeartbeatEvery);
    } else if (shown && joined.empty() && now >= next_list) {
      const Reply reply = Request(L"GET", lobbies, session);
      lock.lock();
      TakeList(reply);
      next_list = Clock::now() + kListEvery;
    } else {
      lock.lock();
    }
  }
  // Closing the game leaves the lobby at once instead of timing out of it.
  const std::string joined = g_joined.id;
  lock.unlock();
  relay::Stop();
  const std::string session = jmlive::GameJolt(0).session;
  if (!joined.empty() && !session.empty()) {
    Request(L"POST", lobbies + "/" + joined + "/leave", session, "{}", 2000);
  }
}

void Leave(std::string notice = {}) {
  {
    std::lock_guard lock(g_mutex);
    if (g_phase == Phase::kNone) return;
    g_leave = true;
    g_leave_notice = std::move(notice);
  }
  g_wake.notify_all();
}

// ---- The window ------------------------------------------------------------------

std::string Count(int players, int max_players) {
  return std::to_string(players) + "/" + std::to_string(max_players);
}

// Runs on the Guide's drawing thread each frame the window is up.
void BuildMenu(rexglue_guide::Menu& menu) {
  using Tone = rexglue_guide::Menu::Tone;
  if (!Live()) {
    menu.Text("Sign in with Game Jolt to play online.");
    if (menu.Button("Sign In")) {
      rexglue_guide::ShowMenu(kMenuId, false);
      jmlive::ShowSignIn(0);
    }
    return;
  }

  std::unique_lock lock(g_mutex);
  const std::vector<Lobby> lobbies = g_lobbies;
  const bool listed = g_listed;
  const Lobby joined = g_joined;
  const std::vector<Member> members = g_members;
  const std::string notice = g_notice, host = g_host_name;
  const bool joining = !g_want.empty();
  lock.unlock();

  std::string join;
  bool leave = false;
  if (!joined.id.empty()) {
    menu.Header(joined.name + "  " + Count(joined.players, joined.max_players));
    for (const Member& member : members) {
      menu.Text(member.name + (member.host ? "  (host)" : member.spectator ? "  (spectator)" : ""));
    }
    menu.Text("Waiting for " + (host.empty() ? std::string("the host") : host) + " to set up the match...",
              Tone::kDim);
    if (!notice.empty()) menu.Text(notice, Tone::kError);
    leave = menu.Button("Leave Lobby");
  } else {
    menu.Header("Lobbies");
    if (!listed && notice.empty()) menu.Text("Finding lobbies...", Tone::kDim);
    for (const Lobby& lobby : lobbies) {
      const bool full = lobby.players >= lobby.max_players;
      // The "###" id keeps it the same row as its player count changes.
      if (menu.Button(lobby.name + "###" + lobby.id,
                      {.detail = Count(lobby.players, lobby.max_players), .disabled = full || joining})) {
        join = lobby.id;
      }
    }
    if (joining) menu.Text("Joining...", Tone::kDim);
    if (!notice.empty()) menu.Text(notice, Tone::kError);
  }

  if (leave) Leave();
  if (!join.empty()) {
    lock.lock();
    g_want = join;
    g_notice.clear();
    lock.unlock();
    g_wake.notify_all();
  }
}

// The host's Y on the results sends every member's game back to the lobby through the relay.
constexpr uint8_t kToLobby = 3;           // control message: the kind, then the host's count of them
std::atomic<bool> g_results_host{false};  // the results are up and this player hosts: Y is taken
std::atomic<bool> g_y_pressed{false};
std::atomic<bool> g_kickable{false};      // the lobby shows the host its Kick: RB and RT are taken
std::atomic<bool> g_kick_pressed{false};
std::atomic<bool> g_hosting{false};       // the lobby shows the host its Change Course: LT and LB are taken
std::atomic<bool> g_course_pressed{false};
std::atomic<uint32_t> g_to_lobby{0};      // a count received and not acted on yet (0: none)
uint32_t g_lobby_count = 0;               // the last one acted on; the game's main thread
int g_lobby_sends = 0;                    // times the host's message is still to be repeated, against loss

// Takes a member's control message on the relay's thread.
void OnControl(uint8_t src_pid, const uint8_t* data, size_t size) {
  if (size != 5 || data[0] != kToLobby) return;
  uint32_t count;
  std::memcpy(&count, data + 1, 4);
  bool from_host = false;
  for (const Player& player : Players()) from_host = from_host || (player.host && player.pid == src_pid);
  if (from_host && count) g_to_lobby = count;
}

struct ControlInstaller {
  ControlInstaller() { relay::AddControlHook(&OnControl); }
} g_control_installer;

void SendToLobby() {
  uint8_t message[5] = {kToLobby};
  std::memcpy(message + 1, &g_lobby_count, 4);
  relay::SendControl(message, sizeof(message));
}

std::string g_heading;  // the lobby name the menu script has; the game's main thread
bool g_heading_told = false;

void OnInstalled() { g_heading_told = false; }

// Runs on the game's main thread every GUI frame with what the menu script reported, and acts on what the worker
// asked for.
void OnEvents(const std::string& events) {
  pose::Tick();
  boards::Tick();
  // Which of the host's prompts the script has on screen.
  g_results_host = events.find("results;") != std::string::npos;
  g_kickable = events.find("kick;") != std::string::npos;
  g_hosting = events.find("hosting;") != std::string::npos;
  if (g_course_pressed.exchange(false) && g_hosting) {
    std::string out;
    if (!lua::Run("return CC1O_Course()", &out)) lua::LogError("online change course", out);
  }
  if (events.find("course;") != std::string::npos) {
    pose::SendCourse();
    REXLOG_INFO("[online] the host chose a course: the others are told");
  }
  if (events.find("relobby;") != std::string::npos) {
    pose::Requeue();
    REXLOG_INFO("[online] back in the lobby");
  }
  if (g_kick_pressed.exchange(false) && g_kickable) {
    std::string name;
    if (!lua::Run("return CC1O_Kick()", &name)) {
      lua::LogError("online kick", name);
    } else if (!name.empty()) {
      REXLOG_INFO("[online] the host kicks a player");
      {
        std::lock_guard lock(g_mutex);
        g_kick = name;
      }
      g_wake.notify_all();
    }
  }
  bool to_lobby = false;
  if (g_y_pressed.exchange(false) && g_results_host) {
    RandomBytes(&g_lobby_count, sizeof(g_lobby_count));
    g_lobby_count |= 1;
    g_lobby_sends = 20;
    to_lobby = true;
    REXLOG_INFO("[online] the host takes the match back to the lobby");
  }
  if (g_lobby_sends > 0) {
    --g_lobby_sends;
    SendToLobby();
  }
  if (const uint32_t count = g_to_lobby.exchange(0); count && count != g_lobby_count) {
    g_lobby_count = count;
    to_lobby = true;
    REXLOG_INFO("[online] the host took the match back to the lobby");
  }
  if (to_lobby) {
    std::string out;
    if (!lua::Run("return CC1O_Lobby()", &out)) lua::LogError("online back to lobby", out);
  }
  {
    std::string name;
    {
      std::lock_guard lock(g_mutex);
      name = g_joined.name;
    }
    if (!g_heading_told || name != g_heading) {
      std::string quoted = "'";
      for (const char c : name) {
        if (c == '\\' || c == '\'') quoted += '\\';
        if (uint8_t(c) >= 0x20) quoted += c;
      }
      quoted += "'";
      std::string me = "'";
      for (const char c : Name()) {
        if (c != '\\' && c != '\'' && uint8_t(c) >= 0x20) me += c;
      }
      me += "'";
      if (lua::Run(name.empty() ? "CC1O_Heading(nil)" : "CC1O_Heading(" + quoted + "," + me + ")")) {
        g_heading = name;
        g_heading_told = true;
      }
    }
  }
  if (events.find("lobbies;") != std::string::npos) {
    {
      std::lock_guard lock(g_mutex);
      g_notice.clear();
    }
    rexglue_guide::ShowMenu(kMenuId);
    g_wake.notify_all();
  }
  // Back at the main menu, the game has given its match up.
  if (events.find("menu;") != std::string::npos) Leave();
  if (events.find("hostfail;") != std::string::npos) Leave("Couldn't open the match options.");

  switch (g_action.exchange(Action::kNone)) {
    case Action::kHost: {
      rexglue_guide::ShowMenu(kMenuId, false);
      std::string out;
      int seats = 0;
      {
        std::lock_guard lock(g_mutex);
        seats = g_joined.max_players;
      }
      if (!lua::Run("return CC1O_Host(" + std::to_string(seats) + ")", &out)) {
        lua::LogError("online host", out);
        Leave("Couldn't open the match options.");
      }
      REXLOG_INFO("[online] hosting the lobby: opening the game's create-match screens, {} seats", seats);
      break;
    }
    case Action::kInvite:
      rexglue_guide::ShowMenu(kMenuId, false);
      REXLOG_INFO("[online] the host's session is up: handing it to the game as an accepted invite");
      if (auto* kernel = rex::system::kernel_state()) kernel->BroadcastNotification(kNotifyInviteAccepted, 0);
      break;
    case Action::kNone:
      break;
  }
}

}  // namespace

void Initialize(std::string server_url) {
  g_server_url = std::move(server_url);
  lua::Register({"online", kMenuScript, "return CC1O_Poll()", OnInstalled, OnEvents});
  rexglue_guide::AddMenu({.id = kMenuId,
                          .title = "Online Multiplayer",
                          .place = rexglue_guide::MenuPlace::kDetached,
                          .window = {.width = 620.0f, .height = 520.0f, .hold_input = true},
                          .build = BuildMenu});
  g_thread = std::thread(Worker);
  boards::Initialize();
}

void Shutdown() {
  boards::Shutdown();
  {
    std::lock_guard lock(g_mutex);
    g_stop = true;
  }
  g_wake.notify_all();
  if (g_thread.joinable()) g_thread.join();
}

int Get(const std::string& path, std::string* body) {
  const std::string session = jmlive::GameJolt(0).session;
  if (session.empty()) return 0;
  const Reply reply = Request(L"GET", path, session);
  if (body) *body = reply.body.is_discarded() ? std::string() : reply.body.dump();
  return reply.status;
}

void NotePad(uint32_t user, uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger) {
  static std::atomic<uint16_t> before{0};
  if (user != 0) return;
  if (right_trigger > 128) buttons |= 0x0400;  // the triggers as buttons of their own
  if (left_trigger > 128) buttons |= 0x0800;
  const uint16_t pressed = buttons & ~before.exchange(buttons);
  if ((pressed & 0x8000) && g_results_host) g_y_pressed = true;  // Y
  if ((pressed & 0x0600) && g_kickable) g_kick_pressed = true;   // RB or RT
  if ((pressed & 0x0900) && g_hosting) g_course_pressed = true;  // LB or LT
}

bool Live() { return g_live; }
uint64_t Xuid() { return g_xuid; }

std::string Name() {
  std::lock_guard lock(g_mutex);
  return g_name;
}

void RandomBytes(void* out, size_t size) {
  static std::mutex mutex;
  static std::mt19937_64 engine{std::random_device{}()};
  std::lock_guard lock(mutex);
  auto* bytes = static_cast<uint8_t*>(out);
  for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(engine());
}

void OnSessionCreated(bool host, const uint8_t* info) {
  pose::Reset();
  {
    std::lock_guard lock(g_mutex);
    g_session.set = true;
    std::memcpy(g_session.info, info, kSessionInfoSize);
    if (g_phase == Phase::kNone) return;  // a match made outside a lobby: nobody can reach it
    g_phase = Phase::kInSession;
    g_publish = host;
  }
  g_wake.notify_all();
}

void OnSessionDeleted() {
  pose::Reset();
  {
    std::lock_guard lock(g_mutex);
    g_session = {};
  }
  Leave();
}

void OnHostMigrated(bool local, const uint8_t* info) {
  {
    std::lock_guard lock(g_mutex);
    g_session.set = true;
    std::memcpy(g_session.info, info, kSessionInfoSize);
    if (g_phase != Phase::kNone && local) g_publish = true;
  }
  g_wake.notify_all();
}

void CurrentSessionId(uint8_t* id) {
  std::lock_guard lock(g_mutex);
  std::memset(id, 0, 8);
  if (g_session.set) std::memcpy(id, g_session.info, 8);
}

bool CurrentSession(uint8_t* info) {
  std::lock_guard lock(g_mutex);
  if (g_session.set) std::memcpy(info, g_session.info, kSessionInfoSize);
  return g_session.set;
}

std::vector<Player> Players(int* spectator_seats) {
  std::lock_guard lock(g_mutex);
  if (spectator_seats) *spectator_seats = g_phase == Phase::kNone ? 0 : g_joined.spectators;
  std::vector<Player> players;
  if (g_phase == Phase::kNone) return players;
  for (const Member& member : g_members) {
    players.push_back({member.name, XuidForName(member.name), member.pid, member.host, member.spectator,
                       _stricmp(member.name.c_str(), g_name.c_str()) == 0});
  }
  return players;
}

void Spectate(bool watch) {
  {
    std::lock_guard lock(g_mutex);
    if (g_phase == Phase::kNone || g_joined.spectators <= 0) return;
    g_spectate = watch ? 1 : 0;
  }
  g_wake.notify_all();
}

bool HostSession(uint8_t* info) {
  std::lock_guard lock(g_mutex);
  if (g_host_session.set) std::memcpy(info, g_host_session.info, kSessionInfoSize);
  return g_host_session.set;
}

}  // namespace cc1::online
