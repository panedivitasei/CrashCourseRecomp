// fighter_step.cpp: splits each step of a falling fighter (SHOOTER::MoveShooter, sub_82998F58) in two so it lands at
// once on a descending surface instead of hovering over it, at any frame rate.

#include <bit>
#include <cstdint>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REXCVAR_DEFINE_BOOL(cc1_landing_fix, true, "CC1", "Land at once on a surface that is going down.");

REX_EXTERN(__imp__sub_82998F58);  // SHOOTER::MoveShooter(fighter, step, radius, 0, r7, 0, flag, put back on top)
REX_EXTERN(__imp__sub_829A5840);  // the fighter's move: MoveShooter(fighter, step) with the put back on top
REX_EXTERN(__imp__sub_829C2860);  // the game's configuration (fighter)

namespace cc1 {
namespace {

// FIGHTER offsets: grounded flag, fall velocity, push vector, a count above zero while something else moves the
// fighter, and flags (0x10: not moved, 1: passed on to MoveShooter).
constexpr uint32_t kGrounded = 1980, kFall = 2060, kPush = 2080, kMoved = 2160, kFlags = 3584;
constexpr uint32_t kGravity = 320;              // in the configuration
constexpr float kNoGround = -0.16f;             // of gravity: a fighter rising faster takes no ground
constexpr uint32_t kRadius = 0x8214B7D0 + 208;  // MoveShooter's radius, as the fighter's move passes it
constexpr uint32_t kMoveArg = 0x82B40C60;       // MoveShooter's r7, as the fighter's move passes it
constexpr double kLast = 0.25;                  // fraction of the step taken with the put back on top

float Float(const uint8_t* base, uint32_t at) {
  return std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(base + at));
}

// True when the step belongs to an airborne fighter that may take the ground.
bool Falling(PPCContext& ctx, uint8_t* base, uint32_t fighter, double step) {
  if (!REXCVAR_GET(cc1_landing_fix) || !(step > 0.0)) return false;
  if (base[fighter + kGrounded] || (rex::memory::load_and_swap<uint32_t>(base + fighter + kFlags) & 0x10) ||
      int32_t(rex::memory::load_and_swap<uint32_t>(base + fighter + kMoved)) > 0) {
    return false;
  }
  ctx.r3.u64 = fighter;
  __imp__sub_829C2860(ctx, base);
  return Float(base, fighter + kFall) < kNoGround * Float(base, ctx.r3.u32 + kGravity);
}

void Step(PPCContext& ctx, uint8_t* base) {
  const uint32_t fighter = ctx.r3.u32;
  const double step = ctx.f1.f64;
  if (Falling(ctx, base, fighter, step)) {
    static bool said = false;
    if (!std::exchange(said, true)) REXLOG_INFO("[fighter] falling steps are taken in two parts to land at once");
    const double last = step * kLast, first = step - last;
    // The first part consumes the push, so it is scaled to move as far as over the whole step.
    for (uint32_t at = fighter + kPush; at < fighter + kPush + 12; at += 4) {
      const float push = Float(base, at);
      if (push != 0.0f) {
        rex::memory::store_and_swap<uint32_t>(base + at, std::bit_cast<uint32_t>(float(push * (step / first))));
      }
    }
    ctx.r3.u64 = fighter;
    ctx.f1.f64 = first;
    ctx.f2.f64 = Float(base, kRadius);
    ctx.r6.u64 = 0;
    ctx.r7.u64 = rex::memory::load_and_swap<uint64_t>(base + kMoveArg);
    ctx.r8.u64 = 0;
    ctx.r9.u64 = rex::memory::load_and_swap<uint32_t>(base + fighter + kFlags) & 1;
    ctx.r10.u64 = 0;
    __imp__sub_82998F58(ctx, base);
    ctx.r3.u64 = fighter;
    ctx.f1.f64 = last;
    __imp__sub_829A5840(ctx, base);
    return;
  }
  ctx.r3.u64 = fighter;
  ctx.f1.f64 = step;
  __imp__sub_829A5840(ctx, base);
}

}  // namespace
}  // namespace cc1

REX_HOOK_RAW(sub_829A5840) { cc1::Step(ctx, base); }
