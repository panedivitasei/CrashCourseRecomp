// Internal glue between video_native.cpp (guest API surface and shader
// resolution) and renderer.cpp (plume backend).
// GPL-3.0 — see LICENSE.

#pragma once

#include <cstdint>
#include <vector>

namespace plume {
class RenderDevice;
class RenderCommandQueue;
}  // namespace plume

namespace rex::videonative {
struct ResolvedShader;
}

namespace rex::videonative::detail {

plume::RenderDevice* Device();
plume::RenderCommandQueue* Queue();

// Shaders resolved for the current draw, set by OnDraw in video_native.cpp
// before the renderer submits.
const ResolvedShader* CurrentResolvedVertexShader();
const ResolvedShader* CurrentResolvedPixelShader();
// Whether the guest has a pixel shader bound for the current draw: false is a
// depth-only draw, true with a null resolved PS is a pack miss that skips the
// draw.
bool CurrentDrawPixelShaderBound();
// Installs the shader resolution captured at enqueue time before the
// render-queue worker executes a deferred draw.
void OverrideResolvedShaders(const ResolvedShader* vs,
                             const ResolvedShader* ps, bool ps_bound);
void ClearResolvedShaderOverride();

// Built-in shader blobs from the shader pack, empty if absent and valid once
// the pack is loaded.
const std::vector<uint8_t>& RectExpandGsDxil();
const std::vector<uint8_t>& PointExpandGsDxil();
const std::vector<uint8_t>& ClipGsDxil();
const std::vector<uint8_t>& BlitVsDxil();
const std::vector<uint8_t>& BlitPsDxil();

}  // namespace rex::videonative::detail
