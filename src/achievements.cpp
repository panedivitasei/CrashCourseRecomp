// achievements.cpp: awards Competitive Spirit (2) for finishing ahead of another racer in an online match through
// the race-end list of sub_829C0CE8, and blocks awards (sub_8295DF98, sub_8295E088) while spectating.

#include <algorithm>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "spectator.h"

REX_EXTERN(__imp__sub_829C0CE8);
REX_EXTERN(__imp__sub_8295E088);
REX_EXTERN(__imp__sub_8295DF98);

namespace cc1 {
namespace {

constexpr uint32_t kGame = 0x82C0479C;     // GAMEXA*: 64-byte player records at +392, their count at +384
constexpr uint32_t kNetwork = 0x82C52210;  // session kind at +168 (2: LIVE), in-match flag at +197
constexpr uint32_t kTime = 8, kProfile = 24, kLeft = 56, kCheated = 57;
constexpr uint32_t kCompetitiveSpirit = 2, kRoom = 7;

uint32_t Word(uint8_t* base, uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); }

// Adds Competitive Spirit to the race-end award list of a local player's profile.
void RaceEnd(PPCContext& ctx, uint8_t* base) {
  const uint32_t profile = ctx.r3.u32, ids = ctx.r4.u32, count_at = ctx.r5.u32;
  __imp__sub_829C0CE8(ctx, base);
  const uint32_t game = Word(base, kGame), network = Word(base, kNetwork);
  if (!game || !network || Word(base, network + 168) != 2 || !base[network + 197]) return;
  const uint32_t players = std::min<uint32_t>(Word(base, game + 384), 4);
  const auto record = [&](uint32_t index) { return game + 392 + 64 * index; };
  uint32_t own = players;
  for (uint32_t i = 0; i < players; ++i) {
    if (Word(base, record(i) + kProfile) == profile) own = i;
  }
  if (own == players) return;
  const int32_t time = int32_t(Word(base, record(own) + kTime));
  if (time <= 0 || base[record(own) + kLeft] || base[record(own) + kCheated]) return;
  bool ahead = false;
  for (uint32_t i = 0; i < players; ++i) {
    if (i == own || base[record(i) + kLeft] || base[record(i) + kCheated]) continue;
    ahead = ahead || int32_t(Word(base, record(i) + kTime)) > time;
  }
  const uint32_t count = Word(base, count_at);
  if (!ahead || count >= kRoom) return;
  for (uint32_t i = 0; i < count; ++i) {
    if (Word(base, ids + 4 * i) == kCompetitiveSpirit) return;
  }
  rex::memory::store_and_swap<uint32_t>(base + ids + 4 * count, kCompetitiveSpirit);
  rex::memory::store_and_swap<uint32_t>(base + count_at, count + 1);
}

}  // namespace
}  // namespace cc1

REX_HOOK_RAW(sub_829C0CE8) { cc1::RaceEnd(ctx, base); }
REX_HOOK_RAW(sub_8295E088) {
  if (!cc1::spectator::Watched().empty()) {
    ctx.r3.u64 = 1;  // reported as earned, so nothing counts towards it
    return;
  }
  __imp__sub_8295E088(ctx, base);
}
REX_HOOK_RAW(sub_8295DF98) {
  if (!cc1::spectator::Watched().empty()) return;
  __imp__sub_8295DF98(ctx, base);
}
