// Sampler semantics: decodes fetch-constant sampler state per the ring
// renderer's contract and maps it to the native renderer's sampler slots.
// GPL-3.0 — see LICENSE in src/videonative.

#pragma once

#include <cstdint>

#include <plume_render_interface.h>

namespace rex::videonative::semantics {

// Decoded ring-contract sampler state for one fetch constant, without
// per-instruction attribute overrides.
struct SamplerSpec {
  plume::RenderFilter min_filter;
  plume::RenderFilter mag_filter;
  bool aniso;
  uint32_t max_aniso;
  bool w_matters;  // 3D texture: W axis participates in addressing
  plume::RenderTextureAddressMode u, v, w;
  bool border_used;
  plume::RenderBorderColor border;
  // Sampler state of the 360 that the host path cannot express (host textures
  // are single-mip), logged instead of applied.
  float lod_bias;
  bool mip_point;
  bool ycbcr_border;
};

// Decodes 6 host-endian fetch-constant dwords per the ring contract
// (SamplerInfo::Prepare, texture_util::GetClampModesForDimension,
// D3D12TextureCache::NormalizeClampMode/WriteSampler).
void DecodeSamplerSpec(const uint32_t dw[6], SamplerSpec* out);

// Maps a spec to a sampler descriptor slot: static 0-12 when exact, dynamic
// 13-15 (write-once) otherwise, nearest static when those are full.
uint32_t MapToSlot(const SamplerSpec& spec);

// Called once after the static samplers (slots 0-12) are populated.
void InitSamplerArm(plume::RenderDevice* device,
                    plume::RenderDescriptorSet* sampler_set);

}  // namespace rex::videonative::semantics
