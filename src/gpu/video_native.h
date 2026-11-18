// rexvideonative — title-agnostic native implementation of the guest-facing XDK
// D3D API, hooked per title with REX_HOOK.
// Ported from UnleashedRecomp (https://github.com/hedge-dev/UnleashedRecomp,
// GPL-3.0) onto the plume RHI. See LICENSE in this directory: this module is
// GPL-3.0, unlike the rest of the SDK.

#pragma once

#include <cstdint>

namespace rex::videonative {

using GuestAddr = uint32_t;

// ---- Lifecycle ----

// Creates the plume device/swapchain and internal caches, lazily from
// CreateDevice or explicitly at app startup, and is safe to call more than
// once.
bool Init();
void Shutdown();

// True when the cvar `gpu_native` is set, the gate for the CreateDevice hook.
bool Enabled();

// True when enabled and the host device initialized; app hooks fall through to
// the recompiled guest code otherwise.
bool IsActive();

// Registers the recompiled XDK vfetch patcher (base sub_82536700 / TU1
// sub_82545DD0) so resolution can produce the patched VS ucode the pack is
// keyed on, with stride_table being 16 bytes of stride>>2 per stream.
using VfetchPatchFn = void (*)(uint32_t vs_obj, uint32_t code_copy,
                               uint32_t decl, uint32_t stride_table,
                               uint32_t variant);
void SetVfetchPatcher(VfetchPatchFn fn);

// Records the raw D3D device EA (real-SetRenderTarget r3) for the RB-shadow
// RT-key fold.
void NoteRawDevice(uint32_t device_ea);

// Called from the title's texture-release hook (DCC2 TU1 0x829F3CE0, header at
// [r3+36]) to drop the guest-uploaded cache entry keyed on that header, so a
// reused address is not served the previous texture's pixels.
void InvalidateTextureByHeader(uint32_t header_addr);

// Engine-authoritative RTT pass target, called from the title's
// StartTextureRendering hook with the TEXTUREBASEDX9's fields: width/height
// (+8/+12), EDRAM color/depth surfaces (+44/+72), resolve-dest D3DTexture
// (+28), D3DFORMAT (+96), MSAA type (+84) and tiling config index (+88, 999 =
// untiled), gated by cvar gpu_sem_rtt.
void NoteRttBegin(uint32_t texbase, uint32_t width, uint32_t height,
                  uint32_t color_surf, uint32_t depth_surf, uint32_t dest_tex,
                  uint32_t format, uint32_t msaa, uint32_t tiling);

// Delivers pending resolve write-backs into guest memory synchronously, for the
// guest thread at the game's resolve fence wait, and is a no-op when
// gpu_resolve_writeback is off or nothing is pending.
void FlushResolveWritebacksSync();

// True when write-back bytes must be delivered synchronously
// (gpu_writeback_sync), false by default so guest-thread callers such
// as the title's fence hook do not stall on delivery.
bool WritebackSyncEnabled();

// GPU fence writeback emulation: queues an EVENT_WRITE_SHD fence's writeback
// address, which receives an incrementing counter at the next Swap, for the
// hook on the XDK fence-packet writer (base 0x8253FDD8 / TU1 0x8254F4A8).
void QueueFenceWriteback(GuestAddr addr);

// Guest D3D surface: signatures mirror the mapped XDK functions and take/return
// guest addresses, marshalled from PPCContext by the app-side REX_HOOK table.

// --- device / swap ---
// Direct3D_CreateDevice (CC2 0x82533E10), where the game's main and secondary
// devices both map to the same host device.
uint32_t CreateDevice(GuestAddr d3d, uint32_t type, uint32_t unk, uint32_t flags,
                      GuestAddr present_params, GuestAddr out_device);
// D3DDevice_Swap (CC2 0x82532B50), which takes the front buffer texture.
void Swap(GuestAddr device, GuestAddr front_buffer_tex, GuestAddr params);
void AcquireThreadOwnership(GuestAddr device);
void ReleaseThreadOwnership(GuestAddr device);

// --- render targets / tiling ---
void SetRenderTarget(GuestAddr device, uint32_t index, GuestAddr surface);
void SetDepthStencilSurface(GuestAddr device, GuestAddr surface, uint32_t flags);
// D3DDevice_SetSurfaces(device, D3DSURFACES*{DS,RT0..3}, flags) — the CC2
// unbind idiom is SetSurfaces(all-NULL, 1).
void SetSurfaces(GuestAddr device, GuestAddr surfaces, uint32_t flags);
// D3DDevice_CreateSurface (0x8252BB30) takes no device argument and returns the
// surface object pointer in r3 (0 on failure).
uint32_t CreateSurface(uint32_t width, uint32_t height, uint32_t format,
                       uint32_t msaa, GuestAddr params);
// Predicated tiling: the scene is drawn once at full size and tile bands are
// cut out by Resolve.
void BeginTiling(GuestAddr device, uint32_t flags, uint32_t count,
                 GuestAddr rects, GuestAddr clear_color, float clear_z,
                 uint32_t clear_stencil);
void EndTiling(GuestAddr device, uint32_t flags = 0,
               GuestAddr rects = 0, GuestAddr dest_texture = 0);
void SetPredication(GuestAddr device, uint32_t mask);
// Resolve (CC2 0x82531058): EDRAM->texture copy becomes RT->texture alias/copy.
void Resolve(GuestAddr device, uint32_t flags, GuestAddr rect,
             GuestAddr dest_texture, GuestAddr dest_point);

// --- command buffers ---
// Calls recorded on the secondary device between Begin and End are captured per
// CB object and re-invoked at Run, with run-varying state (constants, textures,
// viewport) inherited from live state per the XDK inherit-tags contract.
uint32_t BeginCommandBuffer(GuestAddr device, GuestAddr cb, uint32_t flags);
uint32_t EndCommandBuffer(GuestAddr device);
void RunCommandBuffer(GuestAddr device, GuestAddr cb,
                      uint32_t predication_select);
void SetCommandBufferPredication(GuestAddr device, uint32_t tile_pred,
                                 uint32_t run_pred);
// XDK internal direct RB_BLENDCONTROL[rt] PM4 emitter (base 0x82537E40), which
// bypasses the device shadow and dirty bits and is recorded by the avatar
// material callback (0x829DDDB8) in predication-bracketed pairs.
void SetBlendControlDirect(GuestAddr device, uint32_t rt, uint32_t value);

// --- clear / viewport / scissor ---
void Clear(GuestAddr device, uint32_t count, GuestAddr rects, uint32_t flags,
           uint32_t color, double z, uint32_t stencil);
void SetViewport(GuestAddr device, GuestAddr viewport);  // {X,Y,W,H,minZ,maxZ}
void SetScissorRect(GuestAddr device, GuestAddr rect);

// --- resources ---
uint32_t CreateTexture(GuestAddr device, uint32_t width, uint32_t height,
                       uint32_t levels, uint32_t unk, uint32_t usage,
                       uint32_t format, uint32_t pool, uint32_t type);
uint32_t CreateVertexBuffer(uint32_t byte_size, uint32_t flags);
uint32_t CreateIndexBuffer(uint32_t byte_size, uint32_t flags, uint32_t format);
void ReleaseResource(GuestAddr resource);
void ReleaseDevice(GuestAddr device);

// --- binding ---
void SetTexture(GuestAddr device, uint32_t sampler, GuestAddr texture);
void SetStreamSource(GuestAddr device, uint32_t stream, GuestAddr vb,
                     uint32_t offset, uint32_t unk, uint32_t stride);
void SetIndices(GuestAddr device, GuestAddr ib);
void SetVertexDeclaration(GuestAddr device, GuestAddr decl);

// --- shaders ---
// Set*Shader receive the XDK D3D shader object: dword0 low nibble is the
// resource type (6=VS, 7=PS), VS has its microcode ptr at +0x20 and function
// header (code byte size at header+8) at +0x368, PS at +0x18 and +0x28.
uint32_t CreateVertexShader(GuestAddr microcode);
uint32_t CreatePixelShader(GuestAddr microcode);
void SetVertexShader(GuestAddr device, GuestAddr shader);
void SetPixelShader(GuestAddr device, GuestAddr shader);
void SetVertexShaderConstantF(GuestAddr device, uint32_t start_reg,
                              GuestAddr data, uint32_t count);
void SetPixelShaderConstantF(GuestAddr device, uint32_t start_reg,
                             GuestAddr data, uint32_t count);
void SetShaderGPRAllocation(GuestAddr device, uint32_t flags,
                            uint32_t vs_count, uint32_t ps_count);  // no-op

// --- render states (XDK per-state setters) ---
void SetRenderState_ZEnable(GuestAddr device, uint32_t enable);
void SetRenderState_ZWriteEnable(GuestAddr device, uint32_t enable);
void SetRenderState_ZFunc(GuestAddr device, uint32_t func);
void SetRenderState_CullMode(GuestAddr device, uint32_t mode);  // GPU enc 0/2/6
void SetRenderState_AlphaTestEnable(GuestAddr device, uint32_t enable);
void SetRenderState_AlphaRef(GuestAddr device, uint32_t ref);
void SetRenderState_AlphaFunc(GuestAddr device, uint32_t func);
void SetBlendControl(GuestAddr device, uint32_t rt_index, uint32_t blend_bits);

// --- draws ---
void DrawVertices(GuestAddr device, uint32_t prim_type, uint32_t start,
                  uint32_t count);
void DrawIndexedVertices(GuestAddr device, uint32_t prim_type,
                         uint32_t base_vertex, uint32_t start, uint32_t count);
void DrawVerticesUP(GuestAddr device, uint32_t prim_type, uint32_t count,
                    GuestAddr vertex_data, uint32_t stride);
void DrawIndexedVerticesUP(GuestAddr device, uint32_t prim_type,
                           uint32_t min_index, uint32_t start,
                           uint32_t num_vertices, GuestAddr index_data,
                           uint32_t index_format, GuestAddr vertex_data,
                           uint32_t stride);

}  // namespace rex::videonative
