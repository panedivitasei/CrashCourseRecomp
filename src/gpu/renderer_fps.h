// FPS overlay for the native present path, where the ring presenter's ImGui
// overlays are not attached.
// GPL-3.0 — see LICENSE in this directory.

#pragma once

#include <cstdint>

namespace rex::videonative::fps {

// Result of one presentation tick, whose `pixels` RGBA8 buffer is module-owned,
// valid until the next Tick() and re-uploaded only when `dirty`.
struct Overlay {
  bool visible = false;
  uint32_t width = 0;
  uint32_t height = 0;
  const uint8_t* pixels = nullptr;
  bool dirty = false;
  // Draw scale for the strip: 1.0 for the cardigan path rasterized at final
  // pixel size, 1.5 for the builtin 5x7 fallback.
  float scale = 1.0f;
  // Width in pixels of the strip covered by text and padding, to which the
  // renderer scissors the box.
  uint32_t used_width = 0;
};

// Called once per SwapFrontbuffer to advance the swap-to-swap clock and, when
// the gpu_fps cvar is on, rasterize "<fps> FPS" into the pixel buffer.
Overlay Tick();

}  // namespace rex::videonative::fps
