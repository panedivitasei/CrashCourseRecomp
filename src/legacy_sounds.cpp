// legacy_sounds.cpp: with cc1_legacy_sounds on, skips registering (sub_829E3448) the three sounds patch.pak replaces
// so the game's shipped ones play.

#include "legacy_sounds.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_829E3448);

REXCVAR_DEFINE_BOOL(cc1_legacy_sounds, false, "CC1",
                    "Plays the checkpoint and stumble sounds the game shipped with in place of the "
                    "title update's, read at launch.");

namespace cc1 {
namespace {

constexpr std::array<std::string_view, 3> kSounds{"data/sound/ambient/sfx_fire.wav",
                                                  "data/sound/player/stumble01.wav",
                                                  "data/sound/player/stumble_warning.wav"};

thread_local bool t_update_pack = false;  // the pack being registered is the update's

// Normalises a path to the form packs name their files in: lower case, forward slashes.
std::string Name(const char* text) {
  std::string name;
  for (; text && *text && name.size() < 260; ++text) {
    name += *text == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(*text)));
  }
  return name;
}

}  // namespace

void legacy_sounds::NotePack(const char* path) {
  const std::string name = Name(path);
  t_update_pack = name.size() >= 9 && name.compare(name.size() - 9, 9, "patch.pak") == 0;
}

}  // namespace cc1

REX_HOOK_RAW(sub_829E3448) {
  if (cc1::t_update_pack && REXCVAR_GET(cc1_legacy_sounds)) {
    const uint32_t at = rex::memory::load_and_swap<uint32_t>(base + ctx.r4.u32 + 32);
    const std::string name = cc1::Name(at ? reinterpret_cast<const char*>(base + at) : nullptr);
    if (std::find(cc1::kSounds.begin(), cc1::kSounds.end(), name) != cc1::kSounds.end()) {
      REXLOG_INFO("[sounds] the update's {} left out", name);
      return;
    }
  }
  __imp__sub_829E3448(ctx, base);
}
