// CC1 native-video hook table: MIDASM hooks on the XDK D3D surface, routed
// into rexvideonative (vn::).

#include "generated/cc1/cc1_init.h"

#include <video_native.h>

namespace vn = rex::videonative;

// --- recompiled vfetch patcher bridge ---------------------------------------
// The XDK patches VS microcode per binding, reproduced by calling the
// recompiled patcher (base 0x823CAD58 / TU1 0x8224BDC8) on a scratch copy.
REX_IMPORT(sub_8224BDC8, g_vfetchPatcherImport, u32(u32, u32, u32, u32, u32));

static void VfetchPatchBridge(uint32_t vs_obj, uint32_t code_copy,
                              uint32_t decl, uint32_t stride_table,
                              uint32_t variant) {
  g_vfetchPatcherImport(vs_obj, code_copy, decl, stride_table, variant);
}

namespace {
struct VfetchPatcherRegistrar {
  VfetchPatcherRegistrar() { vn::SetVfetchPatcher(VfetchPatchBridge); }
} g_vfetchPatcherRegistrar;
}  // namespace

// --- fences / device / swap -------------------------------------------------

// QueueShaderWriteback (base 0x823DCA00 / TU1 0x8225F160).
void VideoNative_FenceWrite(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!vn::IsActive()) return;
  vn::QueueFenceWriteback(ctx.r4.u32);
}

// Direct3D_CreateDevice (base 0x823D25D8 / TU1 0x8224DD30): gated on
// Enabled(), and any failure falls back to the guest path before guest state
// is touched.
bool VideoNative_CreateDevice(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!vn::Enabled()) return false;
  const uint32_t hr = vn::CreateDevice(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                       ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
  if (hr != 0) return false;
  ctx.r3.u64 = 0;  // S_OK
  return true;
}

// D3DDevice_Swap (base 0x823D2EC8 / TU1 0x82254A90): (device, frontBuffer,
// params).
bool VideoNative_Swap(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!vn::IsActive()) return false;
  vn::Swap(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  return true;
}

// Pure skips while the layer is active.
#define VN_SKIP_HOOK(name)                    \
  bool name(PPCContext& ctx, uint8_t* base) { \
    (void)ctx;                                \
    (void)base;                               \
    return vn::IsActive();                    \
  }

// D3D BlockOnResourceFence-class wait (base 0x823D1EA0 / TU1 0x8224D488):
// every wait is already satisfied under native instant completion, and
// callers (sub_82249778) loop until it returns 0, so the skip sets r3 = 0.
bool VideoNative_BlockOnFence(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!vn::IsActive()) return false;
  // Write-backs are flushed here only under the legacy cvar, because resolve
  // products are sampled as textures and a flush stalls the guest thread at
  // every fence wait.
  if (vn::WritebackSyncEnabled()) {
    vn::FlushResolveWritebacksSync();
  }
  ctx.r3.u64 = 0;  // signaled
  return true;
}
VN_SKIP_HOOK(VideoNative_PreSwap)                 // base 0x823D2EC0 / TU1 0x82254A88
VN_SKIP_HOOK(VideoNative_ReleaseDevice)           // base 0x823D20E0 / TU1 0x8224D808
VN_SKIP_HOOK(VideoNative_AcquireThreadOwnership)  // base 0x823D2220 / TU1 0x8224D978
VN_SKIP_HOOK(VideoNative_ReleaseThreadOwnership)  // base 0x823D2188 / TU1 0x8224D8E0

// --- surfaces / passes --------------------------------------------------------

bool VideoNative_SetRenderTarget(PPCContext& ctx, uint8_t* base) {  // 0x823C6FC0
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetRenderTarget(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  return true;
}

// The real SetRenderTarget entry (TU1 0x822476D8; 0x82247D40 is a thunk many
// callers bypass), cvar-gated and off by default because hooking it makes the
// picture vary from boot to boot.
REXCVAR_DEFINE_BOOL(cc1_hook_real_setrt, false, "CC1",
                    "Hooks the real SetRenderTarget entry as well as its "
                    "thunk; off hooks the thunk only.");
bool VideoNative_SetRenderTargetReal(PPCContext& ctx, uint8_t* base) {
  // r3 is the raw D3D device (not the draw device), which the RB-shadow
  // RT-key fold reads register-level identity from.
  vn::NoteRawDevice(ctx.r3.u32);
  if (!REXCVAR_GET(cc1_hook_real_setrt)) return false;
  return VideoNative_SetRenderTarget(ctx, base);
}
bool VideoNative_SetDepthStencilSurface(PPCContext& ctx,
                                        uint8_t* base) {  // 0x823C6CE8
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetDepthStencilSurface(ctx.r3.u32, ctx.r4.u32, 0);
  return true;
}
// CreateSurface(width, height, format, msaa, params) -> surface ptr in r3.
bool VideoNative_CreateSurface(PPCContext& ctx, uint8_t* base) {  // 0x823D6CD8
  (void)base;
  if (!vn::IsActive()) return false;
  ctx.r3.u64 = vn::CreateSurface(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                                 ctx.r6.u32, ctx.r7.u32);
  return true;
}
// BeginTiling (base 0x823D7730 / TU1 0x8225B4B8): (device, flags, count,
// D3DRECT* rects, float* clearColor, [f1] clearZ, r8, r9=clearStencil).
bool VideoNative_BeginTiling(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!vn::IsActive()) return false;
  vn::BeginTiling(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
                  float(ctx.f1.f64), ctx.r9.u32);
  return true;
}
// EndTiling(device, flags, rects, destTexture, clearColor, [f1] clearZ, ...):
// with a dest texture the XDK body resolves every tile rect into it, the only
// path CC1's scene takes to the frontbuffer.
bool VideoNative_EndTiling(PPCContext& ctx, uint8_t* base) {  // TU1 0x8225B828
  (void)base;
  if (!vn::IsActive()) return false;
  vn::EndTiling(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}
bool VideoNative_SetPredication(PPCContext& ctx,
                                uint8_t* base) {  // TU1 0x8225B348
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetPredication(ctx.r3.u32, ctx.r4.u32);
  return true;
}
bool VideoNative_Resolve(PPCContext& ctx, uint8_t* base) {  // TU1 0x8224FA28
  (void)base;
  if (!vn::IsActive()) return false;
  vn::Resolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32);
  return true;
}
// Clear(device, count, pRects, flags, color, z, stencil), with z in f1.
bool VideoNative_Clear(PPCContext& ctx, uint8_t* base) {  // TU1 0x8223D958
  (void)base;
  if (!vn::IsActive()) return false;
  vn::Clear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32,
            ctx.f1.f64, ctx.r8.u32);
  return true;
}

