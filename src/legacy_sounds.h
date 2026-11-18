// legacy_sounds.h: restores the sounds the title update replaced (legacy_sounds.cpp).

#pragma once

namespace cc1::legacy_sounds {

// Notes that AddPack is registering the pack at 'path', which owns every file registered on this thread until the
// next call.
void NotePack(const char* path);

}  // namespace cc1::legacy_sounds
