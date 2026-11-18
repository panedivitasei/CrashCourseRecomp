// The runner a spectator sees (spectator.h): the watching player's own fighter is made the puppet of a racer by
// taking the steps, rand values and trigger commands the racer's game sends, and is put where the racer's fighter
// came to every few steps.

#include "spectator.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include <rex/system/kernel_state.h>

#include "online.h"
#include "online/relay.h"

REXCVAR_DEFINE_BOOL(cc1_puppet_trace, false, "CC1", "Logs the puppet's steps and corrections.");
REXCVAR_DEFINE_BOOL(cc1_puppet_course, true, "CC1",
                    "In a race somebody watches, the course's triggers and random choices on the watching "
                    "machine are the racer's; off leaves every machine its own course.");
REXCVAR_DEFINE_BOOL(cc1_puppet_avatar, true, "CC1",
                    "The runner a spectator sees wears the racer's avatar; off, the spectator's own.");

REX_EXTERN(__imp__sub_829AB7C0);  // FIGHTER::Step (fighter): the step's length at +424, its pad at +1820
REX_EXTERN(__imp__sub_8295EDA0);  // the input manager's analog getter (pad, command)
REX_EXTERN(__imp__sub_8295EE98);  // its held getter (pad, command)
REX_EXTERN(__imp__sub_8295EF78);  // its pressed getter (pad, command)
REX_EXTERN(__imp__sub_8299F9D0);  // whether input is blocked (fighter)
REX_EXTERN(__imp__sub_8299FD88);  // SetFighterActionState (fighter, state): the state at +3600
REX_EXTERN(__imp__sub_829344A8);  // sets an ACTOR's position (actor, float[3]): +136
REX_EXTERN(__imp__sub_829A34E8);  // the fighter's reset (fighter): at rest where it is, as after a respawn
REX_EXTERN(__imp__sub_829D3038);  // StartPlaying (game): makes the fighters and the other players' helicopters
REX_EXTERN(__imp__sub_829CD848);  // the fighter's creation (..., r7 template name)
REX_EXTERN(__imp__sub_829CD058);  // the helicopter's message (game)
REX_EXTERN(__imp__sub_829A6C30);  // the fighter's template (fighter, name)
REX_EXTERN(__imp__sub_829CEB30);  // (game, record, time in ms or -1): the record has ended the track
REX_EXTERN(__imp__sub_829CB548);  // progress (game, record) -> f1, for the HUD map's bars
REX_EXTERN(__imp__sub_829A20D0);  // the script's GetFinishPlace
REX_EXTERN(__imp__sub_8299E968);  // the fighter's animation (fighter, r4 which, r5, r6 again, f1)
REX_EXTERN(__imp__sub_824DDB68);  // rand
REX_EXTERN(__imp__sub_82921B18);  // a trigger's command (trigger or start position, toucher's handle, which command)
REX_EXTERN(__imp__sub_8285A268);  // runs a script as an actor (interpreter, thread, the actor's handle)
REX_EXTERN(__imp__sub_829B20A0);  // a helicopter's step (helicopter)
REX_EXTERN(__imp__sub_829CD3B0);  // ranks (game): each record's place (+60)
// The fighter's states, each the accessor of its one object.
REX_EXTERN(__imp__sub_821B3B48);
REX_EXTERN(__imp__sub_821B3BD8);
REX_EXTERN(__imp__sub_821CBDF8);
REX_EXTERN(__imp__sub_821CBE88);
REX_EXTERN(__imp__sub_821CC810);
REX_EXTERN(__imp__sub_821CDB40);
REX_EXTERN(__imp__sub_821CDBD0);
REX_EXTERN(__imp__sub_821CDC60);
REX_EXTERN(__imp__sub_821CDD38);
REX_EXTERN(__imp__sub_821CDDC8);
REX_EXTERN(__imp__sub_821CDE58);
REX_EXTERN(__imp__sub_821CDEE8);
REX_EXTERN(__imp__sub_821CDF78);
REX_EXTERN(__imp__sub_821CE008);
REX_EXTERN(__imp__sub_821CE098);
REX_EXTERN(__imp__sub_821CE130);
REX_EXTERN(__imp__sub_821CE1B0);
REX_EXTERN(__imp__sub_821CE248);

