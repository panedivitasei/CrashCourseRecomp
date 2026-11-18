// rexvideonative — render-thread offload queue (GPL-3.0, see LICENSE).
// Guards at the head of each mutating renderer::* entry point enqueue records
// that snapshot call-time-volatile guest data (constants, state, texture-header
// dwords, UP draw bytes, a device-block window), and a worker thread re-enters
// the same renderer functions with a thread-local pass-through flag.

#pragma once

#include <atomic>
#include <cstdint>

namespace rex::videonative {
struct ResolvedShader;
}

namespace rex::videonative::rq {

// Size of the device-block snapshot window that LoadGuestU32/GuestPtr in
// renderer.cpp consult on the worker, which must span every draw-path device
// read (up to dev+12308, the raw z-enable request SetupDraw recomposes).
constexpr uint32_t kWinSize = 16384;

// True on the render worker thread and false on the guest thread, which alone
// mutates live guest memory.
bool OnWorker();
struct Win {
  const uint8_t* host = nullptr;
  uint32_t base = 0;
  uint32_t size = 0;
};
constexpr int kMaxWins = 24;
extern thread_local Win t_win[kMaxWins];

// True when the calling thread should enqueue instead of executing (queue
// running, cvar on, and not the worker itself).
bool Active();
// True when the worker is running at all (either thread).
bool Running();

// Spawns the worker once renderer::Init succeeds, a no-op when the
// gpu_render_thread cvar is off.
void Start();

// Flushes the queue, then stops and joins the worker; safe when never started.
void StopAndJoin();

// --- enqueue API (called from the renderer guards; one per entry point) ---
void EnqSetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                    float min_z, float max_z);
void EnqSetScissor(int32_t l, int32_t t, int32_t r, int32_t b);
void EnqSetTexture(uint32_t sampler, uint32_t header, const uint32_t dw[6]);
void EnqSetStream(uint32_t stream, uint32_t vb, uint32_t off, uint32_t stride);
void EnqSetIndices(uint32_t ib);
void EnqSetShaderConstantsF(bool pixel, uint32_t start_reg,
                            uint32_t guest_data, uint32_t vec4_count);
void EnqSetShaderConstantsFHost(bool pixel, uint32_t start_reg,
                                const uint32_t* host_dwords,
                                uint32_t vec4_count);
void EnqPatchShaderFloatDwordsHost(bool pixel, uint32_t dword_index,
                                   const uint32_t* host_dwords,
                                   uint32_t count);
void EnqMergeShaderFloatDwordHost(bool pixel, uint32_t dword_index,
                                  uint32_t mask, uint32_t orv);
void EnqClear(uint32_t flags, uint32_t color, float z, const int32_t* rects,
              uint32_t rect_count);
void EnqRegisterSurface(uint32_t obj, uint32_t w, uint32_t h, uint32_t fmt);
void EnqSetRenderTargetSurface(uint32_t index, uint32_t obj);
void EnqSetDepthSurface(uint32_t obj);
void EnqSetTilingExtent(uint32_t w, uint32_t h);
void EnqInvalidateTexture(uint32_t header_addr);
void EnqNoteRtt(uint32_t texbase, uint32_t w, uint32_t h, uint32_t color_surf,
                uint32_t depth_surf, uint32_t dest_tex, uint32_t format,
                uint32_t msaa, uint32_t tiling);
void EnqResolveToTexture(uint32_t dest, uint32_t source, bool has_rect,
                         int32_t l, int32_t t, int32_t r, int32_t b,
                         int32_t dx, int32_t dy);
void EnqResolveDepthToTexture(uint32_t dest, bool has_rect, int32_t l,
                              int32_t t, int32_t r, int32_t b, int32_t dx,
                              int32_t dy);
void EnqSwapFrontbuffer(uint32_t header);
void EnqEndFrameAndPresent();  // includes the back-pressure wait
void EnqUpdateGammaRamp(const uint16_t* rgb768);
// Runs FlushResolveWritebacksInline on the worker and returns a ticket, the
// flush being complete (guest memory holds the bytes) once WbFlushDone() >=
// ticket.
uint64_t EnqFlushResolveWriteback();
uint64_t WbFlushDone();
void EnqPushState();
void EnqPopState();
void EnqSeedReplayFloatConstants(uint32_t device, uint32_t recording_device);
void EnqSetReplayDrawState(const uint32_t* state10, uint32_t dirty_mask);
void EnqSetReplayBoolConstants(const uint32_t* dwords40);
void EnqSetReplayFetchConstants(const uint32_t* records, uint32_t count);
void EnqApplyReplayStatePersistent(const uint32_t* state10,
                                   uint32_t dirty_mask);
void EnqApplyBlendControlDirect(uint32_t rt, uint32_t value);
void EnqApplyLiveStateDirty(uint32_t consumed_mask, const uint32_t vals[5]);
void EnqDrawVertices(uint32_t device, uint32_t prim, uint32_t start_vertex,
                     uint32_t count);
void EnqDrawIndexedVertices(uint32_t device, uint32_t prim,
                            uint32_t base_vertex, uint32_t start_index,
                            uint32_t index_count);
void EnqDrawVerticesUP(uint32_t device, uint32_t prim, uint32_t vertex_count,
                       uint32_t vertex_data, uint32_t stride);
void EnqDrawIndexedVerticesUP(uint32_t device, uint32_t prim, uint32_t min_v,
                              uint32_t num_v, uint32_t index_count,
                              uint32_t index_data, uint32_t index_format,
                              uint32_t vertex_data, uint32_t stride);
// Guest-side pending UP override (replay path), consumed by the next UP draw
// enqueue instead of reading guest memory.
void SetPendingUPOverride(const void* vertex_data, const void* index_data);

}  // namespace rex::videonative::rq
