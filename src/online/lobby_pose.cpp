// Avatar poses in an online lobby, the same on every player's screen: a box's idle animation comes from the session
// id, the box and a per-box counter that A raises and the relay shares, in place of the game's per-machine random
// pick.

#include "lobby_pose.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

#include "../online.h"
#include "relay.h"

REX_EXTERN(__imp__sub_829DA270);  // GUIXA::LoadAvatarInLobby(gui, box, avatar, local)
REX_EXTERN(__imp__sub_829D6250);  // GUIXA::GetRandomLobbyIdleAnim(gui) -> name
REX_EXTERN(__imp__sub_829DA028);  // A in the lobby: (gui, pad) gives the pad's avatar the next idle animation
REX_EXTERN(sub_829DA270);
REX_EXTERN(__imp__sub_8297E608);  // (): the host broadcasts the level configuration
REX_EXTERN(__imp__sub_8297E1C8);  // (slot): queues the slot's avatar for the lobby, as a player's join does

namespace cc1::pose {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int kBoxes = 4;
constexpr uint8_t kCounters = 1;  // control message: type, then a big-endian u32 counter per box
constexpr uint32_t kAnimCount = 16600, kAnimNames = 4 * 4151;  // GUIXA: idle animation count, name pointers
constexpr uint32_t kNextAnim = 0x82BFCCE0;  // the index A uses next
constexpr uint32_t kProfiles = 0x82C047A0;  // -> four player profiles: +4 the pad, +16 the player's box
constexpr uint32_t kCurrentPad = 16420;     // GUIXA: the pad that owns the menus

struct Box {
  bool loaded = false;      // LoadAvatarInLobby has run for it in this session
  bool stale = false;       // its counter rose elsewhere: show the new animation
  int tries = 0;            // frames the game has refused to (a message is up, or the box is gone)
  uint32_t args[4] = {};    // that call's r3-r6
  uint32_t counter = 0;
};

std::mutex g_mutex;  // guards g_boxes: the relay thread takes counters
Box g_boxes[kBoxes];
Clock::time_point g_next_send;

// Game thread only.
int g_loading = -1;     // the box LoadAvatarInLobby is working on
bool g_replay = false;  // that call is a replay made here

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t raw;
  std::memcpy(&raw, base + address, 4);
  return __builtin_bswap32(raw);
}

void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t raw = __builtin_bswap32(value);
  std::memcpy(base + address, &raw, 4);
}

// The idle animation of a box at a counter: an index into the game's list of 'count'.
uint32_t PoseIndex(int box, uint32_t counter, uint32_t count) {
  uint8_t session[8];
  online::CurrentSessionId(session);
  uint32_t hash = 0x811C9DC5u;  // FNV-1a over the session id and the box
  for (const uint8_t byte : session) hash = (hash ^ byte) * 0x01000193u;
  hash = (hash ^ uint8_t(box)) * 0x01000193u;
  return (hash % count + counter) % count;
}

void SendCounters() {
  uint8_t message[1 + 4 * kBoxes] = {kCounters};
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kBoxes; ++i) {
      const uint32_t raw = __builtin_bswap32(g_boxes[i].counter);
      std::memcpy(message + 1 + 4 * i, &raw, 4);
    }
    g_next_send = Clock::now() + std::chrono::seconds(3);
  }
  relay::SendControl(message, sizeof(message));
}

// Takes another member's counters on the relay thread, where the highest wins.
void OnControl(uint8_t src_pid, const uint8_t* data, size_t size) {
  if (size != 1 + 4 * kBoxes || data[0] != kCounters) return;
  std::lock_guard lock(g_mutex);
  for (int i = 0; i < kBoxes; ++i) {
    uint32_t raw;
    std::memcpy(&raw, data + 1 + 4 * i, 4);
    const uint32_t counter = __builtin_bswap32(raw);
    if (counter > g_boxes[i].counter) {
      g_boxes[i].counter = counter;
      g_boxes[i].stale = g_boxes[i].loaded;
    }
  }
}