// --- state / binding ----------------------------------------------------------

bool VideoNative_SetViewport(PPCContext& ctx, uint8_t* base) {  // 0x823C6868
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetViewport(ctx.r3.u32, ctx.r4.u32);
  return true;
}
bool VideoNative_SetScissorRect(PPCContext& ctx, uint8_t* base) {  // 0x823C5F48
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetScissorRect(ctx.r3.u32, ctx.r4.u32);
  return true;
}
// CC1's raw-header avatar texture bind is inlined into this function, with no
// separate SetTextureFromHeader entry point in the binary.
bool VideoNative_SetTexture(PPCContext& ctx, uint8_t* base) {  // 0x823D6F38
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetTexture(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32);
  return true;
}
bool VideoNative_SetStreamSource(PPCContext& ctx, uint8_t* base) {  // 0x823C6040
  (void)base;
  if (!vn::IsActive()) return false;
  // Guest args are (device, stream, vb, offsetBytes, stride, flags), with
  // stride in r7 and flags in r8.
  vn::SetStreamSource(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                      ctx.r8.u32, ctx.r7.u32);
  return true;
}
bool VideoNative_SetIndices(PPCContext& ctx, uint8_t* base) {  // 0x823C61E8
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetIndices(ctx.r3.u32, ctx.r4.u32);
  return true;
}
bool VideoNative_SetVertexDecl(PPCContext& ctx, uint8_t* base) {  // 0x823BEE20
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetVertexDeclaration(ctx.r3.u32, ctx.r4.u32);
  return true;
}
// SetVertexShader/SetPixelShader observe and fall through (return false) so
// the recompiled bodies apply the shader's bind-time default-constant table
// into the device constant block that live draws read from.
bool VideoNative_SetVertexShader(PPCContext& ctx, uint8_t* base) {  // 0x823BEC50
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetVertexShader(ctx.r3.u32, ctx.r4.u32);
  return false;  // run the original body too (bind-time constant table)
}
bool VideoNative_SetPixelShader(PPCContext& ctx, uint8_t* base) {  // 0x823BEA90
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetPixelShader(ctx.r3.u32, ctx.r4.u32);
  return false;  // run the original body too (bind-time constant table)
}
// Set*ShaderConstantF observe and fall through so the XDK bodies maintain the
// device float shadow that live draws source b1 from.
bool VideoNative_SetVSConstF(PPCContext& ctx, uint8_t* base) {  // 0x823BE6E8
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetVertexShaderConstantF(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return false;
}
bool VideoNative_SetPSConstF(PPCContext& ctx, uint8_t* base) {  // 0x823BE7C0
  (void)base;
  if (!vn::IsActive()) return false;
  vn::SetPixelShaderConstantF(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return false;
}
VN_SKIP_HOOK(VideoNative_SetGPRAllocation)  // base 0x823BEFA8 / TU1 0x8223FCA0

// --- draws --------------------------------------------------------------------

bool VideoNative_DrawVertices(PPCContext& ctx, uint8_t* base) {  // 0x823C7A60
  (void)base;
  if (!vn::IsActive()) return false;
  vn::DrawVertices(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  return true;
}
bool VideoNative_DrawIndexedVertices(PPCContext& ctx,
                                     uint8_t* base) {  // 0x823C7E50
  (void)base;
  if (!vn::IsActive()) return false;
  vn::DrawIndexedVertices(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                          ctx.r7.u32);
  return true;
}
bool VideoNative_DrawVerticesUP(PPCContext& ctx, uint8_t* base) {  // 0x823C7460
  (void)base;
  if (!vn::IsActive()) return false;
  vn::DrawVerticesUP(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                     ctx.r7.u32);
  return true;
}
bool VideoNative_DrawIndexedVerticesUP(PPCContext& ctx,
                                       uint8_t* base) {  // 0x823C79C0
  if (!vn::IsActive()) return false;
  // (device, primType, minVertexIndex, numVertices, indexCount, pIndexData,
  // indexFormat, pVertexData, [stack] strideBytes), with the 9th arg on the
  // stack.
  const uint32_t stride = __builtin_bswap32(
      *reinterpret_cast<const uint32_t*>(base + ctx.r1.u32 + 0x54));
  vn::DrawIndexedVerticesUP(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32,
                            stride);
  return true;
}
