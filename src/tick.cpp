// Game step time: the step is worked out here and handed to KERNEL's tick (sub_829630B8) through its clock read,
// either smoothed (cc1_smooth_frames) or fixed in length (cc1_tick_rate).

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "gpu/renderer.h"

REXCVAR_DEFINE_BOOL(cc1_smooth_frames, true, "CC1",
                    "Evens out the time the game steps by from frame to frame; off, each step is the "
                    "measured time of the frame before it.");
REXCVAR_DEFINE_INT32(cc1_tick_rate, 0, "CC1",
                     "Steps of the game a second, each the same length (60 is the console's), and as many "
                     "frames; 0 lets steps follow the frame rate.");
REXCVAR_DEFINE_BOOL(cc1_tick_trace, false, "CC1", "Logs each step: the time measured and the time given.");

REX_EXTERN(__imp__sub_829630B8);  // KERNEL's tick (kernel)
REX_EXTERN(__imp__sub_82942688);  // seconds since the engine started (f1)

namespace cc1 {
namespace {

constexpr uint32_t kLastClock = 0x8324D7DC;  // the clock at the game's last step (float)
constexpr double kStall = 0.1;               // a step this long is a load, not a frame
constexpr double kOnPace = 0.02;             // how near a pace counts as on it

thread_local bool t_given = false;  // the tick's next clock read returns t_step
thread_local double t_step = 0.0;

double Clock(PPCContext& ctx, uint8_t* base) {
  const PPCContext saved = ctx;
  __imp__sub_82942688(ctx, base);
  const double now = ctx.f1.f64;
  ctx = saved;
  return now;
}

// The step for a frame that took 'raw', with the renderer presenting every 'pace' seconds (0: at no set rate).
double Smooth(double raw, double pace) {
  static std::array<double, 16> last{};
  static size_t count = 0, next = 0;
  static double owed = 0.0;  // clock time not yet given out as steps
  if (raw > kStall) {
    count = next = 0;
    return raw;
  }
  last[next] = raw;
  next = (next + 1) % last.size();
  count = std::min(count + 1, last.size());
  double mean = 0.0;
  for (size_t i = 0; i < count; ++i) mean += last[i];
  mean /= double(count);
  double step = pace > 0.0 && std::abs(mean - pace) < pace * kOnPace ? pace : mean;
  owed = std::clamp(owed + raw - step, -kStall, kStall);
  const double back = std::clamp(owed * 0.01, -step * 0.01, step * 0.01);
  owed -= back;
  return step + back;
}

// Waits for the clock to reach the next of the steps 'step' apart, when the display is not what paces them.
double Fixed(PPCContext& ctx, uint8_t* base, double now, double step, double pace) {
  static double due = 0.0;
  if (pace > 0.0 && std::abs(pace - step) < step * kOnPace) {
    due = 0.0;
    return now;
  }
  // When behind (a load, or frames slower than the rate) the missed steps are dropped instead of run in a burst.
  if (due == 0.0 || now - due > 2.0 * step) due = now;
  while (due - now > 0.002) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    now = Clock(ctx, base);
  }
  while (now < due) {
    std::this_thread::yield();
    now = Clock(ctx, base);
  }
  due += step;
  return now;
}

void Tick(PPCContext& ctx, uint8_t* base) {
  static double before = 0.0;
  double now = Clock(ctx, base);
  const int rate = std::clamp(REXCVAR_GET(cc1_tick_rate), 0, 240);
  const double pace = rex::videonative::renderer::PresentInterval();
  double step = 0.0;
  if (rate) {
    step = 1.0 / rate;
    now = Fixed(ctx, base, now, step, pace);
  }
  const double raw = before > 0.0 ? now - before : 0.0;
  before = now;
  if (!rate) step = REXCVAR_GET(cc1_smooth_frames) && raw > 0.0 ? Smooth(raw, pace) : raw;
  if (REXCVAR_GET(cc1_tick_trace)) {
    REXLOG_INFO("[ticktrace] raw={:.3f} step={:.3f} pace={:.3f}", raw * 1e3, step * 1e3, pace * 1e3);
  }
  // The first step is left for the game to measure.
  if (step > 0.0) {
    rex::memory::store_and_swap<uint32_t>(base + kLastClock, 0);
    t_step = step;
    t_given = true;
  }
  __imp__sub_829630B8(ctx, base);
  t_given = false;
}

void ReadClock(PPCContext& ctx, uint8_t* base) {
  if (t_given) {
    t_given = false;
    ctx.f1.f64 = t_step;
    return;
  }
  __imp__sub_82942688(ctx, base);
}

}  // namespace
}  // namespace cc1

REX_HOOK_RAW(sub_829630B8) { cc1::Tick(ctx, base); }
REX_HOOK_RAW(sub_82942688) { cc1::ReadClock(ctx, base); }