struct Installer {
  Installer() { relay::AddControlHook(&OnControl); }
} g_installer;

}  // namespace

void Reset() {
  std::lock_guard lock(g_mutex);
  for (Box& box : g_boxes) box = {};
}

void Tick() {
  uint8_t session[8];
  online::CurrentSessionId(session);
  if (!relay::Pid() || !std::any_of(session, session + 8, [](uint8_t b) { return b != 0; })) return;

  bool send = false;
  int stale = -1;
  uint32_t args[4] = {};
  {
    std::lock_guard lock(g_mutex);
    send = Clock::now() >= g_next_send;  // repeated, so a member who joins later catches up
    for (int i = 0; i < kBoxes && stale < 0; ++i) {
      if (!g_boxes[i].stale) continue;
      g_boxes[i].stale = false;
      stale = i;
      std::memcpy(args, g_boxes[i].args, sizeof(args));
    }
  }
  if (send) SendCounters();
  if (stale < 0) return;

  PPCContext& ctx = *rex::runtime::current_ppc_context();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  const uint32_t gui = args[0], pad = Read32(base, gui + kCurrentPad), count = Read32(base, gui + kAnimCount);
  uint32_t box_field = 0;
  for (uint32_t i = 0; i < 4 && !box_field; ++i) {
    const uint32_t profile = Read32(base, Read32(base, kProfiles) + 4 * i);
    if (profile && Read32(base, profile + 4) == pad) box_field = profile + 16;
  }
  if (!box_field || count < 2 || count > 64) return;

  // The game's routine for A works on the local player's box, so for the length of the call that is the stale one.
  uint32_t counter;
  {
    std::lock_guard lock(g_mutex);
    counter = g_boxes[stale].counter;
  }
  const uint32_t index = PoseIndex(stale, counter, count);
  const uint32_t own_box = Read32(base, box_field);
  const PPCContext saved = ctx;
  ctx.r1.u32 = (ctx.r1.u32 - 0x200) & ~0xFu;
  ctx.r3.u64 = gui;
  ctx.r4.u64 = pad;
  Write32(base, box_field, uint32_t(stale));
  Write32(base, kNextAnim, index);
  __imp__sub_829DA028(ctx, base);
  Write32(base, box_field, own_box);
  ctx = saved;

  // A step of the index means the animation was set, otherwise the box is retried for a few seconds.
  const bool played = Read32(base, kNextAnim) != index;
  std::lock_guard lock(g_mutex);
  if (played) {
    g_boxes[stale].tries = 0;
    REXLOG_INFO("[pose] box {} follows to idle {} of {} (counter {})", stale, index, count, counter);
  } else if (++g_boxes[stale].tries < 180) {
    g_boxes[stale].stale = true;
  } else {
    g_boxes[stale].tries = 0;
  }
}

void SendCourse() {
  PPCContext& ctx = *rex::runtime::current_ppc_context();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  if (!Read32(base, 0x82C52210)) return;
  const PPCContext saved = ctx;
  ctx.r1.u32 = (saved.r1.u32 - 0x200) & ~0xFu;
  __imp__sub_8297E608(ctx, base);
  ctx = saved;
}

// Queues every player's avatar again in GUIXA's precache set (32-byte entries at GUI +16584, worked through each
// frame by sub_829DA558) with the waits at +8 staggered, so the avatars are not all built in one frame.
void Requeue() {
  PPCContext& ctx = *rex::runtime::current_ppc_context();
  uint8_t* base = REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0);
  const uint32_t network = Read32(base, 0x82C52210);  // NETMAN: its players by slot at +232
  const uint32_t players = network ? Read32(base, network + 232) : 0;
  if (!players) return;
  constexpr uint32_t kGui = 0x82C04798, kQueue = 16584, kEntry = 32, kWait = 8;
  const PPCContext saved = ctx;
  float wait = 0.02f;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    if (!Read32(base, players + 4 * slot)) continue;
    const uint32_t gui = Read32(base, kGui);
    const uint32_t before = gui ? Read32(base, gui + kQueue + 4) : 0;
    ctx.r1.u32 = (saved.r1.u32 - 0x200) & ~0xFu;
    ctx.r3.u64 = slot;
    __imp__sub_8297E1C8(ctx, base);
    ctx = saved;
    // The entry that call added is the queue's last.
    const uint32_t first = gui ? Read32(base, gui + kQueue) : 0, last = gui ? Read32(base, gui + kQueue + 4) : 0;
    if (!first || last < first + kEntry || last == before) continue;
    uint32_t bits;
    std::memcpy(&bits, &wait, sizeof(bits));
    Write32(base, last - kEntry + kWait, bits);
    wait += 0.1f;
  }
}

}  // namespace cc1::pose

