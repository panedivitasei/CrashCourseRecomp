// gamer_pictures.cpp: clears the pictures-owed byte (profile +5672) before gui.AwardGamerPictures (sub_829DF388) so
// the award prompt does not return at every launch.

#include <cstdint>

#include <rex/hook.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_829DF388);

REX_HOOK_RAW(sub_829DF388) {
  constexpr uint32_t kProfiles = 0x82C047A0, kPad = 16420, kOwed = 5672;
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const uint32_t pad = ctx.r4.u32 ? word(ctx.r4.u32 + kPad) : 4, profiles = word(kProfiles);
  if (pad < 4 && profiles) {
    if (const uint32_t profile = word(profiles + 4 * pad)) base[profile + kOwed] = 0;
  }
  __imp__sub_829DF388(ctx, base);
}
