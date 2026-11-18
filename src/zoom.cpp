// Zoom setting for a course played alone: divides the course camera's half height (+1452) and leaves the eye and
// its target as the game's.

#include "zoom.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(cc1_zoom, 0, "CC1",
                     "Zoom of the course camera when playing alone, in steps of 20%: 5 is twice as close, -5 "
                     "twice as far, 0 the game's own.");

REX_EXTERN(__imp__sub_829B7BD0);  // SHOOTERCAMERA's update (camera)
REX_EXTERN(__imp__sub_82932F58);  // the activation pass

namespace cc1::zoom {
namespace {

constexpr uint32_t kGame = 0x82C0479C;     // GAMEXA*, null outside a course: its players at +384
constexpr uint32_t kNetwork = 0x82C52210;  // the session's kind at +168: 0 when it is not a network one
constexpr uint32_t kCamera = 0x821555FC;  // SHOOTERCAMERA's vtable
constexpr uint32_t kFov = 1420, kHeight = 1448, kHalf = 1452, kAngle = 1516, kProjectionDirty = 1525;

uint32_t g_camera = 0;  // the camera last updated, for the activation pass

uint32_t Word(uint8_t* base, uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); }
float Float(uint8_t* base, uint32_t at) { return std::bit_cast<float>(Word(base, at)); }
void Put(uint8_t* base, uint32_t at, double value) {
  rex::memory::store_and_swap<uint32_t>(base + at, std::bit_cast<uint32_t>(float(value)));
}

bool Alone(uint8_t* base) {
  const uint32_t game = Word(base, kGame), network = Word(base, kNetwork);
  return game && Word(base, game + 384) == 1 && (!network || Word(base, network + 168) == 0);
}

// How many times closer the picture is.
double Factor(uint8_t* base) {
  if (!Alone(base)) return 1.0;
  const int steps = std::clamp(REXCVAR_GET(cc1_zoom), -kSteps, kSteps);
  const double times = 1.0 + 0.2 * std::abs(steps);
  return steps < 0 ? 1.0 / times : times;
}

// Leaves the camera with the game's field of view seen 'factor' times closer.
void Set(uint8_t* base, uint32_t camera, double factor) {
  const double half = 0.75 * std::tan(double(Float(base, camera + kFov)) * 3.14159265358979323846 / 360.0) / factor;
  if (!(half > 0.0) || std::abs(Float(base, camera + kHalf) - half) <= half * 1e-5) return;
  Put(base, camera + kHalf, half);
  Put(base, camera + kHeight, half * 2.0);
  Put(base, camera + kAngle, std::atan(half) * 2.0);
  base[camera + kProjectionDirty] = 1;
}

void Update(PPCContext& ctx, uint8_t* base) {
  g_camera = ctx.r3.u32;
  Set(base, g_camera, Factor(base));
  __imp__sub_829B7BD0(ctx, base);
}

void Activate(PPCContext& ctx, uint8_t* base) {
  if (!Word(base, kGame)) g_camera = 0;
  const uint32_t camera = g_camera && Word(base, g_camera) == kCamera ? g_camera : 0;
  const double factor = Factor(base);
  if (camera && factor > 1.0) Set(base, camera, 1.0);
  __imp__sub_82932F58(ctx, base);
  // Reapplied here because a paused game skips the update, so the setting shows behind the pause menu.
  if (camera && Word(base, camera) == kCamera) Set(base, camera, factor);
}

}  // namespace

bool Available() {
  auto* memory = REX_KERNEL_MEMORY();
  return memory && Alone(memory->TranslateVirtual<uint8_t*>(0));
}

}  // namespace cc1::zoom

REX_HOOK_RAW(sub_829B7BD0) { cc1::zoom::Update(ctx, base); }
REX_HOOK_RAW(sub_82932F58) { cc1::zoom::Activate(ctx, base); }