REX_HOOK_RAW(sub_829DA270) {
  using namespace cc1::pose;
  const uint32_t box = ctx.r4.u32;
  uint8_t session[8];
  cc1::online::CurrentSessionId(session);
  const bool online = cc1::relay::Pid() && std::any_of(session, session + 8, [](uint8_t b) { return b != 0; });
  bool raised = false;
  if (online && box < kBoxes) {
    std::lock_guard lock(g_mutex);
    Box& state = g_boxes[box];
    if (!g_replay) {
      raised = state.loaded;  // a second load of the box is a reload: a new pose for everyone
      if (raised) ++state.counter;
      state.loaded = true;
      state.args[0] = ctx.r3.u32;
      state.args[1] = ctx.r4.u32;
      state.args[2] = ctx.r5.u32;
      state.args[3] = ctx.r6.u32;
    }
    static int budget = 40;
    if (budget > 0) {
      --budget;
      REXLOG_INFO("[pose] box {} loads (avatar {}, local {}, counter {}{})", box, ctx.r5.u32, ctx.r6.u32 & 0xFF,
                  state.counter, g_replay ? ", from another member" : raised ? ", reload" : "");
    }
    g_loading = int(box);
  }
  __imp__sub_829DA270(ctx, base);
  g_loading = -1;
  if (raised) SendCounters();
}

REX_HOOK_RAW(sub_829D6250) {
  using namespace cc1::pose;
  const uint32_t gui = ctx.r3.u32;
  const uint32_t count = Read32(base, gui + kAnimCount);
  if (g_loading < 0 || count < 1 || count > 64) {
    __imp__sub_829D6250(ctx, base);
    return;
  }
  uint32_t counter;
  {
    std::lock_guard lock(g_mutex);
    counter = g_boxes[g_loading].counter;
  }
  const uint32_t index = PoseIndex(g_loading, counter, count);
  static int budget = 40;
  if (budget > 0) {
    --budget;
    REXLOG_INFO("[pose] box {} takes idle {} of {} (counter {})", g_loading, index, count, counter);
  }
  ctx.r3.u64 = Read32(base, gui + kAnimNames + 4 * index);
}

REX_HOOK_RAW(sub_829DA028) {
  using namespace cc1::pose;
  const uint32_t count = Read32(base, ctx.r3.u32 + kAnimCount);
  int box = -1;
  uint32_t counter = 0;
  {
    std::lock_guard lock(g_mutex);
    for (int i = 0; i < kBoxes; ++i) {
      if (g_boxes[i].loaded && (g_boxes[i].args[3] & 0xFF)) box = i;  // the local player's
    }
    if (box >= 0) counter = g_boxes[box].counter;
  }
  if (box < 0 || count < 2 || count > 64 || !cc1::relay::Pid()) {
    __imp__sub_829DA028(ctx, base);
    return;
  }
  // The game plays the animation at its index and steps the index: a step means it played.
  const uint32_t index = PoseIndex(box, counter + 1, count);
  Write32(base, kNextAnim, index);
  __imp__sub_829DA028(ctx, base);
  if (Read32(base, kNextAnim) == index) return;
  {
    std::lock_guard lock(g_mutex);
    ++g_boxes[box].counter;
  }
  REXLOG_INFO("[pose] box {} steps to idle {} of {} (counter {})", box, index, count, counter + 1);
  SendCounters();
}