namespace cc1::spectator {
namespace {

using GuestFunction = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kGame = 0x82C0479C;     // GAMEXA*, null outside a course
constexpr uint32_t kNetwork = 0x82C52210;  // the session's kind at +168: 2 over LIVE
constexpr uint32_t kKernel = 0x82C52238;   // KERNEL*: the step's length at +12
constexpr uint32_t kStepLength = 424, kPad = 1820, kState = 3600, kX = 136, kY = 140, kZ = 144;
constexpr uint32_t kGrounded = 1980, kSprinting = 4020;
constexpr uint32_t kFinished = 4480, kFinishedIn = 4484, kCrossed = 4488;
constexpr uint32_t kConfig = 0x82C51140;  // GAMECONFIG*
constexpr uint32_t kRecords = 392, kRecordSize = 64, kCount = 384, kEnded = 340;
constexpr uint32_t kNetPlayer = 20, kLeft = 56, kCheated = 57;
constexpr uint32_t kActors = 0x82C52208;  // -> the engine: its actors by handle at +54068, 16 bytes each
constexpr uint32_t kHelicopters = 160, kOwnRecord = 648, kSentDepth = 1460;
constexpr uint32_t kTime = 8, kRunning = 12, kPlace = 60;
constexpr float kOutOfSight = 20000.0f;
// What a correction copies as it is: the fighter's velocities (actual, of its move, of its fall, of a push), its
// health, its speed factor and its sprint gauge.
constexpr uint32_t kWords[] = {172, 176, 180, 2044, 2048, 2052, 2056, 2060, 2064, 2080, 2084, 2088, 1456, 3708, 4024};
constexpr size_t kHealthWord = 12;

struct State {
  GuestFunction accessor;
  uint32_t object;
  const char* name;
};
constexpr State kStates[] = {
    {__imp__sub_821B3B48, 0x82BFCD18, "startup"}, {__imp__sub_821B3BD8, 0x82BFCD20, "finisher"},
    {__imp__sub_821CBDF8, 0x8324D4F0, "move"},    {__imp__sub_821CBE88, 0x8324D4F8, "wipe"},
    {__imp__sub_821CC810, 0x8324D50C, "idle"},    {__imp__sub_821CDB40, 0x8324D514, "brake"},
    {__imp__sub_821CDBD0, 0x8324D51C, "turn"},    {__imp__sub_821CDC60, 0x8324D524, "slide"},
    {__imp__sub_821CDD38, 0x8324D52C, "crouch"},  {__imp__sub_821CDDC8, 0x8324D534, "reaction"},
    {__imp__sub_821CDE58, 0x8324D53C, "block"},   {__imp__sub_821CDEE8, 0x8324D544, "flying death"},
    {__imp__sub_821CDF78, 0x8324D54C, "death"},   {__imp__sub_821CE008, 0x8324D554, "land"},
    {__imp__sub_821CE098, 0x8324D55C, "jump"},    {__imp__sub_821CE130, 0x8324D564, "falling"},
    {__imp__sub_821CE1B0, 0x8324D56C, "rope"},    {__imp__sub_821CE248, 0x8324D574, "squished"},
};
constexpr uint8_t kNoState = 0xFF, kRope = 16;
constexpr uint8_t kWipe = 3, kFlyingDeath = 11, kDeath = 12, kSquished = 17;  // the states a fighter fades out in
constexpr uint32_t kHeldRope = 4392;
constexpr uint32_t kOpacity = 348;  // an actor's: a fighter's death fades it out, and its respawn sets it to 1

// ---- The wire (relay control datagrams; both ends are this build) -----------------------

constexpr uint8_t kSteps = 2;  // the kind
constexpr int kRepeated = 6;   // steps a datagram carries: the newest and the ones before it, against loss
constexpr int kTruthEvery = 3;

#pragma pack(push, 1)
// An animation the racer's fighter was given outside its steps.
struct Pick {
  uint8_t which, r5, again, rands;
  float f1;
  uint16_t rand[2];
};
// What a step of the racer's fighter was given.
struct Step {
  float length;
  uint16_t analog[4];  // commands 4-7
  uint8_t held;        // commands 44, 32, 42
  uint8_t pressed;     // commands 42, 1, 2, 3
  uint8_t blocked;
  uint8_t rands;       // what rand gave in it
  uint16_t rand[6];
  uint8_t picks;       // the animations chosen since the step before
  Pick pick[2];
};
// What the racer's fighter came to after step 'sequence'.
struct Truth {
  uint32_t sequence;
  float x, y;
  uint32_t words[std::size(kWords)];  // as in the game's memory
  uint8_t grounded, sprinting, state;
};
// A trigger's command the racer's fighter set off.
struct Event {
  uint32_t number;    // the racer's count of them
  uint32_t sequence;  // the step it came in
  uint32_t element;   // the trigger
  int32_t command;
};
constexpr int kEvents = 8;  // events a datagram carries: the newest and the ones before it, against loss
struct Header {
  uint8_t kind;
  uint8_t count;       // steps that follow
  uint8_t truth;       // a Truth follows them
  uint8_t events;      // the number of Events after that
  uint16_t race;       // the sender's count of fighters: a new one starts the sequence again
  uint32_t first;      // the first step's sequence
};
#pragma pack(pop)

// ---- What rand gives ----------------------------------------------------------------------------

// The racer: where the next values rand gives go, and how many more there is room for.
thread_local uint16_t* t_rand_kept = nullptr;
thread_local uint8_t* t_rand_count = nullptr;
thread_local uint8_t t_rand_room = 0;
// The puppet: the values rand is to give next.
thread_local const uint16_t* t_rand_given = nullptr;
thread_local uint8_t t_rand_left = 0;

// A course element's own random numbers.
thread_local uint32_t t_element = 0;             // the element whose script is running (0: none, or not synced)
std::unordered_map<uint32_t, uint32_t> g_drawn;  // how many each element has had this race; the game's thread
uint32_t g_salt = 0;                             // the race's

uint32_t Mixed(uint32_t a, uint32_t b, uint32_t c) {
  uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA6Bu ^ (c + 0x165667B1u) * 0xC2B2AE35u;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  return h ^ (h >> 16);
}

void Random(PPCContext& ctx, uint8_t* base) {
  if (t_element) {
    ctx.r3.u64 = Mixed(g_salt, t_element, g_drawn[t_element]++) & 0x7FFF;
    return;
  }
  if (t_rand_left) {
    --t_rand_left;
    ctx.r3.u64 = *t_rand_given++;
    return;
  }
  __imp__sub_824DDB68(ctx, base);
  if (t_rand_room) {
    --t_rand_room;
    t_rand_kept[(*t_rand_count)++] = uint16_t(ctx.r3.u32);
  }
}

// ---- Guest memory and calls -----------------------------------------------------------------

uint32_t Word(uint8_t* base, uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); }
void PutWord(uint8_t* base, uint32_t at, uint32_t value) { rex::memory::store_and_swap<uint32_t>(base + at, value); }
float Float(uint8_t* base, uint32_t at) { return std::bit_cast<float>(Word(base, at)); }
void PutFloat(uint8_t* base, uint32_t at, float value) { PutWord(base, at, std::bit_cast<uint32_t>(value)); }

// The actor of a handle, as sub_821E13C0 finds it, or 0 when it is gone.
uint32_t Actor(uint8_t* base, uint64_t handle) {
  const int index = int16_t(handle >> 16);
  const uint32_t engine = Word(base, kActors);
  if (!engine || index < 0 || index >= 6000) return 0;
  const uint32_t actor = Word(base, engine + 54068 + 16 * uint32_t(index) + 4);
  return actor && rex::memory::load_and_swap<uint64_t>(base + actor + 16) == handle ? actor : 0;
}

uint32_t Call(PPCContext& ctx, uint8_t* base, GuestFunction function, uint32_t r3 = 0, uint32_t r4 = 0,
              uint32_t r5 = 0) {
  const PPCContext saved = ctx;
  ctx.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// ---- The race, as this machine is in it (the game's thread) -------------------------------------

enum class Role { kNone, kRacer, kSpectator };

struct Stream {
  bool known = false;
  uint16_t race = 0;
  uint32_t next = 0;  // the sequence after the newest step taken in
  std::deque<Step> steps;  // ending at next - 1
  std::deque<Truth> truths;
  std::deque<Event> events;  // by number
};
std::mutex g_streams_mutex;
Stream g_streams[relay::kMaxPid + 1];  // by seat

// A racer the watching player can follow.
struct Racer {
  int seat;         // at the relay
  uint32_t record;  // in the game
  uint32_t net;     // its net player
  std::string name;
};

Role g_role = Role::kNone;
uint32_t g_game = 0;             // the game the role was worked out for
std::vector<int> g_to;           // racer: the relay seats of the players who watch
std::vector<uint32_t> g_out;     // racer: their records
std::vector<Racer> g_racers;     // spectator: oldest member first
size_t g_watched = 0;            // spectator: which of them
std::atomic<int> g_cycle{0};     // LB and RB pressed since: steps through g_racers
bool g_fresh = false;            // spectator: a new race has begun
uint32_t g_own = 0;              // spectator: this player's record
uint32_t g_local = 0;            // this machine's fighter
uint32_t g_sequence = 0;         // racer: the step its fighter is in, or comes to next
std::deque<Event> g_events;      // racer: the last commands its fighter set off
uint32_t g_event_number = 0;
thread_local bool t_replayed = false;  // spectator: a racer's command is being run
Pick g_picked[2];                // racer: the animations chosen since the last step
uint8_t g_picks = 0;
bool g_stepping = false;         // racer: its fighter is in its step
std::vector<uint32_t> g_field;   // the racers' records in order, this player's among them when they race

// The XUID of a record's player, from NETMAN's player (its array at +232, the XUID at +16).
uint64_t Xuid(uint8_t* base, uint32_t game, uint32_t record) {
  const uint32_t network = Word(base, kNetwork);
  const uint32_t players = network ? Word(base, network + 232) : 0;
  const uint32_t net = Word(base, game + kRecords + kRecordSize * record + kNetPlayer);
  const uint32_t player = players && net < 32 ? Word(base, players + 4 * net) : 0;
  return player ? rex::memory::load_and_swap<uint64_t>(base + player + 16) : 0;
}

// Works out the roles of a race about to start, with 'game' going into StartPlaying.
void Latch(uint8_t* base, uint32_t game) {
  g_game = game;
  g_role = Role::kNone;
  g_to.clear();
  g_out.clear();
  g_racers.clear();
  g_field.clear();
  g_events.clear();
  g_event_number = 0;
  g_drawn.clear();
  {
    uint8_t session[8];
    online::CurrentSessionId(session);
    g_salt = 0x811C9DC5u;
    for (const uint8_t byte : session) g_salt = (g_salt ^ byte) * 0x01000193u;
  }
  g_picks = 0;
  g_watched = 0;
  g_cycle = 0;
  g_fresh = true;
  {
    // Streams left from the race before are dropped.
    std::lock_guard lock(g_streams_mutex);
    for (Stream& stream : g_streams) stream = {};
  }
  const uint32_t network = Word(base, kNetwork);
  int seats = 0;
  const std::vector<online::Player> players = online::Players(&seats);
  if (!network || Word(base, network + 168) != 2 || seats <= 0) return;
  bool watching = false;
  const uint32_t count = std::min<uint32_t>(Word(base, game + kCount), 4);
  for (const online::Player& player : players) {
    uint32_t record = count;
    for (uint32_t i = 0; i < count; ++i) {
      if (Xuid(base, game, i) == player.xuid) record = i;
    }
    if (record == count) {
      REXLOG_WARN("[puppet] {} is in the lobby and not in the game", player.name);
      continue;
    }
    if (player.spectator && player.local) {
      watching = true;
      g_own = record;
    } else if (player.spectator) {
      if (player.pid) g_to.push_back(player.pid);
      g_out.push_back(record);
    } else if (!player.local && player.pid) {
      g_racers.push_back(
          {player.pid, record, Word(base, game + kRecords + kRecordSize * record + kNetPlayer), player.name});
    }
  }
  if (watching) {
    if (!g_racers.empty()) g_role = Role::kSpectator;
  } else if (!g_out.empty()) {
    g_role = Role::kRacer;
  }
  for (uint32_t record = 0; record < count; ++record) {
    const bool out = std::find(g_out.begin(), g_out.end(), record) != g_out.end() || (watching && record == g_own);
    if (!out) g_field.push_back(record);
  }
  REXLOG_INFO("[puppet] this race: {} ({} racers to watch, {} watching)",
              g_role == Role::kSpectator ? "watching" : g_role == Role::kRacer ? "racing, watched" : "as the game has it",
              g_racers.size(), g_out.size());
}

// The game's StartPlaying: on a racer's machine the watching players' records are marked "left" around it and ended
// without a time after it.
void StartPlaying(PPCContext& ctx, uint8_t* base) {
  const uint32_t game = ctx.r3.u32;
  Latch(base, game);
  const auto mark = [&] {
    if (g_role == Role::kRacer) {
      for (const uint32_t record : g_out) base[game + kRecords + kRecordSize * record + kLeft] = 1;
    }
  };
  mark();
  __imp__sub_829D3038(ctx, base);
  mark();
  if (g_role != Role::kRacer) return;
  for (const uint32_t record : g_out) Call(ctx, base, __imp__sub_829CEB30, game, record, 0xFFFFFFFFu);
}

// Where GAMECONFIG has the name of a net player's remote character; 0 when it has none.
uint32_t Character(uint8_t* base, uint32_t net) {
  const uint32_t config = Word(base, kConfig);
  const uint32_t table = config && net < Word(base, config + 356) ? Word(base, config + 360) : 0;
  const uint32_t at = table ? table + 24 * net + 8 : 0;
  const uint32_t name = at ? Word(base, at) : 0;
  return name && base[name] == 'p' ? at : 0;
}

// The template a fighter is made from: the puppet is the first racer's remote character.
void CreateFighter(PPCContext& ctx, uint8_t* base) {
  if (g_role == Role::kSpectator && REXCVAR_GET(cc1_puppet_avatar)) {
    if (const uint32_t at = Character(base, g_racers[0].net)) {
      REXLOG_INFO("[puppet] the puppet is {}", reinterpret_cast<const char*>(base + Word(base, at)));
      ctx.r7.u64 = Word(base, at);
    }
  }
  __imp__sub_829CD848(ctx, base);
}

// Dresses the puppet as another racer's remote character.
void Dress(PPCContext& ctx, uint8_t* base, uint32_t fighter, const Racer& racer) {
  if (!REXCVAR_GET(cc1_puppet_avatar)) return;
  const uint32_t at = Character(base, racer.net);
  if (!at) return;
  REXLOG_INFO("[puppet] the puppet is now {}", reinterpret_cast<const char*>(base + Word(base, at)));
  Call(ctx, base, __imp__sub_829A6C30, fighter, at);
}

// ---- The course's elements ----------------------------------------------------------------------

constexpr uint32_t kFighter = 0x8215716C;  // FIGHTER's vtable
constexpr char kLane[] = "_clone_track_";
constexpr size_t kLanePrefix = 15;  // with the lane's digit and the slash

// What an actor of the lane is known by between machines; 0 for any other.
uint32_t Element(uint8_t* base, uint32_t actor) {
  const uint32_t name_at = actor ? Word(base, actor + 8) : 0;
  if (!name_at) return 0;
  const char* name = reinterpret_cast<const char*>(base + name_at);
  if (std::strncmp(name, kLane, sizeof(kLane) - 1) != 0 || std::strlen(name) <= kLanePrefix) return 0;
  uint32_t hash = 0x811C9DC5u;
  for (const char* c = name + kLanePrefix; *c; ++c) hash = (hash ^ uint8_t(*c)) * 0x01000193u;
  return hash ? hash : 1;
}

std::unordered_map<uint32_t, uint32_t> g_elements;  // this machine's actors by element
uint32_t g_surveyed = 0;                            // the game they were listed for
int g_surveys = 0;

// Lists the lane's actors from the scene's list (the engine's +54068, 16 bytes each, with the actor at +4, the next
// one's index at +14 and the first's index at the engine's +164358).
void Survey(uint8_t* base) {
  g_elements.clear();
  g_surveyed = g_game;
  const uint32_t engine = Word(base, kActors);
  if (!engine) return;
  int index = int16_t(rex::memory::load_and_swap<uint16_t>(base + engine + 164358));
  for (int guard = 0; index >= 0 && index < 6000 && guard < 6000; ++guard) {
    const uint32_t entry = engine + 54068 + 16 * uint32_t(index);
    const uint32_t actor = Word(base, entry + 4);
    if (const uint32_t element = Element(base, actor)) g_elements[element] = actor;
    index = int16_t(rex::memory::load_and_swap<uint16_t>(base + entry + 14));
  }
}

// This machine's actor for an element; 0 when it has none.
uint32_t Find(uint8_t* base, uint32_t element) {
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (g_surveyed != g_game || (attempt && g_surveys < 20)) {
      Survey(base);
      ++g_surveys;
    }
    const auto found = g_elements.find(element);
    if (found != g_elements.end() && Element(base, found->second) == element) return found->second;
  }
  return 0;
}

bool Synced(uint8_t* base) { return g_role != Role::kNone && Word(base, kGame) == g_game && REXCVAR_GET(cc1_puppet_course); }

// A trigger's command: the racer's fighter's are recorded for sending, and the puppet's own touches set nothing
// off.
void Command(PPCContext& ctx, uint8_t* base) {
  if (t_replayed || !Synced(base) || !g_local || Word(base, g_local) != kFighter ||
      ctx.r4.u64 != rex::memory::load_and_swap<uint64_t>(base + g_local + 16)) {
    return __imp__sub_82921B18(ctx, base);
  }
  if (g_role == Role::kSpectator) return;  // the puppet's touch: the racer's comes in its place
  if (const uint32_t element = Element(base, ctx.r3.u32)) {
    g_events.push_back({++g_event_number, g_sequence, element, int32_t(ctx.r5.u32)});
    while (g_events.size() > kEvents) g_events.pop_front();
  }
  __imp__sub_82921B18(ctx, base);
}

// Runs the racer's commands up to a step the puppet has taken, after the last one run.
void Replay(PPCContext& ctx, uint8_t* base, uint32_t fighter, const std::vector<Event>& events, uint32_t sequence,
            uint32_t& run) {
  if (!REXCVAR_GET(cc1_puppet_course)) return;
  for (const Event& event : events) {
    if (event.number <= run || event.sequence > sequence) continue;
    run = event.number;
    const uint32_t trigger = Find(base, event.element);
    if (!trigger) {
      if (REXCVAR_GET(cc1_puppet_trace)) REXLOG_INFO("[puppet] a trigger of the racer's is not here ({:08X})", event.element);
      continue;
    }
    const PPCContext saved = ctx;
    ctx.r1.u32 = (saved.r1.u32 - 0x200) & ~0xFu;
    ctx.r3.u64 = trigger;
    ctx.r4.u64 = rex::memory::load_and_swap<uint64_t>(base + fighter + 16);
    ctx.r5.u64 = uint64_t(int64_t(event.command));
    t_replayed = true;
    __imp__sub_82921B18(ctx, base);
    t_replayed = false;
    ctx = saved;
  }
}

// A script starting or resuming as its actor, which selects the element whose random numbers it draws.
void Script(PPCContext& ctx, uint8_t* base) {
  const uint32_t before = t_element;
  t_element = Synced(base) ? Element(base, Actor(base, ctx.r5.u64)) : 0;
  __imp__sub_8285A268(ctx, base);
  t_element = before;
}

// ---- The racer: what its fighter's steps were given ----------------------------------------

uint8_t StateOf(uint8_t* base, uint32_t fighter) {
  const uint32_t state = Word(base, fighter + kState);
  for (size_t i = 0; i < std::size(kStates); ++i) {
    if (kStates[i].object == state) return uint8_t(i);
  }
  return kNoState;
}

void StepRacer(PPCContext& ctx, uint8_t* base) {
  static uint32_t counted = 0;
  uint32_t& sequence = g_sequence;
  static uint16_t race = 0;
  static Step recent[kRepeated];
  static Truth truth{};
  static bool have_truth = false;
  const uint32_t fighter = ctx.r3.u32, pad = Word(base, fighter + kPad);
  if (counted != fighter) {
    counted = fighter;
    ++race;
    sequence = 0;
    have_truth = false;
  }
  Step step{};
  step.length = Float(base, fighter + kStepLength);
  for (uint32_t i = 0; i < 4; ++i) {
    step.analog[i] = uint16_t(std::min<uint32_t>(Call(ctx, base, __imp__sub_8295EDA0, pad, 4 + i), 0xFFFF));
  }
  const auto bit = [&](GuestFunction getter, uint32_t command, int at) {
    return uint8_t((Call(ctx, base, getter, pad, command) & 0xFF) ? 1 << at : 0);
  };
  step.held = bit(__imp__sub_8295EE98, 44, 0) | bit(__imp__sub_8295EE98, 32, 1) | bit(__imp__sub_8295EE98, 42, 2);
  step.pressed = bit(__imp__sub_8295EF78, 42, 0) | bit(__imp__sub_8295EF78, 1, 1) | bit(__imp__sub_8295EF78, 2, 2) |
                 bit(__imp__sub_8295EF78, 3, 3);
  step.blocked = (Call(ctx, base, __imp__sub_8299F9D0, fighter) & 0xFF) != 0;
  step.picks = g_picks;
  std::memcpy(step.pick, g_picked, sizeof(step.pick));
  g_picks = 0;

  t_rand_kept = step.rand;
  t_rand_count = &step.rands;
  t_rand_room = uint8_t(std::size(step.rand));
  g_stepping = true;
  __imp__sub_829AB7C0(ctx, base);
  g_stepping = false;
  t_rand_room = 0;

  recent[sequence % kRepeated] = step;
  if (sequence % kTruthEvery == 0) {
    truth.sequence = sequence;
    truth.x = Float(base, fighter + kX);
    truth.y = Float(base, fighter + kY);
    for (size_t i = 0; i < std::size(kWords); ++i) truth.words[i] = Word(base, fighter + kWords[i]);
    truth.grounded = base[fighter + kGrounded];
    truth.sprinting = base[fighter + kSprinting];
    truth.state = StateOf(base, fighter);
    have_truth = true;
  }
  uint8_t packet[sizeof(Header) + kRepeated * sizeof(Step) + sizeof(Truth) + kEvents * sizeof(Event)];
  const uint32_t count = std::min<uint32_t>(sequence + 1, kRepeated);
  const Header header{kSteps,        uint8_t(count), uint8_t(have_truth), uint8_t(g_events.size()),
                      race,          sequence + 1 - count};
  size_t size = 0;
  std::memcpy(packet, &header, sizeof(header));
  size += sizeof(header);
  for (uint32_t i = header.first; i <= sequence; ++i, size += sizeof(Step)) {
    std::memcpy(packet + size, &recent[i % kRepeated], sizeof(Step));
  }
  if (have_truth) {
    std::memcpy(packet + size, &truth, sizeof(truth));
    size += sizeof(truth);
  }
  for (const Event& event : g_events) {
    std::memcpy(packet + size, &event, sizeof(event));
    size += sizeof(event);
  }
  for (const int seat : g_to) relay::SendControl(packet, size, uint8_t(seat));
  ++sequence;
}

// ---- The spectator: the racers' steps, as they arrive (the relay's thread) ----------------------

void OnControl(uint8_t seat, const uint8_t* data, size_t size) {
  Header header;
  if (size < sizeof(header) || data[0] != kSteps || seat > relay::kMaxPid) return;
  std::memcpy(&header, data, sizeof(header));
  const size_t events_at = sizeof(header) + header.count * sizeof(Step) + (header.truth ? sizeof(Truth) : 0);
  if (header.count > kRepeated || header.events > kEvents || size != events_at + header.events * sizeof(Event)) {
    return;
  }
  std::lock_guard lock(g_streams_mutex);
  Stream& stream = g_streams[seat];
  if (!stream.known || stream.race != header.race) {
    stream = {};
    stream.known = true;
    stream.race = header.race;
    stream.next = header.first;
  }
  for (uint32_t i = 0; i < header.count; ++i) {
    const uint32_t sequence = header.first + i;
    if (sequence < stream.next) continue;
    Step step;
    std::memcpy(&step, data + sizeof(header) + i * sizeof(Step), sizeof(step));
    // Steps lost past the repeats are gone: the next truth puts the puppet right.
    stream.next = sequence + 1;
    stream.steps.push_back(step);
  }
  while (stream.steps.size() > 600) stream.steps.pop_front();
  if (header.truth) {
    Truth truth;
    std::memcpy(&truth, data + sizeof(header) + header.count * sizeof(Step), sizeof(truth));
    if (stream.truths.empty() || truth.sequence > stream.truths.back().sequence) stream.truths.push_back(truth);
    while (stream.truths.size() > 16) stream.truths.pop_front();
  }
  for (uint32_t i = 0; i < header.events; ++i) {
    Event event;
    std::memcpy(&event, data + events_at + i * sizeof(Event), sizeof(event));
    if (stream.events.empty() || event.number > stream.events.back().number) stream.events.push_back(event);
  }
  while (stream.events.size() > 64) stream.events.pop_front();
}

struct Installer {
  Installer() { relay::AddControlHook(&OnControl); }
} g_installer;

// ---- The spectator: the puppet (the game's thread) ----------------------------------------------

// The step the puppet is taking, for the getters' hooks.
thread_local const Step* t_step = nullptr;
thread_local uint32_t t_pad = 0;
thread_local bool t_at_rest = false;  // a step with nothing held: the racer's have not come yet

constexpr double kNear = 0.01, kFar = 3.0;  // metres off the racer: eased there past the first, set down there past the second
constexpr double kBuffered = 0.07;          // seconds of the racer's steps kept in hand against the network's unevenness

// What the puppet has been doing, for the log.
struct Tally {
  int frames = 0, none = 0, one = 0, more = 0, dry = 0, eased = 0, set_down = 0, states = 0;
  double length = 0.0, buffered = 0.0;
  int steps = 0;
} g_tally;

float g_ease_x = 0.0f, g_ease_y = 0.0f;  // how far the puppet still is from where the racer was

void Place(PPCContext& ctx, uint8_t* base, uint32_t fighter, float x, float y) {
  // The lane (z) is the fighter's own.
  const uint32_t at = ((ctx.r1.u32 - 0x200) & ~0xFu) + 0x100;
  PutFloat(base, at, x);
  PutFloat(base, at + 4, y);
  PutWord(base, at + 8, Word(base, fighter + kZ));
  Call(ctx, base, __imp__sub_829344A8, fighter, at);
}

void Correct(PPCContext& ctx, uint8_t* base, uint32_t fighter, const Truth& truth, bool set_down) {
  const float x = Float(base, fighter + kX), y = Float(base, fighter + kY);
  const double off = std::hypot(double(truth.x) - x, double(truth.y) - y);
  set_down = set_down || off > kFar;
  g_ease_x = g_ease_y = 0.0f;
  if (set_down) {
    Place(ctx, base, fighter, truth.x, truth.y);
    Call(ctx, base, __imp__sub_829A34E8, fighter);
    ++g_tally.set_down;
  } else if (off > kNear) {
    // Eased, with each following step closing part of the gap.
    g_ease_x = truth.x - x;
    g_ease_y = truth.y - y;
    ++g_tally.eased;
  }
  // On a rope is a state with a rope (+4392), and ropes swing independently on each machine, so a puppet that
  // missed the rope the racer caught keeps the state it has and follows where the racer is.
  const bool ropeless = truth.state == kRope && !Word(base, fighter + kHeldRope);
  if (truth.state < std::size(kStates) && !ropeless && Word(base, fighter + kState) != kStates[truth.state].object) {
    Call(ctx, base, __imp__sub_8299FD88, fighter, Call(ctx, base, kStates[truth.state].accessor));
    ++g_tally.states;
  }
  for (size_t i = 0; i < std::size(kWords); ++i) PutWord(base, fighter + kWords[i], truth.words[i]);
  base[fighter + kGrounded] = truth.grounded;
  base[fighter + kSprinting] = truth.sprinting;
  // A puppet that died is made visible again here, in place of its own respawn, whenever the racer is alive.
  static_assert(kWords[kHealthWord] == 1456);
  const bool alive = std::bit_cast<float>(truth.words[kHealthWord]) > 0.0f && truth.state != kWipe &&
                     truth.state != kFlyingDeath && truth.state != kDeath && truth.state != kSquished;
  if (alive && Float(base, fighter + kOpacity) < 1.0f) PutFloat(base, fighter + kOpacity, 1.0f);
}

void Take(PPCContext& ctx, uint8_t* base, const Step& step) {
  const uint32_t fighter = ctx.r3.u32, kernel = Word(base, kKernel);
  const uint32_t own = kernel ? Word(base, kernel + 12) : 0;
  const PPCContext saved = ctx;
  // The animations the racer's fighter was given since its last step, with the same variants.
  for (uint8_t i = 0; i < std::min<uint8_t>(step.picks, uint8_t(std::size(step.pick))); ++i) {
    const Pick& pick = step.pick[i];
    if (!pick.rands && Word(base, fighter + 3660) == pick.which) continue;  // the puppet has it, and it has no variants
    ctx.r1.u32 = (saved.r1.u32 - 0x200) & ~0xFu;
    ctx.r3.u64 = fighter;
    ctx.r4.u64 = pick.which;
    ctx.r5.u64 = pick.r5;
    ctx.r6.u64 = 1;  // again: the puppet chose its own already
    ctx.f1.f64 = double(pick.f1);
    t_rand_given = pick.rand;
    t_rand_left = std::min<uint8_t>(pick.rands, uint8_t(std::size(pick.rand)));
    __imp__sub_8299E968(ctx, base);
    t_rand_left = 0;
    ctx = saved;
  }
  PutFloat(base, fighter + kStepLength, step.length);
  if (kernel) PutFloat(base, kernel + 12, step.length);
  t_step = &step;
  t_pad = Word(base, fighter + kPad);
  t_rand_given = step.rand;
  t_rand_left = std::min<uint8_t>(step.rands, uint8_t(std::size(step.rand)));
  __imp__sub_829AB7C0(ctx, base);
  t_rand_left = 0;
  ctx = saved;
  t_step = nullptr;
  if (kernel) PutWord(base, kernel + 12, own);
  if (g_ease_x != 0.0f || g_ease_y != 0.0f) {
    const float dx = g_ease_x * 0.3f, dy = g_ease_y * 0.3f;
    Place(ctx, base, fighter, Float(base, fighter + kX) + dx, Float(base, fighter + kY) + dy);
    g_ease_x -= dx;
    g_ease_y -= dy;
    if (std::abs(g_ease_x) + std::abs(g_ease_y) < 0.002f) g_ease_x = g_ease_y = 0.0f;
  }
}

// The watching player's record with the place of the racer watched, for as long as this lives.
struct AsTheRacer {
  uint8_t* base;
  uint32_t at;
  uint32_t place;
  explicit AsTheRacer(uint8_t* base) : base(base) {
    const Racer& racer = g_racers[std::min(g_watched, g_racers.size() - 1)];
    at = g_game + kRecords + kRecordSize * g_own + kPlace;
    place = Word(base, at);
    PutWord(base, at, Word(base, g_game + kRecords + kRecordSize * racer.record + kPlace));
  }
  ~AsTheRacer() { PutWord(base, at, place); }
};

void StepSpectator(PPCContext& ctx, uint8_t* base) {
  static double credit = 0.0;        // time of the racer's the puppet may still step through
  static bool playing = false;       // steps are being taken: false until enough of them are in hand
  static uint32_t taken = 0;         // the sequence after the last step taken
  static uint16_t race = 0;
  static int seat = 0;
  static bool set_down = false;      // the next correction is a fresh start
  const uint32_t fighter = ctx.r3.u32;
  const uint32_t own = g_game + kRecords + kRecordSize * g_own;
  const auto ended = [&](const Racer& racer) {
    return base[g_game + kEnded + racer.record] || base[g_game + kRecords + kRecordSize * racer.record + kLeft];
  };

  if (const int cycle = g_cycle.exchange(0); cycle && g_racers.size() > 1) {
    const int count = int(g_racers.size());
    g_watched = size_t(((int(g_watched) + cycle) % count + count) % count);
  }
  g_watched = std::min(g_watched, g_racers.size() - 1);
  // When the racer watched has ended and another has not, the puppet moves on to that one.
  if (ended(g_racers[g_watched])) {
    for (size_t i = 0; i < g_racers.size(); ++i) {
      if (!ended(g_racers[i])) g_watched = i;
    }
  }
  const int watched = g_racers[g_watched].seat;
  const AsTheRacer placed(base);
  // The puppet ends the track when every racer has, as a player the game keeps no time for.
  const bool over = std::all_of(g_racers.begin(), g_racers.end(), ended);
  const auto hold = [&] {
    if (over) base[own + kCheated] = 1;
    base[fighter + kFinished] = over ? 1 : 0;
    if (over) PutFloat(base, fighter + kFinishedIn, 0.0f);
  };
  hold();

  std::vector<Step> steps;
  std::vector<uint32_t> sequences;
  std::vector<Truth> truths;
  std::vector<Event> events;
  static uint32_t run = 0;  // the racer's last command run here
  bool started = false, another_racer = false;
  double buffered = 0.0;
  {
    std::lock_guard lock(g_streams_mutex);
    Stream& stream = g_streams[std::clamp(watched, 0, int(relay::kMaxPid))];
    if (std::exchange(g_fresh, false)) seat = 0;
    const bool another = seat != watched, begun = !another && stream.known && race != stream.race;
    if (another || begun) {
      // Takes up a racer's steps from where they are now: mid-race the puppet is set down where the racer is and
      // has not crossed the line, and at the start it stands where the game put it.
      set_down = another && seat != 0;
      another_racer = set_down;
      // Commands a racer set off before being taken up mid-race are not run again, while from the start all are.
      run = set_down && !stream.events.empty() ? stream.events.back().number : 0;
      if (set_down) base[fighter + kCrossed] = 0;
      seat = watched;
      race = stream.race;
      taken = stream.next - uint32_t(std::min<size_t>(stream.steps.size(), 4));
      playing = false;
      credit = 0.0;
    }
    started = stream.known;
    const uint32_t first = stream.next - uint32_t(stream.steps.size());  // the oldest step held
    if (taken < first) taken = first;
    for (uint32_t i = taken; i < stream.next && i < taken + 120; ++i) buffered += double(stream.steps[i - first].length);
    if (buffered > 1.0) {
      // When a second behind (the game stood still), the puppet resumes from near the newest step.
      taken = stream.next - 4;
      buffered = 0.0;
      set_down = true;
      credit = 0.0;
    }
    if (!playing && buffered >= kBuffered) playing = true;
    if (playing) {
      // Steps are taken a little faster with more in hand than wanted and a little slower with less, and the credit
      // decays towards zero so that frames do not alternate between no step and two.
      const double rate = std::clamp(1.0 + (buffered - kBuffered) * 0.6, 0.9, 1.2);
      credit = std::min(credit * 0.95 + double(Float(base, fighter + kStepLength)) * rate, 0.1);
      for (int most = 3; taken < stream.next && most; --most) {
        const Step& step = stream.steps[taken - first];
        if (credit < double(step.length) * 0.5) break;
        credit -= double(step.length);
        steps.push_back(step);
        sequences.push_back(taken++);
      }
      if (taken == stream.next) {
        credit = std::min(credit, 0.0);
        ++g_tally.dry;
      }
    }
    truths.assign(stream.truths.begin(), stream.truths.end());
    events.assign(stream.events.begin(), stream.events.end());
  }

  if (another_racer) Dress(ctx, base, fighter, g_racers[g_watched]);
  if (!started) {
    // The racer's steps have not begun to come: the puppet waits as the game has it, with nothing held.
    static const Step rest{};
    t_step = &rest;
    t_pad = Word(base, fighter + kPad);
    t_at_rest = true;
    __imp__sub_829AB7C0(ctx, base);
    t_at_rest = false;
    t_step = nullptr;
    hold();
    return;
  }
  for (size_t i = 0; i < steps.size(); ++i) {
    Take(ctx, base, steps[i]);
    g_tally.length += double(steps[i].length);
    ++g_tally.steps;
    for (const Truth& truth : truths) {
      if (truth.sequence != sequences[i]) continue;
      Correct(ctx, base, fighter, truth, set_down);
      set_down = false;
    }
    Replay(ctx, base, fighter, events, sequences[i], run);
    hold();
  }
  ++g_tally.frames;
  ++(steps.empty() ? g_tally.none : steps.size() == 1 ? g_tally.one : g_tally.more);
  g_tally.buffered += buffered;
  if (g_tally.frames >= 300) {
    if (REXCVAR_GET(cc1_puppet_trace)) {
      REXLOG_INFO("[puppet] 300 frames: {} with no step, {} with one, {} with more, {} ran dry; a step {:.2f} ms, "
                  "{:.0f} ms in hand; {} eased, {} set down, {} states",
                  g_tally.none, g_tally.one, g_tally.more, g_tally.dry,
                  g_tally.steps ? g_tally.length * 1e3 / g_tally.steps : 0.0, g_tally.buffered * 1e3 / g_tally.frames,
                  g_tally.eased, g_tally.set_down, g_tally.states);
    }
    g_tally = {};
  }
}

void StepFighter(PPCContext& ctx, uint8_t* base) {
  g_local = ctx.r3.u32;
  // Out of the game the roles were worked out for, or of its LIVE session, the fighter is the player's own.
  const uint32_t network = Word(base, kNetwork);
  if (g_role != Role::kNone && (Word(base, kGame) != g_game || !network || Word(base, network + 168) != 2)) {
    g_role = Role::kNone;
  }
  switch (g_role) {
    case Role::kRacer: return StepRacer(ctx, base);
    case Role::kSpectator: return StepSpectator(ctx, base);
    case Role::kNone: return __imp__sub_829AB7C0(ctx, base);
  }
}

bool Watching(uint8_t* base) { return g_role == Role::kSpectator && Word(base, kGame) == g_game && !g_racers.empty(); }

// A helicopter's step: that of the racer watched goes where it is not seen.
void StepHelicopter(PPCContext& ctx, uint8_t* base) {
  const uint32_t helicopter = ctx.r3.u32;
  bool away = false;
  if (Watching(base)) {
    const Racer& racer = g_racers[std::min(g_watched, g_racers.size() - 1)];
    const uint32_t at = g_game + kHelicopters + 32 * Word(base, g_game + kOwnRecord) + 8 * racer.record;
    away = Actor(base, rex::memory::load_and_swap<uint64_t>(base + at)) == helicopter;
  }
  if (away) PutFloat(base, helicopter + kSentDepth, kOutOfSight);
  const PPCContext saved = ctx;
  __imp__sub_829B20A0(ctx, base);
  if (away && Float(base, helicopter + kZ) != kOutOfSight) {
    ctx = saved;
    const uint32_t at = ((ctx.r1.u32 - 0x200) & ~0xFu) + 0x100;
    PutWord(base, at, Word(base, helicopter + kX));
    PutWord(base, at + 4, Word(base, helicopter + kY));
    PutFloat(base, at + 8, kOutOfSight);
    Call(ctx, base, __imp__sub_829344A8, helicopter, at);
  }
}

// The racer's fighter is given an animation: outside its steps, the choice goes with its next step.
void Animate(PPCContext& ctx, uint8_t* base) {
  if (g_role != Role::kRacer || g_stepping || Word(base, kGame) != g_game) return __imp__sub_8299E968(ctx, base);
  // The last ones before the step are the ones that show.
  if (g_picks == std::size(g_picked)) {
    g_picked[0] = g_picked[1];
    --g_picks;
  }
  Pick& pick = g_picked[g_picks++];
  pick = {uint8_t(ctx.r4.u32), uint8_t(ctx.r5.u32), uint8_t(ctx.r6.u32), 0, float(ctx.f1.f64), {}};
  t_rand_kept = pick.rand;
  t_rand_count = &pick.rands;
  t_rand_room = uint8_t(std::size(pick.rand));
  __imp__sub_8299E968(ctx, base);
  t_rand_room = 0;
}

// The script's GetFinishPlace, answered with the place of the racer watched.
void FinishPlace(PPCContext& ctx, uint8_t* base) {
  if (!Watching(base)) return __imp__sub_829A20D0(ctx, base);
  const AsTheRacer placed(base);
  __imp__sub_829A20D0(ctx, base);
}

// A bar of the HUD's map asking how far its player is: the bars are the racers, in order.
void Progress(PPCContext& ctx, uint8_t* base) {
  if (g_role != Role::kNone && ctx.r3.u32 == g_game && Word(base, kGame) == g_game) {
    const uint32_t bar = ctx.r4.u32;
    if (bar < g_field.size()) {
      ctx.r4.u64 = g_field[bar];
    } else if (bar < 4) {
      ctx.f1.f64 = 0.0;  // a bar with no racer: the script has it off the map
      return;
    }
  }
  __imp__sub_829CB548(ctx, base);
}

// The game's ranks: the watching player is in nobody's, and shows the place of the racer watched.
void Rank(PPCContext& ctx, uint8_t* base) {
  if (!Watching(base) || ctx.r3.u32 != g_game) return __imp__sub_829CD3B0(ctx, base);
  const uint32_t own = g_game + kRecords + kRecordSize * g_own;
  const uint8_t cheated = base[own + kCheated];
  base[own + kCheated] = 1;
  __imp__sub_829CD3B0(ctx, base);
  base[own + kCheated] = cheated;
  if (cheated) return;  // the race is over: the record is as the game left it
  const Racer& racer = g_racers[std::min(g_watched, g_racers.size() - 1)];
  PutWord(base, own + kTime, Word(base, own + kRunning));
  PutWord(base, own + kPlace, Word(base, g_game + kRecords + kRecordSize * racer.record + kPlace));
}

// The helicopter's message: the watching player has no helicopter on the racers' machines.
void SendBillboard(PPCContext& ctx, uint8_t* base) {
  if (g_role == Role::kSpectator && ctx.r3.u32 == g_game) return;
  __imp__sub_829CD058(ctx, base);
}

// Answers a getter the puppet's step asks from the racer's step, or returns false to leave it to the game.
bool Answer(PPCContext& ctx, int kind) {
  if (!t_step || ctx.r3.u32 != t_pad) return false;
  const uint32_t command = ctx.r4.u32;
  uint32_t answer = 0;
  if (kind == 0) {
    if (command >= 4 && command <= 7) answer = t_step->analog[command - 4];
  } else if (kind == 1) {
    answer = command == 44 ? t_step->held & 1 : command == 32 ? (t_step->held >> 1) & 1 : command == 42 ? (t_step->held >> 2) & 1 : 0;
  } else {
    answer = command == 42 ? t_step->pressed & 1
             : command == 1 ? (t_step->pressed >> 1) & 1
             : command == 2 ? (t_step->pressed >> 2) & 1
             : command == 3 ? (t_step->pressed >> 3) & 1
                            : 0;
  }
  ctx.r3.u64 = answer;
  return true;
}

}  // namespace

