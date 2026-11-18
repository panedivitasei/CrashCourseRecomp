// guest_hooks.cpp: binds function-entry hooks (REX_HOOK_RAW over __imp__sub_*) and the
// manifest's midasm hooks to their bodies in probe.cpp and video_hooks.cpp.

#include <cstdint>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/system/thread_state.h>

namespace {
PPCContext& GuestContext() { return *rex::runtime::current_ppc_context(); }
uint8_t* GuestBase() { return REX_KERNEL_MEMORY()->TranslateVirtual<uint8_t*>(0); }
}  // namespace

// Hook bodies.
void CC1_AddPackProbe(PPCContext& ctx, uint8_t* base);
void CC1_ScriptEventFilter(PPCContext& ctx, uint8_t* base);
void CC1_ChickenFlagKeep(PPCContext& ctx, uint8_t* base);
void CC1_StartTexRenderProbe(PPCContext& ctx, uint8_t* base);
void CC1_BrtEntryProbe(PPCContext& ctx, uint8_t* base);
void VideoNative_FenceWrite(PPCContext& ctx, uint8_t* base);
bool VideoNative_CreateDevice(PPCContext& ctx, uint8_t* base);
bool VideoNative_Swap(PPCContext& ctx, uint8_t* base);
bool VideoNative_PreSwap(PPCContext& ctx, uint8_t* base);
bool VideoNative_ReleaseDevice(PPCContext& ctx, uint8_t* base);
bool VideoNative_AcquireThreadOwnership(PPCContext& ctx, uint8_t* base);
bool VideoNative_ReleaseThreadOwnership(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetRenderTarget(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetRenderTargetReal(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetDepthStencilSurface(PPCContext& ctx, uint8_t* base);
bool VideoNative_CreateSurface(PPCContext& ctx, uint8_t* base);
bool VideoNative_BeginTiling(PPCContext& ctx, uint8_t* base);
bool VideoNative_EndTiling(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetPredication(PPCContext& ctx, uint8_t* base);
bool VideoNative_Resolve(PPCContext& ctx, uint8_t* base);
bool VideoNative_Clear(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetViewport(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetScissorRect(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetTexture(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetStreamSource(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetIndices(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetVertexDecl(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetVertexShader(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetPixelShader(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetVSConstF(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetPSConstF(PPCContext& ctx, uint8_t* base);
bool VideoNative_SetGPRAllocation(PPCContext& ctx, uint8_t* base);
bool VideoNative_DrawVertices(PPCContext& ctx, uint8_t* base);
bool VideoNative_DrawIndexedVertices(PPCContext& ctx, uint8_t* base);
bool VideoNative_DrawVerticesUP(PPCContext& ctx, uint8_t* base);
bool VideoNative_DrawIndexedVerticesUP(PPCContext& ctx, uint8_t* base);
bool VideoNative_BlockOnFence(PPCContext& ctx, uint8_t* base);
void CC1_LightCamFillForce(PPCContext& ctx, uint8_t* base);
void CC1_CasterMaskForce(PPCContext& ctx, uint8_t* base);
void CC1_SkipDemoFence(PPCContext& ctx, uint8_t* base, PPCRegister& r31);
void CC1_StartTexRenderFailA(PPCContext& ctx, uint8_t* base);
void CC1_StartTexRenderFailB(PPCContext& ctx, uint8_t* base);
void CC1_StartTexRenderFailC(PPCContext& ctx, uint8_t* base);
void CC1_RenderPassedProbe(PPCContext& ctx, uint8_t* base);
void CC1_PassLoopTop(PPCContext& ctx, uint8_t* base, PPCRegister& r31);
void CC1_PassGateVerdict(PPCContext& ctx, uint8_t* base, PPCRegister& r31);
void CC1_ShadowGateProbe(PPCContext& ctx, uint8_t* base);
void CC1_UnlinkGuard(PPCContext& ctx, uint8_t* base);
void CC1_ShadowBeginProbe(PPCContext& ctx, uint8_t* base);
void CC1_ShadowCasterDrawProbe(PPCContext& ctx, uint8_t* base);
void CC1_ShadowEndProbe(PPCContext& ctx, uint8_t* base);

// ---- Function-entry hooks ------------------------------------------------
REX_EXTERN(__imp__sub_829E37E8);
REX_HOOK_RAW(sub_829E37E8) {
  CC1_AddPackProbe(ctx, base);
  __imp__sub_829E37E8(ctx, base);
}
REX_EXTERN(__imp__sub_8248C870);
REX_HOOK_RAW(sub_8248C870) {
  CC1_ScriptEventFilter(ctx, base);
  __imp__sub_8248C870(ctx, base);
}
REX_EXTERN(__imp__sub_821B41E8);
REX_HOOK_RAW(sub_821B41E8) {
  CC1_ChickenFlagKeep(ctx, base);
  __imp__sub_821B41E8(ctx, base);
}
REX_EXTERN(__imp__sub_828BAF00);
REX_HOOK_RAW(sub_828BAF00) {
  CC1_StartTexRenderProbe(ctx, base);
  __imp__sub_828BAF00(ctx, base);
}
REX_EXTERN(__imp__sub_82930F48);
REX_HOOK_RAW(sub_82930F48) {
  CC1_BrtEntryProbe(ctx, base);
  __imp__sub_82930F48(ctx, base);
}
REX_EXTERN(__imp__sub_8225F160);
REX_HOOK_RAW(sub_8225F160) {
  VideoNative_FenceWrite(ctx, base);
  __imp__sub_8225F160(ctx, base);
}
REX_EXTERN(__imp__sub_8224DD30);
REX_HOOK_RAW(sub_8224DD30) {
  if (VideoNative_CreateDevice(ctx, base)) return;
  __imp__sub_8224DD30(ctx, base);
}
REX_EXTERN(__imp__sub_82254A90);
REX_HOOK_RAW(sub_82254A90) {
  if (VideoNative_Swap(ctx, base)) return;
  __imp__sub_82254A90(ctx, base);
}
REX_EXTERN(__imp__sub_82254A88);
REX_HOOK_RAW(sub_82254A88) {
  if (VideoNative_PreSwap(ctx, base)) return;
  __imp__sub_82254A88(ctx, base);
}
REX_EXTERN(__imp__sub_8224D808);
REX_HOOK_RAW(sub_8224D808) {
  if (VideoNative_ReleaseDevice(ctx, base)) return;
  __imp__sub_8224D808(ctx, base);
}
REX_EXTERN(__imp__sub_8224D978);
REX_HOOK_RAW(sub_8224D978) {
  if (VideoNative_AcquireThreadOwnership(ctx, base)) return;
  __imp__sub_8224D978(ctx, base);
}
REX_EXTERN(__imp__sub_8224D8E0);
REX_HOOK_RAW(sub_8224D8E0) {
  if (VideoNative_ReleaseThreadOwnership(ctx, base)) return;
  __imp__sub_8224D8E0(ctx, base);
}
REX_EXTERN(__imp__sub_82247D40);
REX_HOOK_RAW(sub_82247D40) {
  if (VideoNative_SetRenderTarget(ctx, base)) return;
  __imp__sub_82247D40(ctx, base);
}
REX_EXTERN(__imp__sub_822476D8);
REX_HOOK_RAW(sub_822476D8) {
  if (VideoNative_SetRenderTargetReal(ctx, base)) return;
  __imp__sub_822476D8(ctx, base);
}
REX_EXTERN(__imp__sub_82247A68);
REX_HOOK_RAW(sub_82247A68) {
  if (VideoNative_SetDepthStencilSurface(ctx, base)) return;
  __imp__sub_82247A68(ctx, base);
}
REX_EXTERN(__imp__sub_82258A98);
REX_HOOK_RAW(sub_82258A98) {
  if (VideoNative_CreateSurface(ctx, base)) return;
  __imp__sub_82258A98(ctx, base);
}
REX_EXTERN(__imp__sub_8225B4B8);
REX_HOOK_RAW(sub_8225B4B8) {
  if (VideoNative_BeginTiling(ctx, base)) return;
  __imp__sub_8225B4B8(ctx, base);
}
REX_EXTERN(__imp__sub_8225B828);
REX_HOOK_RAW(sub_8225B828) {
  if (VideoNative_EndTiling(ctx, base)) return;
  __imp__sub_8225B828(ctx, base);
}
REX_EXTERN(__imp__sub_8225B348);
REX_HOOK_RAW(sub_8225B348) {
  if (VideoNative_SetPredication(ctx, base)) return;
  __imp__sub_8225B348(ctx, base);
}
REX_EXTERN(__imp__sub_8224FA28);
REX_HOOK_RAW(sub_8224FA28) {
  if (VideoNative_Resolve(ctx, base)) return;
  __imp__sub_8224FA28(ctx, base);
}
REX_EXTERN(__imp__sub_8223D958);
REX_HOOK_RAW(sub_8223D958) {
  if (VideoNative_Clear(ctx, base)) return;
  __imp__sub_8223D958(ctx, base);
}
REX_EXTERN(__imp__sub_822475A8);
REX_HOOK_RAW(sub_822475A8) {
  if (VideoNative_SetViewport(ctx, base)) return;
  __imp__sub_822475A8(ctx, base);
}
REX_EXTERN(__imp__sub_82246C88);
REX_HOOK_RAW(sub_82246C88) {
  if (VideoNative_SetScissorRect(ctx, base)) return;
  __imp__sub_82246C88(ctx, base);
}
REX_EXTERN(__imp__sub_82258CF8);
REX_HOOK_RAW(sub_82258CF8) {
  if (VideoNative_SetTexture(ctx, base)) return;
  __imp__sub_82258CF8(ctx, base);
}
REX_EXTERN(__imp__sub_82246D80);
REX_HOOK_RAW(sub_82246D80) {
  if (VideoNative_SetStreamSource(ctx, base)) return;
  __imp__sub_82246D80(ctx, base);
}
REX_EXTERN(__imp__sub_82246F28);
REX_HOOK_RAW(sub_82246F28) {
  if (VideoNative_SetIndices(ctx, base)) return;
  __imp__sub_82246F28(ctx, base);
}
REX_EXTERN(__imp__sub_8223FB18);
REX_HOOK_RAW(sub_8223FB18) {
  if (VideoNative_SetVertexDecl(ctx, base)) return;
  __imp__sub_8223FB18(ctx, base);
}
REX_EXTERN(__imp__sub_8223F900);
REX_HOOK_RAW(sub_8223F900) {
  if (VideoNative_SetVertexShader(ctx, base)) return;
  __imp__sub_8223F900(ctx, base);
}
REX_EXTERN(__imp__sub_8223F6F8);
REX_HOOK_RAW(sub_8223F6F8) {
  if (VideoNative_SetPixelShader(ctx, base)) return;
  __imp__sub_8223F6F8(ctx, base);
}
REX_EXTERN(__imp__sub_8223F350);
REX_HOOK_RAW(sub_8223F350) {
  if (VideoNative_SetVSConstF(ctx, base)) return;
  __imp__sub_8223F350(ctx, base);
}
REX_EXTERN(__imp__sub_8223F428);
REX_HOOK_RAW(sub_8223F428) {
  if (VideoNative_SetPSConstF(ctx, base)) return;
  __imp__sub_8223F428(ctx, base);
}
REX_EXTERN(__imp__sub_8223FCA0);
REX_HOOK_RAW(sub_8223FCA0) {
  if (VideoNative_SetGPRAllocation(ctx, base)) return;
  __imp__sub_8223FCA0(ctx, base);
}
REX_EXTERN(__imp__sub_82248840);
REX_HOOK_RAW(sub_82248840) {
  if (VideoNative_DrawVertices(ctx, base)) return;
  __imp__sub_82248840(ctx, base);
}
REX_EXTERN(__imp__sub_82248C58);
REX_HOOK_RAW(sub_82248C58) {
  if (VideoNative_DrawIndexedVertices(ctx, base)) return;
  __imp__sub_82248C58(ctx, base);
}
REX_EXTERN(__imp__sub_82248218);
REX_HOOK_RAW(sub_82248218) {
  if (VideoNative_DrawVerticesUP(ctx, base)) return;
  __imp__sub_82248218(ctx, base);
}
REX_EXTERN(__imp__sub_822487A0);
REX_HOOK_RAW(sub_822487A0) {
  if (VideoNative_DrawIndexedVerticesUP(ctx, base)) return;
  __imp__sub_822487A0(ctx, base);
}
REX_EXTERN(__imp__sub_8224D488);
REX_HOOK_RAW(sub_8224D488) {
  if (VideoNative_BlockOnFence(ctx, base)) return;
  __imp__sub_8224D488(ctx, base);
}
REX_EXTERN(__imp__sub_8294C7C0);
REX_HOOK_RAW(sub_8294C7C0) {
  CC1_LightCamFillForce(ctx, base);
  __imp__sub_8294C7C0(ctx, base);
}
REX_EXTERN(__imp__sub_8294A3D0);
REX_HOOK_RAW(sub_8294A3D0) {
  CC1_CasterMaskForce(ctx, base);
  __imp__sub_8294A3D0(ctx, base);
}
REX_EXTERN(__imp__sub_8294AA78);
REX_HOOK_RAW(sub_8294AA78) {
  CC1_CasterMaskForce(ctx, base);
  __imp__sub_8294AA78(ctx, base);
}
REX_EXTERN(__imp__sub_8294AD98);
REX_HOOK_RAW(sub_8294AD98) {
  CC1_CasterMaskForce(ctx, base);
  __imp__sub_8294AD98(ctx, base);
}
REX_EXTERN(__imp__sub_8294B098);
REX_HOOK_RAW(sub_8294B098) {
  CC1_CasterMaskForce(ctx, base);
  __imp__sub_8294B098(ctx, base);
}

// ---- Mid-function hooks (cc1_manifest.toml) ------------------------
void CC1_SkipDemoFence() { CC1_SkipDemoFence(GuestContext(), GuestBase(), GuestContext().r31); }  // 829AB900
void CC1_StartTexRenderFailA() { CC1_StartTexRenderFailA(GuestContext(), GuestBase()); }  // 828BAF34
void CC1_StartTexRenderFailB() { CC1_StartTexRenderFailB(GuestContext(), GuestBase()); }  // 828BAF74
void CC1_StartTexRenderFailC() { CC1_StartTexRenderFailC(GuestContext(), GuestBase()); }  // 828BAFA8
void CC1_RenderPassedProbe() { CC1_RenderPassedProbe(GuestContext(), GuestBase()); }  // 82969380
void CC1_PassLoopTop() { CC1_PassLoopTop(GuestContext(), GuestBase(), GuestContext().r31); }  // 82968C14
void CC1_PassGateVerdict() { CC1_PassGateVerdict(GuestContext(), GuestBase(), GuestContext().r31); }  // 82968CE8
void CC1_ShadowGateProbe() { CC1_ShadowGateProbe(GuestContext(), GuestBase()); }  // 828B9410
void CC1_UnlinkGuard() { CC1_UnlinkGuard(GuestContext(), GuestBase()); }  // 82269778
void CC1_ShadowBeginProbe() { CC1_ShadowBeginProbe(GuestContext(), GuestBase()); }  // 828B9500
void CC1_ShadowCasterDrawProbe() { CC1_ShadowCasterDrawProbe(GuestContext(), GuestBase()); }  // 828B95FC
void CC1_ShadowEndProbe() { CC1_ShadowEndProbe(GuestContext(), GuestBase()); }  // 828B961C
