// Native renderer: draws the guest's D3D calls through plume with the
// XenosRecomp native shader pack (bindless DxbcShaderTranslator contract, see
// docs/native_shaders.md).
// GPL-3.0 — see LICENSE in this directory.

#pragma once

#include <cstdint>

namespace rex::videonative {

class ShaderCache;
struct ResolvedShader;

namespace renderer {

// Called from Init() after the plume device/queue/swapchain exist.
bool Init();
void Shutdown();

// Frame flow: EnsureFrameOpen() lazily acquires the backbuffer and opens the
// frame's command list; EndFrameAndPresent() closes, executes and presents.
void EnsureFrameOpen();
void EndFrameAndPresent();
// Resolve write-back of guest-visible resolve bytes, a fallback path whose
// pending count is readable cross-thread and whose flush runs on the
// renderer-execution thread.
uint32_t ResolveWritebacksPending();
bool ResolveWritebackEnabled();
// Delivers write-back bytes synchronously at each resolve instead of at frame
// end, off by default because it stalls the guest thread on a fence per
// resolve.
bool WritebackSyncEnabled();
void FlushResolveWritebacksInline();
// Records the raw D3D device EA (the real SetRenderTarget's r3, distinct from
// the draw device) for the RB-shadow RT-key fold.
void NoteRawDevice(uint32_t device_ea);

// State capture from hooks.
void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float min_z,
                 float max_z);
void SetScissor(int32_t left, int32_t top, int32_t right, int32_t bottom);
void SetTexture(uint32_t sampler, uint32_t guest_texture_header);
// Worker-side variant that applies a bind whose 6 fetch dwords were captured at
// call time by the render queue (render_queue.h).
void SetTextureWithSnapshot(uint32_t sampler, uint32_t header,
                            const uint32_t* dw6);
void SetStream(uint32_t stream, uint32_t vb_object, uint32_t offset_bytes,
               uint32_t stride_bytes);
void SetIndices(uint32_t ib_object);
void SetShaderConstantsF(bool pixel, uint32_t start_reg, uint32_t guest_data,
                         uint32_t vec4_count);
// Host-order data variant for command-buffer replay (record-time capture).
void SetShaderConstantsFHost(bool pixel, uint32_t start_reg,
                             const uint32_t* host_dwords, uint32_t vec4_count);
// Dword-granular float patch and merge into the replay mirror, reproducing the
// XDK shader definition table's group-2 raw copies and group-3 masked merges,
// with dword_index counting 4-byte units within the stage's 256-register float
// block.
void PatchShaderFloatDwordsHost(bool pixel, uint32_t dword_index,
                                const uint32_t* host_dwords, uint32_t count);
void MergeShaderFloatDwordHost(bool pixel, uint32_t dword_index, uint32_t mask,
                               uint32_t orv);
// rects is an optional host-order {l,t,r,b} array that scopes the clear like
// the XDK's pRects.
void Clear(uint32_t device, uint32_t flags, uint32_t color, float z,
           const int32_t* rects = nullptr, uint32_t rect_count = 0);

// Adapter vendor, set once at device init for vendor-gated features (the
// Intel-only gpu_vs_trim mode).
void SetAdapterIsIntel(bool is_intel);

// Render targets / resolve / present composite.
void RegisterSurface(uint32_t surface_obj, uint32_t width, uint32_t height,
                     uint32_t guest_format);
// Binds a guest color surface to MRT slot 0-3, where slot 0 is the primary
// surface (0 = swapchain) and slots 1-3 add their formats to the RT set.
void SetRenderTargetSurface(uint32_t index, uint32_t surface_obj);
void SetDepthSurface(uint32_t surface_obj);
// Predicated tiling: while an extent is set (BeginTiling..EndTiling), host RTs
// for the bound tile-height EDRAM surfaces cover the full frame, and (0,0)
// turns tiling off.
void SetTilingExtent(uint32_t width, uint32_t height);
// Drops the cached upload of a destroyed guest texture so a reused address is
// not served stale pixels.
void InvalidateTextureByHeader(uint32_t header_addr);
// Engine-authoritative RTT pass target: while the noted TEXTUREBASEDX9's
// surfaces are bound, host RT identity and dimensions come from it instead of
// EDRAM inference.
void NoteRttBegin(uint32_t texbase, uint32_t width, uint32_t height,
                  uint32_t color_surf, uint32_t depth_surf, uint32_t dest_tex,
                  uint32_t format, uint32_t msaa, uint32_t tiling);
// Source rect (frame coordinates) and dest point follow XDK Resolve semantics,
// has_rect=false copies the full surface to (0,0), and source is the color MRT
// index 0-3 (Resolve flags bits 0-2).
void ResolveToTexture(uint32_t dest_texture_header, uint32_t source,
                      bool has_rect, int32_t src_left, int32_t src_top,
                      int32_t src_right, int32_t src_bottom, int32_t dest_x,
                      int32_t dest_y);
// Depth resolve (Resolve source 4): copies the active RT's host depth into
// the destination texture (k_24_8-family header -> R32_FLOAT host texture).
void ResolveDepthToTexture(uint32_t dest_texture_header, bool has_rect,
                           int32_t src_left, int32_t src_top,
                           int32_t src_right, int32_t src_bottom,
                           int32_t dest_x, int32_t dest_y);
void SwapFrontbuffer(uint32_t frontbuffer_texture_header);
// Uploads the game's display gamma ramp (host-order R[256],G[256],B[256] 16-bit
// words) into the present blit's LUT, applied only while cvar
// gpu_gamma_ramp is set.
void UpdateGammaRamp(const uint16_t* rgb768);

// Guest-state snapshot around command-buffer replay.
void PushState();
void PopState();

// Seeds the replay float-constant mirror from the live device float shadow
// (m_Constants.Alu: VS at dev+1920, PS at dev+6016), taking registers the run
// device never set from recording_device when one is given.
void SeedReplayFloatConstants(uint32_t device, uint32_t recording_device);

// Per-draw render-state override for command-buffer replay: 10 raw dwords
// (RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_SU_SC_MODE_CNTL,
// RB_COLOR_MASK, RB_ALPHA_REF, composed ZEnable, RB_BLENDCONTROL1-3) captured
// at record time, where clear dirty_mask bits inherit the run device's live
// shadow and nullptr clears the override.
void SetReplayDrawState(const uint32_t* state10, uint32_t dirty_mask);
// Bool/loop constants (40 host-order dwords) captured from the recording device
// at record time, cleared by nullptr.
void SetReplayBoolConstants(const uint32_t* dwords40);
// Recording-device fetch-constant shadow records for the current replayed draw
// ({slot, dw0..dw5} x count), cleared by nullptr.
void SetReplayFetchConstants(const uint32_t* records, uint32_t count);
// Applies a record-time state capture's authoritative blend/mask slots to the
// persistent register file, as the XDK flushes pending state before a
// nested-run token.
void ApplyReplayStatePersistent(const uint32_t* state10, uint32_t dirty_mask);
// Direct RB_BLENDCONTROL[rt] register-file write that bypasses shadow and dirty
// bits, matching the XDK's no-shadow PM4 emit.
void ApplyBlendControlDirect(uint32_t rt, uint32_t value);
// Folds the run device's live blend/mask dirty bits into the register file and
// drains them, as the XDK Run pre-flush does; called at RunCommandBuffer start
// and by SetupDraw for live draws only.
void ConsumeLiveStateDirty(uint32_t device);
// Worker-side apply of the values ConsumeLiveStateDirty captured.
void ApplyLiveStateDirtyHost(uint32_t consumed_mask, const uint32_t vals[5]);
// Seconds between presents as vsync or the frame cap holds them, 0 when neither
// does; callable from any thread.
double PresentInterval();

// Draw entry points (already routed through shader resolution by the caller).
void DrawVertices(uint32_t device, uint32_t prim_type, uint32_t start_vertex,
                  uint32_t vertex_count);
void DrawIndexedVertices(uint32_t device, uint32_t prim_type,
                         uint32_t base_vertex, uint32_t start_index,
                         uint32_t index_count);
// UP-draw source override: replayed command buffers draw from the vertex/index
// bytes captured at record time, set before a replayed UP draw and cleared
// after.
void SetUPDataOverride(const void* vertex_data, const void* index_data);

void DrawVerticesUP(uint32_t device, uint32_t prim_type, uint32_t vertex_count,
                    uint32_t vertex_data, uint32_t stride_bytes);
void DrawIndexedVerticesUP(uint32_t device, uint32_t prim_type,
                           uint32_t min_vertex, uint32_t num_vertices,
                           uint32_t index_count, uint32_t index_data,
                           uint32_t index_format, uint32_t vertex_data,
                           uint32_t stride_bytes);

}  // namespace renderer
}  // namespace rex::videonative