bool PuppetStep() { return t_step != nullptr; }

int Bars() {
  auto* memory = REX_KERNEL_MEMORY();
  if (g_role == Role::kNone || !memory) return -1;
  return Word(memory->TranslateVirtual<uint8_t*>(0), kGame) == g_game ? int(g_field.size()) : -1;
}

std::string Watched() {
  auto* memory = REX_KERNEL_MEMORY();
  if (g_role != Role::kSpectator || !memory || g_racers.empty()) return {};
  if (Word(memory->TranslateVirtual<uint8_t*>(0), kGame) != g_game) return {};
  return g_racers[std::min(g_watched, g_racers.size() - 1)].name;
}

void NotePad(uint32_t user, uint16_t buttons) {
  static std::atomic<uint16_t> before{0};
  if (user != 0) return;
  const uint16_t pressed = buttons & ~before.exchange(buttons);
  if (pressed & 0x0100) --g_cycle;  // LB
  if (pressed & 0x0200) ++g_cycle;  // RB
  if (pressed & 0x4000) PressX();
}

}  // namespace cc1::spectator

REX_HOOK_RAW(sub_829AB7C0) { cc1::spectator::StepFighter(ctx, base); }
REX_HOOK_RAW(sub_829D3038) { cc1::spectator::StartPlaying(ctx, base); }
REX_HOOK_RAW(sub_829CD848) { cc1::spectator::CreateFighter(ctx, base); }
REX_HOOK_RAW(sub_829CD058) { cc1::spectator::SendBillboard(ctx, base); }
REX_HOOK_RAW(sub_829B20A0) { cc1::spectator::StepHelicopter(ctx, base); }
REX_HOOK_RAW(sub_829CD3B0) { cc1::spectator::Rank(ctx, base); }
REX_HOOK_RAW(sub_829CB548) { cc1::spectator::Progress(ctx, base); }
REX_HOOK_RAW(sub_829A20D0) { cc1::spectator::FinishPlace(ctx, base); }
REX_HOOK_RAW(sub_8299E968) { cc1::spectator::Animate(ctx, base); }
REX_HOOK_RAW(sub_824DDB68) { cc1::spectator::Random(ctx, base); }
REX_HOOK_RAW(sub_82921B18) { cc1::spectator::Command(ctx, base); }
REX_HOOK_RAW(sub_8285A268) { cc1::spectator::Script(ctx, base); }
REX_HOOK_RAW(sub_8295EDA0) {
  if (!cc1::spectator::Answer(ctx, 0)) __imp__sub_8295EDA0(ctx, base);
}
REX_HOOK_RAW(sub_8295EE98) {
  if (!cc1::spectator::Answer(ctx, 1)) __imp__sub_8295EE98(ctx, base);
}
REX_HOOK_RAW(sub_8295EF78) {
  if (!cc1::spectator::Answer(ctx, 2)) __imp__sub_8295EF78(ctx, base);
}
REX_HOOK_RAW(sub_8299F9D0) {
  if (cc1::spectator::t_step && !cc1::spectator::t_at_rest) {
    ctx.r3.u64 = cc1::spectator::t_step->blocked;
    return;
  }
  __imp__sub_8299F9D0(ctx, base);
}
