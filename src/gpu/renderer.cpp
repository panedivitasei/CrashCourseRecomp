// rexvideonative renderer — see renderer.h. GPL-3.0 — see LICENSE.

// Root layout per docs/native_shaders.md: roots 0..4 are b0..b4 space0, root 5
// is the shared-memory buffer at t0, root 6 a dummy UAV at u0, set 0 the
// bindless samplers and sets 1..3 the bindless textures in space1/2/3.

#include "renderer.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>

#include "renderer_prover_vs.h"
#include "shader_cache.h"
#include <filesystem>
#include <map>
#include <memory>
#include <deque>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif


#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/thread.h>
#include <rex/ui/renderdoc_api.h>
#include <rex/ui/window.h>

#include <plume_render_interface.h>
#include <plume_render_interface_builders.h>

#include "overlay.h"
#include "renderer_fps.h"
#include "renderer_overlay_shaders.h"
#include "renderer_easu_ps.h"
#include "renderer_depth_ps.h"
#include "renderer_fps_ps.h"
#include "renderer_rcas_ps.h"
#include "render_queue.h"
#include "semantics/sampler_semantics.h"
#include "shader_cache.h"
#include "video_native_internal.h"

REXCVAR_DEFINE_INT32(gpu_capture_frame, 0, "Diagnostics",
                     "Trigger a RenderDoc capture at this native-layer frame "
                     "index (0 = off; requires launching under RenderDoc)");
REXCVAR_DEFINE_INT32(gpu_capture_on_draws, 0, "Diagnostics",
                     "Triggers one RenderDoc capture at the first frame with "
                     "at least this many draws (0 = off).");
REXCVAR_DEFINE_INT32(gpu_fps_cap, 0, "GPU",
                     "Caps the frame rate in frames per second (0 = no cap), paced by V-Sync when it "
                     "is on and the cap is the display's refresh rate or a whole fraction of it.");
REXCVAR_DEFINE_BOOL(gpu_vsync, true, "GPU",
                    "Presents in step with the display's refresh (V-Sync); off "
                    "presents immediately with tearing allowed and gpu_fps_cap pacing "
                    "the frames.");
REXCVAR_DEFINE_INT32(gpu_resolution_scale, 100, "GPU",
                     "Internal resolution as a percent of the game's own 720p, 50-200 "
                     "in steps of 25, read at launch.");
REXCVAR_DEFINE_BOOL(performance_mode, false, "GPU",
                    "Low-end hardware mode: render at a reduced internal resolution "
                    "(gpu_perf_scale) and upscale to the window.");
REXCVAR_DECLARE(bool, gpu_defaults_win);  // defined in video_native.cpp
REXCVAR_DEFINE_BOOL(gpu_tex_swizzle_full, true, "GPU",
                    "Composes fetch-constant destination swizzles through "
                    "the format's host base swizzle with constants honored; "
                    "off applies the pure-reorder policy of gpu_tex_swizzle.");
REXCVAR_DEFINE_BOOL(gpu_tex_swizzle, true, "GPU",
                    "Honors pure channel-reorder destination swizzles while "
                    "swizzles with forced-0/1 selectors stay identity; off "
                    "keeps every swizzle identity.");
REXCVAR_DEFINE_BOOL(gpu_shadow_flip, false, "GPU",
                    "Renders depth-only (shadow map) passes flipped in Y; "
                    "the shadowflip_toggle file flips the setting while the "
                    "game runs.");
REXCVAR_DEFINE_BOOL(gpu_tex_reupload, false, "GPU",
                    "Re-uploads a cached guest texture when a sparse hash "
                    "check finds its texels changed, at most twice and "
                    "within gpu_tex_heal_window frames of its creation.");
REXCVAR_DEFINE_BOOL(gpu_deferred_vb_copy, true, "GPU",
                    "Copies guest vertex and index ranges into the upload "
                    "rings at present time instead of draw time; UP draws "
                    "stay call-time copies.");
REXCVAR_DEFINE_BOOL(gpu_rect_gs_off, false, "Diagnostics",
                    "Skips the RECTLIST expansion geometry shader, which "
                    "leaves rects rendered as half-quads, to isolate the "
                    "cost of that stage.");
REXCVAR_DEFINE_BOOL(gpu_perfui, true, "Diagnostics",
                    "With performance_mode, renders the composite and UI "
                    "surface at full resolution while scene passes keep the "
                    "downscale; off downscales the whole frame, HUD included.");
REXCVAR_DEFINE_BOOL(gpu_perfui_easu, true, "Diagnostics",
                    "With performance_mode, upscales the half-res scene in "
                    "the seed blit with FSR 1.0 EASU; off uses a bilinear "
                    "stretch.");
REXCVAR_DEFINE_STRING(gpu_perf_scale, "2/3", "Diagnostics",
                      "Internal scale of performance_mode as \"num/den\" "
                      "with num < den and den <= 4, such as \"2/3\" or "
                      "\"1/2\".");
REXCVAR_DEFINE_INT32(gpu_tex_evict_frames, 900, "Diagnostics",
                     "Texture-cache entries bound within this many frames "
                     "are never evicted, even at the cache watermark.");
REXCVAR_DEFINE_INT32(gpu_tex_cache_max, 1500, "Diagnostics",
                     "Texture-cache watermark in entries, above which the "
                     "least recently used entries retire and their "
                     "descriptor slots recycle (0 = no limit).");
REXCVAR_DEFINE_INT32(gpu_tex_cache_mb, 320, "Diagnostics",
                     "Texture-cache VRAM watermark in approximate MB, above "
                     "which the coldest entries retire (0 = no limit).");
REXCVAR_DEFINE_INT32(gpu_rt_cache_mb, 384, "Diagnostics",
                     "Render-target cache VRAM watermark in approximate MB, "
                     "above which cold render-target sets other than the "
                     "active and last-drawn ones retire (0 = no limit).");
REXCVAR_DEFINE_BOOL(gpu_resolve_lazy_verify, false, "Diagnostics",
                    "Cross-checks the renderer's tracked render-target and "
                    "destination layouts against plume's barrier ledger at "
                    "every lazy skip and bind or repair decision, logs "
                    "divergences and emits the skipped barrier.");
REXCVAR_DEFINE_BOOL(gpu_resolve_writeback, false, "GPU",
                    "Writes color resolve results back into guest memory at "
                    "the guest's resolve fence waits, for the lighting "
                    "canvas the engine reads on the CPU.");
REXCVAR_DEFINE_BOOL(gpu_wb_skip_sampled, true, "GPU",
                    "Skips resolve write-back for destinations the GPU "
                    "samples; off writes back every eligible resolve.");
REXCVAR_DEFINE_BOOL(gpu_rt_key_rbinfo, false, "GPU",
                    "Folds the guest device's RB surface-info shadow words "
                    "(dev+4*(2593+slot)) into the render-target key, which "
                    "separates passes that retarget EDRAM through the raw "
                    "register shadows.");
REXCVAR_DEFINE_BOOL(gpu_resolve_lazy, true, "GPU",
                    "Skips the source render target's transition back to "
                    "render state after a resolve unless it is the active "
                    "target, and skips entry transitions the tracked layout "
                    "marks redundant.");
REXCVAR_DEFINE_INT32(gpu_vs_trim, 2, "GPU",
                     "Trimmed-signature vertex shader variants: 0 = off, "
                     "1 = on, 2 = Intel adapters only.");
REXCVAR_DEFINE_BOOL(gpu_vb_gpu_local, true, "GPU",
                    "Keeps the vertex ring in GPU-local memory with staged "
                    "copies instead of a CPU-mapped upload heap, effective "
                    "only under the render queue.");
REXCVAR_DEFINE_BOOL(gpu_vb_persist, true, "GPU",
                    "Caches static stream vertex-buffer uploads in a "
                    "persistent region of the vertex ring for reuse across "
                    "frames; off uploads them every frame.");
REXCVAR_DEFINE_BOOL(gpu_write_exp_bias, true, "GPU",
                    "Scales shader outputs by 2^bias from RB_COLOR_INFO's "
                    "color exponent bias when writing to biased render "
                    "targets.");
REXCVAR_DEFINE_BOOL(gpu_capture_on_dres, false, "Diagnostics",
                    "Triggers one RenderDoc capture when a depth resolve at "
                    "least 2048 wide fires.");
REXCVAR_DEFINE_BOOL(gpu_per_axis_clamp, true, "GPU",
                    "Honors the fetch constant's clamp_x and clamp_y "
                    "independently through per-axis samplers (indices 4-12); "
                    "off honors clamp_x only.");
REXCVAR_DEFINE_BOOL(dcc2_fx_alpha_pop_test, false, "Diagnostics",
                    "Zeroes every vertex whose packed color alpha byte is "
                    "0xFF in small UP FX batches (stride 24, at most 256 "
                    "vertices), a test that also dims legitimate full-alpha "
                    "quads.");
REXCVAR_DEFINE_BOOL(gpu_resolve_redirect, true, "GPU",
                    "Serves host-rendered resolve destinations to texture "
                    "binds that match their base address; off samples the "
                    "guest-memory upload.");
REXCVAR_DEFINE_INT32(gpu_redirect_min_dim, 0, "GPU",
                     "Minimum resolved-entry width for the base-address "
                     "redirect to apply (0 = redirect everything).");
REXCVAR_DEFINE_INT32(gpu_tex_idle_check, 120, "GPU",
                     "A cached guest texture not bound for this many frames "
                     "has its guest texels sampled again at its next bind "
                     "and is uploaded afresh when they hold another image "
                     "(0 = never).");
REXCVAR_DEFINE_INT32(gpu_tex_heal_window, 2700, "GPU",
                     "Frames after a texture-cache entry's creation during "
                     "which the content re-checks of gpu_tex_reupload run "
                     "(0 = none).");
REXCVAR_DEFINE_BOOL(gpu_shadow_depth_half, false, "GPU",
                    "Stores depth-only (shadow map) passes at half depth "
                    "range, the guest float24 EDRAM convention; the "
                    "shadowhalf_toggle file flips the setting while the game "
                    "runs.");
REXCVAR_DEFINE_BOOL(gpu_cull_flip, false, "GPU",
                    "Inverts the front-face winding sense for all native "
                    "pipelines.");
REXCVAR_DEFINE_BOOL(gpu_vp_grow, true, "GPU",
                    "Grows a tile-height host render target to the height of "
                    "a draw viewport that exceeds it and suppresses the "
                    "guest's per-tile EDRAM clears on grown targets.");
REXCVAR_DEFINE_BOOL(gpu_breadcrumbs, false, "Diagnostics",
                    "Writes a GPU progress marker after every draw so that a "
                    "hang report names the draw the GPU stopped at, at some "
                    "cost in pipelining.");
REXCVAR_DEFINE_BOOL(gpu_stretch, false, "GPU",
                    "Stretches the game image to fill the window; off keeps "
                    "the 16:9 aspect with black bars.");
REXCVAR_DEFINE_BOOL(gpu_b1_dedup, true, "GPU",
                    "Skips the per-draw 8 KB float-constant upload when the "
                    "source bytes are unchanged within the frame; off "
                    "uploads at every draw.");
REXCVAR_DEFINE_STRING(gpu_watch_vs, "", "Diagnostics",
                      "Captures the per-frame input state (fetch constants, "
                      "float file hashes, index stats) of draws whose VS "
                      "ucode hash matches this hex value, kept in a ring of "
                      "8 and dumped with the hang report.");
REXCVAR_DEFINE_STRING(gpu_skip_vs, "", "Diagnostics",
                      "Skips draws whose VS ucode hash matches this hex "
                      "value.");
REXCVAR_DEFINE_INT32(gpu_texture_retire_frames, 8, "GPU",
                     "Frames a retired texture entry stays alive before "
                     "destruction.");
REXCVAR_DEFINE_BOOL(gpu_bind_scissor_reset, true, "GPU",
                    "A render-target change resets the scissor to the full "
                    "target unless the guest set one after the bind, as the "
                    "XDK window scissor does.");
REXCVAR_DEFINE_BOOL(gpu_gamma_ramp, true, "GPU",
                    "Applies the game's display gamma ramp "
                    "(D3DDevice_SetGammaRamp, the 256-entry D3DGAMMARAMP in "
                    "the device block) in the native present blit.");

namespace rex::videonative::renderer {

#ifndef CC1_SHADER_PROVER
#define CC1_SHADER_PROVER 0
#endif

using namespace plume;

namespace {

// Reads the SDK's resolution_scale cvar by name since its storage is not
// exported.
int32_t SdkResolutionScale() { return rex::cvar::Query<int32_t>("resolution_scale"); }

// Frame-cap sleep on a high-resolution waitable timer, since plain Sleep() is
// quantized to the ~15.6 ms system tick; short or timer-less waits yield.
void SleepPrecise(std::chrono::microseconds duration) {
#ifdef _WIN32
  struct Timer {
    HANDLE handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
    ~Timer() {
      if (handle) CloseHandle(handle);
    }
  };
  thread_local Timer timer;
  if (duration.count() >= 100 && timer.handle) {
    LARGE_INTEGER due;
    due.QuadPart = -static_cast<LONGLONG>(duration.count()) * 10;  // relative, 100 ns units
    if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer.handle, INFINITE) == WAIT_OBJECT_0) {
      return;
    }
  }
#endif
  rex::thread::MaybeYield();
}

// Internal resolution scale as a rational num/den, latched at first use and
// applied only at the D3D boundary while all bookkeeping stays in guest units.
uint32_t g_hsNum = 1, g_hsDen = 1;
void LatchHostScale() {
  static bool latched = false;
  if (latched) return;
  latched = true;
  const int32_t v = SdkResolutionScale();
  // The settings page's Resolution Scale is 50-200% in steps of 25, which keeps
  // both terms of the ratio within HostScaleCode's nibbles.
  const int32_t percent = REXCVAR_GET(gpu_resolution_scale);
  if (percent != 100 && percent >= 50 && percent <= 200 && percent % 25 == 0) {
    g_hsNum = uint32_t(percent) / 25;
    g_hsDen = 4;
    while (g_hsNum % 2 == 0 && g_hsDen % 2 == 0) {
      g_hsNum /= 2;
      g_hsDen /= 2;
    }
    REXGPU_INFO("videonative: internal resolution {}% ({}/{})", percent, g_hsNum, g_hsDen);
  } else if (v == 2) {
    g_hsNum = 2;
    REXGPU_INFO(
        "videonative: internal resolution 2x (720p content rendered at "
        "1440p-class)");
  } else if (REXCVAR_GET(performance_mode)) {
    // An explicit resolution_scale=2 wins over the mode; the ratio comes from
    // gpu_perf_scale and falls back to 2/3 on malformed values.
    uint32_t num = 2, den = 3;
    {
      const std::string s = REXCVAR_GET(gpu_perf_scale);
      uint32_t n = 0, d = 0;
      if (std::sscanf(s.c_str(), "%u/%u", &n, &d) == 2 && n >= 1 && d > n &&
          d <= 4) {
        num = n;
        den = d;
      }
    }
    g_hsNum = num;
    g_hsDen = den;
    REXGPU_INFO(
        "videonative: performance mode: {}/{} internal resolution "
        "(presented at window size)",
        num, den);
  } else if (v != 1 && v != 0) {
    REXGPU_WARN(
        "videonative: resolution_scale {} unsupported on the native "
        "path (1 or 2) - using 1",
        v);
  }
}
bool Scaled() {
  LatchHostScale();
  return g_hsNum != g_hsDen;
}
// Allocation dims round up so a surface is never smaller than any scaled rect
// laid inside it.
uint32_t HostDim(uint32_t x) {
  LatchHostScale();
  return std::max(1u, (x * g_hsNum + g_hsDen - 1) / g_hsDen);
}
// Rect corners and copy offsets round down so a shared guest edge maps to one
// host value and adjacent tile bands neither gap nor overlap.
int32_t HostCoord(int32_t x) {
  LatchHostScale();
  return int32_t((int64_t(x) * g_hsNum) / g_hsDen);
}
float HostScaleF() {
  LatchHostScale();
  return float(g_hsNum) / float(g_hsDen);
}
// One-byte scale identity (num << 4 | den) for cached-entry compatibility
// checks.
uint8_t HostScaleCode() {
  LatchHostScale();
  return uint8_t((g_hsNum << 4) | g_hsDen);
}

constexpr uint32_t kNumFrames = 2;
constexpr uint32_t kSwapChainBufferCount = 3;
constexpr RenderFormat kColorFormat = RenderFormat::B8G8R8A8_UNORM;
constexpr RenderFormat kDepthFormat = RenderFormat::D32_FLOAT;
// Every shader pack PS clamps its descriptor indices with a baked min(idx,
// 2047u), so this count cannot change without regenerating all packs.
constexpr uint32_t kTextureDescriptorCount = 2048;
// Reserved dimension-matched blank descriptors, since a view dimension that
// mismatches the shader's declared type is D3D12 UB: slots 0 and 2047 are 2D,
// 2046 is cube, 2045 is 3D, and allocation runs 1..2044.
constexpr uint32_t kBlank3dDescriptor = kTextureDescriptorCount - 3;
constexpr uint32_t kBlankCubeDescriptor = kTextureDescriptorCount - 2;
constexpr uint32_t kFirstReservedDescriptor = kTextureDescriptorCount - 3;
constexpr uint32_t kSamplerDescriptorCount = 16;

// Ring-contract sampler decode (semantics/sampler_semantics.*), with the cvars
// defined here so every REXCVAR_GET stays in this TU.
REXCVAR_DEFINE_BOOL(gpu_sem_sampler, true, "GPU",
                    "Decodes samplers in full: filters, anisotropy, per-axis "
                    "clamp with mirror-once and border, and cube and 1D "
                    "dimension rules; off picks by the clamp bit only.");

// Engine-authoritative RTT pass targets: the title's StartTextureRendering hook
// reports the TEXTUREBASEDX9 being rendered into, and host RT identity and
// dimensions come from it instead of EDRAM inference.
REXCVAR_DEFINE_BOOL(gpu_sem_rtt, true, "GPU",
                    "Takes host render-target identity and dimensions for "
                    "render-to-texture passes from the title's "
                    "TEXTUREBASEDX9 note; off infers them from EDRAM state "
                    "only.");

// Depth resolves through a fullscreen-triangle render (depth_resolve.ps.hlsl)
// instead of the whole-plane staging copy D3D12 requires for partial depth
// copies.
REXCVAR_DEFINE_BOOL(gpu_depth_resolve_shader, false, "GPU",
                    "Resolves depth by rendering a depth SRV into the "
                    "destination; off uses the whole-plane staging copy.");
REXCVAR_DEFINE_BOOL(gpu_untile_packed, true, "GPU",
                    "Decodes sub-tile tiled textures from their "
                    "packed-mip-tail offset; off decodes from the tile "
                    "origin.");

// The guest's CPU-side lighting bake reads the shadow depth resolves, so they
// are written back to guest RAM.
REXCVAR_DEFINE_BOOL(gpu_depth_writeback, true, "GPU",
                    "Writes depth resolve results back into guest memory "
                    "(D32 host to D24S8 guest), where the lighting bake "
                    "reads the shadow map.");

// Synchronous write-back stalls the guest thread on a fence wait per resolve,
// so delivery is deferred by default from the content snapshotted at resolve
// time.
REXCVAR_DEFINE_BOOL(gpu_writeback_sync, false, "GPU",
                    "Delivers resolve write-back synchronously at each "
                    "resolve, stalling the guest thread; off delivers at "
                    "frame end.");

// Host RTs are keyed by the full (SURF, C0-C3, DEPTH) register tuple so
// distinct guest passes do not collapse onto one RT set.
REXCVAR_DEFINE_BOOL(gpu_pass_separation, true, "GPU",
                    "Keys host render targets by the full color0-3 and depth "
                    "surface tuple; off keys by slot 0 only.");

// A/B lever only, default off: CC1 uses no gamma formats, so PWL-encoding the
// write-back corrupts bytes the game expects linear.
REXCVAR_DEFINE_BOOL(gpu_gamma_writeback, false, "GPU",
                    "PWL-encodes color write-back bytes.");
// Hong Kong's streamed sections upload 31-35 MB of stream VBs per frame, so the
// ring is 128 MB and the allocator fails instead of wrapping.
constexpr uint32_t kUploadBufferSize = 128 * 1024 * 1024;

// Root descriptor ordinals, in addRootDescriptor order in Init.
constexpr uint32_t kRootB0System = 0;
constexpr uint32_t kRootB1FloatsVs = 1;
constexpr uint32_t kRootB1FloatsPs = 2;
constexpr uint32_t kRootB2BoolLoop = 3;
constexpr uint32_t kRootB3Fetch = 4;
constexpr uint32_t kRootB4IndicesVs = 5;
constexpr uint32_t kRootB4IndicesPs = 6;
constexpr uint32_t kRootSharedMemSrv = 7;
constexpr uint32_t kRootDummyUav = 8;

// ---- Device / frame objects ----

RenderDevice* g_device;
RenderCommandQueue* g_queue;

std::unique_ptr<RenderSwapChain> g_swapChain;
bool g_swapChainValid = false;
uint32_t g_backBufferIndex = 0;
uint32_t g_frame = 0;
bool g_frameOpen = false;
std::unique_ptr<RenderCommandList> g_commandLists[kNumFrames];
std::unique_ptr<RenderCommandFence> g_commandFences[kNumFrames];
bool g_commandListPending[kNumFrames] = {};
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphores[kNumFrames];
std::unique_ptr<RenderCommandSemaphore> g_renderSemaphores[kNumFrames];
std::unique_ptr<RenderTexture> g_depthTexture;
std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;

std::unique_ptr<RenderPipelineLayout> g_pipelineLayout;
std::unique_ptr<RenderDescriptorSet> g_textureDescriptorSet;
std::unique_ptr<RenderDescriptorSet> g_samplerDescriptorSet;
std::unique_ptr<RenderBuffer> g_dummyUav;
std::unique_ptr<RenderTexture> g_blankTexture;
std::unique_ptr<RenderTextureView> g_blankTextureView;
std::unique_ptr<RenderTexture> g_blankTextureCube;
std::unique_ptr<RenderTextureView> g_blankTextureCubeView;
std::unique_ptr<RenderTexture> g_blankTexture3d;
std::unique_ptr<RenderTextureView> g_blankTexture3dView;
std::vector<std::unique_ptr<RenderSampler>> g_samplers;

uint64_t g_frameIndex = 0;
uint64_t g_drawsThisFrame = 0;
// Set at device init from the adapter description for vendor-gated features.
bool g_adapterIsIntel = false;

// ---- Upload ring (per frame), simplified UnleashedRecomp UploadAllocator ----

struct UploadAllocation {
  const RenderBuffer* buffer;
  uint64_t offset;
  uint8_t* memory;
};

struct UploadAllocator {
  struct Page {
    std::unique_ptr<RenderBuffer> buffer;
    uint8_t* memory = nullptr;
  };
  std::vector<Page> pages;
  uint32_t index = 0;
  uint32_t offset = 0;

  UploadAllocation Allocate(uint32_t size, uint32_t alignment) {
    assert(size <= kUploadBufferSize);
    offset = (offset + alignment - 1) & ~(alignment - 1);
    if (offset + size > kUploadBufferSize) {
      ++index;
      offset = 0;
    }
    if (pages.size() <= index) pages.resize(index + 1);
    Page& page = pages[index];
    if (!page.buffer) {
      page.buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(
          kUploadBufferSize, RenderBufferFlag::CONSTANT |
                                 RenderBufferFlag::VERTEX |
                                 RenderBufferFlag::INDEX |
                                 RenderBufferFlag::FORMATTED));
      page.memory = reinterpret_cast<uint8_t*>(page.buffer->map());
      page.buffer->setName(fmt::format("upload page {}", index));
      // The [gpuva] census line lets a DRED page-fault VA be mapped to its
      // buffer and offset.
      REXGPU_INFO("videonative: [gpuva] upload page {} base={:#x}+{:#x}",
                  index, page.buffer->getDeviceAddress(), kUploadBufferSize);
    }
    UploadAllocation result{page.buffer.get(), offset, page.memory + offset};
    offset += size;
    return result;
  }

  void Reset() {
    index = 0;
    offset = 0;
  }
};

UploadAllocator g_upload[kNumFrames];

// Vertex ring behind the shared-memory SRV: a persistent region holds static
// stream VBs at the same offset in every frame's buffer, per-frame allocations
// sit above it, and in GPU-local mode writes land in a staging ring and are
// copied into a DEFAULT-heap buffer.
constexpr uint32_t kVbStagingSize = 32 * 1024 * 1024;
bool VbGpuLocal();  // latched below (needs rq::Running at first frame)
struct VertexRing {
  std::unique_ptr<RenderBuffer> buffer;
  std::unique_ptr<RenderBuffer> staging;
  uint8_t* memory = nullptr;          // mapped upload memory (upload mode)
  uint8_t* staging_memory = nullptr;  // mapped staging memory (gpu mode)
  uint32_t offset = 0;
  uint32_t staging_offset = 0;
  bool gpu_local = false;
  bool in_copy_state = false;  // barrier bookkeeping for the DEFAULT buffer

  void EnsureBuffer() {
    if (buffer) return;
    gpu_local = VbGpuLocal();
    if (gpu_local) {
      buffer = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(
          kUploadBufferSize, RenderBufferFlag::FORMATTED));
      buffer->setName("vertex ring (gpu)");
      staging = g_device->createBuffer(
          RenderBufferDesc::UploadBuffer(kVbStagingSize));
      staging_memory = reinterpret_cast<uint8_t*>(staging->map());
      staging->setName("vertex staging");
    } else {
      buffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(
          kUploadBufferSize, RenderBufferFlag::FORMATTED));
      memory = reinterpret_cast<uint8_t*>(buffer->map());
      buffer->setName("vertex ring");
    }
    REXGPU_INFO("videonative: [gpuva] vertex ring base={:#x}+{:#x} gpu={}",
                buffer->getDeviceAddress(), kUploadBufferSize,
                gpu_local ? 1 : 0);
  }

  // Reserves `size` bytes of ring address space; in gpu mode the returned
  // pointer is staging memory and the caller finishes with CommitCopy().
  UploadAllocation Allocate(uint32_t size) {
    // An allocation larger than the buffer is refused since the caller memcpys
    // `size` bytes.
    if (size > kUploadBufferSize) {
      return UploadAllocation{nullptr, 0, nullptr};
    }
    EnsureBuffer();
    offset = (offset + 15) & ~15u;
    if (offset + size > kUploadBufferSize) {
      // The ring never wraps because earlier allocations are still referenced
      // by this frame's draws, so exhaustion fails and the caller drops the
      // draw.
      static uint64_t overflow_logs = 0;
      if ((overflow_logs++ & 63) == 0) {
        REXGPU_ERROR(
            "videonative: vertex ring exhausted ({} + {} > {}), draw dropped "
            "({} so far this session)",
            offset, size, kUploadBufferSize, overflow_logs);
      }
      return UploadAllocation{nullptr, 0, nullptr};
    }
    if (!gpu_local) {
      UploadAllocation result{buffer.get(), offset, memory + offset};
      offset += size;
      return result;
    }
    staging_offset = (staging_offset + 15) & ~15u;
    if (staging_offset + size > kVbStagingSize) {
      static uint64_t staging_logs = 0;
      if ((staging_logs++ & 63) == 0) {
        REXGPU_ERROR(
            "videonative: vertex staging exhausted ({} + {} > {}), draw "
            "dropped ({} so far this session)",
            staging_offset, size, kVbStagingSize, staging_logs);
      }
      return UploadAllocation{nullptr, 0, nullptr};
    }
    UploadAllocation result{buffer.get(), offset,
                            staging_memory + staging_offset};
    offset += size;
    staging_offset += size;
    return result;
  }

  // In gpu mode, records the staging-to-buffer copy for an allocation; called
  // after the write and before the draw that fetches from it.
  void CommitCopy(RenderCommandList* cl, uint32_t dest_offset,
                  uint32_t src_staging_end, uint32_t size) {
    if (!gpu_local || !size) return;
    ToCopyState(cl);
    cl->copyBufferRegion(buffer->at(dest_offset),
                         staging->at(src_staging_end - size), size);
  }

  void ToCopyState(RenderCommandList* cl) {
    if (in_copy_state) return;
    cl->barriers(RenderBarrierStage::COPY,
                 RenderBufferBarrier(buffer.get(), RenderBufferAccess::WRITE));
    in_copy_state = true;
  }
  void ToReadState(RenderCommandList* cl) {
    if (!in_copy_state) return;
    cl->barriers(RenderBarrierStage::GRAPHICS,
                 RenderBufferBarrier(buffer.get(), RenderBufferAccess::READ));
    in_copy_state = false;
  }

  void Reset(uint32_t floor) {
    offset = floor;
    staging_offset = 0;
    // Buffers decay across list boundaries, so each frame starts in the read
    // state and the first copy re-emits its write barrier.
    in_copy_state = false;
  }
};

VertexRing g_vertexRing[kNumFrames];

// Latched once at first buffer creation since both parities must agree and
// rq::Running() is stable by the first draw.
bool VbGpuLocal() {
  static const bool latched =
      REXCVAR_GET(gpu_vb_gpu_local) && rq::Running();
  return latched;
}

// Persistent-region state, worker thread only: the region owns [0,
// kVbPersistCap) in every frame's buffer, entries are keyed by BindStreamData's
// content-hash key, and a guest key whose content changes twice is demoted to
// the per-frame ring.
constexpr uint32_t kVbPersistCap = 96 * 1024 * 1024;  // dynamics get 32MB
uint32_t g_vbPersistWatermark = 0;
std::unordered_map<uint64_t, uint32_t> g_vbPersistCache;  // cache_key -> off
struct PersistGuestInfo {
  uint64_t last_key = 0;  // content key of the resident upload
  uint32_t changes = 0;   // content flips seen (2 = demote to per-frame)
  uint32_t size = 0;
};
std::unordered_map<uint64_t, PersistGuestInfo> g_vbPersistByGuest;
uint64_t g_vbPersistOrphans = 0;   // bytes abandoned by content changes
bool g_vbPersistFlushWanted = false;  // region full → flush at next idle

// Allocates `size` bytes at the same offset in every frame's buffer, returning
// 0xFFFFFFFF when the persistent region is full.
uint32_t PersistAlloc(uint32_t size) {
  uint32_t off = (g_vbPersistWatermark + 15) & ~15u;
  if (off + size > kVbPersistCap) {
    g_vbPersistFlushWanted = true;  // reclaim at the next idle frame open
    static uint64_t full_logs = 4;
    if (full_logs) {
      full_logs--;
      REXGPU_WARN(
          "videonative: persistent VB region full ({} + {} > {}) — "
          "flush queued (orphaned {} KB)",
          off, size, kVbPersistCap, g_vbPersistOrphans >> 10);
    }
    return 0xFFFFFFFFu;
  }
  for (uint32_t i = 0; i < kNumFrames; i++) {
    g_vertexRing[i].EnsureBuffer();
  }
  g_vbPersistWatermark = off + size;
  return off;
}

// Per-frame dedupe of vertex-data uploads, keyed (guest data ptr, size) -> ring
// byte offset and cleared at frame open.
std::unordered_map<uint64_t, uint32_t> g_vbUploadCache;
// Per-draw mask of streams whose vfetch pair BindStreamData translated to a
// ring offset.
uint32_t g_drawVfetchWrittenMask = 0;

// ---- Captured guest state ----

struct StreamState {
  uint32_t vb_object = 0;
  uint32_t offset_bytes = 0;
  uint32_t stride_bytes = 0;
};

struct GuestState {
  uint32_t viewport_x = 0, viewport_y = 0, viewport_w = 1280, viewport_h = 720;
  float viewport_min_z = 0.0f, viewport_max_z = 1.0f;
  int32_t scissor[4] = {0, 0, 1280, 720};
  uint32_t textures[32] = {};  // guest texture header addresses by fetch slot
  // Bind-time snapshot of each bound header's 6 fetch dwords, consumed by draws
  // in place of a draw-time re-read (XDK copy semantics, see SetTexture).
  uint32_t texture_dw[32][6] = {};
  StreamState streams[16];
  uint32_t index_buffer_object = 0;
  // Bound guest surface objects (0 = swapchain backbuffer / default depth):
  // slot 0 drives the RT set's dimensions, EDRAM base and depth-only detection,
  // and slots 1-3 contribute their format.
  uint32_t color_surface[4] = {};
  uint32_t depth_surface = 0;
  // Guest float constants, already byte-swapped to host endian.
  float vs_floats[256 * 4] = {};
  float ps_floats[256 * 4] = {};
};

GuestState g_state;

// System constants (b0), laid out as DxbcShaderTranslator::SystemConstants for
// the fields the native pack reads (16 float4 registers = 256 bytes).

// kSysFlag_* bits consumed by the pack (rexglue_shader_common.h).
constexpr uint32_t kSysFlagWNotReciprocal = 1u << 3;
constexpr uint32_t kSysFlagAlphaPassIfLess = 1u << 7;
constexpr uint32_t kSysFlagAlphaPassIfEqual = 1u << 8;
constexpr uint32_t kSysFlagAlphaPassIfGreater = 1u << 9;

struct SystemConstants {
  uint32_t flags;             // c0.x
  uint32_t pad0[2];           // c0.yz
  uint32_t line_loop_index;   // c0.w
  uint32_t vertex_index_endian;  // c1.x
  int32_t vertex_index_offset;   // c1.y
  uint32_t vertex_index_min;     // c1.z
  uint32_t vertex_index_max;     // c1.w
  // c2..c7: enabled user clip planes, compacted, filled from dev+10272+16*i per
  // PA_CL_CLIP_CNTL UCP_ENA bit and consumed by the pack's clip_planes GS.
  float user_clip_planes[6][4];  // c2..c7
  float ndc_scale[3];            // c8.xyz
  uint32_t pad2;                 // c8.w
  float ndc_offset[3];           // c9.xyz
  uint32_t pad3;                 // c9.w
  // c10: point sprite sizing in pixel diameters for the pack's point_expand GS,
  // with xy the PA_SU_POINT_SIZE constant size and zw the PA_SU_POINT_MINMAX
  // clamp for the vertex-exported size.
  float point_constant_diameter[2];   // c10.xy
  float point_vertex_diameter_min;    // c10.z
  float point_vertex_diameter_max;    // c10.w
  uint32_t pad4[4 * 2];          // c11..c12
  uint32_t pad5[3];              // c13.xyz
  float alpha_test_reference;    // c13.w
  uint32_t pad6[4];              // c14
  float color_exp_bias[4];       // c15
};
static_assert(sizeof(SystemConstants) == 256);

// ---- Texture cache ----

struct CachedTexture {
  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderTextureView> view;
  // 0 is the no-slot sentinel; slots 0 and 2047 are reserved blanks so the
  // sentinel and the shader-side clamp both land on a blank descriptor.
  uint32_t descriptor_index = 0;
  uint32_t width = 0, height = 0;
  RenderFormat host_format = RenderFormat::UNKNOWN;
  // View dimension created (guest DataDimension), 1 = 2D-array and 3 = cube; a
  // binding of a different declared dimension gets a blank descriptor instead
  // (see kBlankCubeDescriptor).
  uint8_t view_dim = 1;
  // Host allocation scale of a resolve target (HostScaleCode() at creation);
  // guest-uploaded file textures stay 1:1.
  uint8_t host_scale = 0x11;
  bool valid = false;
  // Content comes from host resolves, not guest memory uploads.
  bool host_rendered = false;
  // Set once a guest texture bind samples this entry through the resolve
  // redirect, so its resolves need no guest-memory write-back
  // (gpu_wb_skip_sampled).
  bool gpu_sampled = false;
  // Framebuffer wrapping this texture as a single R32_FLOAT color target for
  // shader depth resolves; rt_capable records creation with RENDER_TARGET,
  // which plume cannot report back.
  std::unique_ptr<RenderFramebuffer> resolve_fb;
  bool rt_capable = false;
  uint64_t last_resolve_frame = 0;  // 0 = never resolved into
  // Stamped on every cache hit or creation in GetOrCreateTexture; feeds the
  // idle-eviction sweep.
  uint64_t last_bind_frame = 0;
  // Approximate host allocation size, added to g_texCacheBytes on insert and
  // released by RetireEntry.
  uint32_t approx_bytes = 0;
  // Creation-time header identity: a guest-uploaded entry is evicted only when
  // the live header no longer matches, since re-uploading a reachable entry
  // reads guest memory the game may have recycled.
  uint32_t header_addr = 0;
  uint32_t header_dw[6] = {};
  RenderTextureLayout layout = RenderTextureLayout::UNKNOWN;
  // [texdirty] census: sparse content hash of the guest texel data at bind
  // time, for detecting in-place rewrites.
  uint64_t content_hash = 0;
  uint64_t last_content_check = 0;
  uint32_t content_changes = 0;
  // SampleTexels hash of the guest data this entry was uploaded from and the
  // size it covered (0 = not taken), for gpu_tex_idle_check.
  uint64_t sample_hash = 0;
  uint32_t sample_size = 0;
  // Frame this entry was created; content re-checks stop once it outlives
  // gpu_tex_heal_window frames.
  uint64_t created_frame = 0;
};

void* g_inspectHwnd = nullptr;

// Seconds per present under the active frame cap or vsync (0 = unpaced), read
// by PresentInterval.
std::atomic<double> g_presentInterval{0.0};

// Refresh rate of the monitor the game window is on (0 = unknown), queried
// every 2 s so a move to another display is picked up.
uint32_t DisplayRefreshHz() {
  static uint32_t hz = 0;
  static uint64_t last_query = 0;
  const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
  const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
  if (last_query && now - last_query < 2 * freq) return hz;
  last_query = now;
  HMONITOR monitor =
      g_inspectHwnd ? MonitorFromWindow(static_cast<HWND>(g_inspectHwnd), MONITOR_DEFAULTTONEAREST)
                    : MonitorFromWindow(nullptr, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFOEXW info = {};
  info.cbSize = sizeof(info);
  DEVMODEW mode = {};
  mode.dmSize = sizeof(mode);
  if (monitor && GetMonitorInfoW(monitor, &info) &&
      EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    hz = mode.dmDisplayFrequency;
  } else {
    hz = 0;
  }
  return hz;
}
std::unordered_map<uint64_t, CachedTexture> g_textureCache;
uint32_t g_nextTextureDescriptor = 1;  // 0 = blank
// Fallback for the frontbuffer flip (see SwapFrontbuffer): last resolve
// destination by guest texture size, purged when its texture is erased.
std::unordered_map<uint64_t, CachedTexture*> g_lastResolvedBySize;
// Debug: last 10-bit color resolve, last shadow-map depth resolve, and a
// present-blit override cycling through them (file trigger
// "lightaccum_toggle").
CachedTexture* g_lastLightAccumResolve = nullptr;
CachedTexture* g_prevFmt19Resolve = nullptr;  // GBuffer normals resolve
CachedTexture* g_lastShadowMapResolve = nullptr;
uint32_t g_debugShowLightAccum = 0;
// Debug: substitutes an all-white 1x1 R32F texture for the sun shadow map on
// lighting draws (file trigger "shadowwhite_toggle").
bool g_debugWhiteShadow = false;
// Retired texture entries are destroyed a few frames later since in-flight
// command lists may still reference them; the total is capped as a
// descriptor-exhaustion guard.
std::deque<std::pair<uint64_t, CachedTexture>> g_retiredTextures;

// Every CachedTexture destruction goes through RetireEntry so in-flight draws
// never sample a freed texture; g_texCacheBytes totals the approximate bytes of
// live entries.
uint64_t g_texCacheBytes = 0;

void RetireEntry(CachedTexture& e) {
  if (e.texture) {
    g_texCacheBytes -= std::min<uint64_t>(g_texCacheBytes, e.approx_bytes);
    g_retiredTextures.emplace_back(g_frameIndex, std::move(e));
  }
}

// Format-aware bytes/px estimate for the cache watermark.
uint32_t ApproxTexBytes(uint32_t w, uint32_t h, RenderFormat fmt) {
  uint32_t bpp = 4;
  switch (fmt) {
    case RenderFormat::R16G16B16A16_FLOAT:
      bpp = 8;
      break;
    case RenderFormat::BC1_UNORM:
    case RenderFormat::BC4_UNORM:
      bpp = 1;
      break;
    case RenderFormat::BC2_UNORM:
    case RenderFormat::BC3_UNORM:
    case RenderFormat::BC5_UNORM:
      bpp = 2;  // 1 byte/px block + mips headroom
      break;
    default:
      break;
  }
  return w * h * bpp;
}
uint32_t g_retiredTextureTotal = 0;
// Recycled bindless slots, pushed by the retire drain after the slot is
// scrubbed to blank, so the descriptor heap is not exhausted over long play.
std::vector<uint32_t> g_freeDescriptors;

uint32_t AllocTextureDescriptor() {
  if (!g_freeDescriptors.empty()) {
    const uint32_t d = g_freeDescriptors.back();
    g_freeDescriptors.pop_back();
    return d;
  }
  if (g_nextTextureDescriptor < kFirstReservedDescriptor) {
    return g_nextTextureDescriptor++;
  }
  static uint64_t exhaust_logs = 8;
  if (exhaust_logs) {
    exhaust_logs--;
    REXGPU_WARN(
        "videonative: texture descriptor heap exhausted ({} slots, none "
        "free) — new textures render blank",
        kFirstReservedDescriptor);
  }
  return 0;
}
std::unique_ptr<plume::RenderTexture> g_whiteTexture;
std::unique_ptr<plume::RenderTextureView> g_whiteTextureView;
uint32_t g_whiteTextureDescriptor = 0;
// Resolve destinations by guest base address, so a bind through a header with
// different flag bits still finds the host-rendered entry.
std::unordered_map<uint32_t, uint64_t> g_resolvedBaseToKey;
// Depth-resolve staging: the whole depth plane is copied to a buffer and the
// requested region copied out, using a rotating slot set so each buffer
// transitions write->read once per frame.
struct DepthStagingSlot {
  std::unique_ptr<RenderBuffer> buffer;
  uint64_t size = 0;
};
DepthStagingSlot g_depthStaging[8];
uint32_t g_depthStagingRot = 0;

// Whole-plane staging memo, keyed on the source texture, the frame index and a
// mutation counter bumped by every draw and clear, so repeated band resolves of
// an unchanged source share one plane copy.
struct DepthStageMemo {
  const RenderTexture* src = nullptr;
  RenderBuffer* buffer = nullptr;
  uint64_t frame = ~0ull;
  uint64_t mutation = ~0ull;
  uint32_t host_w = 0, host_h = 0, row_pitch = 0;
};
DepthStageMemo g_depthStageMemo;
// Bumped on every draw and clear to mark the bound RT's contents as possibly
// changed.
uint64_t g_rtMutationSeq = 0;
std::unique_ptr<RenderShader> g_depthResolvePs;
std::unique_ptr<RenderPipeline> g_depthResolvePipeline;

// Guest EDRAM surfaces -> host render targets: the CreateSurface hook in
// video_native.cpp writes {magic, width, height, format, msaa} into the guest
// surface object, and 0 or an unrecognized surface means the swapchain
// backbuffer.

struct SurfaceInfo {
  uint32_t width = 0, height = 0;
  uint32_t guest_format = 0;
  uint32_t edram_base = 0;  // tile index — distinct surfaces must not alias
  int32_t exp_bias = 0;     // RB_COLOR_INFO color exponent bias (signed)
  bool valid = false;
};

// Guest passes render up to 4 MRT color outputs, so every cached RT carries the
// full set to back resolves of sources 1-3.
constexpr uint32_t kMrtCount = 4;

struct CachedRenderTarget {
  std::unique_ptr<RenderTexture> color[kMrtCount];
  std::unique_ptr<RenderTexture> depth;
  std::unique_ptr<RenderFramebuffer> framebuffer;
  uint32_t width = 0, height = 0;
  uint32_t edram_base = 0;
  // Allocated 1:1 regardless of the global downscale (the composite+UI surface
  // under performance_mode); never set when upscaling.
  bool full_res = false;
  // Frame the full-res variant was last seeded from its half-res sibling's
  // composited scene, done once per frame.
  uint64_t seeded_frame = ~0ull;
  // LRU stamp and approximate VRAM bytes (5 attachments) for the RT-cache
  // watermark.
  uint64_t last_bind_frame = 0;
  uint64_t approx_bytes = 0;
  // Lazy SRV of color[0] for the seed blit, created on the sibling entry.
  std::unique_ptr<RenderTextureView> seed_view;
  uint32_t seed_descriptor = 0;
  // Depth read as R32_FLOAT for the shader resolve path, created lazily once
  // per RT.
  std::unique_ptr<RenderTextureView> depth_srv;
  uint32_t depth_descriptor = 0;
  // Per-slot attachment formats; formats[0] is the RT's format for
  // slot-agnostic consumers and unbound slots inherit it so resolves of sources
  // 1-3 keep backing.
  RenderFormat formats[kMrtCount] = {
      RenderFormat::UNKNOWN, RenderFormat::UNKNOWN, RenderFormat::UNKNOWN,
      RenderFormat::UNKNOWN};
  RenderTextureLayout color_layout[kMrtCount] = {
      RenderTextureLayout::UNKNOWN, RenderTextureLayout::UNKNOWN,
      RenderTextureLayout::UNKNOWN, RenderTextureLayout::UNKNOWN};
  RenderTextureLayout depth_layout = RenderTextureLayout::UNKNOWN;
};

// Predicated-tiling extent (BeginTiling..EndTiling): while nonzero, host RTs
// for tile-height EDRAM surfaces are expanded to cover the full frame.
uint32_t g_tilingWidth = 0;
uint32_t g_tilingHeight = 0;
// Bracket-less tiled passes (CC1's per-frame shadow window) grow from the draw
// viewport instead: SetupDraw stores the wanted extent here, scoped to the
// triggering guest surface object.
uint32_t g_growWidth = 0;
uint32_t g_growHeight = 0;
uint32_t g_growSurface = 0;
bool g_activeRtGrown = false;     // active RT came from viewport growth
uint32_t g_activeRtHeight = 0;    // host height of the active RT
uint32_t g_activeRtWidth = 0;     // host width of the active RT
// performance_mode UI sharpness: the guest surface object bound when the
// presented resolve fires renders 1:1, mapped to the frame the present pick
// last confirmed it, and expires unless re-confirmed within
// kFullResConfirmWindow frames.
std::unordered_map<uint32_t, uint64_t> g_fullResSurfaces;
constexpr uint64_t kFullResConfirmWindow = 30;
// dest-size key -> surface object active at that resolve, feeding the learn
// step.
std::unordered_map<uint64_t, uint32_t> g_lastResolveSurface;
bool g_activeRtFullRes = false;   // active RT is a full-res (1:1) variant

// TDR forensics: rolling record of the last draws' shader pairs (see
// SetupDraw), dumped by DumpDrawRing on present or resize failure.
struct DrawRec {
  uint64_t vs = 0, ps = 0;
  uint32_t frame = 0;
};
constexpr uint32_t kDrawRingSize = 1024;
DrawRec g_drawRing[kDrawRingSize];
uint32_t g_drawRingPos = 0;

// GPU breadcrumbs: a serial written after every issued draw via
// WriteBufferImmediate into a persistently-mapped readback buffer, so on a hang
// *g_breadcrumbMap is the last draw the GPU completed.
std::unique_ptr<RenderBuffer> g_breadcrumbBuffer;
volatile uint32_t* g_breadcrumbMap = nullptr;
uint32_t g_drawSerial = 0;
struct IssuedDraw {
  uint32_t serial = 0;
  uint32_t frame = 0;
  uint64_t vs = 0, ps = 0;
  uint32_t count = 0;  // vertex/index count
  uint16_t prim = 0;   // guest prim type
  uint8_t indexed = 0;
};
// Sized to exceed the deepest issued-vs-completed gap (kNumFrames in flight x
// ~650 draws/frame).
constexpr uint32_t kIssuedRing = 8192;
IssuedDraw g_issuedRing[kIssuedRing];

void MarkDraw(RenderCommandList* cl, uint32_t count, bool indexed,
              uint32_t prim) {
  if (!REXCVAR_GET(gpu_breadcrumbs) || !g_breadcrumbBuffer) return;
  g_drawSerial++;
  IssuedDraw& r = g_issuedRing[g_drawSerial % kIssuedRing];
  r.serial = g_drawSerial;
  r.frame = uint32_t(g_frameIndex);
  const ResolvedShader* vs = detail::CurrentResolvedVertexShader();
  const ResolvedShader* ps = detail::CurrentResolvedPixelShader();
  r.vs = vs ? vs->ucode_hash : 0;  // 0 = renderer-internal draw
  r.ps = ps ? ps->ucode_hash : 0;
  r.count = count;
  r.prim = uint16_t(prim);
  r.indexed = indexed ? 1 : 0;
  cl->writeBufferImmediate(g_breadcrumbBuffer.get(), 0, g_drawSerial);
}

uint32_t LoadGuestU32(uint32_t addr);
const uint8_t* GuestPtr(uint32_t addr);
extern uint64_t g_activeRtKey;
extern RenderFormat g_activeRtFormat;

// [watchvs] deep capture: full input identity of every draw of the watched VS,
// with a per-frame instance index so a record can be diffed against the same
// pass of the previous frame.
struct WatchRec {
  uint32_t serial = 0, frame = 0, count = 0, prim = 0;
  uint32_t inst = 0;  // Nth watched draw within this frame
  uint32_t ib_ptr = 0, ib_min = 0, ib_max = 0;
  uint64_t ib_hash = 0;
  uint64_t rt_key = 0;
  uint32_t rt_format = 0;
  uint32_t boolloop[40];   // b2 block (bools x8 + loops x32, host order)
  uint32_t fetch[192];     // 32 fetch slots x 6 dwords
  uint32_t vsf[1024];      // 256x16B VS float file (raw big-endian dwords)
  uint32_t psf[1024];      // 256x16B PS float file
};
constexpr uint32_t kWatchRing = 32;
WatchRec g_watchRing[kWatchRing];
uint32_t g_watchCount = 0;
uint32_t g_watchFrame = 0, g_watchFrameInst = 0;
uint64_t g_watchVs = 0;
bool g_watchParsed = false;

void MaybeCaptureWatch(uint32_t device, uint32_t count, uint32_t prim,
                       uint32_t idx_data_guest, bool idx32) {
  if (!g_watchParsed) {
    g_watchParsed = true;
    const std::string& s = REXCVAR_GET(gpu_watch_vs);
    if (!s.empty()) g_watchVs = strtoull(s.c_str(), nullptr, 16);
  }
  if (!g_watchVs) return;
  const ResolvedShader* vs = detail::CurrentResolvedVertexShader();
  if (!vs || vs->ucode_hash != g_watchVs) return;
  WatchRec& r = g_watchRing[g_watchCount++ % kWatchRing];
  r.serial = g_drawSerial + 1;  // MarkDraw's upcoming serial for this draw
  r.frame = uint32_t(g_frameIndex);
  if (r.frame != g_watchFrame) {
    g_watchFrame = r.frame;
    g_watchFrameInst = 0;
  }
  r.inst = g_watchFrameInst++;
  r.count = count;
  r.prim = prim;
  r.rt_key = g_activeRtKey;
  r.rt_format = uint32_t(g_activeRtFormat);
  for (uint32_t i = 0; i < 40; i++) {
    r.boolloop[i] = LoadGuestU32(device + 10112 + 4 * i);
  }
  for (uint32_t i = 0; i < 192; i++) {
    r.fetch[i] = LoadGuestU32(device + 1152 + 4 * i);
  }
  const uint8_t* vsf = GuestPtr(device + 1920);
  const uint8_t* psf = GuestPtr(device + 6016);
  if (vsf) std::memcpy(r.vsf, vsf, 4096);
  if (psf) std::memcpy(r.psf, psf, 4096);
  r.ib_ptr = idx_data_guest;
  r.ib_hash = 0;
  r.ib_min = 0xFFFFFFFFu;
  r.ib_max = 0;
  if (const uint8_t* ib = GuestPtr(idx_data_guest)) {
    r.ib_hash = XXH3_64bits(ib, size_t(count) * (idx32 ? 4 : 2));
    if (idx32) {
      const uint32_t* p = reinterpret_cast<const uint32_t*>(ib);
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t v = __builtin_bswap32(p[i]);
        r.ib_min = v < r.ib_min ? v : r.ib_min;
        r.ib_max = v > r.ib_max ? v : r.ib_max;
      }
    } else {
      const uint16_t* p = reinterpret_cast<const uint16_t*>(ib);
      for (uint32_t i = 0; i < count; i++) {
        const uint32_t v = __builtin_bswap16(p[i]);
        r.ib_min = v < r.ib_min ? v : r.ib_min;
        r.ib_max = v > r.ib_max ? v : r.ib_max;
      }
    }
  }
}

float WatchBeFloat(uint32_t be) {
  const uint32_t w = __builtin_bswap32(be);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// Prints every float register whose 16 bytes differ between two records.
void DumpWatchFloatDiff(const char* which, const uint32_t* a,
                        const uint32_t* b) {
  for (uint32_t reg = 0; reg < 256; reg++) {
    const uint32_t* pa = a + reg * 4;
    const uint32_t* pb = b + reg * 4;
    if (pa[0] == pb[0] && pa[1] == pb[1] && pa[2] == pb[2] && pa[3] == pb[3])
      continue;
    REXGPU_WARN(
        "videonative: [watchvs]   {} c{}: ({:.6g},{:.6g},{:.6g},{:.6g}) -> "
        "({:.6g},{:.6g},{:.6g},{:.6g})",
        which, reg, WatchBeFloat(pa[0]), WatchBeFloat(pa[1]),
        WatchBeFloat(pa[2]), WatchBeFloat(pa[3]), WatchBeFloat(pb[0]),
        WatchBeFloat(pb[1]), WatchBeFloat(pb[2]), WatchBeFloat(pb[3]));
  }
}

void DumpWatch(uint32_t fatal_serial) {
  if (!g_watchVs || !g_watchCount) return;
  const uint32_t n = g_watchCount < kWatchRing ? g_watchCount : kWatchRing;
  REXGPU_WARN("videonative: [watchvs] {:016X}: {} records (fatal serial {}):",
              g_watchVs, n, fatal_serial);
  const WatchRec* fatal = nullptr;
  const WatchRec* prev_like = nullptr;  // same instance slot, latest earlier
  for (uint32_t k = 0; k < n; k++) {
    // Oldest -> newest in ring order.
    const WatchRec& r = g_watchRing[(g_watchCount - n + k) % kWatchRing];
    if (r.serial == fatal_serial) fatal = &r;
    REXGPU_WARN(
        "videonative: [watchvs] serial={}{} f{} i{} n={} rt={:x}/{} "
        "ib={:#x} ibh={:016X} ib[{}..{}] bools={:08X} {:08X}",
        r.serial, r.serial == fatal_serial ? " <-FATAL" : "", r.frame, r.inst,
        r.count, r.rt_key, r.rt_format, r.ib_ptr, r.ib_hash, r.ib_min,
        r.ib_max, r.boolloop[0], r.boolloop[4]);
    for (uint32_t slot = 0; slot < 32; slot++) {
      const uint32_t* d = &r.fetch[slot * 6];
      if (!(d[0] | d[1] | d[2] | d[3] | d[4] | d[5])) continue;
      REXGPU_WARN(
          "videonative: [watchvs]   fc{}: {:08X} {:08X} {:08X} {:08X} "
          "{:08X} {:08X}",
          slot, d[0], d[1], d[2], d[3], d[4], d[5]);
    }
  }
  if (!fatal) {
    REXGPU_WARN(
        "videonative: [watchvs] fatal serial {} is not in the ring — "
        "kWatchRing is too small",
        fatal_serial);
    return;
  }
  // The latest earlier record with the same per-frame instance index is the
  // same pass of the previous frame, the like-for-like diff base.
  for (uint32_t k = 0; k < n; k++) {
    const WatchRec& r = g_watchRing[(g_watchCount - n + k) % kWatchRing];
    if (r.serial < fatal->serial && r.inst == fatal->inst &&
        (!prev_like || r.serial > prev_like->serial)) {
      prev_like = &r;
    }
  }
  if (!prev_like) {
    REXGPU_WARN("videonative: [watchvs] no like-instance predecessor in ring");
    return;
  }
  REXGPU_WARN(
      "videonative: [watchvs] float diff prev serial={} f{} -> fatal "
      "serial={} f{} (only changed registers):",
      prev_like->serial, prev_like->frame, fatal->serial, fatal->frame);
  DumpWatchFloatDiff("vs", prev_like->vsf, fatal->vsf);
  DumpWatchFloatDiff("ps", prev_like->psf, fatal->psf);
  for (uint32_t i = 0; i < 40; i++) {
    if (prev_like->boolloop[i] != fatal->boolloop[i]) {
      REXGPU_WARN("videonative: [watchvs]   b2[{}]: {:08X} -> {:08X}", i,
                  prev_like->boolloop[i], fatal->boolloop[i]);
    }
  }
}

void DumpBreadcrumb(const char* why) {
  if (!g_breadcrumbMap) return;
  const uint32_t done = *g_breadcrumbMap;
  REXGPU_WARN(
      "videonative: [breadcrumb] ({}) GPU completed draw serial={} of {} "
      "issued (gap {}) — serial {}+1 = the hanging draw:",
      why, done, g_drawSerial, g_drawSerial - done, done);
  for (uint32_t s = (done >= 3 ? done - 3 : 1); s <= done + 8; s++) {
    if (s == 0 || s > g_drawSerial) continue;
    const IssuedDraw& r = g_issuedRing[s % kIssuedRing];
    REXGPU_WARN(
        "videonative: [breadcrumb]   serial={}{} f{} vs={:016X} ps={:016X} "
        "{}={} prim={}{}",
        s, s == done ? " <-LAST-DONE" : (s == done + 1 ? " <-HANGING?" : ""),
        r.frame, r.vs, r.ps, r.indexed ? "idx" : "vtx", r.count, r.prim,
        r.serial != s ? " [RING-OVERWRITTEN: stale entry]" : "");
  }
  DumpWatch(done + 1);
}

void DumpDrawRing(const char* why) {
  // Unique pairs over the whole ring, then the last 16 in order.
  std::map<std::pair<uint64_t, uint64_t>, uint32_t> pairs;
  for (uint32_t i = 0; i < kDrawRingSize; i++) {
    const DrawRec& r = g_drawRing[i];
    if (r.vs || r.ps) pairs[{r.vs, r.ps}]++;
  }
  REXGPU_WARN("videonative: [drawring] dump ({}): {} unique shader pairs",
              why, pairs.size());
  for (const auto& [p, count] : pairs) {
    REXGPU_WARN("videonative: [drawring]   vs={:016X} ps={:016X} x{}",
                p.first, p.second, count);
  }
  std::string tail;
  for (uint32_t i = 0; i < 16; i++) {
    const DrawRec& r =
        g_drawRing[(g_drawRingPos + kDrawRingSize - 16 + i) % kDrawRingSize];
    tail += fmt::format(" f{}:{:08X}/{:08X}", r.frame % 1000,
                        uint32_t(r.vs >> 32), uint32_t(r.ps >> 32));
  }
  REXGPU_WARN("videonative: [drawring] tail(hi32):{}", tail);
}
uint32_t g_passResolves = 0;      // resolves since the last target change
bool g_scissorExplicit = false;   // guest set a scissor since the last bind

// Keyed by (width, height, host format) so same-shaped transient EDRAM surfaces
// share one host RT.
std::unordered_map<uint64_t, CachedRenderTarget> g_renderTargets;
uint64_t g_activeRtKey = 0;  // 0 = swapchain framebuffer
// The active RT object and a layouts-dirty flag for the lazy resolve path: a
// mid-pass resolve leaves the active RT's surfaces in COPY_SOURCE and the next
// draw or same-key rebind repairs them on demand.
CachedRenderTarget* g_activeRt = nullptr;
bool g_activeRtLayoutsDirty = false;

// [lazyverify] The lazy resolve path skips barriers the tracked layouts call
// redundant, which is sound only while they match plume's barrier ledger;
// verify mode logs a divergence and emits the barrier anyway.
bool LazyLedgerStale(const RenderTexture* tex, RenderTextureLayout tracked,
                     const char* what, uint64_t key, uint32_t slot) {
  if (!REXCVAR_GET(gpu_resolve_lazy_verify)) return false;
  if (!tex) return false;
  const RenderTextureLayout known = tex->getKnownLayout();
  if (known == RenderTextureLayout::UNKNOWN || known == tracked) return false;
  static uint64_t lazyverify_logs = 200;
  if (lazyverify_logs) {
    lazyverify_logs--;
    REXGPU_WARN(
        "videonative: [lazyverify] stale {} ledger: tracked={} plume={} "
        "key={:#x} slot={} passResolves={} activeRt={:#x}",
        what, uint32_t(tracked), uint32_t(known), key, slot, g_passResolves,
        g_activeRtKey);
  }
  return true;
}

void RepairActiveRtLayouts() {
  if (!g_activeRtLayoutsDirty) return;
  g_activeRtLayoutsDirty = false;
  CachedRenderTarget* rt = g_activeRt;
  if (!rt) return;
  uint32_t n = 0;
  RenderTextureBarrier barriers[kMrtCount + 1];
  for (uint32_t i = 0; i < kMrtCount; i++) {
    if (rt->color_layout[i] != RenderTextureLayout::COLOR_WRITE ||
        LazyLedgerStale(rt->color[i].get(), rt->color_layout[i],
                        "repair-color", g_activeRtKey, i)) {
      barriers[n++] = RenderTextureBarrier(rt->color[i].get(),
                                           RenderTextureLayout::COLOR_WRITE);
      rt->color_layout[i] = RenderTextureLayout::COLOR_WRITE;
    }
  }
  if (rt->depth_layout != RenderTextureLayout::DEPTH_WRITE ||
      LazyLedgerStale(rt->depth.get(), rt->depth_layout, "repair-depth",
                      g_activeRtKey, 0)) {
    barriers[n++] = RenderTextureBarrier(rt->depth.get(),
                                         RenderTextureLayout::DEPTH_WRITE);
    rt->depth_layout = RenderTextureLayout::DEPTH_WRITE;
  }
  if (n) {
    g_commandLists[g_frame]->barriers(RenderBarrierStage::GRAPHICS, barriers,
                                      n);
  }
}
// The RT the most recent draw targeted, which resolves read from.
uint64_t g_lastDrawnRtKey = 0;

// Display gamma-ramp LUT (256x3 R16_UNORM, rows = R,G,B curves) sampled by the
// present blit when bound (b4 [0].y), mirroring the guest D3DGAMMARAMP set
// through D3DDevice_SetGammaRamp.
std::unique_ptr<plume::RenderTexture> g_gammaRampTexture;
std::unique_ptr<plume::RenderTextureView> g_gammaRampView;
uint32_t g_gammaRampDescriptor = 0;
RenderFormat g_activeRtFormat = kColorFormat;
// Per-slot formats of the active RT set (all kColorFormat when the swapchain is
// bound), consumed by pipeline creation so each MRT slot compiles against its
// attachment format.
RenderFormat g_activeRtFormats[4] = {kColorFormat, kColorFormat, kColorFormat,
                                     kColorFormat};

uint32_t LoadGuestU32(uint32_t addr) {
  // On the rq worker, draw records carry a device-block snapshot served through
  // a thread-local window so deferred execution reads call-time state.
  for (const rq::Win& w : rq::t_win) {
    if (w.host && addr - w.base < w.size) {
      uint32_t v;
      std::memcpy(&v, w.host + (addr - w.base), 4);
      return __builtin_bswap32(v);
    }
  }
  return __builtin_bswap32(
      *rex::system::kernel_memory()->TranslateVirtual<const uint32_t*>(addr));
}
void StoreGuestU32(uint32_t addr, uint32_t value) {
  *rex::system::kernel_memory()->TranslateVirtual<uint32_t*>(addr) =
      __builtin_bswap32(value);
}
const uint8_t* GuestPtr(uint32_t addr) {
  for (const rq::Win& w : rq::t_win) {
    if (w.host && addr - w.base < w.size) return w.host + (addr - w.base);
  }
  return rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(addr);
}

// Whether [p, p + size) is committed, readable host memory, which a freed guest
// block may not be.
bool HostReadable(const uint8_t* p, size_t size) {
  const uint8_t* end = p + size;
  while (p < end) {
    MEMORY_BASIC_INFORMATION info;
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT ||
        !info.Protect || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
      return false;
    }
    p = static_cast<const uint8_t*>(info.BaseAddress) + info.RegionSize;
  }
  return true;
}

// Hash of three stretches of a guest texture's first level (start, middle,
// end), or 0 when the memory cannot be read.
uint64_t SampleTexels(uint32_t base_address, uint32_t data_size) {
  const uint8_t* texels = GuestPtr(base_address);
  if (!texels || !data_size) return 0;
  const uint32_t span = std::min<uint32_t>(data_size, 1024);
  uint64_t hash = 0;
  for (uint32_t part = 0; part < 3; ++part) {
    const uint32_t at = uint32_t(uint64_t(data_size - span) * part / 2) & ~3u;
    if (!HostReadable(texels + at, span)) return 0;
    hash = XXH3_64bits_withSeed(texels + at, span, hash);
  }
  return hash | 1;
}

// Persistent effective blend/mask registers, matching the single global RB_*
// register set on hardware, fed from CbCall::st_dirty slots on replayed draws
// and from the XDK's dev+16 dirty bits on live draws (see the draw-state block
// in SetupDraw).
uint32_t g_fxBlendControl[4] = {0x00010001u, 0x00010001u, 0x00010001u,
                                0x00010001u};
uint32_t g_fxColorMask = 0xFFFFu;

// 360 texture tiling (Xenia's TiledOffset2D functions).
uint32_t TiledOffset2DOuter(uint32_t y, uint32_t width, uint32_t log2_bpp) {
  uint32_t macro = ((y >> 5) * (width >> 5)) << (log2_bpp + 7);
  uint32_t micro = ((y & 6) << 2) << log2_bpp;
  return macro + ((micro & ~0xFu) << 1) + (micro & 0xF) +
         ((y & 8) << (3 + log2_bpp)) + ((y & 1) << 4);
}
uint32_t TiledOffset2DInner(uint32_t x, uint32_t y, uint32_t log2_bpp,
                            uint32_t base_offset) {
  uint32_t macro = (x >> 5) << (log2_bpp + 7);
  uint32_t micro = (x & 7) << log2_bpp;
  uint32_t offset =
      base_offset + (macro + ((micro & ~0xFu) << 1) + (micro & 0xF));
  return ((offset & ~0x1FFu) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) +
         ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

void UntileSurface(uint8_t* dst, const uint8_t* src, uint32_t pitch_blocks,
                   uint32_t width_blocks, uint32_t height_blocks,
                   uint32_t bytes_per_block, uint32_t x0_blocks = 0,
                   uint32_t y0_blocks = 0) {
  uint32_t log2_bpp = 0;
  switch (bytes_per_block) {
    case 1: log2_bpp = 0; break;
    case 2: log2_bpp = 1; break;
    case 4: log2_bpp = 2; break;
    case 8: log2_bpp = 3; break;
    case 16: log2_bpp = 4; break;
    default: return;
  }
  // x0/y0 is the packed-mip-tail block offset: textures with min dimension <=
  // 16 texels live inside the 32x32 tile at a nonzero origin
  // (GetPackedMipOffset in the ring's texture/util.cpp).
  for (uint32_t y = 0; y < height_blocks; y++) {
    uint32_t outer = TiledOffset2DOuter(y + y0_blocks, pitch_blocks, log2_bpp);
    for (uint32_t x = 0; x < width_blocks; x++) {
      uint32_t src_off =
          TiledOffset2DInner(x + x0_blocks, y + y0_blocks, log2_bpp, outer);
      std::memcpy(dst + (y * width_blocks + x) * bytes_per_block, src + src_off,
                  bytes_per_block);
    }
  }
}

void SwapBytes16(uint8_t* data, size_t size) {
  auto* p = reinterpret_cast<uint16_t*>(data);
  for (size_t i = 0; i < size / 2; i++) p[i] = __builtin_bswap16(p[i]);
}
void SwapBytes32(uint8_t* data, size_t size) {
  auto* p = reinterpret_cast<uint32_t*>(data);
  for (size_t i = 0; i < size / 4; i++) p[i] = __builtin_bswap32(p[i]);
}

// Parses a guest GPUTEXTURE_FETCH header (6 BE dwords) into a cached host
// texture (invalid on unsupported format); for_resolve skips the guest-memory
// upload, the overrides force host dimensions and format to match the source
// RT, and resolve_scale is 0 = global scale, 1 = 1:1, -1 = any.
CachedTexture* GetOrCreateTexture(uint32_t header_addr,
                                  bool for_resolve = false,
                                  uint32_t override_w = 0,
                                  uint32_t override_h = 0,
                                  RenderFormat override_format =
                                      RenderFormat::UNKNOWN,
                                  const uint32_t* dw_snapshot = nullptr,
                                  int resolve_scale = 0) {
  uint32_t dw[6];
  if (dw_snapshot) {
    // Bind-time snapshot (XDK copy semantics, see SetTexture), since by draw
    // time the header may hold a recycled block's dwords.
    std::memcpy(dw, dw_snapshot, sizeof(dw));
  } else {
    for (int i = 0; i < 6; i++) dw[i] = LoadGuestU32(header_addr + i * 4);
  }
  const uint64_t key = XXH3_64bits(dw, sizeof(dw));
  // Sampling a resolved base address hits the host-rendered entry even when the
  // sampling header's flag bits differ from the resolve-dest header's.
  if (!for_resolve && REXCVAR_GET(gpu_resolve_redirect)) {
    auto resolved = g_resolvedBaseToKey.find(dw[1] & 0xFFFFF000u);
    if (resolved != g_resolvedBaseToKey.end()) {
      auto rit = g_textureCache.find(resolved->second);
      if (rit != g_textureCache.end() &&
          rit->second.width >=
              uint32_t(REXCVAR_GET(gpu_redirect_min_dim))) {
        // The redirect serves only samples of the same surface, so the
        // requesting header's dimensions must match the resolved entry's, since
        // guest allocations can reuse a resolve destination's base.
        const uint32_t req_w = (dw[2] & 0x1FFF) + 1;
        const uint32_t req_h = ((dw[2] >> 13) & 0x1FFF) + 1;
        CachedTexture& rt = rit->second;
        if ((rt.width == req_w && rt.height == req_h) ||
            !rt.host_rendered) {
          // LRU stamp: one-time bakes resolved at level load are sampled only
          // through this redirect and would otherwise look idle to the eviction
          // sweep.
          rt.last_bind_frame = g_frameIndex;
          rt.gpu_sampled = true;
          return &rt;
        }
      }
    }
  }
  auto it = g_textureCache.find(key);
  if (it != g_textureCache.end()) {
    const uint64_t idle_frames = g_frameIndex - it->second.last_bind_frame;
    it->second.last_bind_frame = g_frameIndex;  // hot: the evict sweep skips it
    // An entry refused at upload time (unsupported format), or one with
    // different host dimensions or format than the resolve needs, is rebuilt as
    // a resolve destination.
    const bool dims_ok =
        !override_w ||
        (it->second.width == override_w && it->second.height == override_h);
    const bool format_ok = override_format == RenderFormat::UNKNOWN ||
                           it->second.host_format == override_format;
    // A resolve destination must be allocated at the requested host scale.
    const bool scale_ok =
        resolve_scale < 0 ||
        it->second.host_scale ==
            (resolve_scale == 1 ? uint8_t(0x11) : HostScaleCode());
    if (!for_resolve || (it->second.valid && dims_ok && format_ok && scale_ok)) {
      // [texdirty] Re-upload on change: animated or streamed textures rewrite
      // guest texel data after the first upload, which a sparse content hash
      // detects, and the stale entry is retired and rebuilt through the
      // creation path below.
      CachedTexture& e = it->second;
      bool reupload = false;
      // Bounded healing: content is re-checked only while the entry is younger
      // than gpu_tex_heal_window frames and for at most 2 changes,
      // since an aged entry's guest memory may have been freed and recycled.
      const uint64_t heal_window =
          uint64_t(std::max(0, REXCVAR_GET(gpu_tex_heal_window)));
      bool header_alive = true;
      if (dw_snapshot) {
        // The live header must still equal all 6 dwords of the bind snapshot.
        for (int i = 0; i < 6 && header_alive; i++) {
          header_alive = LoadGuestU32(header_addr + i * 4) == dw[i];
        }
      }
      if (!for_resolve && e.valid && !e.host_rendered && e.width <= 256 &&
          e.height <= 256 && e.last_content_check != g_frameIndex &&
          (g_frameIndex & 7) == 0 && g_retiredTextureTotal < 512 &&
          REXCVAR_GET(gpu_tex_reupload) &&
          g_frameIndex - e.created_frame < heal_window &&
          e.content_changes < 2 && header_alive) {
        e.last_content_check = g_frameIndex;
        const uint8_t* texels = GuestPtr(dw[1] & 0xFFFFF000u);
        if (texels) {
          const uint64_t h = XXH3_64bits(texels, 2048);
          if (e.content_hash && h != e.content_hash) {
            reupload = true;
            ++e.content_changes;
          }
          e.content_hash = h;
        }
      }
      // The key is the six fetch dwords alone, so an entry idle for more than
      // gpu_tex_idle_check frames has its guest texels sampled again
      // and is retired when another image is there.
      const int idle_check = REXCVAR_GET(gpu_tex_idle_check);
      if (!reupload && !for_resolve && e.valid && !e.host_rendered &&
          e.sample_hash && idle_check > 0 &&
          idle_frames > uint64_t(idle_check)) {
        const uint64_t now =
            SampleTexels(dw[1] & 0xFFFFF000u, e.sample_size);
        if (now && now != e.sample_hash) {
          reupload = true;
          REXGPU_INFO(
              "videonative: [texalias] base={:#x} {}x{} fmt={} idle {} "
              "frames: another image is there now -> re-upload (frame {})",
              dw[1] & 0xFFFFF000u, e.width, e.height,
              uint32_t(e.host_format), idle_frames, g_frameIndex);
        }
      }
      if (!reupload) return &it->second;
      g_retiredTextureTotal++;
      // Last-resolve pointers into the entry are dropped before it is retired,
      // then it is erased once and recreated below.
      for (auto lit = g_lastResolvedBySize.begin();
           lit != g_lastResolvedBySize.end();) {
        if (lit->second == &it->second) {
          lit = g_lastResolvedBySize.erase(lit);
        } else {
          ++lit;
        }
      }
      g_retiredTextures.emplace_back(g_frameIndex, std::move(it->second));
      g_textureCache.erase(it);
      // Falls through to full recreation and upload with the fresh guest data.
    } else {
      for (auto lit = g_lastResolvedBySize.begin();
           lit != g_lastResolvedBySize.end();) {
        if (lit->second == &it->second) {
          lit = g_lastResolvedBySize.erase(lit);
        } else {
          ++lit;
        }
      }
      // Retired instead of destroyed, since in-flight draws may still sample
      // it.
      RetireEntry(it->second);
      g_textureCache.erase(it);
    }
  }

  CachedTexture entry;
  entry.created_frame = g_frameIndex;  // bounds the healing window
  // xe_gpu_texture_fetch_t dword0: pitch in 32-texel tiles at bits 22..30,
  // tiled at bit 31.
  const uint32_t pitch_tiles = (dw[0] >> 22) & 0x1FF;
  const bool tiled = (dw[0] >> 31) & 1;
  const uint32_t format = dw[1] & 0x3F;
  const uint32_t endian = (dw[1] >> 6) & 0x3;
  const uint32_t base_address = (dw[1] & 0xFFFFF000);
  const uint32_t width = (dw[2] & 0x1FFF) + 1;
  const uint32_t height = ((dw[2] >> 13) & 0x1FFF) + 1;
  // DataDimension at dword5 bits 9-10 (xenos.h): 0=1D, 1=2D/stacked, 2=3D,
  // 3=cube; cube maps are 6 consecutive tiled 2D faces in guest memory and need
  // a cube SRV.
  const uint32_t dimension = (dw[5] >> 9) & 3;
  const bool cube = dimension == 3;
  if (dimension == 2) {
    static uint64_t vol_logs = 4;
    if (vol_logs) {
      vol_logs--;
      REXGPU_WARN("videonative: 3D texture {}x{} fmt={} unsupported (treated "
                  "as 2D)",
                  width, height, format);
    }
  }

  RenderFormat host_format = RenderFormat::UNKNOWN;
  uint32_t block_dim = 1, bytes_per_block = 0;
  switch (format) {
    case 6:  // k_8_8_8_8
      host_format = RenderFormat::R8G8B8A8_UNORM;
      bytes_per_block = 4;
      break;
    case 7:   // k_2_10_10_10 (packed layout matches DXGI after 8-in-32 swap)
    case 54:  // k_2_10_10_10_AS_16_16_16_16, the resolve alias of 2_10_10_10
              // (XDK D3D_SurfaceInitFields normalizes 54 -> 7).
      host_format = RenderFormat::R10G10B10A2_UNORM;
      bytes_per_block = 4;
      break;
    case 32:  // k_16_16_16_16_FLOAT (A16B16G16R16F — bloom chain, RTT type 3)
      host_format = RenderFormat::R16G16B16A16_FLOAT;
      bytes_per_block = 8;
      break;
    case 2:  // k_8
      host_format = RenderFormat::R8_UNORM;
      bytes_per_block = 1;
      break;
    case 18:  // k_DXT1
      host_format = RenderFormat::BC1_UNORM;
      block_dim = 4;
      bytes_per_block = 8;
      break;
    case 19:  // k_DXT2_3
      host_format = RenderFormat::BC2_UNORM;
      block_dim = 4;
      bytes_per_block = 16;
      break;
    case 20:  // k_DXT4_5
      host_format = RenderFormat::BC3_UNORM;
      block_dim = 4;
      bytes_per_block = 16;
      break;
    case 22:  // k_24_8 (depth-as-texture)
    case 23:  // k_24_8_FLOAT depth resolve destinations, where host depth
              // copies land as R32_FLOAT.
      host_format = RenderFormat::R32_FLOAT;
      bytes_per_block = 4;
      break;
    default:
      REXGPU_WARN("videonative: unsupported texture format {} ({}x{})", format,
                  width, height);
      entry.valid = false;
      {
    auto prior = g_textureCache.find(key);
    if (prior != g_textureCache.end()) RetireEntry(prior->second);
  }
  entry.last_bind_frame = g_frameIndex;
  entry.approx_bytes = entry.texture ? ApproxTexBytes(entry.width, entry.height, entry.host_format) : 0;
  g_texCacheBytes += entry.approx_bytes;
  entry.header_addr = header_addr;
  std::memcpy(entry.header_dw, dw, sizeof(entry.header_dw));
  return &(g_textureCache[key] = std::move(entry));
  }

  if (for_resolve) {
    // Host-rendered: allocate only, with the host format following the source
    // RT when the caller says so, since host copies cannot convert formats.
    if (override_format != RenderFormat::UNKNOWN) {
      host_format = override_format;
    }
    const uint32_t alloc_w = override_w ? override_w : width;
    const uint32_t alloc_h = override_h ? override_h : height;
    const bool alloc_full = resolve_scale == 1;
    RenderTextureDesc rdesc = RenderTextureDesc::Texture2D(
        alloc_full ? alloc_w : HostDim(alloc_w),
        alloc_full ? alloc_h : HostDim(alloc_h), 1, host_format);
    // R32_FLOAT depth resolve destinations are render targets under the shader
    // resolve path, which renders the region in.
    if (host_format == RenderFormat::R32_FLOAT &&
        REXCVAR_GET(gpu_depth_resolve_shader)) {
      rdesc.flags |= RenderTextureFlag::RENDER_TARGET;
      entry.rt_capable = true;
    }
    entry.texture = g_device->createTexture(rdesc);
    if (!entry.texture) {
      REXGPU_WARN(
          "videonative: resolve tex creation failed ({}x{} fmt={}) — entry "
          "invalid",
          alloc_w, alloc_h, uint32_t(host_format));
      entry.valid = false;
      auto prior2 = g_textureCache.find(key);
      if (prior2 != g_textureCache.end()) RetireEntry(prior2->second);
      entry.last_bind_frame = g_frameIndex;
  entry.approx_bytes = entry.texture ? ApproxTexBytes(entry.width, entry.height, entry.host_format) : 0;
  g_texCacheBytes += entry.approx_bytes;
  entry.header_addr = header_addr;
  std::memcpy(entry.header_dw, dw, sizeof(entry.header_dw));
  return &(g_textureCache[key] = std::move(entry));
    }
    entry.host_scale = alloc_full ? uint8_t(0x11) : HostScaleCode();
    entry.texture->setName(
        fmt::format("resolve tex {}x{} (sc {:#x}) base={:#x}", alloc_w,
                    alloc_h, entry.host_scale, base_address));
    // 2D-array view: the pack contract declares xe_textures_2d as
    // Texture2DArray, and a plain 2D SRV through an array declaration is
    // undefined in D3D12.
    entry.view = entry.texture->createTextureView(
        RenderTextureViewDesc::Texture2DArray(host_format));
    entry.width = alloc_w;
    entry.height = alloc_h;
    entry.host_format = host_format;
    entry.host_rendered = true;
    entry.layout = RenderTextureLayout::UNKNOWN;
    entry.descriptor_index = AllocTextureDescriptor();
    if (entry.descriptor_index) {
      g_textureDescriptorSet->setTexture(entry.descriptor_index,
                                         entry.texture.get(),
                                         RenderTextureLayout::SHADER_READ,
                                         entry.view.get());
    }
    entry.valid = true;
    g_resolvedBaseToKey[base_address] = key;
    {
    auto prior = g_textureCache.find(key);
    if (prior != g_textureCache.end()) RetireEntry(prior->second);
  }
  entry.last_bind_frame = g_frameIndex;
  entry.approx_bytes = entry.texture ? ApproxTexBytes(entry.width, entry.height, entry.host_format) : 0;
  g_texCacheBytes += entry.approx_bytes;
  entry.header_addr = header_addr;
  std::memcpy(entry.header_dw, dw, sizeof(entry.header_dw));
  return &(g_textureCache[key] = std::move(entry));
  }

  const uint32_t width_blocks = (width + block_dim - 1) / block_dim;
  const uint32_t height_blocks = (height + block_dim - 1) / block_dim;
  // Tiled textures have a minimum pitch of one tile (32 texels) and the
  // fetch-constant pitch field can read 0 for sub-tile textures, so the untile
  // pitch is clamped to the tile width.
  uint32_t pitch_blocks =
      pitch_tiles ? (pitch_tiles * 32 / block_dim) : width_blocks;
  if (tiled && REXCVAR_GET(gpu_untile_packed)) {
    pitch_blocks = std::max(pitch_blocks, 32u / block_dim);
  }
  const uint32_t data_size = width_blocks * height_blocks * bytes_per_block;
  // Cube faces are stored consecutively, each laid out like a full 2D
  // texture; tiled faces are padded to the 32-texel tile grid vertically.
  const uint32_t tile_blocks = 32 / block_dim;
  const uint32_t aligned_height_blocks =
      tiled ? ((height_blocks + tile_blocks - 1) & ~(tile_blocks - 1))
            : height_blocks;
  const uint32_t face_stride =
      pitch_blocks * aligned_height_blocks * bytes_per_block;
  const uint32_t face_count = cube ? 6 : 1;

  RenderTextureDesc desc =
      cube ? RenderTextureDesc::Texture(RenderTextureDimension::TEXTURE_2D,
                                        width, height, 1, 1, 6, host_format)
           : RenderTextureDesc::Texture2D(width, height, 1, host_format);
  entry.texture = g_device->createTexture(desc);
  entry.texture->setName(
      fmt::format("guest tex {}x{} fmt={}", width, height, format));
  // Fetch-constant destination swizzle (dword3 bits 1-12, 3 bits per output
  // channel: 0-3 = data components X..W, 4/6 = zero, 5/7 = one); the swizzle
  // dwords are part of the 6-dword cache key, so distinct swizzles get distinct
  // entries.
  RenderComponentMapping component_mapping;
  // Default swizzle policy: pure channel-reorder swizzles are honored and
  // identity is kept whenever any selector forces 0 or 1.
  const uint32_t guest_swizzle_raw = (dw[3] >> 1) & 0xFFF;
  bool has_const_selector = false;
  for (uint32_t i = 0; i < 4; i++) {
    if (((guest_swizzle_raw >> (3 * i)) & 0b100) != 0) has_const_selector = true;
  }
  if (REXCVAR_GET(gpu_tex_swizzle_full)) {
    // Ring composition (GuestToHostSwizzle): the fetch swizzle's data selectors
    // index the format's host base swizzle, mirrored from the d3d12
    // texture_cache host_formats_[].swizzle (R=0 G=1 B=2 A=3, 3 bits per
    // output, x at bit 0), and 4/6 -> zero, 5/7 -> one.
    static constexpr uint16_t kRGBA = 0x688, kRRRR = 0x000, kRGGG = 0x248,
                              kRGBB = 0x488, kRBGG = 0x250;
    static constexpr uint16_t kHostFormatSwizzle[64] = {
        kRRRR, kRRRR, kRRRR, kRGBA, kRGBB, kRBGG, kRGBA, kRGBA,  // 0-7
        kRRRR, kRRRR, kRGGG, kRGBB, kRGBB, kRGGG, kRGBA, kRGBA,  // 8-15
        kRGBB, kRGBB, kRGBA, kRGBA, kRGBA, kRGBA, kRRRR, kRRRR,  // 16-23
        kRRRR, kRGGG, kRGBA, kRRRR, kRGGG, kRGBA, kRRRR, kRGGG,  // 24-31
        kRGBA, kRRRR, kRGGG, kRGBA, kRRRR, kRGGG, kRGBA, kRRRR,  // 32-39
        kRGGG, kRRRR, kRGGG, kRRRR, kRRRR, kRGGG, kRRRR, kRRRR,  // 40-47
        kRGGG, kRGGG, kRGBA, kRGBA, kRGBA, kRGBA, kRGBA, kRGBB,  // 48-55
        kRGBB, kRGBB, kRRRR, kRRRR, kRGGG, kRGBA, kRGBA, kRGBA,  // 56-63
    };
    const uint32_t base = kHostFormatSwizzle[format & 63];
    RenderSwizzle* out[4] = {&component_mapping.r, &component_mapping.g,
                             &component_mapping.b, &component_mapping.a};
    for (uint32_t i = 0; i < 4; i++) {
      uint32_t sel = (guest_swizzle_raw >> (3 * i)) & 0x7;
      uint32_t host;
      if (sel & 0b100) {
        host = sel & 0b101;  // 4/6 -> 4 (zero), 5/7 -> 5 (one)
      } else {
        host = (base >> (3 * sel)) & 0x7;
      }
      *out[i] = host == 4 ? RenderSwizzle::ZERO
                : host == 5
                    ? RenderSwizzle::ONE
                    : RenderSwizzle(uint32_t(RenderSwizzle::R) + host);
    }
  } else if (REXCVAR_GET(gpu_tex_swizzle) && !has_const_selector) {
    const bool host_rrrr = (format == 2 || format == 22 || format == 23);
    const uint32_t guest_swizzle = guest_swizzle_raw;
    RenderSwizzle* out[4] = {&component_mapping.r, &component_mapping.g,
                             &component_mapping.b, &component_mapping.a};
    for (uint32_t i = 0; i < 4; i++) {
      const uint32_t sel = (guest_swizzle >> (3 * i)) & 0x7;
      if (sel & 0b100) {
        *out[i] = (sel & 1) ? RenderSwizzle::ONE : RenderSwizzle::ZERO;
      } else {
        const uint32_t host_component = host_rrrr ? 0 : sel;
        *out[i] =
            RenderSwizzle(uint32_t(RenderSwizzle::R) + host_component);
      }
    }
  }
  RenderTextureViewDesc view_desc =
      cube ? RenderTextureViewDesc::TextureCube(host_format)
           : RenderTextureViewDesc::Texture2DArray(host_format);
  view_desc.componentMapping = component_mapping;
  entry.view = entry.texture->createTextureView(view_desc);
  entry.view_dim = cube ? 3 : 1;
  entry.width = width;
  entry.height = height;
  entry.host_format = host_format;

  // Uploads through the frame's ring with an inline copy on the direct queue;
  // D3D12 placed footprints need a 256-byte-aligned row pitch, so rows are
  // re-strided into the staging copy.
  const uint32_t row_bytes = width_blocks * bytes_per_block;
  const uint32_t padded_row_bytes = (row_bytes + 255u) & ~255u;
  const uint32_t staging_size = padded_row_bytes * height_blocks;
  auto& commandList = g_commandLists[g_frame];
  commandList->barriers(RenderBarrierStage::COPY,
                        RenderTextureBarrier(entry.texture.get(),
                                             RenderTextureLayout::COPY_DEST));
  std::vector<uint8_t> linear(data_size);
  for (uint32_t face = 0; face < face_count; face++) {
    const uint8_t* src = GuestPtr(base_address + face * face_stride);
    if (tiled) {
      // Packed mip tail (ring GetPackedMipOffset, mip 0): with min dimension <=
      // 16 texels the base level is packed inside the 32x32 tile, at y=16 when
      // wider than tall and at x=16 otherwise, in texel units scaled to blocks.
      uint32_t px = 0, py = 0;
      if (REXCVAR_GET(gpu_untile_packed) &&
          std::min(width, height) <= 16) {
        if (width > height) {
          py = 16 / block_dim;
        } else {
          px = 16 / block_dim;
        }
      }
      UntileSurface(linear.data(), src, pitch_blocks, width_blocks,
                    height_blocks, bytes_per_block, px, py);
    } else {
      for (uint32_t y = 0; y < height_blocks; y++) {
        std::memcpy(linear.data() + y * width_blocks * bytes_per_block,
                    src + y * pitch_blocks * bytes_per_block,
                    width_blocks * bytes_per_block);
      }
    }
    // Endianness: 1 = 8-in-16, 2 = 8-in-32.
    if (endian == 1) SwapBytes16(linear.data(), linear.size());
    if (endian == 2) SwapBytes32(linear.data(), linear.size());

    UploadAllocation staging =
        g_upload[g_frame].Allocate((staging_size + 511) & ~511u, 512);
    if (padded_row_bytes == row_bytes) {
      std::memcpy(staging.memory, linear.data(), data_size);
    } else {
      for (uint32_t y = 0; y < height_blocks; y++) {
        std::memcpy(staging.memory + y * padded_row_bytes,
                    linear.data() + y * row_bytes, row_bytes);
      }
    }
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(entry.texture.get(), 0, face),
        RenderTextureCopyLocation::PlacedFootprint(
            staging.buffer, host_format, width, height, 1,
            (padded_row_bytes / bytes_per_block) * block_dim, staging.offset));
  }
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(entry.texture.get(),
                                             RenderTextureLayout::SHADER_READ));
  entry.layout = RenderTextureLayout::SHADER_READ;

  entry.descriptor_index = AllocTextureDescriptor();
  if (entry.descriptor_index) {
    g_textureDescriptorSet->setTexture(entry.descriptor_index,
                                       entry.texture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       entry.view.get());
  }
  entry.valid = true;
  // Fingerprint of the uploaded content, which the re-upload check
  // (gpu_tex_reupload) compares against live guest memory.
  if (!entry.host_rendered && width <= 256 && height <= 256) {
    if (const uint8_t* texels = GuestPtr(base_address)) {
      entry.content_hash = XXH3_64bits(texels, 2048);
    }
  }
  if (!entry.host_rendered) {
    entry.sample_size = data_size;
    entry.sample_hash = SampleTexels(base_address, data_size);
  }
  {
    auto prior = g_textureCache.find(key);
    if (prior != g_textureCache.end()) RetireEntry(prior->second);
  }
  entry.last_bind_frame = g_frameIndex;
  entry.approx_bytes = entry.texture ? ApproxTexBytes(entry.width, entry.height, entry.host_format) : 0;
  g_texCacheBytes += entry.approx_bytes;
  entry.header_addr = header_addr;
  std::memcpy(entry.header_dw, dw, sizeof(entry.header_dw));
  return &(g_textureCache[key] = std::move(entry));
}

// ---- Render-target retargeting ----

// Registry of surfaces created through the CreateSurface hook, unused when the
// recompiled XDK CreateSurface runs.
std::unordered_map<uint32_t, SurfaceInfo> g_surfaces;

// Parses an XDK D3DSurface object (CreateSurface 0x8252BB30, 48 bytes): +20 =
// 0xFFFF0000 stamp, +36 = ((w-1)<<18)|((h-1)<<3), +40 = format dword.
SurfaceInfo ParseSurface(uint32_t surface_obj) {
  SurfaceInfo info;
  if (!surface_obj) return info;
  auto it = g_surfaces.find(surface_obj);
  if (it != g_surfaces.end()) return it->second;
  // Only surface pointers reach SetRenderTarget, so validation is by
  // plausibility, since the +20 fence field is 0xFFFF0000 only at init.
  const uint32_t size_dword = LoadGuestU32(surface_obj + 36);
  info.width = (size_dword >> 18) + 1;
  info.height = ((size_dword >> 3) & 0x7FFF) + 1;
  info.guest_format = LoadGuestU32(surface_obj + 40);
  // Surface+28 is the RB_COLOR_INFO dword computed by D3D_SurfaceInitFields
  // (0x8252B540): bits 0-11 EDRAM base tile, 16-19 hw color format, 20-25
  // signed color exponent bias.
  const uint32_t edram_info = LoadGuestU32(surface_obj + 28);
  info.exp_bias = int32_t(edram_info << 6) >> 26;  // sign-extend bits 20-25
  // RTs are not keyed by EDRAM base, so the base tile (low 12 bits of
  // RB_COLOR_INFO) is left at 0.
  info.edram_base = 0;
  info.valid = (size_dword & 7) == 0 && info.width > 1 &&
               info.width <= 8192 && info.height > 1 && info.height <= 8192;
  return info;
}

RenderFormat TranslateSurfaceFormat(uint32_t guest_format) {
  // D3DFORMAT-packed like fetch dword1, GPUTEXTUREFORMAT in the low 6 bits; the
  // raw dword is stored at surface+40, so both 7 and its alias 54 arrive here.
  switch (guest_format & 0x3F) {
    case 6: return RenderFormat::R8G8B8A8_UNORM;  // k_8_8_8_8
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16 (RTT type 11, D3DFMT 0x18280136)
      // 10-bit host RTs, matching the ring and keeping these apart from
      // same-size 8888 RTs in the (w, h, host format) key.
      return RenderFormat::R10G10B10A2_UNORM;
    case 32:
      // k_16_16_16_16_FLOAT, CreateRenderToTexture type 3 (D3DFMT 0x1A22AB60,
      // A16B16G16R16F): the bloom chain, which needs HDR range and a host
      // format distinct from the 8888 RTs.
      return RenderFormat::R16G16B16A16_FLOAT;
    default: {
      static std::set<uint32_t> logged;
      if (logged.insert(guest_format).second) {
        REXGPU_WARN(
            "videonative: [rtlog] unhandled surface D3DFMT {:#010x} "
            "(data_format={} signs={:#x} numfmt={}) -> RGBA8 fallback",
            guest_format, guest_format & 0x3F, (guest_format >> 9) & 0xFF,
            (guest_format >> 17) & 1);
      }
      return RenderFormat::R8G8B8A8_UNORM;  // conservative default
    }
  }
}

uint64_t RtKeyOf(uint32_t width, uint32_t height,
                 const RenderFormat formats[kMrtCount], uint32_t edram_base) {
  // Slot formats folded into the low 16 bits, weighted so slot order matters;
  // the surface format set is small enough that distinct weights cannot
  // collide.
  const uint64_t fmix =
      (uint64_t(formats[0]) * 131 + uint64_t(formats[1]) * 31 +
       uint64_t(formats[2]) * 17 + uint64_t(formats[3]) * 7) &
      0xFFFFu;
  return (uint64_t(edram_base) << 48) | (uint64_t(width) << 32) |
         (uint64_t(height) << 16) | fmix;
}

// RT-cache VRAM accounting (bounded by the sweep in the stats block).
uint64_t g_rtCacheBytes = 0;
std::deque<std::pair<uint64_t, CachedRenderTarget>> g_retiredRts;

CachedRenderTarget* GetOrCreateRenderTarget(uint32_t width, uint32_t height,
                                            const RenderFormat formats[kMrtCount],
                                            uint32_t edram_base,
                                            bool full_res = false) {
  // Implausible dimensions from transient garbage surface parses are rejected;
  // the cap is the largest legitimate shape, CC1's grown 2080x2048 shadow/bake
  // RT.
  if (uint64_t(width) * height > 2080ull * 2048ull) {
    static uint64_t whale_logs = 8;
    if (whale_logs) {
      whale_logs--;
      REXGPU_WARN(
          "videonative: [rtlog] rejected implausible RT request {}x{} "
          "(garbage surface parse) — draws at this binding are skipped",
          width, height);
    }
    return nullptr;
  }
  const uint64_t key = RtKeyOf(width, height, formats, edram_base) |
                       (full_res ? (1ull << 63) : 0ull);
  auto it = g_renderTargets.find(key);
  if (it != g_renderTargets.end()) {
    it->second.last_bind_frame = g_frameIndex;
    return &it->second;
  }

  CachedRenderTarget rt;
  rt.full_res = full_res;
  const uint32_t host_w = full_res ? width : HostDim(width);
  const uint32_t host_h = full_res ? height : HostDim(height);
  const RenderTexture* colors[kMrtCount];
  for (uint32_t i = 0; i < kMrtCount; i++) {
    rt.color[i] = g_device->createTexture(
        RenderTextureDesc::ColorTarget(host_w, host_h, formats[i]));
    rt.color[i]->setName(fmt::format("rt color{} {}x{} (host {}x{})", i,
                                     width, height, host_w, host_h));
    colors[i] = rt.color[i].get();
  }
  // The depth resource is typeless when the shader resolve path is on, so the
  // plane can also be read through an R32_FLOAT SRV; the DSV and pipelines keep
  // D32_FLOAT since plume specializes R32_TYPELESS -> D32_FLOAT for depth
  // views.
  rt.depth = g_device->createTexture(RenderTextureDesc::DepthTarget(
      host_w, host_h,
      REXCVAR_GET(gpu_depth_resolve_shader)
          ? RenderFormat::R32_TYPELESS
          : kDepthFormat));
  rt.depth->setName(
      fmt::format("rt depth {}x{} (host {}x{})", width, height, host_w, host_h));
  RenderFramebufferDesc desc;
  desc.colorAttachments = colors;
  desc.colorAttachmentsCount = kMrtCount;
  desc.depthAttachment = rt.depth.get();
  rt.framebuffer = g_device->createFramebuffer(desc);
  rt.width = width;
  rt.height = height;
  rt.edram_base = edram_base;
  for (uint32_t i = 0; i < kMrtCount; i++) rt.formats[i] = formats[i];
  rt.last_bind_frame = g_frameIndex;
  rt.approx_bytes = 0;
  for (uint32_t i = 0; i < kMrtCount; i++) {
    rt.approx_bytes += ApproxTexBytes(host_w, host_h, formats[i]);
  }
  rt.approx_bytes += uint64_t(host_w) * host_h * 4;  // depth
  g_rtCacheBytes += rt.approx_bytes;
  CachedRenderTarget* stored = &(g_renderTargets[key] = std::move(rt));
  // All attachments are zero-initialized, since the game resolves RT1-3 content
  // from passes that never write them.
  if (g_frameOpen) {
    auto& cl = g_commandLists[g_frame];
    RenderTextureBarrier init_barriers[kMrtCount + 1];
    for (uint32_t i = 0; i < kMrtCount; i++) {
      init_barriers[i] = RenderTextureBarrier(stored->color[i].get(),
                                              RenderTextureLayout::COLOR_WRITE);
      stored->color_layout[i] = RenderTextureLayout::COLOR_WRITE;
    }
    init_barriers[kMrtCount] = RenderTextureBarrier(
        stored->depth.get(), RenderTextureLayout::DEPTH_WRITE);
    stored->depth_layout = RenderTextureLayout::DEPTH_WRITE;
    cl->barriers(RenderBarrierStage::GRAPHICS, init_barriers, kMrtCount + 1);
    cl->setFramebuffer(stored->framebuffer.get());
    for (uint32_t i = 0; i < kMrtCount; i++) {
      cl->clearColor(i, RenderColor(0.0f, 0.0f, 0.0f, 0.0f), nullptr, 0);
    }
    cl->clearDepth(true, 0.0f, nullptr, 0);
    // The caller (ApplyRenderTargetState) re-binds its framebuffer right after,
    // forced here even if the key matches.
    g_activeRtKey = ~0ull;
  }
  return stored;
}

uint64_t RtKey(const CachedRenderTarget* rt) {
  return RtKeyOf(rt->width, rt->height, rt->formats, rt->edram_base) |
         (rt->full_res ? (1ull << 63) : 0ull);
}

// Seeds a freshly bound full-res variant from its half-res sibling's current
// content (defined after the blit resources below).
void SeedFullResVariant(CachedRenderTarget* rt, uint64_t key);

// The active pass is depth-only (shadow map) and renders Y-flipped (see
// SetupDraw) so the resolved depth texture matches the guest row order the
// game's analytic light-space UVs assume.
bool g_depthOnlyTarget = false;

// Live RTT note (see gpu_sem_rtt): set by NoteRttBegin, applied per
// bind only when the bound surface EA equals one of the note's own EDRAM
// surfaces, and cleared at frame end.
struct SemRttNote {
  bool active = false;
  uint32_t texbase = 0, width = 0, height = 0;
  uint32_t color_surf = 0, depth_surf = 0, dest_tex = 0;
  uint32_t format = 0, msaa = 0, tiling = 0;
};
SemRttNote g_semRtt;

// ApplyRenderTargetState points the command list at the framebuffer for the
// bound guest surfaces (swapchain when none); g_currentDevice is the device EA
// of the draw being set up.
uint32_t g_currentDevice = 0;
// The raw D3D device (the setter's a1, distinct from the draw device), recorded
// from r3 by the SetRenderTarget hook and cached for the run.
uint32_t g_rawDeviceEA = 0;

void ApplyRenderTargetState() {
  auto& commandList = g_commandLists[g_frame];
  g_depthOnlyTarget = false;
  SurfaceInfo info = ParseSurface(g_state.color_surface[0]);
  if (!info.valid) {
    // Depth-only pass (no color surface bound, the sun shadow map): a host RT
    // sized to the depth surface is bound so z lands in a resolvable depth
    // texture.
    const SurfaceInfo dinfo = ParseSurface(g_state.depth_surface);
    if (dinfo.valid) {
      info = dinfo;
      info.guest_format = 6;  // color attachment unused (PS-less, mask 0)
      g_depthOnlyTarget = true;
    } else {
      if (g_activeRtKey != 0) {
        // Back to the swapchain framebuffer.
        commandList->setFramebuffer(g_framebuffers[g_backBufferIndex].get());
        g_activeRtKey = 0;
        g_activeRtFullRes = false;
        g_activeRtFormat = kColorFormat;
        for (uint32_t i = 0; i < kMrtCount; i++) {
          g_activeRtFormats[i] = kColorFormat;
        }
        g_activeRtGrown = false;
        g_activeRtHeight = 720;
        g_activeRtWidth = 1280;
        g_passResolves = 0;
        if (REXCVAR_GET(gpu_bind_scissor_reset) &&
            !g_scissorExplicit) {
          g_state.scissor[0] = 0;
          g_state.scissor[1] = 0;
          g_state.scissor[2] = 1280;
          g_state.scissor[3] = 720;
        }
        g_scissorExplicit = false;
      }
      return;
    }
  }

  // RT extent: an RTT note whose EDRAM surface is the one bound supplies
  // logical dims and identity, otherwise genuine tile bands (full tiling width
  // within pitch padding, shorter height) are expanded to the BeginTiling
  // extent or to the viewport-derived growth request.
  bool sem_rtt_applied = false;
  if (REXCVAR_GET(gpu_sem_rtt) && g_semRtt.active &&
      g_semRtt.width && g_semRtt.height) {
    const bool match =
        (g_state.color_surface[0] &&
         g_state.color_surface[0] == g_semRtt.color_surf) ||
        (g_depthOnlyTarget && g_state.depth_surface &&
         g_state.depth_surface == g_semRtt.depth_surf);
    if (match) {
      info.width = g_semRtt.width;
      info.height = g_semRtt.height;
      info.edram_base =
          ((g_semRtt.texbase >> 4) ^ (g_semRtt.texbase >> 16)) & 0xFFFFu;
      sem_rtt_applied = true;
    }
  }
  bool grown = false;
  if (sem_rtt_applied) {
    // Dims are authoritative, so no tiling or viewport growth heuristics apply.
  } else if (g_tilingWidth && g_tilingHeight && info.width >= g_tilingWidth &&
      info.width <= g_tilingWidth + 64 && info.height < g_tilingHeight) {
    info.height = g_tilingHeight;
  } else if (g_growWidth && g_growHeight && g_growSurface &&
             (g_state.color_surface[0] == g_growSurface ||
              g_state.depth_surface == g_growSurface) &&
             info.width >= g_growWidth && info.width <= g_growWidth + 64 &&
             info.height < g_growHeight) {
    info.height = g_growHeight;
    grown = true;
  }

  const RenderFormat format = TranslateSurfaceFormat(info.guest_format);
  // Per-slot attachment formats: slot 0 from the primary surface, slots 1-3
  // from their own bound surfaces when present, and unbound slots inherit slot
  // 0's format.
  RenderFormat formats[kMrtCount];
  formats[0] = format;
  for (uint32_t i = 1; i < kMrtCount; i++) {
    formats[i] = format;
    if (!g_depthOnlyTarget && g_state.color_surface[i]) {
      const SurfaceInfo si = ParseSurface(g_state.color_surface[i]);
      if (si.valid) formats[i] = TranslateSurfaceFormat(si.guest_format);
    }
  }
  // performance_mode UI sharpness: the learned composite+UI surface gets a 1:1
  // host RT variant, in downscale mode only (g_hsNum < g_hsDen).
  bool want_full = false;
  if (Scaled() && g_hsNum < g_hsDen && !g_depthOnlyTarget &&
      REXCVAR_GET(gpu_perfui)) {
    auto lit = g_fullResSurfaces.find(g_state.color_surface[0]);
    want_full = lit != g_fullResSurfaces.end() &&
                g_frameIndex - lit->second <= kFullResConfirmWindow;
  }
  // Register-level RT identity fold: the RB shadow words on the raw device
  // distinguish EDRAM retargets the surface object never shows.
  if (!sem_rtt_applied && REXCVAR_GET(gpu_rt_key_rbinfo) &&
      g_rawDeviceEA) {
    const uint32_t rb0 = LoadGuestU32(g_rawDeviceEA + 4u * 2593u);
    const uint32_t rbd = LoadGuestU32(g_rawDeviceEA + 4u * 2594u);
    const uint32_t salt =
        (rb0 ^ (rb0 >> 16) ^ (rbd << 8) ^ (rbd >> 8)) & 0xFFFFu;
    info.edram_base ^= salt;
  }
  // The other bound surfaces (color 1-3 and depth) are folded into the RT
  // identity, matching the ring's (SURF, C0-C3, DEPTH) pass key, using the
  // spare key bits above the 12-bit EDRAM tile index.
  if (!sem_rtt_applied && REXCVAR_GET(gpu_pass_separation) &&
      !g_depthOnlyTarget) {
    uint32_t fold = 0;
    for (uint32_t i = 1; i < kMrtCount; i++) {
      if (g_state.color_surface[i]) {
        const SurfaceInfo si = ParseSurface(g_state.color_surface[i]);
        if (si.valid) fold ^= (si.edram_base + 0x9E37u * i);
      }
    }
    const SurfaceInfo di = ParseSurface(g_state.depth_surface);
    if (di.valid) fold ^= di.edram_base * 0x85EBu;
    fold = (fold ^ (fold >> 4) ^ (fold >> 8) ^ (fold >> 12)) & 0xFu;
    info.edram_base ^= fold << 12;
  }
  CachedRenderTarget* rt = GetOrCreateRenderTarget(
      info.width, info.height, formats, info.edram_base, want_full);
  if (!rt) {
    // On an implausible-dimension rejection the swapchain is bound so state
    // stays sane.
    if (g_activeRtKey != 0) {
      commandList->setFramebuffer(g_framebuffers[g_backBufferIndex].get());
      g_activeRtKey = 0;
      g_activeRtFullRes = false;
      g_activeRtFormat = kColorFormat;
      for (uint32_t i = 0; i < kMrtCount; i++) g_activeRtFormats[i] = kColorFormat;
      g_activeRtGrown = false;
      g_activeRtHeight = 720;
      g_activeRtWidth = 1280;
      g_passResolves = 0;
    }
    return;
  }
  const uint64_t key = RtKey(rt);
  g_activeRtFullRes = want_full;
  g_activeRtHeight = info.height;
  g_activeRtWidth = info.width;
  if (key == g_activeRtKey) {
    // Same target, so nothing rebinds, but a lazy mid-pass resolve may have
    // left surfaces in COPY_SOURCE and they are repaired on demand.
    RepairActiveRtLayouts();
    return;
  }

  uint32_t barrier_count = 0;
  RenderTextureBarrier barriers[kMrtCount + 1];
  for (uint32_t i = 0; i < kMrtCount; i++) {
    if (rt->color_layout[i] != RenderTextureLayout::COLOR_WRITE ||
        LazyLedgerStale(rt->color[i].get(), rt->color_layout[i], "bind-color",
                        key, i)) {
      barriers[barrier_count++] = RenderTextureBarrier(
          rt->color[i].get(), RenderTextureLayout::COLOR_WRITE);
      rt->color_layout[i] = RenderTextureLayout::COLOR_WRITE;
    }
  }
  if (rt->depth_layout != RenderTextureLayout::DEPTH_WRITE ||
      LazyLedgerStale(rt->depth.get(), rt->depth_layout, "bind-depth", key,
                      0)) {
    barriers[barrier_count++] =
        RenderTextureBarrier(rt->depth.get(), RenderTextureLayout::DEPTH_WRITE);
    rt->depth_layout = RenderTextureLayout::DEPTH_WRITE;
  }
  if (barrier_count) {
    commandList->barriers(RenderBarrierStage::GRAPHICS, barriers,
                          barrier_count);
  }
  commandList->setFramebuffer(rt->framebuffer.get());
  g_activeRtKey = key;
  g_activeRt = rt;
  g_activeRtLayoutsDirty = false;  // the block above restored WRITE states
  g_activeRtFormat = format;
  for (uint32_t i = 0; i < kMrtCount; i++) g_activeRtFormats[i] = rt->formats[i];
  g_activeRtGrown = grown;
  g_passResolves = 0;
  // The first bind of a full-res variant each frame seeds it from the half-res
  // sibling, since the game's frame tail expects the composited scene to
  // already be in EDRAM.
  if (want_full) SeedFullResVariant(rt, key);
  // XDK window-scissor semantics: SetRenderTarget resets the scissor to the
  // full target, so RTs larger than the screen do not inherit a stale screen
  // scissor.
  if (REXCVAR_GET(gpu_bind_scissor_reset) && !g_scissorExplicit) {
    g_state.scissor[0] = 0;
    g_state.scissor[1] = 0;
    g_state.scissor[2] = int32_t(info.width);
    g_state.scissor[3] = int32_t(info.height);
  }
  g_scissorExplicit = false;
}

// ---- Pipeline cache ----

struct PipelineKey {
  const void* vs_dxil;
  const void* ps_dxil;
  uint32_t blend_control[kMrtCount];  // RB_BLENDCONTROL0-3 (per-RT)
  uint32_t depth_control;
  uint32_t cull_bits;
  uint32_t topology;
  uint32_t rect_gs;     // RECTLIST expansion GS attached
  uint32_t point_gs;    // POINTLIST expansion GS attached
  uint32_t clip_gs;     // user-clip-plane GS attached
  uint32_t rt_formats[kMrtCount];  // per-slot color target formats
  uint32_t rt_count;    // color attachment count (4 for cached RTs, 1 swap)
  uint32_t color_mask;  // RB_COLOR_MASK (4 nibbles = RT0-3 write masks)
  bool operator==(const PipelineKey& o) const {
    return !std::memcmp(this, &o, sizeof(o));
  }
};
struct PipelineKeyHash {
  size_t operator()(const PipelineKey& k) const {
    return size_t(XXH3_64bits(&k, sizeof(k)));
  }
};

struct CachedPipeline {
  std::unique_ptr<RenderShader> vs;
  std::unique_ptr<RenderShader> ps;
  std::unique_ptr<RenderPipeline> pipeline;
};
std::unordered_map<PipelineKey, CachedPipeline, PipelineKeyHash> g_pipelines;

// Rectangle-list expansion GS, created on the first rect pipeline and shared by
// all.
std::unique_ptr<RenderShader> g_rectGs;
bool g_rectGsTried = false;

// Point-sprite expansion GS (POINTLIST -> quads sized by the VS oPts export).
std::unique_ptr<RenderShader> g_pointGs;
bool g_pointGsTried = false;

// User-clip-plane GS (passthrough emitting SV_ClipDistance from b0 c2..c7),
// attached only when the PA_CL_CLIP_CNTL UCP mask is nonzero.
std::unique_ptr<RenderShader> g_clipGs;
bool g_clipGsTried = false;

// FPS overlay resources, drawn by DrawFpsOverlay at the tail of the Swap
// composite and created on first visible tick.
std::unique_ptr<RenderTexture> g_fpsTexture;
std::unique_ptr<RenderTextureView> g_fpsView;
uint32_t g_fpsDescriptor = 0;

// Fullscreen frontbuffer blit (Swap composite), created on first use.
std::unique_ptr<RenderShader> g_blitVs, g_blitPs;
std::unique_ptr<RenderPipeline> g_blitPipeline;
// FPS overlay variant of the blit pipeline with alpha blending, so glyph
// outlines composite over the scene with no backdrop box.
std::unique_ptr<RenderPipeline> g_fpsPipeline;
// ImGui overlay resources (DrawUiOverlay, after the FPS strip).
std::unique_ptr<RenderPipeline> g_overlayPipeline;
std::unique_ptr<RenderShader> g_overlayVs;
std::unique_ptr<RenderShader> g_overlayPs;
struct OverlayHostTexture {
  std::weak_ptr<overlay::TextureData> owner;
  std::unique_ptr<RenderTexture> texture;
  std::unique_ptr<RenderTextureView> view;
  uint32_t descriptor = 0;
};
std::unordered_map<const overlay::TextureData*, OverlayHostTexture> g_overlayTextures;
// The overlay frame each in-flight command list drew, keeping its textures
// alive until the slot's fence has passed.
std::shared_ptr<const overlay::Frame> g_overlayInFlight[kNumFrames];
std::shared_ptr<overlay::TextureData> g_overlayWhite;

std::unique_ptr<RenderShader> g_fpsPs;  // alpha-preserving blit PS variant
// Image rect of the last frontbuffer blit (x, y, w, h) in swapchain pixels, so
// overlays anchor to the game image and not the window.
float g_presentRect[4] = {0.0f, 0.0f, 0.0f, 0.0f};
bool g_blitTried = false;
// Frontbuffer-flip fallback: the game may Swap the texture of the pair that has
// not received a resolve yet, so g_lastResolvedBySize tracks the last resolve
// per guest size.
uint64_t SizeKeyOf(uint32_t width, uint32_t height) {
  return (uint64_t(width) << 32) | height;
}

// Seed pipelines for the full-res UI variant: FSR 1.0 as EASU (half-res sibling
// -> full-res scratch) then RCAS (scratch -> variant), falling back to EASU
// direct into the variant and then to the plain bilinear blit PS.
std::unique_ptr<RenderPipeline> g_seedPipeline;      // bilinear -> variant
std::unique_ptr<RenderPipeline> g_seedEasuPipeline;  // EASU -> variant
std::unique_ptr<RenderPipeline> g_seedEasuScratchPipeline;  // EASU -> scratch
std::unique_ptr<RenderPipeline> g_seedRcasPipeline;         // RCAS -> variant
std::unique_ptr<RenderShader> g_easuPs;
std::unique_ptr<RenderShader> g_rcasPs;
uint64_t g_seedPipelineFmts = 0;
// The EASU intermediate (full-res, variant fmt0).
std::unique_ptr<RenderTexture> g_seedScratchTex;
std::unique_ptr<RenderTextureView> g_seedScratchView;
std::unique_ptr<RenderFramebuffer> g_seedScratchFb;
uint32_t g_seedScratchDescriptor = 0;
uint32_t g_seedScratchW = 0, g_seedScratchH = 0;
RenderFormat g_seedScratchFmt = RenderFormat::UNKNOWN;
// RCAS sharpness con = exp2(-stops), at the FSR1 demo default of 0.25 stops.
constexpr float kRcasCon = 0.84089641525f;

void SeedFullResVariant(CachedRenderTarget* rt, uint64_t key) {
  if (rt->seeded_frame == g_frameIndex) return;  // once per frame
  if (!g_blitVs || !g_blitPs) return;            // first Swap not reached yet
  auto sib = g_renderTargets.find(key & ~(1ull << 63));
  if (sib == g_renderTargets.end()) return;      // nothing to inherit
  CachedRenderTarget& half = sib->second;
  rt->seeded_frame = g_frameIndex;

  // Lazy SRV + bindless descriptor for the sibling's color[0].
  if (!half.seed_descriptor) {
    if (g_nextTextureDescriptor >= kFirstReservedDescriptor) return;
    half.seed_view = half.color[0]->createTextureView(
        RenderTextureViewDesc::Texture2DArray(half.formats[0]));
    half.seed_descriptor = g_nextTextureDescriptor++;
    g_textureDescriptorSet->setTexture(half.seed_descriptor,
                                       half.color[0].get(),
                                       RenderTextureLayout::SHADER_READ,
                                       half.seed_view.get());
  }
  const uint64_t fmts = (uint64_t(rt->formats[0]) << 48) |
                        (uint64_t(rt->formats[1]) << 32) |
                        (uint64_t(rt->formats[2]) << 16) |
                        uint64_t(rt->formats[3]);
  if ((!g_seedPipeline && !g_seedEasuPipeline) || g_seedPipelineFmts != fmts) {
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    desc.vertexShader = g_blitVs.get();
    desc.pixelShader = g_blitPs.get();
    for (uint32_t i = 0; i < kMrtCount; i++) {
      desc.renderTargetFormat[i] = rt->formats[i];
    }
    desc.renderTargetCount = kMrtCount;
    desc.depthTargetFormat = kDepthFormat;
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    desc.cullMode = RenderCullMode::NONE;
    g_seedPipeline = g_device->createGraphicsPipeline(desc);
    if (!g_easuPs) {
      g_easuPs = g_device->createShader(kEasuPsDxil, sizeof(kEasuPsDxil),
                                        "main", RenderShaderFormat::DXIL);
    }
    if (!g_rcasPs) {
      g_rcasPs = g_device->createShader(kRcasPsDxil, sizeof(kRcasPsDxil),
                                        "main", RenderShaderFormat::DXIL);
    }
    if (g_easuPs) {
      desc.pixelShader = g_easuPs.get();
      g_seedEasuPipeline = g_device->createGraphicsPipeline(desc);
    }
    if (g_rcasPs) {
      desc.pixelShader = g_rcasPs.get();
      g_seedRcasPipeline = g_device->createGraphicsPipeline(desc);
    }
    if (g_easuPs) {
      // EASU -> scratch: single RT, no depth.
      desc.pixelShader = g_easuPs.get();
      desc.renderTargetFormat[0] = rt->formats[0];
      for (uint32_t i = 1; i < kMrtCount; i++) {
        desc.renderTargetFormat[i] = RenderFormat::UNKNOWN;
      }
      desc.renderTargetCount = 1;
      desc.depthTargetFormat = RenderFormat::UNKNOWN;
      g_seedEasuScratchPipeline = g_device->createGraphicsPipeline(desc);
    }
    g_seedPipelineFmts = fmts;
    if (!g_seedPipeline && !g_seedEasuPipeline) {
      REXGPU_WARN("videonative: [perfui] seed pipeline creation failed");
      return;
    }
  }
  // Scratch for the two-pass path (EASU output, RCAS input).
  const bool want_easu =
      g_seedEasuPipeline && REXCVAR_GET(gpu_perfui_easu);
  bool two_pass = want_easu && g_seedEasuScratchPipeline && g_seedRcasPipeline;
  if (two_pass && (g_seedScratchW != rt->width || g_seedScratchH != rt->height ||
                   g_seedScratchFmt != rt->formats[0])) {
    if (g_nextTextureDescriptor >= kFirstReservedDescriptor) {
      two_pass = false;
    } else {
      g_seedScratchTex = g_device->createTexture(
          RenderTextureDesc::ColorTarget(rt->width, rt->height,
                                         rt->formats[0]));
      if (g_seedScratchTex) {
        g_seedScratchTex->setName("perfui seed scratch");
        g_seedScratchView = g_seedScratchTex->createTextureView(
            RenderTextureViewDesc::Texture2DArray(rt->formats[0]));
        if (!g_seedScratchDescriptor) {
          g_seedScratchDescriptor = g_nextTextureDescriptor++;
        }
        g_textureDescriptorSet->setTexture(g_seedScratchDescriptor,
                                           g_seedScratchTex.get(),
                                           RenderTextureLayout::SHADER_READ,
                                           g_seedScratchView.get());
        const RenderTexture* color = g_seedScratchTex.get();
        RenderFramebufferDesc fbDesc;
        fbDesc.colorAttachments = &color;
        fbDesc.colorAttachmentsCount = 1;
        fbDesc.depthAttachment = nullptr;
        g_seedScratchFb = g_device->createFramebuffer(fbDesc);
        g_seedScratchW = rt->width;
        g_seedScratchH = rt->height;
        g_seedScratchFmt = rt->formats[0];
      }
      if (!g_seedScratchTex || !g_seedScratchFb) two_pass = false;
    }
  }

  auto& commandList = g_commandLists[g_frame];
  // Sibling color[0] goes to SHADER_READ for the sample and back to COLOR_WRITE
  // after, keeping its tracked layout consistent for the next scene pass.
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(half.color[0].get(),
                                             RenderTextureLayout::SHADER_READ));
  half.color_layout[0] = RenderTextureLayout::SHADER_READ;
  commandList->setViewports(
      RenderViewport(0.0f, 0.0f, float(rt->width), float(rt->height), 0.0f,
                     1.0f));
  commandList->setScissors(
      RenderRect(0, 0, int32_t(rt->width), int32_t(rt->height)));

  const uint32_t in_w = HostDim(half.width), in_h = HostDim(half.height);
  const float con0[4] = {
      float(in_w) / float(rt->width), float(in_h) / float(rt->height),
      0.5f * float(in_w) / float(rt->width) - 0.5f,
      0.5f * float(in_h) / float(rt->height) - 0.5f};
  const char* mode = "bilinear";
  if (two_pass) {
    mode = "EASU+RCAS";
    // Pass A: EASU -> scratch.
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(g_seedScratchTex.get(),
                             RenderTextureLayout::COLOR_WRITE));
    commandList->setFramebuffer(g_seedScratchFb.get());
    commandList->setPipeline(g_seedEasuScratchPipeline.get());
    uint32_t indicesA[32] = {half.seed_descriptor};
    std::memcpy(&indicesA[4], con0, sizeof(con0));
    indicesA[8] = in_w;
    indicesA[9] = in_h;
    UploadAllocation b4a = g_upload[g_frame].Allocate(sizeof(indicesA), 256);
    if (!b4a.memory) return;
    std::memcpy(b4a.memory, indicesA, sizeof(indicesA));
    commandList->setGraphicsRootDescriptor(b4a.buffer->at(b4a.offset),
                                           kRootB4IndicesPs);
    commandList->drawInstanced(3, 1, 0, 0);
    // Pass B: RCAS -> the variant.
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(g_seedScratchTex.get(),
                             RenderTextureLayout::SHADER_READ));
    commandList->setFramebuffer(rt->framebuffer.get());
    commandList->setPipeline(g_seedRcasPipeline.get());
    uint32_t indicesB[32] = {g_seedScratchDescriptor};
    const float rcas_con = kRcasCon;
    std::memcpy(&indicesB[4], &rcas_con, sizeof(rcas_con));
    indicesB[8] = rt->width;
    indicesB[9] = rt->height;
    UploadAllocation b4b = g_upload[g_frame].Allocate(sizeof(indicesB), 256);
    if (!b4b.memory) return;
    std::memcpy(b4b.memory, indicesB, sizeof(indicesB));
    commandList->setGraphicsRootDescriptor(b4b.buffer->at(b4b.offset),
                                           kRootB4IndicesPs);
    commandList->drawInstanced(3, 1, 0, 0);
  } else {
    // Single pass into the variant: EASU direct, else bilinear.
    RenderPipeline* pipeline =
        want_easu ? g_seedEasuPipeline.get() : g_seedPipeline.get();
    if (!pipeline) return;
    mode = want_easu ? "EASU" : "bilinear";
    commandList->setPipeline(pipeline);
    uint32_t indices[32] = {half.seed_descriptor};
    if (want_easu) {
      std::memcpy(&indices[4], con0, sizeof(con0));
      indices[8] = in_w;
      indices[9] = in_h;
    }
    UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(indices), 256);
    if (!b4.memory) return;
    std::memcpy(b4.memory, indices, sizeof(indices));
    commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                           kRootB4IndicesPs);
    commandList->drawInstanced(3, 1, 0, 0);
  }
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(half.color[0].get(),
                                             RenderTextureLayout::COLOR_WRITE));
  half.color_layout[0] = RenderTextureLayout::COLOR_WRITE;
  static uint64_t seed_logs = 6;
  if (seed_logs) {
    seed_logs--;
    REXGPU_INFO(
        "videonative: [perfui] seeded full-res variant {}x{} from the "
        "scaled sibling ({}; frame {})",
        rt->width, rt->height, mode, g_frameIndex);
  }
}

RenderBlend TranslateBlend(uint32_t f) {
  switch (f) {
    case 0: return RenderBlend::ZERO;
    case 1: return RenderBlend::ONE;
    case 4: return RenderBlend::SRC_COLOR;
    case 5: return RenderBlend::INV_SRC_COLOR;
    case 6: return RenderBlend::SRC_ALPHA;
    case 7: return RenderBlend::INV_SRC_ALPHA;
    case 8: return RenderBlend::DEST_COLOR;
    case 9: return RenderBlend::INV_DEST_COLOR;
    case 10: return RenderBlend::DEST_ALPHA;
    case 11: return RenderBlend::INV_DEST_ALPHA;
    default: return RenderBlend::ONE;
  }
}
RenderBlendOperation TranslateBlendOp(uint32_t op) {
  switch (op) {
    case 0: return RenderBlendOperation::ADD;
    case 1: return RenderBlendOperation::SUBTRACT;
    case 2: return RenderBlendOperation::MIN;
    case 3: return RenderBlendOperation::MAX;
    case 4: return RenderBlendOperation::REV_SUBTRACT;
    default: return RenderBlendOperation::ADD;
  }
}
RenderComparisonFunction TranslateCompare(uint32_t f) {
  switch (f & 7) {
    case 0: return RenderComparisonFunction::NEVER;
    case 1: return RenderComparisonFunction::LESS;
    case 2: return RenderComparisonFunction::EQUAL;
    case 3: return RenderComparisonFunction::LESS_EQUAL;
    case 4: return RenderComparisonFunction::GREATER;
    case 5: return RenderComparisonFunction::NOT_EQUAL;
    case 6: return RenderComparisonFunction::GREATER_EQUAL;
    default: return RenderComparisonFunction::ALWAYS;
  }
}

const CachedPipeline* GetOrCreatePipeline(const ResolvedShader* vs,
                                          const ResolvedShader* ps,
                                          const uint32_t* blend_controls,
                                          uint32_t depth_control,
                                          uint32_t cull_bits,
                                          RenderPrimitiveTopology topology,
                                          bool rect, bool point, bool ucp,
                                          uint32_t color_mask) {
  // User clip planes ride a passthrough GS; rect/point expansion draws own the
  // GS stage, so those keep their expansion GS and go unclipped, with a warning
  // from the caller.
  if (ucp && (rect || point)) ucp = false;
  if (ucp) {
    if (!g_clipGsTried) {
      g_clipGsTried = true;
      const std::vector<uint8_t>& dxil = detail::ClipGsDxil();
      if (!dxil.empty()) {
        g_clipGs = g_device->createShader(dxil.data(), dxil.size(), "main",
                                          RenderShaderFormat::DXIL);
      }
    }
    // A pack without the clip GS draws unclipped instead of skipping the draw.
    if (!g_clipGs) ucp = false;
  }
  if (rect) {
    if (!g_rectGsTried) {
      g_rectGsTried = true;
      const std::vector<uint8_t>& dxil = detail::RectExpandGsDxil();
      if (!dxil.empty()) {
        g_rectGs = g_device->createShader(dxil.data(), dxil.size(), "main",
                                          RenderShaderFormat::DXIL);
      }
    }
    if (!g_rectGs) return nullptr;  // pack has no rect GS
  }
  if (point) {
    if (!g_pointGsTried) {
      g_pointGsTried = true;
      const std::vector<uint8_t>& dxil = detail::PointExpandGsDxil();
      if (!dxil.empty()) {
        g_pointGs = g_device->createShader(dxil.data(), dxil.size(), "main",
                                           RenderShaderFormat::DXIL);
      }
    }
    // Without the point GS, plain 1px points are drawn instead of skipping the
    // draw.
    if (!g_pointGs) point = false;
  }

  // The trimmed-signature VS variant is preferred for its lower per-vertex
  // attribute cost; GS pipelines keep the full variant since the expansion GS
  // links against the full oVar0-15+oPts signature, and the chosen blob's
  // pointer keys the pipeline.
  const bool wants_gs = rect || point || ucp;
  const int32_t trim_mode = REXCVAR_GET(gpu_vs_trim);
  const bool trim_enabled =
      trim_mode == 1 || (trim_mode == 2 && g_adapterIsIntel);
  const std::vector<uint8_t>& vs_blob =
      (!wants_gs && trim_enabled && !vs->pack->dxil_trim.empty())
          ? vs->pack->dxil_trim
          : vs->pack->dxil;

  PipelineKey key{};
  key.vs_dxil = vs_blob.data();
  // ps == nullptr is a deliberate PS-less depth-only draw, such as the CC2
  // depth restore whose VS writes oPos.z from the resolved depth texture.
  key.ps_dxil = ps ? ps->pack->dxil.data() : nullptr;
  for (uint32_t i = 0; i < kMrtCount; i++) {
    key.blend_control[i] = blend_controls[i];
  }
  key.depth_control = depth_control;
  key.cull_bits = cull_bits & 0xF;  // bit3 = y-flipped pass (winding inverts)
  key.topology = uint32_t(topology);
  key.rect_gs = rect ? 1 : 0;
  key.point_gs = point ? 1 : 0;
  key.clip_gs = ucp ? 1 : 0;
  for (uint32_t i = 0; i < kMrtCount; i++) {
    key.rt_formats[i] = uint32_t(g_activeRtFormats[i]);
  }
  key.rt_count = g_activeRtKey ? kMrtCount : 1;
  key.color_mask = color_mask & 0xFFFF;
  auto it = g_pipelines.find(key);
  if (it != g_pipelines.end()) return &it->second;

  CachedPipeline entry;
  entry.vs = g_device->createShader(vs_blob.data(), vs_blob.size(), "main",
                                    RenderShaderFormat::DXIL);
  if (ps) {
    entry.ps = g_device->createShader(ps->pack->dxil.data(),
                                      ps->pack->dxil.size(), "main",
                                      RenderShaderFormat::DXIL);
  }

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_pipelineLayout.get();
  desc.vertexShader = entry.vs.get();
  desc.pixelShader = entry.ps ? entry.ps.get() : nullptr;
  if (rect) desc.geometryShader = g_rectGs.get();
  if (point) desc.geometryShader = g_pointGs.get();
  if (ucp) desc.geometryShader = g_clipGs.get();
  for (uint32_t i = 0; i < key.rt_count; i++) {
    desc.renderTargetFormat[i] = g_activeRtFormats[i];
  }
  desc.renderTargetCount = key.rt_count;
  desc.depthTargetFormat = kDepthFormat;
  desc.primitiveTopology = topology;

  // RB_DEPTHCONTROL: bit1 z_enable, bit2 z_write, func bits 4..6.
  desc.depthEnabled = (depth_control >> 1) & 1;
  desc.depthWriteEnabled = (depth_control >> 2) & 1;
  desc.depthFunction = TranslateCompare(depth_control >> 4);
  desc.depthClipEnabled = true;

  // PA_SU_SC_MODE_CNTL: bit0 cull front, bit1 cull back, bit2 face, plus a
  // host-side bit3 for Y-flipped passes whose winding inverts; rectangles are
  // never culled on Xenos.
  if (rect) {
    desc.cullMode = RenderCullMode::NONE;
  } else if (cull_bits & 1) {
    desc.cullMode = RenderCullMode::FRONT;
  } else if (cull_bits & 2) {
    desc.cullMode = RenderCullMode::BACK;
  } else {
    desc.cullMode = RenderCullMode::NONE;
  }
  // Matches the ring's pipeline_cache.cpp, front_counter_clockwise = (face ==
  // 0), inverted for Y-flipped passes and by the gpu_cull_flip A/B
  // cvar.
  const bool front_ccw = (((cull_bits & 4) == 0) ^ ((cull_bits & 8) != 0)) ^
                         REXCVAR_GET(gpu_cull_flip);
  desc.frontFace = front_ccw ? RenderFrontFace::COUNTER_CLOCKWISE
                             : RenderFrontFace::CLOCKWISE;

  // RB_BLENDCONTROL0-3 (one per RT): srcblend 0:4, comb 5:7, dstblend 8:12,
  // alpha at 16+; RB_COLOR_MASK nibble i is RT i's RGBA write mask.
  for (uint32_t i = 0; i < key.rt_count; i++) {
    const uint32_t bc = blend_controls[i];
    RenderBlendDesc rt_blend;
    const uint32_t src = bc & 0x1F;
    const uint32_t op = (bc >> 5) & 0x7;
    const uint32_t dst = (bc >> 8) & 0x1F;
    const uint32_t src_a = (bc >> 16) & 0x1F;
    const uint32_t op_a = (bc >> 21) & 0x7;
    const uint32_t dst_a = (bc >> 24) & 0x1F;
    rt_blend.blendEnabled = !(src == 1 && dst == 0 && op == 0 && src_a == 1 &&
                              dst_a == 0 && op_a == 0);
    rt_blend.srcBlend = TranslateBlend(src);
    rt_blend.dstBlend = TranslateBlend(dst);
    rt_blend.blendOp = TranslateBlendOp(op);
    rt_blend.srcBlendAlpha = TranslateBlend(src_a);
    rt_blend.dstBlendAlpha = TranslateBlend(dst_a);
    rt_blend.blendOpAlpha = TranslateBlendOp(op_a);
    rt_blend.renderTargetWriteMask = uint8_t((color_mask >> (4 * i)) & 0xF);
    if (!ps) {
      // Depth-only: no PS means undefined color output, so all writes are
      // masked.
      rt_blend.blendEnabled = false;
      rt_blend.renderTargetWriteMask = 0;
    }
    desc.renderTargetBlend[i] = rt_blend;
  }

  entry.pipeline = g_device->createGraphicsPipeline(desc);
  // A PS reading an interpolant its VS never writes fails linkage against the
  // trimmed signature, so creation is retried once with the full-signature VS
  // and logged.
  if (!entry.pipeline && vs_blob.data() == vs->pack->dxil_trim.data() &&
      !vs->pack->dxil.empty()) {
    entry.vs = g_device->createShader(vs->pack->dxil.data(),
                                      vs->pack->dxil.size(), "main",
                                      RenderShaderFormat::DXIL);
    desc.vertexShader = entry.vs.get();
    entry.pipeline = g_device->createGraphicsPipeline(desc);
    static uint64_t trim_retry_log = 8;
    if (trim_retry_log) {
      trim_retry_log--;
      REXGPU_WARN(
          "videonative: trimmed VS {:016X} failed linkage with ps={:016X} — "
          "retried with the full signature ({})",
          vs->ucode_hash, ps ? ps->ucode_hash : 0,
          entry.pipeline ? "ok" : "STILL FAILED");
    }
  }
  // A failed PSO caches null and its draws are skipped, so the shader pair is
  // logged to identify the feature at fault.
  if (!entry.pipeline) {
    static uint64_t pso_fail_log = 16;
    if (pso_fail_log) {
      pso_fail_log--;
      REXGPU_WARN(
          "videonative: pipeline creation failed vs={:016X} ps={:016X} "
          "topo={} rect={} point={} mask={:#x} — draws with this pair skip",
          vs ? vs->ucode_hash : 0, ps ? ps->ucode_hash : 0,
          uint32_t(topology), rect, point, color_mask);
    }
  }
  return &(g_pipelines[key] = std::move(entry));
}

// ---- Frame flow ----

void CreateFramebuffers() {
  g_framebuffers.clear();
  RenderTextureDesc depthDesc = RenderTextureDesc::DepthTarget(
      g_swapChain->getWidth(), g_swapChain->getHeight(), kDepthFormat);
  g_depthTexture = g_device->createTexture(depthDesc);
  for (uint32_t i = 0; i < g_swapChain->getTextureCount(); i++) {
    const RenderTexture* color = g_swapChain->getTexture(i);
    const RenderTexture* depth = g_depthTexture.get();
    RenderFramebufferDesc desc;
    desc.colorAttachments = &color;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth;
    g_framebuffers.push_back(g_device->createFramebuffer(desc));
  }
}

}  // namespace

bool Init() {
  g_device = detail::Device();
  g_queue = detail::Queue();
  if (!g_device || !g_queue) return false;

  for (auto& cl : g_commandLists) cl = g_queue->createCommandList();
  for (auto& f : g_commandFences) f = g_device->createCommandFence();
  for (auto& s : g_acquireSemaphores) s = g_device->createCommandSemaphore();
  for (auto& s : g_renderSemaphores) s = g_device->createCommandSemaphore();

  void* hwnd = nullptr;
  if (auto* runtime = rex::Runtime::instance()) {
    if (auto* window = runtime->display_window()) {
      hwnd = window->GetNativeWindowHandle();
    }
  }
  g_inspectHwnd = hwnd;
  if (hwnd) {
    REXGPU_INFO("videonative: creating swapchain on hwnd {}",
                reinterpret_cast<uintptr_t>(hwnd));
    RenderSwapChainDesc swapDesc(static_cast<RenderWindow>(hwnd), kColorFormat,
                                 kSwapChainBufferCount);
    g_swapChain = g_queue->createSwapChain(swapDesc);
    // D3D/DXGI teardown never runs from CRT exit: this handler registers after
    // the graphics globals exist, so it runs before their destructors and leaks
    // them, since the swapchain destructor can deadlock in DXGI under
    // RenderDoc's hooks.
    static bool exit_guard_registered = false;
    if (!exit_guard_registered) {
      exit_guard_registered = true;
      std::atexit([] {
        g_swapChain.release();
        for (auto& cl : g_commandLists) cl.release();
        for (auto& f : g_commandFences) f.release();
        for (auto& s2 : g_acquireSemaphores) s2.release();
        for (auto& s2 : g_renderSemaphores) s2.release();
      });
    }
    if (g_swapChain) {
      g_swapChainValid = !g_swapChain->needsResize();
      if (g_swapChainValid) CreateFramebuffers();
    }
  }
  if (!g_swapChain) {
    REXGPU_WARN("videonative: no swapchain (hwnd {}) — headless", hwnd);
  }

  // Pipeline layout per the native shader contract: b1 (float constants) and b4
  // (descriptor indices) are per-stage, so two root params alias each with
  // vertex/pixel visibility, while b0/b2/b3 are stage-shared.
  RenderPipelineLayoutBuilder layoutBuilder;
  layoutBuilder.begin(false, true);
  layoutBuilder.addRootDescriptor(  // kRootB0System
      0, 0, RenderRootDescriptorType::CONSTANT_BUFFER);
  layoutBuilder.addRootDescriptor(  // kRootB1FloatsVs
      1, 0, RenderRootDescriptorType::CONSTANT_BUFFER,
      RenderShaderVisibility::VERTEX);
  layoutBuilder.addRootDescriptor(  // kRootB1FloatsPs
      1, 0, RenderRootDescriptorType::CONSTANT_BUFFER,
      RenderShaderVisibility::PIXEL);
  layoutBuilder.addRootDescriptor(  // kRootB2BoolLoop
      2, 0, RenderRootDescriptorType::CONSTANT_BUFFER);
  layoutBuilder.addRootDescriptor(  // kRootB3Fetch
      3, 0, RenderRootDescriptorType::CONSTANT_BUFFER);
  layoutBuilder.addRootDescriptor(  // kRootB4IndicesVs
      4, 0, RenderRootDescriptorType::CONSTANT_BUFFER,
      RenderShaderVisibility::VERTEX);
  layoutBuilder.addRootDescriptor(  // kRootB4IndicesPs
      4, 0, RenderRootDescriptorType::CONSTANT_BUFFER,
      RenderShaderVisibility::PIXEL);
  layoutBuilder.addRootDescriptor(  // kRootSharedMemSrv
      0, 0, RenderRootDescriptorType::SHADER_RESOURCE);
  layoutBuilder.addRootDescriptor(  // kRootDummyUav
      0, 0, RenderRootDescriptorType::UNORDERED_ACCESS);

  RenderDescriptorSetBuilder samplerSetBuilder;
  samplerSetBuilder.begin();
  samplerSetBuilder.addSampler(0, kSamplerDescriptorCount);
  samplerSetBuilder.end(true, kSamplerDescriptorCount);
  layoutBuilder.addDescriptorSet(samplerSetBuilder);  // set 0 -> space0

  RenderDescriptorSetBuilder textureSetBuilder;
  textureSetBuilder.begin();
  textureSetBuilder.addTexture(0, kTextureDescriptorCount);
  textureSetBuilder.end(true, kTextureDescriptorCount);
  layoutBuilder.addDescriptorSet(textureSetBuilder);  // set 1 -> space1 (2D)
  layoutBuilder.addDescriptorSet(textureSetBuilder);  // set 2 -> space2 (3D)
  layoutBuilder.addDescriptorSet(textureSetBuilder);  // set 3 -> space3 (cube)
  layoutBuilder.end();
  g_pipelineLayout = layoutBuilder.create(g_device);

  g_samplerDescriptorSet = samplerSetBuilder.create(g_device);
  g_textureDescriptorSet = textureSetBuilder.create(g_device);

  // Samplers: 0 = linear wrap, 1 = linear clamp, 2 = point wrap, 3 = point
  // clamp.
  for (uint32_t i = 0; i < 4; i++) {
    RenderSamplerDesc samplerDesc;
    const bool linear = i < 2;
    const bool wrap = (i & 1) == 0;
    samplerDesc.minFilter = linear ? RenderFilter::LINEAR : RenderFilter::NEAREST;
    samplerDesc.magFilter = samplerDesc.minFilter;
    samplerDesc.addressU = wrap ? RenderTextureAddressMode::WRAP
                                : RenderTextureAddressMode::CLAMP;
    samplerDesc.addressV = samplerDesc.addressU;
    samplerDesc.addressW = samplerDesc.addressU;
    auto sampler = g_device->createSampler(samplerDesc);
    g_samplerDescriptorSet->setSampler(i, sampler.get());
    g_samplers.push_back(std::move(sampler));
  }
  // Samplers 4-12: linear with per-axis address modes, index = 4 + u*3 + v,
  // mode order {WRAP, MIRROR, CLAMP}, since Xenos fetch constants carry clamp_x
  // and clamp_y independently.
  {
    static const RenderTextureAddressMode kAxisModes[3] = {
        RenderTextureAddressMode::WRAP, RenderTextureAddressMode::MIRROR,
        RenderTextureAddressMode::CLAMP};
    for (uint32_t u = 0; u < 3; u++) {
      for (uint32_t v = 0; v < 3; v++) {
        RenderSamplerDesc samplerDesc;
        samplerDesc.minFilter = RenderFilter::LINEAR;
        samplerDesc.magFilter = RenderFilter::LINEAR;
        samplerDesc.addressU = kAxisModes[u];
        samplerDesc.addressV = kAxisModes[v];
        samplerDesc.addressW = kAxisModes[v];
        auto sampler = g_device->createSampler(samplerDesc);
        g_samplerDescriptorSet->setSampler(4 + u * 3 + v, sampler.get());
        g_samplers.push_back(std::move(sampler));
      }
    }
  }
  // Slots 13-15 hold sampler 0 so every slot the shader-side clamp
  // (xe_sampler_index, min 15) can produce is valid; the sampler semantics path
  // reuses them as write-once dynamic slots.
  for (uint32_t i = 13; i < kSamplerDescriptorCount; i++) {
    g_samplerDescriptorSet->setSampler(i, g_samplers.front().get());
  }
  semantics::InitSamplerArm(g_device, g_samplerDescriptorSet.get());

  // Blank texture in every descriptor so unset slots are valid.
  g_blankTexture = g_device->createTexture(
      RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  g_blankTextureView = g_blankTexture->createTextureView(
      RenderTextureViewDesc::Texture2DArray(RenderFormat::R8G8B8A8_UNORM));
  for (uint32_t i = 0; i < kTextureDescriptorCount; i++) {
    g_textureDescriptorSet->setTexture(i, g_blankTexture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       g_blankTextureView.get());
  }
  // Dimension-matched blanks: TextureCube/Texture3D declarations never
  // dereference a 2D-array descriptor, so reserved slots serve a real cube and
  // a real 3D resource.
  g_blankTextureCube = g_device->createTexture(RenderTextureDesc::Texture(
      RenderTextureDimension::TEXTURE_2D, 1, 1, 1, 1, 6,
      RenderFormat::R8G8B8A8_UNORM));
  g_blankTextureCubeView = g_blankTextureCube->createTextureView(
      RenderTextureViewDesc::TextureCube(RenderFormat::R8G8B8A8_UNORM));
  g_textureDescriptorSet->setTexture(kBlankCubeDescriptor,
                                     g_blankTextureCube.get(),
                                     RenderTextureLayout::SHADER_READ,
                                     g_blankTextureCubeView.get());
  g_blankTexture3d = g_device->createTexture(RenderTextureDesc::Texture3D(
      1, 1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  g_blankTexture3dView = g_blankTexture3d->createTextureView(
      RenderTextureViewDesc::Texture3D(RenderFormat::R8G8B8A8_UNORM));
  g_textureDescriptorSet->setTexture(kBlank3dDescriptor,
                                     g_blankTexture3d.get(),
                                     RenderTextureLayout::SHADER_READ,
                                     g_blankTexture3dView.get());

  // The blank textures are zero-initialized, since committed resources are not
  // guaranteed cleared.
  {
    // Row pitch must be 256-byte aligned for buffer->texture copies, hence a
    // 64-texel row even though only texel 0 matters.
    auto staging = g_device->createBuffer(RenderBufferDesc::UploadBuffer(256));
    std::memset(staging->map(), 0, 256);
    staging->unmap();
    auto& initList = g_commandLists[0];
    initList->begin();
    const RenderTextureBarrier to_copy[] = {
        RenderTextureBarrier(g_blankTexture.get(),
                             RenderTextureLayout::COPY_DEST),
        RenderTextureBarrier(g_blankTextureCube.get(),
                             RenderTextureLayout::COPY_DEST),
        RenderTextureBarrier(g_blankTexture3d.get(),
                             RenderTextureLayout::COPY_DEST)};
    initList->barriers(RenderBarrierStage::COPY, nullptr, 0, to_copy, 3);
    initList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g_blankTexture.get(), 0),
        RenderTextureCopyLocation::PlacedFootprint(
            staging.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, 0));
    for (uint32_t face = 0; face < 6; face++) {
      initList->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(g_blankTextureCube.get(),
                                                 face),
          RenderTextureCopyLocation::PlacedFootprint(
              staging.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, 0));
    }
    initList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g_blankTexture3d.get(), 0),
        RenderTextureCopyLocation::PlacedFootprint(
            staging.get(), RenderFormat::R8G8B8A8_UNORM, 1, 1, 1, 64, 0));
    const RenderTextureBarrier to_read[] = {
        RenderTextureBarrier(g_blankTexture.get(),
                             RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(g_blankTextureCube.get(),
                             RenderTextureLayout::SHADER_READ),
        RenderTextureBarrier(g_blankTexture3d.get(),
                             RenderTextureLayout::SHADER_READ)};
    initList->barriers(RenderBarrierStage::GRAPHICS, nullptr, 0, to_read, 3);
    initList->end();
    const RenderCommandList* initLists[] = {initList.get()};
    g_queue->executeCommandLists(initLists, 1, nullptr, 0, nullptr, 0,
                                 g_commandFences[0].get());
    g_queue->waitForCommandFence(g_commandFences[0].get());
  }

  g_dummyUav = g_device->createBuffer(RenderBufferDesc::DefaultBuffer(
      256, RenderBufferFlag::UNORDERED_ACCESS));
  REXGPU_INFO("videonative: [gpuva] dummy uav base={:#x}+0x100",
              g_dummyUav->getDeviceAddress());
  g_breadcrumbBuffer =
      g_device->createBuffer(RenderBufferDesc::ReadbackBuffer(64));
  if (g_breadcrumbBuffer) {
    g_breadcrumbBuffer->setName("draw breadcrumbs");
    g_breadcrumbMap =
        reinterpret_cast<volatile uint32_t*>(g_breadcrumbBuffer->map());
  }
  // The diagnostics master switch (cvar diagnostics) applies before video init,
  // and this line records what the run captures.
  REXGPU_INFO("videonative: [breadcrumb] per-draw markers {}",
              REXCVAR_GET(gpu_breadcrumbs) ? "ON" : "OFF");

  REXGPU_INFO("videonative: renderer initialized (swapchain {}x{})",
              g_swapChain ? g_swapChain->getWidth() : 0,
              g_swapChain ? g_swapChain->getHeight() : 0);
  return true;
}

void Shutdown() {
  rq::StopAndJoin();
  for (uint32_t i = 0; i < kNumFrames; i++) {
    if (g_commandListPending[i]) {
      g_queue->waitForCommandFence(g_commandFences[i].get());
      g_commandListPending[i] = false;
    }
  }
  g_pipelines.clear();
  g_rectGs.reset();
  g_rectGsTried = false;
  g_pointGs.reset();
  g_pointGsTried = false;
  g_blitPipeline.reset();
  g_fpsPipeline.reset();
  g_fpsPs.reset();
  for (auto& in_flight : g_overlayInFlight) in_flight.reset();
  g_overlayTextures.clear();
  g_overlayPipeline.reset();
  g_overlayVs.reset();
  g_overlayPs.reset();
  g_blitVs.reset();
  g_blitPs.reset();
  g_blitTried = false;
  g_fpsTexture.reset();
  g_fpsView.reset();
  g_fpsDescriptor = 0;
  g_whiteTextureView.reset();
  g_whiteTexture.reset();
  g_whiteTextureDescriptor = 0;
  g_renderTargets.clear();
  g_lastResolvedBySize.clear();  // points into g_textureCache
  g_lastLightAccumResolve = nullptr;
  g_prevFmt19Resolve = nullptr;
  g_lastShadowMapResolve = nullptr;
  g_textureCache.clear();
  for (auto& slot : g_depthStaging) {
    slot.buffer.reset();
    slot.size = 0;
  }
  g_depthStagingRot = 0;
  g_depthStageMemo = {};  // never leave the memo pointing at a freed buffer
  if (g_breadcrumbBuffer) {
    g_breadcrumbMap = nullptr;
    g_breadcrumbBuffer->unmap();
    g_breadcrumbBuffer.reset();
  }
  g_framebuffers.clear();
  g_depthTexture.reset();
  g_swapChain.reset();
}

void EnsureFrameOpen() {
  if (g_frameOpen || !g_swapChain) return;

  if (g_swapChain->needsResize() || !g_swapChainValid) {
    for (uint32_t i = 0; i < kNumFrames; i++) {
      if (g_commandListPending[i]) {
        g_queue->waitForCommandFence(g_commandFences[i].get());
        g_commandListPending[i] = false;
      }
    }
    g_framebuffers.clear();
    g_swapChainValid = g_swapChain->resize();
    if (g_swapChainValid) CreateFramebuffers();
    // A resize that keeps failing means permanent black (device removed / TDR),
    // so it is logged; reason details land in plume_present_dbg.txt.
    if (!g_swapChainValid) {
      static uint64_t resize_fail_log = 8;
      if (resize_fail_log) {
        resize_fail_log--;
        REXGPU_WARN(
            "videonative: swapchain resize failed — rendering is down "
            "(device removed/TDR likely; see plume_present_dbg.txt)");
      }
    }
  }
  if (!g_swapChainValid) return;

  if (g_commandListPending[g_frame]) {
    g_queue->waitForCommandFence(g_commandFences[g_frame].get());
    g_commandListPending[g_frame] = false;
  }
  // Persistent-region reclamation: the region has no per-entry free, so when it
  // fills everything is flushed at this idle point after waiting the other
  // parity's fence too, rate-limited.
  if (g_vbPersistFlushWanted) {
    static uint64_t last_flush_tick = 0;
    const uint64_t now = rex::chrono::Clock::QueryHostTickCount();
    const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
    if (!last_flush_tick || now - last_flush_tick > 60 * freq) {
      last_flush_tick = now;
      for (uint32_t i = 0; i < kNumFrames; i++) {
        if (g_commandListPending[i]) {
          g_queue->waitForCommandFence(g_commandFences[i].get());
          g_commandListPending[i] = false;
        }
      }
      REXGPU_INFO(
          "videonative: [vbpersist] region flushed ({} MB, {} entries, "
          "orphan {} KB) — re-warming",
          g_vbPersistWatermark >> 20, g_vbPersistCache.size(),
          g_vbPersistOrphans >> 10);
      g_vbPersistCache.clear();
      g_vbPersistByGuest.clear();
      g_vbPersistWatermark = 0;
      g_vbPersistOrphans = 0;
    }
    g_vbPersistFlushWanted = false;
  }
  g_upload[g_frame].Reset();
  g_vertexRing[g_frame].Reset(kVbPersistCap);
  g_vbUploadCache.clear();

  if (!g_swapChain->acquireTexture(g_acquireSemaphores[g_frame].get(),
                                   &g_backBufferIndex)) {
    g_swapChainValid = false;
    return;
  }

  auto& commandList = g_commandLists[g_frame];
  commandList->begin();
  const RenderTextureBarrier frameBarriers[] = {
      RenderTextureBarrier(g_swapChain->getTexture(g_backBufferIndex),
                           RenderTextureLayout::COLOR_WRITE),
      RenderTextureBarrier(g_depthTexture.get(),
                           RenderTextureLayout::DEPTH_WRITE)};
  commandList->barriers(RenderBarrierStage::GRAPHICS, frameBarriers, 2);
  commandList->setFramebuffer(g_framebuffers[g_backBufferIndex].get());
  commandList->clearColor(0, RenderColor(0.0f, 0.0f, 0.05f, 1.0f));
  commandList->clearDepth(true, 1.0f);
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 3);
  g_frameOpen = true;
  g_drawsThisFrame = 0;
  // The frame opens on the swapchain framebuffer.
  g_activeRtKey = 0;
  g_activeRtFormat = kColorFormat;
}

// Pack equivalence prover: checks every pack pixel shader against the CPU Xenos
// ucode interpreter with identical float constants and constant interpolants,
// writing [prover] verdicts and <ucode_dir>/prover_report.txt.
REXCVAR_DEFINE_STRING(gpu_prove_pack, "", "Diagnostics",
                      "Ucode dump directory (shader_<HASH>.ucode.bin.frag) "
                      "against which every pack pixel shader is proven with "
                      "the CPU ucode interpreter at first present; "
                      "empty = off.");

#if CC1_SHADER_PROVER
namespace {

// Deterministic value streams, without libc rand, for reproducible results.
uint32_t ProverHash32(uint64_t x) {
  x ^= x >> 33;
  x *= 0xFF51AFD7ED558CCDull;
  x ^= x >> 33;
  x *= 0xC4CEB9FE1A85EC53ull;
  x ^= x >> 33;
  return uint32_t(x);
}
float ProverFloat(uint64_t domain, uint32_t index, uint32_t seed) {
  const uint32_t h = ProverHash32(domain * 0x9E3779B97F4A7C15ull + index +
                                  (uint64_t(seed) << 40));
  // Mix of magnitudes including negatives, in [-4, +4) with a bias toward [-1,
  // 1).
  const float unit = (h & 0xFFFFFF) / float(0x1000000);   // [0,1)
  const float scaled = (h & 0x8000000) ? unit : unit * 4.0f;
  return (h & 0x80000000u) ? -scaled : scaled;
}


}  // namespace

void ProvePack() {
  const std::string ucode_dir = REXCVAR_GET(gpu_prove_pack);
  if (ucode_dir.empty() || !g_device || !g_frameOpen) return;
  const ShaderCache& cache = PackCacheForProver();
  if (!cache.loaded()) {
    REXGPU_WARN("videonative: [prover] pack not loaded — nothing to prove");
    return;
  }
  constexpr uint32_t kSeeds = 8;
  constexpr uint32_t kRtDim = 4;
  auto& commandList = g_commandLists[g_frame];

  // Offscreen float RT, so the comparison has no UNORM quantization.
  auto rt_tex = g_device->createTexture(RenderTextureDesc::ColorTarget(
      kRtDim, kRtDim, RenderFormat::R32G32B32A32_FLOAT));
  const RenderTexture* rt_color = rt_tex.get();
  RenderFramebufferDesc fb_desc;
  fb_desc.colorAttachments = &rt_color;
  fb_desc.colorAttachmentsCount = 1;
  fb_desc.depthAttachment = nullptr;
  auto rt_fb = g_device->createFramebuffer(fb_desc);
  auto readback = g_device->createBuffer(
      RenderBufferDesc::ReadbackBuffer(kRtDim * kRtDim * 16 + 256));

  // Prover VS variants, one per interpolant count for exact-signature linkage.
  std::unique_ptr<RenderShader> prover_vs[17];
  for (int n = 0; n < 17; n++) {
    prover_vs[n] = g_device->createShader(kProverVsVariants[n].data,
                                          kProverVsVariants[n].size, "main",
                                          RenderShaderFormat::DXIL);
  }

  // Per-fetch-constant 1x1 float textures, whose values depend only on (fc,
  // seed) and are re-uploaded per seed.
  std::unique_ptr<RenderTexture> fc_tex[32];
  std::unique_ptr<RenderTextureView> fc_view[32];
  uint32_t fc_desc[32] = {};
  for (uint32_t fc = 0; fc < 32; fc++) {
    fc_tex[fc] = g_device->createTexture(RenderTextureDesc::Texture2D(
        1, 1, 1, RenderFormat::R32G32B32A32_FLOAT));
    fc_view[fc] = fc_tex[fc]->createTextureView(
        RenderTextureViewDesc::Texture2DArray(
            RenderFormat::R32G32B32A32_FLOAT));
    if (g_nextTextureDescriptor < kFirstReservedDescriptor) {
      fc_desc[fc] = g_nextTextureDescriptor++;
      g_textureDescriptorSet->setTexture(fc_desc[fc], fc_tex[fc].get(),
                                         RenderTextureLayout::SHADER_READ,
                                         fc_view[fc].get());
    }
  }

  graphics::RegisterFile rf;
  std::memset(rf.values, 0, sizeof(rf.values));
  graphics::reg::SQ_PS_CONST ps_const{};
  ps_const.base = 0;
  ps_const.size = 255;
  rf.values[graphics::XE_GPU_REG_SQ_PS_CONST] = ps_const.value;

  struct CaptureSink : public graphics::ShaderInterpreter::ExportSink {
    float color[4] = {};
    uint32_t mask = 0;
    bool memexport = false;
    void AllocExport(graphics::ucode::AllocType type, uint32_t) override {
      if (type == graphics::ucode::AllocType::kMemory) memexport = true;
    }
    void Export(graphics::ucode::ExportRegister reg, const float* value,
                uint32_t value_mask) override {
      if (reg == graphics::ucode::ExportRegister::kPSColor0) {
        for (uint32_t c = 0; c < 4; c++) {
          if (value_mask & (1u << c)) color[c] = value[c];
        }
        mask |= value_mask;
      }
    }
  };

  const auto& mem = *rex::system::kernel_memory();
  graphics::ShaderInterpreter interp(rf, mem);
  // Texture fetches return the interpreter's own values, since the SDK's
  // interpreter has no fetch-sink hook to seed them per fetch constant.

  uint32_t total = 0, passed = 0, skipped = 0;
  uint32_t consec_pso_fails = 0;
  bool any_pso_ok = false;
  std::string report;
  char line[256];

  for (const auto& [hash, pack] : cache.pixel_shaders()) {
    // Load the original ucode.
    char path[512];
    snprintf(path, sizeof(path), "%s/shader_%016llX.ucode.bin.frag",
             ucode_dir.c_str(), (unsigned long long)hash);
    FILE* f = fopen(path, "rb");
    if (!f) {
      skipped++;
      snprintf(line, sizeof(line), "SKIP %016llX no ucode\n",
               (unsigned long long)hash);
      report += line;
      continue;
    }
    fseek(f, 0, SEEK_END);
    const long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint32_t> ucode((fsize + 3) / 4);
    fread(ucode.data(), 1, fsize, f);
    fclose(f);
    // Dumps hold guest big-endian dwords; the interpreter wants host order.
    for (auto& w : ucode) w = __builtin_bswap32(w);

    total++;
    // Memexport screen: the shader is CPU-interpreted once before any GPU work,
    // since a memory-export shader would write out of bounds of the small
    // stand-in shared-memory UAV.
    {
      CaptureSink screen;
      interp.SetExportSink(&screen);
      interp.SetShader(graphics::xenos::ShaderType::kPixel, ucode.data());
      std::memset(interp.temp_registers(), 0, 64 * 4 * sizeof(float));
      interp.Execute();
      if (screen.memexport) {
        skipped++;
        total--;
        snprintf(line, sizeof(line), "SKIP %016llX memexport\n",
                 (unsigned long long)hash);
        report += line;
        continue;
      }
    }
    // Candidate PSO: exact-signature VS variant plus the pack PS, tried in
    // descending order.
    RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = g_pipelineLayout.get();
    auto pack_ps = g_device->createShader(pack.dxil.data(), pack.dxil.size(),
                                          "main", RenderShaderFormat::DXIL);
    desc.pixelShader = pack_ps.get();
    desc.renderTargetFormat[0] = RenderFormat::R32G32B32A32_FLOAT;
    desc.renderTargetCount = 1;
    desc.depthTargetFormat = RenderFormat::UNKNOWN;
    desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.depthEnabled = false;
    desc.depthWriteEnabled = false;
    desc.cullMode = RenderCullMode::NONE;
    std::unique_ptr<RenderPipeline> pso;
    for (int n = 16; n >= 0 && !pso; n--) {
      if (!prover_vs[n]) continue;
      desc.vertexShader = prover_vs[n].get();
      pso = g_device->createGraphicsPipeline(desc);
    }
    if (!pso) {
      skipped++;
      snprintf(line, sizeof(line), "SKIP %016llX pso-fail\n",
               (unsigned long long)hash);
      report += line;
      if (any_pso_ok && ++consec_pso_fails > 5) {
        REXGPU_WARN(
            "videonative: [prover] aborted — consecutive PSO failures after "
            "success (device possibly removed)");
        report += "ABORT device-removed-suspect\n";
        break;
      }
      continue;
    }
    any_pso_ok = true;
    consec_pso_fails = 0;

    float worst = 0.0f;
    bool ok = true;
    for (uint32_t seed = 0; seed < kSeeds && ok; seed++) {
      // The per-seed submit cycle re-begins the command list, which drops the
      // pipeline layout and descriptor sets, so they are set again here.
      commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
      commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 0);
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
      commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 3);
      // --- shared inputs ---
      float floats[256][4];
      for (uint32_t r = 0; r < 256; r++)
        for (uint32_t c = 0; c < 4; c++)
          floats[r][c] = ProverFloat(r, c, seed);

      // --- CPU reference ---
      std::memcpy(&rf.values[graphics::XE_GPU_REG_SHADER_CONSTANT_000_X],
                  floats, sizeof(floats));
      CaptureSink cap;
      interp.SetExportSink(&cap);
      interp.SetShader(graphics::xenos::ShaderType::kPixel, ucode.data());
      float* regs = interp.temp_registers();
      std::memset(regs, 0, 64 * 4 * sizeof(float));
      for (uint32_t v = 0; v < 16; v++)
        for (uint32_t c = 0; c < 4; c++)
          regs[v * 4 + c] = floats[240 + v][c];
      interp.Execute();

      // --- GPU candidate ---
      for (uint32_t fc = 0; fc < 32; fc++) {
        commandList->barriers(
            RenderBarrierStage::COPY,
            RenderTextureBarrier(fc_tex[fc].get(),
                                 RenderTextureLayout::COPY_DEST));
        UploadAllocation tx = g_upload[g_frame].Allocate(256, 512);
        float texel[4];
        for (uint32_t c = 0; c < 4; c++)
          texel[c] = ProverFloat(0x7E7C0000u + fc, c, seed);
        std::memcpy(tx.memory, texel, 16);
        commandList->copyTextureRegion(
            RenderTextureCopyLocation::Subresource(fc_tex[fc].get(), 0),
            RenderTextureCopyLocation::PlacedFootprint(
                tx.buffer, RenderFormat::R32G32B32A32_FLOAT, 1, 1, 1, 16,
                tx.offset));
        commandList->barriers(
            RenderBarrierStage::GRAPHICS,
            RenderTextureBarrier(fc_tex[fc].get(),
                                 RenderTextureLayout::SHADER_READ));
      }
      SystemConstants sys{};
      sys.flags = kSysFlagAlphaPassIfLess | kSysFlagAlphaPassIfEqual |
                  kSysFlagAlphaPassIfGreater;
      float* exp_bias = reinterpret_cast<float*>(&sys) + 15 * 4;
      exp_bias[0] = exp_bias[1] = exp_bias[2] = exp_bias[3] = 1.0f;
      UploadAllocation b0 = g_upload[g_frame].Allocate(sizeof(sys), 256);
      std::memcpy(b0.memory, &sys, sizeof(sys));
      UploadAllocation b1 = g_upload[g_frame].Allocate(sizeof(floats), 256);
      std::memcpy(b1.memory, floats, sizeof(floats));
      UploadAllocation b2 = g_upload[g_frame].Allocate(40 * 4, 256);
      std::memset(b2.memory, 0, 40 * 4);
      uint32_t fetch_words[32 * 6] = {};
      for (uint32_t fc = 0; fc < 32; fc++) {
        fetch_words[fc * 6 + 2] = 0;  // 1x1: size-minus-1 fields zero
      }
      UploadAllocation b3 = g_upload[g_frame].Allocate(sizeof(fetch_words), 256);
      std::memcpy(b3.memory, fetch_words, sizeof(fetch_words));
      uint32_t indices[32] = {};
      for (size_t b = 0; b < pack.bindings.size() && b < 32; b++) {
        const auto& binding = pack.bindings[b];
        indices[b] = binding.is_sampler
                         ? 0
                         : fc_desc[binding.fetch_constant & 31];
      }
      UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(indices), 256);
      if (!b0.memory || !b1.memory || !b2.memory || !b3.memory ||
          !b4.memory) {
        REXGPU_WARN(
            "videonative: [prover] upload arena exhausted — aborting at "
            "{}/{} shaders",
            passed, total);
        ok = false;
        break;
      }
      std::memcpy(b4.memory, indices, sizeof(indices));

      commandList->barriers(
          RenderBarrierStage::GRAPHICS,
          RenderTextureBarrier(rt_tex.get(),
                               RenderTextureLayout::COLOR_WRITE));
      commandList->setFramebuffer(rt_fb.get());
      commandList->setPipeline(pso.get());
      commandList->setViewports(
          RenderViewport(0.0f, 0.0f, float(kRtDim), float(kRtDim), 0.0f,
                         1.0f));
      commandList->setScissors(RenderRect(0, 0, kRtDim, kRtDim));
      commandList->setGraphicsRootDescriptor(b0.buffer->at(b0.offset),
                                             kRootB0System);
      commandList->setGraphicsRootDescriptor(b1.buffer->at(b1.offset),
                                             kRootB1FloatsVs);
      commandList->setGraphicsRootDescriptor(b1.buffer->at(b1.offset),
                                             kRootB1FloatsPs);
      commandList->setGraphicsRootDescriptor(b2.buffer->at(b2.offset),
                                             kRootB2BoolLoop);
      commandList->setGraphicsRootDescriptor(b3.buffer->at(b3.offset),
                                             kRootB3Fetch);
      commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                             kRootB4IndicesVs);
      commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                             kRootB4IndicesPs);
      // Every root parameter must be bound, since an unset root descriptor is
      // D3D12 UB that removes the device.
      auto& ring = g_vertexRing[g_frame];
      if (ring.buffer) {
        commandList->setGraphicsRootDescriptor(ring.buffer->at(0),
                                               kRootSharedMemSrv);
      } else {
        commandList->setGraphicsRootDescriptor(b1.buffer->at(b1.offset),
                                               kRootSharedMemSrv);
      }
      commandList->setGraphicsRootDescriptor(g_dummyUav->at(0), kRootDummyUav);
      commandList->drawInstanced(3, 1, 0, 0);
      commandList->barriers(
          RenderBarrierStage::COPY,
          RenderTextureBarrier(rt_tex.get(),
                               RenderTextureLayout::COPY_SOURCE));
      commandList->copyTextureRegion(
          RenderTextureCopyLocation::PlacedFootprint(
              readback.get(), RenderFormat::R32G32B32A32_FLOAT, kRtDim,
              kRtDim, 1, 64 / 4, 0),
          RenderTextureCopyLocation::Subresource(rt_tex.get(), 0));
      commandList->end();
      const RenderCommandList* lists[] = {commandList.get()};
      g_queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0,
                                   g_commandFences[g_frame].get());
      g_queue->waitForCommandFence(g_commandFences[g_frame].get());
      const float* gpu =
          static_cast<const float*>(readback->map());
      float gpu_px[4] = {gpu[0], gpu[1], gpu[2], gpu[3]};
      readback->unmap();
      commandList->begin();

      for (uint32_t c = 0; c < 4; c++) {
        const float a = gpu_px[c], b = cap.color[c];
        const bool a_nan = a != a, b_nan = b != b;
        if (a_nan || b_nan) {
          if (a_nan != b_nan) ok = false;
          continue;
        }
        const float diff = std::abs(a - b);
        const float rel = diff / std::max(1.0f, std::abs(b));
        worst = std::max(worst, rel);
        if (rel > 2e-3f) ok = false;
      }
      if (!ok) {
        snprintf(line, sizeof(line),
                 "FAIL %016llX seed=%u cpu=(%g,%g,%g,%g) gpu=(%g,%g,%g,%g)\n",
                 (unsigned long long)hash, seed, cap.color[0], cap.color[1],
                 cap.color[2], cap.color[3], gpu_px[0], gpu_px[1], gpu_px[2],
                 gpu_px[3]);
        report += line;
      }
    }
    if (ok) {
      passed++;
      snprintf(line, sizeof(line), "PASS %016llX maxrel=%g\n",
               (unsigned long long)hash, worst);
      report += line;
    }
    static uint64_t prover_logs = 200;
    if (prover_logs) {
      prover_logs--;
      REXGPU_INFO("videonative: [prover] {:016X} {} (maxrel {:g})", hash,
                  ok ? "PASS" : "FAIL", worst);
    }
  }
  interp.SetExportSink(nullptr);
  REXGPU_INFO(
      "videonative: [prover] done — {}/{} pixel shaders equivalent, {} "
      "skipped",
      passed, total, skipped);
  const std::string report_path = ucode_dir + "/prover_report.txt";
  if (FILE* rf_out = fopen(report_path.c_str(), "wb")) {
    char head[128];
    snprintf(head, sizeof(head), "pack prover: %u/%u PASS, %u skipped\n",
             passed, total, skipped);
    fwrite(head, 1, strlen(head), rf_out);
    fwrite(report.data(), 1, report.size(), rf_out);
    fclose(rf_out);
    REXGPU_INFO("videonative: [prover] report -> {}", report_path);
  }
  // Restores the frame's framework state, mirroring the write-back reopen.
  commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
  commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 0);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
  commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 3);
  commandList->setFramebuffer(g_framebuffers[g_backBufferIndex].get());
}
#else
// The prover runs pack shaders through the SDK's CPU ucode interpreter, which
// the runtime does not export; build with -DCC1_SHADER_PROVER=1 against an SDK
// that does.
void ProvePack() {
  REXGPU_WARN("videonative: [prover] not in this build (CC1_SHADER_PROVER)");
}
#endif  // CC1_SHADER_PROVER

// Resolve write-back: CC1's engine CPU-reads the resolved lighting canvas, so
// each color resolve is queued and, at the guest's fence wait, read back and
// written into guest memory with the dest header's pitch, tiling and endian;
// entries never cross frames.
struct ResolveWriteback {
  RenderFormat host_format;
  uint32_t base_address;   // guest bytes (header dw1 & ~0xFFF)
  uint32_t pitch_texels;   // header dw0 pitch (32-texel units) * 32
  uint32_t endian;         // header dw1 bits 6-7 (2 = 8-in-32)
  bool tiled;              // header dw0 bit 31
  uint32_t tex_w, tex_h;   // guest texture dims
  uint32_t x, y, w, h;     // resolved dest rect (guest texels)
  // Content is snapshotted into the readback arena at resolve time and the
  // flush only delivers bytes, so a deferred flush cannot copy a later frame's
  // RT content into an earlier ping-pong buffer.
  uint32_t snap_offset;    // byte offset into g_wbSnapBuffer
  uint32_t snap_pitch;     // 256-aligned row pitch in the arena
  // Shadow depth resolves: host D32 float is converted to guest D24S8
  // (depth<<8, stencil 0) at delivery.
  bool depth_convert;
  // The dest texture has Xenos gamma signs, so linear host bytes are
  // PWL-encoded at delivery.
  bool gamma_encode;
};

// Xenos piecewise-linear gamma encode (linear -> PWL), 8-bit LUT.
const uint8_t* PwlGammaLut() {
  static uint8_t lut[256];
  static bool init = false;
  if (!init) {
    init = true;
    for (int i = 0; i < 256; i++) {
      const float x = float(i) / 255.0f;
      const float y = x < 0.0625f ? x * 4.0f
                      : x < 0.125f ? x * 2.0f + 0.125f
                      : x < 0.5f   ? x + 0.25f
                                   : x * 0.5f + 0.5f;
      lut[i] = uint8_t(y * 255.0f + 0.5f);
    }
  }
  return lut;
}

uint32_t PwlEncodeRgba(uint32_t rgba) {
  const uint8_t* lut = PwlGammaLut();
  return (rgba & 0xFF000000u) | (uint32_t(lut[(rgba >> 16) & 0xFF]) << 16) |
         (uint32_t(lut[(rgba >> 8) & 0xFF]) << 8) | lut[rgba & 0xFF];
}
std::vector<ResolveWriteback> g_resolveWritebacks;
std::atomic<uint32_t> g_resolveWritebacksPending{0};
// Snapshot arena, grown on demand; a grow flushes first so live records never
// reference a replaced buffer.
std::unique_ptr<RenderBuffer> g_wbSnapBuffer;
uint32_t g_wbSnapCap = 0;
uint32_t g_wbSnapUsed = 0;

uint32_t ResolveWritebacksPending() {
  return g_resolveWritebacksPending.load(std::memory_order_acquire);
}

bool ResolveWritebackEnabled() {
  return REXCVAR_GET(gpu_resolve_writeback);
}

bool WritebackSyncEnabled() {
  return REXCVAR_GET(gpu_writeback_sync);
}

void NoteRawDevice(uint32_t device_ea) {
  if (!device_ea || g_rawDeviceEA == device_ea) return;
  g_rawDeviceEA = device_ea;
}

void FlushResolveWritebacksInline() {
  if (g_resolveWritebacks.empty()) return;
  auto drop_all = [] {
    g_resolveWritebacks.clear();
    g_resolveWritebacksPending.store(0, std::memory_order_release);
  };
  if (!g_device || !g_swapChainValid) {
    drop_all();
    return;
  }
  if (Scaled()) {
    static uint64_t scale_logs = 2;
    if (scale_logs) {
      scale_logs--;
      REXGPU_WARN(
          "videonative: [writeback] skipped under resolution_scale "
          "(host rects are scaled; guest bytes would be wrong)");
    }
    drop_all();
    return;
  }
  auto& commandList = g_commandLists[g_frame];
  const bool frame_open = g_frameOpen;
  if (!frame_open) {
    if (g_commandListPending[g_frame]) {
      g_queue->waitForCommandFence(g_commandFences[g_frame].get());
      g_commandListPending[g_frame] = false;
    }
    commandList->begin();
  }
  // Records carry their content in the snapshot arena, so this flush only needs
  // the recorded copies executed, then maps and delivers.
  if (!g_wbSnapBuffer) {
    drop_all();
    if (!frame_open) {
      commandList->end();
      const RenderCommandList* empty_lists[] = {commandList.get()};
      g_queue->executeCommandLists(empty_lists, 1, nullptr, 0, nullptr, 0,
                                   g_commandFences[g_frame].get());
      g_queue->waitForCommandFence(g_commandFences[g_frame].get());
    }
    return;
  }
  commandList->end();
  const RenderCommandList* lists[] = {commandList.get()};
  g_queue->executeCommandLists(lists, 1, nullptr, 0, nullptr, 0,
                               g_commandFences[g_frame].get());
  g_queue->waitForCommandFence(g_commandFences[g_frame].get());
  g_commandListPending[g_frame] = false;

  const uint8_t* bytes = static_cast<const uint8_t*>(g_wbSnapBuffer->map());
  for (size_t i = 0; i < g_resolveWritebacks.size(); i++) {
    const ResolveWriteback& wb = g_resolveWritebacks[i];
    const uint32_t pitch = wb.snap_pitch;
    const uint32_t pitch_texels =
        wb.pitch_texels ? wb.pitch_texels : ((wb.tex_w + 31u) & ~31u);
    // Direct translation instead of GuestPtr, which consults the worker's
    // snapshot windows, since this is a write to live guest memory.
    uint8_t* guest =
        rex::system::kernel_memory()->TranslateVirtual<uint8_t*>(
            wb.base_address);
    if (!guest) continue;
    static uint64_t endian_logs = 2;
    if (wb.endian != 2 && wb.endian != 0 && endian_logs) {
      endian_logs--;
      REXGPU_WARN("videonative: [writeback] endian {} unsupported (raw copy)",
                  wb.endian);
    }
    for (uint32_t row = 0; row < wb.h; row++) {
      const uint32_t* src = reinterpret_cast<const uint32_t*>(
          bytes + wb.snap_offset + row * pitch);
      const uint32_t gy = wb.y + row;
      if (!wb.tiled) {
        uint32_t* dst = reinterpret_cast<uint32_t*>(
            guest + (uint64_t(gy) * pitch_texels + wb.x) * 4);
        if (wb.depth_convert) {
          for (uint32_t t = 0; t < wb.w; t++) {
            float z;
            std::memcpy(&z, &src[t], 4);
            z = z < 0.f ? 0.f : (z > 1.f ? 1.f : z);
            const uint32_t d = uint32_t(z * 16777215.0f + 0.5f) << 8;
            dst[t] = wb.endian == 2 ? __builtin_bswap32(d) : d;
          }
        } else if (wb.gamma_encode) {
          for (uint32_t t = 0; t < wb.w; t++) {
            const uint32_t e = PwlEncodeRgba(src[t]);
            dst[t] = wb.endian == 2 ? __builtin_bswap32(e) : e;
          }
        } else if (wb.endian == 2) {
          for (uint32_t t = 0; t < wb.w; t++)
            dst[t] = __builtin_bswap32(src[t]);
        } else {
          std::memcpy(dst, src, wb.w * 4u);
        }
      } else {
        const uint32_t outer = TiledOffset2DOuter(gy, pitch_texels, 2);
        for (uint32_t t = 0; t < wb.w; t++) {
          const uint32_t off = TiledOffset2DInner(wb.x + t, gy, 2, outer);
          uint32_t raw = src[t];
          if (wb.depth_convert) {
            float z;
            std::memcpy(&z, &raw, 4);
            z = z < 0.f ? 0.f : (z > 1.f ? 1.f : z);
            raw = uint32_t(z * 16777215.0f + 0.5f) << 8;
          } else if (wb.gamma_encode) {
            raw = PwlEncodeRgba(raw);
          }
          const uint32_t v = wb.endian == 2 ? __builtin_bswap32(raw) : raw;
          std::memcpy(guest + off, &v, 4);
        }
      }
    }
  }
  g_wbSnapBuffer->unmap();
  g_wbSnapUsed = 0;
  drop_all();

  if (frame_open) {
    // Reopen mid-frame: the framework binds are restored and per-draw state
    // (pipeline/viewport/scissor/root descriptors) re-emits every draw.
    commandList->begin();
    commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
    commandList->setGraphicsDescriptorSet(g_samplerDescriptorSet.get(), 0);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 1);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 2);
    commandList->setGraphicsDescriptorSet(g_textureDescriptorSet.get(), 3);
    if (g_activeRtKey) {
      auto it = g_renderTargets.find(g_activeRtKey);
      commandList->setFramebuffer(
          it != g_renderTargets.end()
              ? it->second.framebuffer.get()
              : g_framebuffers[g_backBufferIndex].get());
    } else {
      commandList->setFramebuffer(g_framebuffers[g_backBufferIndex].get());
    }
  }
  // With no frame open the list stays closed and the next frame begin owns it.
}

// Deferred guest->upload copies: queued at draw time and executed at
// EndFrameAndPresent before submission, so the GPU sees the guest buffers'
// end-of-frame state as hardware does; swap is 0 = raw memcpy, 2 = bswap16
// words, 4 = bswap32 words.
struct DeferredCopy {
  const void* src;
  void* dst;
  uint32_t size;
  uint8_t swap;
};
std::vector<DeferredCopy> g_deferredCopies;

void DrainDeferredCopies() {
  for (const DeferredCopy& c : g_deferredCopies) {
    if (c.swap == 2) {
      const uint16_t* src = reinterpret_cast<const uint16_t*>(c.src);
      uint16_t* dst = reinterpret_cast<uint16_t*>(c.dst);
      for (uint32_t i = 0; i < c.size / 2; i++) {
        dst[i] = __builtin_bswap16(src[i]);
      }
    } else if (c.swap == 4) {
      const uint32_t* src = reinterpret_cast<const uint32_t*>(c.src);
      uint32_t* dst = reinterpret_cast<uint32_t*>(c.dst);
      for (uint32_t i = 0; i < c.size / 4; i++) {
        dst[i] = __builtin_bswap32(src[i]);
      }
    } else {
      std::memcpy(c.dst, c.src, c.size);
    }
  }
  g_deferredCopies.clear();
}

void EndFrameAndPresent() {
  if (rq::Active()) { rq::EnqEndFrameAndPresent(); return; }
  // Pack prover: one shot at the first present, with the device warm and the
  // pack loaded.
  {
    static bool s_proved = false;
    if (!s_proved && !REXCVAR_GET(gpu_prove_pack).empty()) {
      s_proved = true;
      ProvePack();
    }
  }
  // Safety net: flushes any write-backs that slipped past the resolve-time
  // delivery, since entries cannot cross the frame.
  if (!g_resolveWritebacks.empty()) {
    FlushResolveWritebacksInline();
  }
  // The deferred guest->upload copies run first, so every draw recorded this
  // frame samples the guest VB/IB state as of end-of-frame.
  DrainDeferredCopies();
  g_frameIndex++;
  // Per-tile-loop clear suppression is frame-scoped, so the next frame's
  // pass-opening clear is not suppressed by this frame's resolve count; a
  // second shadow window on the same surface within one frame would still lose
  // its opening clear.
  g_passResolves = 0;
  // Retired texture entries are destroyed once
  // gpu_texture_retire_frames frames have passed, a frame-count window
  // and not a fence.
  while (!g_retiredTextures.empty() &&
         g_frameIndex - g_retiredTextures.front().first >
             uint64_t(REXCVAR_GET(gpu_texture_retire_frames))) {
    // Fence-safe point (window >> kNumFrames): the dead entry's descriptor slot
    // is scrubbed back to blank before the texture is destroyed.
    const CachedTexture& dead = g_retiredTextures.front().second;
    if (dead.descriptor_index > 0 &&
        dead.descriptor_index < kFirstReservedDescriptor) {
      // Scrubbed with the blank of the dead texture's own view dimension, so a
      // stale reference through the matching declaration stays defined.
      const bool was_cube = dead.view_dim == 3;
      g_textureDescriptorSet->setTexture(
          dead.descriptor_index,
          was_cube ? g_blankTextureCube.get() : g_blankTexture.get(),
          RenderTextureLayout::SHADER_READ,
          was_cube ? g_blankTextureCubeView.get() : g_blankTextureView.get());
      // Nothing in flight references the slot, so it is recycled (see
      // AllocTextureDescriptor).
      g_freeDescriptors.push_back(dead.descriptor_index);
    }
    g_retiredTextures.pop_front();
  }
  // Watermark eviction sweep, every 64 frames: only when the cache exceeds
  // gpu_tex_cache_max or gpu_tex_cache_mb do the coldest
  // entries (LRU by last_bind_frame, minimum age gpu_tex_evict_frames)
  // retire through the drain above.
  const int32_t cache_max = REXCVAR_GET(gpu_tex_cache_max);
  const uint64_t bytes_max =
      uint64_t(std::max(0, REXCVAR_GET(gpu_tex_cache_mb))) << 20;
  const bool over_count =
      cache_max > 0 && g_textureCache.size() > size_t(cache_max);
  const bool over_bytes = bytes_max && g_texCacheBytes > bytes_max;
  if ((over_count || over_bytes) && (g_frameIndex & 63) == 0) {
    const uint64_t min_age =
        uint64_t(std::max(0, REXCVAR_GET(gpu_tex_evict_frames)));
    // Eviction safety: host-rendered entries re-resolve, and a guest-uploaded
    // entry is safe only when unreachable, meaning its live header no longer
    // matches the creation snapshot.
    const auto evict_safe = [min_age](const CachedTexture& e) {
      if (e.host_rendered) {
        // One-time bakes resolve once at boot or level load and cannot be
        // recovered after eviction, so only entries re-resolved recently may
        // go.
        if (!e.last_resolve_frame) return true;  // never held content
        return g_frameIndex - e.last_resolve_frame <= min_age * 2;
      }
      if (!e.header_addr) return true;  // untracked -> unreachable
      for (int i = 0; i < 6; i++) {
        if (LoadGuestU32(e.header_addr + i * 4) != e.header_dw[i]) {
          return true;  // header dead/recycled: entry unreachable
        }
      }
      return false;
    };
    // Eviction candidates (old enough and safe), coldest first.
    std::vector<std::pair<uint64_t, uint64_t>> cand;  // (last_bind, key)
    cand.reserve(g_textureCache.size());
    for (auto& [k, e] : g_textureCache) {
      if (e.texture && g_frameIndex - e.last_bind_frame > min_age &&
          evict_safe(e)) {
        cand.emplace_back(e.last_bind_frame, k);
      }
    }
    // Trims to ~87% of whichever watermark tripped, coldest first, so sweeps
    // stay rare.
    const size_t target_count =
        cache_max > 0 ? size_t(cache_max) - size_t(cache_max) / 8
                      : size_t(-1);
    const uint64_t target_bytes =
        bytes_max ? bytes_max - bytes_max / 8 : ~0ull;
    size_t take = 0;
    if (!cand.empty()) {
      std::sort(cand.begin(), cand.end());
      size_t evicted_i = 0;
      for (size_t i = 0; i < cand.size(); i++) {
        if (g_textureCache.size() - evicted_i <= target_count &&
            g_texCacheBytes <= target_bytes) {
          break;
        }
        auto tit = g_textureCache.find(cand[i].second);
        if (tit == g_textureCache.end()) continue;
        CachedTexture& e = tit->second;
        // Every raw-pointer reference is detached before the entry moves.
        if (g_lastLightAccumResolve == &e) g_lastLightAccumResolve = nullptr;
        if (g_prevFmt19Resolve == &e) g_prevFmt19Resolve = nullptr;
        if (g_lastShadowMapResolve == &e) g_lastShadowMapResolve = nullptr;
        for (auto sit = g_lastResolvedBySize.begin();
             sit != g_lastResolvedBySize.end();) {
          if (sit->second == &e) {
            sit = g_lastResolvedBySize.erase(sit);
          } else {
            ++sit;
          }
        }
        for (auto rit = g_resolvedBaseToKey.begin();
             rit != g_resolvedBaseToKey.end();) {
          if (rit->second == tit->first) {
            rit = g_resolvedBaseToKey.erase(rit);
          } else {
            ++rit;
          }
        }
        RetireEntry(e);
        g_textureCache.erase(tit);
        evicted_i++;
      }
      take = evicted_i;
    }
    if (take) {
      REXGPU_INFO(
          "videonative: [texevict] watermark: {} coldest retired (cache={} "
          "texMB={} desc={}/{} free={})",
          take, g_textureCache.size(), g_texCacheBytes >> 20,
          g_nextTextureDescriptor, kFirstReservedDescriptor,
          g_freeDescriptors.size());
    } else {
      static uint64_t hot_warn = 4;
      if (hot_warn) {
        hot_warn--;
        REXGPU_WARN(
            "videonative: [texevict] cache over watermark ({} entries, "
            "{} MB) but no entries older than the min age — pressure ahead",
            g_textureCache.size(), g_texCacheBytes >> 20);
      }
    }
  }
  // Retired RT sets are destroyed at the same fence-safe point as textures, and
  // a retired entry's seed SRV slot is scrubbed to blank and recycled.
  while (!g_retiredRts.empty() &&
         g_frameIndex - g_retiredRts.front().first >
             uint64_t(REXCVAR_GET(gpu_texture_retire_frames))) {
    CachedRenderTarget& dead = g_retiredRts.front().second;
    if (dead.seed_descriptor > 0 &&
        dead.seed_descriptor < kFirstReservedDescriptor) {
      g_textureDescriptorSet->setTexture(dead.seed_descriptor,
                                         g_blankTexture.get(),
                                         RenderTextureLayout::SHADER_READ,
                                         g_blankTextureView.get());
      g_freeDescriptors.push_back(dead.seed_descriptor);
    }
    g_retiredRts.pop_front();
  }
  // RT-cache watermark sweep: g_renderTargets grows per unique (dims,
  // format-set), so it is bounded like the texture cache (LRU, min-age,
  // fence-safe), never touching the active and last-drawn sets.
  {
    const uint64_t rt_bytes_max =
        uint64_t(std::max(0, REXCVAR_GET(gpu_rt_cache_mb))) << 20;
    if (rt_bytes_max && g_rtCacheBytes > rt_bytes_max &&
        (g_frameIndex & 63) == 0) {
      const uint64_t min_age =
          uint64_t(std::max(0, REXCVAR_GET(gpu_tex_evict_frames)));
      std::vector<std::pair<uint64_t, uint64_t>> cand;
      for (auto& [k, e] : g_renderTargets) {
        if (k != g_activeRtKey && k != g_lastDrawnRtKey &&
            g_frameIndex - e.last_bind_frame > min_age) {
          cand.emplace_back(e.last_bind_frame, k);
        }
      }
      std::sort(cand.begin(), cand.end());
      const uint64_t target = rt_bytes_max - rt_bytes_max / 8;
      uint32_t evicted = 0;
      for (size_t i = 0; i < cand.size() && g_rtCacheBytes > target; i++) {
        auto rit = g_renderTargets.find(cand[i].second);
        if (rit == g_renderTargets.end()) continue;
        g_rtCacheBytes -=
            std::min<uint64_t>(g_rtCacheBytes, rit->second.approx_bytes);
        g_retiredRts.emplace_back(g_frameIndex, std::move(rit->second));
        g_renderTargets.erase(rit);
        evicted++;
      }
      if (evicted) {
        REXGPU_INFO(
            "videonative: [rtevict] {} cold RT sets retired (rts={} "
            "rtMB={})",
            evicted, g_renderTargets.size(), g_rtCacheBytes >> 20);
      }
    }
  }
  const int32_t capture_on_draws = REXCVAR_GET(gpu_capture_on_draws);
  if (capture_on_draws > 0 &&
      g_drawsThisFrame >= uint64_t(capture_on_draws)) {
    static bool fired = false;
    if (!fired) {
      fired = true;
      static auto rdoc_draws = rex::ui::RenderDocAPI::CreateIfConnected();
      if (rdoc_draws) {
        rdoc_draws->api_1_0_0()->TriggerCapture();
        REXGPU_INFO(
            "videonative: RenderDoc capture triggered at frame {} ({} draws)",
            g_frameIndex, g_drawsThisFrame);
      }
    }
  }
  const int32_t capture_frame = REXCVAR_GET(gpu_capture_frame);
  if (capture_frame > 0 && g_frameIndex == uint64_t(capture_frame)) {
    // TriggerCapture snapshots the next frame presented.
    static auto rdoc = rex::ui::RenderDocAPI::CreateIfConnected();
    if (rdoc) {
      rdoc->api_1_0_0()->TriggerCapture();
      REXGPU_INFO("videonative: RenderDoc capture triggered at frame {}",
                  g_frameIndex);
    } else {
      REXGPU_WARN("videonative: capture frame reached but RenderDoc not "
                  "attached");
    }
  }
  static int g_multiCaptureRemaining = 0;
  // Click-to-inspect debug mode: while armed by the inspect_now file trigger, a
  // left-click in the game window logs its backbuffer coordinates and fires a
  // 3-frame RenderDoc capture when attached.
  static bool g_inspectArmed = false;
  static bool g_lbuttonWasDown = false;
  // File trigger mirroring the ring path's rd_capture_now
  // (command_processor.cpp): delete-on-fire, re-armable, checked at ~4Hz, and
  // active only when launched under RenderDoc.
  {
    static std::chrono::steady_clock::time_point last_file_check;
    const auto now = std::chrono::steady_clock::now();
#ifdef _WIN32
    if (g_inspectArmed && g_inspectHwnd) {
      const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
      if (down && !g_lbuttonWasDown) {
        POINT pt;
        RECT rc;
        if (GetCursorPos(&pt) &&
            ScreenToClient(static_cast<HWND>(g_inspectHwnd), &pt) &&
            GetClientRect(static_cast<HWND>(g_inspectHwnd), &rc) &&
            pt.x >= 0 && pt.y >= 0 && pt.x < rc.right && pt.y < rc.bottom) {
          REXGPU_INFO(
              "videonative: [inspect] click client=({},{}) size=({},{}) "
              "frame={}",
              pt.x, pt.y, rc.right, rc.bottom, g_frameIndex);
          g_multiCaptureRemaining = 3;
        }
      }
      g_lbuttonWasDown = down;
    }
#endif
    if (g_multiCaptureRemaining > 0) {
      static auto rdoc_multi = rex::ui::RenderDocAPI::CreateIfConnected();
      if (rdoc_multi) {
        rdoc_multi->api_1_0_0()->TriggerCapture();
        g_multiCaptureRemaining--;
        REXGPU_INFO("videonative: multi-capture frame {} ({} left)",
                    g_frameIndex, g_multiCaptureRemaining);
      } else {
        g_multiCaptureRemaining = 0;
      }
    }
    if (now - last_file_check >= std::chrono::milliseconds(250)) {
      last_file_check = now;
      std::error_code ec;
      if (std::filesystem::remove("rd_capture_now", ec)) {
        static auto rdoc_file = rex::ui::RenderDocAPI::CreateIfConnected();
        if (rdoc_file) {
          rdoc_file->api_1_0_0()->TriggerCapture();
          REXGPU_INFO(
              "videonative: RenderDoc capture triggered by rd_capture_now "
              "(frame {})",
              g_frameIndex);
        } else {
          REXGPU_WARN("videonative: rd_capture_now seen but RenderDoc not "
                      "attached");
        }
      }
      // rd_capture_multi arms a capture of 4 consecutive frames, fired per
      // present below.
      if (std::filesystem::remove("rd_capture_multi", ec)) {
        g_multiCaptureRemaining = 4;
      }
      if (std::filesystem::remove("inspect_now", ec)) {
        g_inspectArmed = !g_inspectArmed;
        REXGPU_INFO("videonative: [inspect] mode {} (frame {})",
                    g_inspectArmed ? "ARMED -- left-click an object to tag it"
                                   : "off",
                    g_frameIndex);
      }
      // Same-boot A/B for the bind-default semantics: touching this file next
      // to the exe flips the cvar live, and ApplyShaderFloatDefaults reads it
      // per bind.
      if (std::filesystem::remove("defaults_win_toggle", ec)) {
        bool& v = REXCVAR_GET(gpu_defaults_win);
        v = !v;
        REXGPU_INFO("videonative: defaults_win_toggle -> {} (frame {})", v,
                    g_frameIndex);
      }
      // Debug: all-white shadow map substitution for lighting draws.
      if (std::filesystem::remove("shadowwhite_toggle", ec)) {
        g_debugWhiteShadow = !g_debugWhiteShadow;
        REXGPU_INFO("videonative: shadowwhite_toggle -> {} (frame {})",
                    g_debugWhiteShadow, g_frameIndex);
      }
      // Same-boot A/B for the depth-only (shadow map) Y-flip: flips the cvar
      // live, and SetupDraw reads it per draw for the pipeline cull bit and b0
      // ndc.
      if (std::filesystem::remove("shadowflip_toggle", ec)) {
        bool& v = REXCVAR_GET(gpu_shadow_flip);
        v = !v;
        REXGPU_INFO("videonative: shadowflip_toggle -> {} (frame {})", v,
                    g_frameIndex);
      }
      // Same-boot A/B for the shadow depth-value convention (guest float24
      // halving), see the viewport block in SetupDraw.
      if (std::filesystem::remove("shadowhalf_toggle", ec)) {
        bool& v = REXCVAR_GET(gpu_shadow_depth_half);
        v = !v;
        REXGPU_INFO("videonative: shadowhalf_toggle -> {} (frame {})", v,
                    g_frameIndex);
      }
      // Debug view: cycles the present source (0 off, 1 light accum, 2 sun
      // shadow map, 3 GBuffer normals).
      if (std::filesystem::remove("lightaccum_toggle", ec)) {
        g_debugShowLightAccum = (g_debugShowLightAccum + 1) % 4;
        REXGPU_INFO("videonative: lightaccum_toggle -> mode {} (frame {})",
                    g_debugShowLightAccum, g_frameIndex);
      }
      // Same-boot A/B for the display gamma ramp: flips the cvar live, and the
      // blit reads it per present.
      if (std::filesystem::remove("gamma_toggle", ec)) {
        bool& v = REXCVAR_GET(gpu_gamma_ramp);
        v = !v;
        REXGPU_INFO("videonative: gamma_toggle -> {} (frame {})", v,
                    g_frameIndex);
      }
    }
  }
  if (!g_swapChain) return;
  if (!g_frameOpen) {
    // Nothing was drawn this frame, so it is opened only to clear and present.
    EnsureFrameOpen();
    if (!g_frameOpen) return;
  }

  auto& commandList = g_commandLists[g_frame];

  // Debug: one-shot live backbuffer readback, bypassing RenderDoc replay, to
  // check the presented pixels at runtime.
  const bool do_readback =
      REXCVAR_GET(gpu_capture_frame) < 0 &&
      g_frameIndex == uint64_t(-REXCVAR_GET(gpu_capture_frame));
  std::unique_ptr<RenderBuffer> readback;
  uint32_t rb_width = 0, rb_height = 0, rb_pitch = 0;
  if (do_readback) {
    rb_width = g_swapChain->getWidth();
    rb_height = g_swapChain->getHeight();
    rb_pitch = (rb_width * 4 + 255) & ~255u;
    readback = g_device->createBuffer(
        RenderBufferDesc::ReadbackBuffer(uint64_t(rb_pitch) * rb_height));
    if (!readback) {
      REXGPU_WARN("videonative: readback buffer creation failed");
    }
  }
  if (do_readback && readback) {
    commandList->barriers(
        RenderBarrierStage::COPY,
        RenderTextureBarrier(g_swapChain->getTexture(g_backBufferIndex),
                             RenderTextureLayout::COPY_SOURCE));
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(
            readback.get(), kColorFormat, rb_width, rb_height, 1,
            rb_pitch / 4, 0),
        RenderTextureCopyLocation::Subresource(
            g_swapChain->getTexture(g_backBufferIndex), 0));
  }

  commandList->barriers(
      RenderBarrierStage::GRAPHICS,
      RenderTextureBarrier(g_swapChain->getTexture(g_backBufferIndex),
                           RenderTextureLayout::PRESENT));
  commandList->end();

  const RenderCommandList* commandLists[] = {commandList.get()};
  RenderCommandSemaphore* waitSemaphores[] = {
      g_acquireSemaphores[g_frame].get()};
  RenderCommandSemaphore* signalSemaphores[] = {
      g_renderSemaphores[g_frame].get()};
  g_queue->executeCommandLists(commandLists, 1, waitSemaphores, 1,
                               signalSemaphores, 1,
                               g_commandFences[g_frame].get());
  if (do_readback && readback) {
    g_queue->waitForCommandFence(g_commandFences[g_frame].get());
    const uint8_t* pixels = static_cast<const uint8_t*>(readback->map());
    // Center + corner samples and a quick non-black census.
    uint64_t nonblack = 0;
    for (uint32_t y = 0; y < rb_height; y += 8) {
      const uint32_t* row =
          reinterpret_cast<const uint32_t*>(pixels + y * rb_pitch);
      for (uint32_t x = 0; x < rb_width; x += 8) {
        if ((row[x] & 0xFFFFFF) != 0) nonblack++;
      }
    }
    const uint32_t* mid = reinterpret_cast<const uint32_t*>(
        pixels + (rb_height / 2) * rb_pitch);
    REXGPU_INFO(
        "videonative: live readback frame {}: center={:08X} corner={:08X} "
        "nonblack {}/{}",
        g_frameIndex, mid[rb_width / 2],
        reinterpret_cast<const uint32_t*>(pixels)[8], nonblack,
        uint64_t(rb_width / 8) * (rb_height / 8));
    readback->unmap();
    g_commandListPending[g_frame] = false;
  } else {
    g_commandListPending[g_frame] = true;
  }
  // Host-side frame cap (gpu_fps_cap), same contract as the ring CP's swap
  // throttle: the present is held until the minimum interval since the previous
  // one, timed from the target so wake-up jitter does not lower the average,
  // and resynced after stalls.
  {
    const int32_t limit = REXCVAR_GET(gpu_fps_cap);
    static uint64_t throttle_last_tick = 0;
    // With vsync on and a display refresh at or below the cap, vsync already
    // paces the presents, and running the timer cap as well would beat against
    // it and stutter.
    const uint32_t refresh_hz = DisplayRefreshHz();
    const bool vsync = REXCVAR_GET(gpu_vsync) && refresh_hz > 0;
    // A cap the refresh rate is a whole multiple of (30 at 60 Hz, 72 at 144 Hz)
    // is paced by vsync as well, each frame shown for that many refreshes.
    uint32_t refreshes = 1;
    if (vsync && limit > 0 && uint32_t(limit) + 1 < refresh_hz) {
      const uint32_t whole = (refresh_hz + uint32_t(limit) / 2) / uint32_t(limit);
      const uint32_t off = refresh_hz > whole * uint32_t(limit) ? refresh_hz - whole * uint32_t(limit)
                                                                : whole * uint32_t(limit) - refresh_hz;
      if (whole >= 2 && whole <= 4 && off <= whole) refreshes = whole;
    }
    g_swapChain->setSyncInterval(refreshes);
    const bool vsync_paces = vsync && (uint32_t(limit) + 1 >= refresh_hz || refreshes > 1);
    g_presentInterval.store(limit > 0 && !vsync_paces ? 1.0 / limit
                            : vsync ? double(refreshes) / refresh_hz : 0.0,
                            std::memory_order_relaxed);
    if (limit > 0 && !vsync_paces) {
      const uint64_t freq = rex::chrono::Clock::QueryHostTickFrequency();
      const uint64_t interval = std::max<uint64_t>(1, freq / uint64_t(limit));
      uint64_t now = rex::chrono::Clock::QueryHostTickCount();
      if (throttle_last_tick) {
        const uint64_t target = throttle_last_tick + interval;
        while (now < target) {
          const uint64_t remaining_us = (target - now) * 1000000 / freq;
          if (remaining_us > 1000) {
            SleepPrecise(
                std::chrono::microseconds(remaining_us - 500));
          } else {
            rex::thread::MaybeYield();
          }
          now = rex::chrono::Clock::QueryHostTickCount();
        }
        throttle_last_tick = (now - target < interval) ? target : now;
      } else {
        throttle_last_tick = now;
      }
    } else {
      throttle_last_tick = 0;
    }
  }
  // Forwards the vsync cvar: off presents with tearing allowed and
  // gpu_fps_cap paces alone, on keeps the swapchain synced to the monitor
  // refresh.
  g_swapChain->setVsyncEnabled(REXCVAR_GET(gpu_vsync));
  g_swapChainValid =
      g_swapChain->present(g_backBufferIndex, signalSemaphores, 1);
  overlay::NotifyPresented();  // the app records its next UI frame
  static uint64_t present_failures = 0;
  if (!g_swapChainValid && (present_failures++ % 256) == 0) {
    REXGPU_WARN(
        "videonative: present failed ({} so far — hr/DRED breadcrumbs in "
        "plume_present_dbg.txt next to the exe)",
        present_failures);
    if (present_failures == 1) {
      DumpBreadcrumb("present failure");
      DumpDrawRing("present failure");
    }
  }
  g_frame = (g_frame + 1) % kNumFrames;
  g_frameOpen = false;
}

// ---- State capture ----

void SetViewport(uint32_t x, uint32_t y, uint32_t w, uint32_t h, float min_z,
                 float max_z) {
  if (rq::Active()) { rq::EnqSetViewport(x, y, w, h, min_z, max_z); return; }
  g_state.viewport_x = x;
  g_state.viewport_y = y;
  g_state.viewport_w = w ? w : 1;
  g_state.viewport_h = h ? h : 1;
  g_state.viewport_min_z = min_z;
  g_state.viewport_max_z = max_z;
}
// Guest texture destroyed: its cached upload is dropped so a header reused for
// a different texture at the same guest address is not served the old content;
// host-rendered entries are kept, since their content comes from the GPU.
void InvalidateTextureByHeader(uint32_t header_addr) {
  if (!header_addr) return;
  if (rq::Active()) { rq::EnqInvalidateTexture(header_addr); return; }
  for (auto it = g_textureCache.begin(); it != g_textureCache.end();) {
    if (it->second.header_addr == header_addr && !it->second.host_rendered) {
      RetireEntry(it->second);  // fence-safe: descriptor scrubbed on drain
      it = g_textureCache.erase(it);
    } else {
      ++it;
    }
  }
}

void NoteRttBegin(uint32_t texbase, uint32_t width, uint32_t height,
                  uint32_t color_surf, uint32_t depth_surf, uint32_t dest_tex,
                  uint32_t format, uint32_t msaa, uint32_t tiling) {
  if (rq::Active()) {
    rq::EnqNoteRtt(texbase, width, height, color_surf, depth_surf, dest_tex,
                   format, msaa, tiling);
    return;
  }
  // Sanity gate: implausible dims mean the title read drifted TU offsets, so
  // the note is dropped and logged.
  if (width < 16 || width > 4096 || height < 16 || height > 4096) {
    static uint64_t bad_logs = 4;
    if (bad_logs) {
      bad_logs--;
      REXGPU_WARN(
          "videonative: [semrtt] note rejected texbase={:#x} dims {}x{} "
          "(possible offset drift)",
          texbase, width, height);
    }
    g_semRtt.active = false;
    return;
  }
  g_semRtt.active = true;
  g_semRtt.texbase = texbase;
  g_semRtt.width = width;
  g_semRtt.height = height;
  g_semRtt.color_surf = color_surf;
  g_semRtt.depth_surf = depth_surf;
  g_semRtt.dest_tex = dest_tex;
  g_semRtt.format = format;
  g_semRtt.msaa = msaa;
  g_semRtt.tiling = tiling;
}

void SetScissor(int32_t left, int32_t top, int32_t right, int32_t bottom) {
  if (rq::Active()) { rq::EnqSetScissor(left, top, right, bottom); return; }
  g_state.scissor[0] = left;
  g_state.scissor[1] = top;
  g_state.scissor[2] = right;
  g_state.scissor[3] = bottom;
  g_scissorExplicit = true;  // guest scissor wins over the bind-time reset
}
void SetTexture(uint32_t sampler, uint32_t header) {
  if (sampler >= 32) return;
  uint32_t dw[6] = {};
  if (header) {
    for (int i = 0; i < 6; i++) dw[i] = LoadGuestU32(header + i * 4);
  }
  if (rq::Active()) {
    rq::EnqSetTexture(sampler, header, dw);
    return;
  }
  SetTextureWithSnapshot(sampler, header, dw);
}

void SetTextureWithSnapshot(uint32_t sampler, uint32_t header,
                            const uint32_t* dw6) {
  if (sampler >= 32) return;
  g_state.textures[sampler] = header;
  // Bind-time fetch-dword snapshot: the XDK copies the header's 6 fetch dwords
  // into the device shadow at SetTexture time, so draws key the cache on those
  // dwords and not on a header the game may have released and recycled.
  if (header) {
    std::memcpy(g_state.texture_dw[sampler], dw6, 24);
  } else {
    std::memset(g_state.texture_dw[sampler], 0,
                sizeof(g_state.texture_dw[sampler]));
  }
}
void SetAdapterIsIntel(bool is_intel) { g_adapterIsIntel = is_intel; }

void SetStream(uint32_t stream, uint32_t vb_object, uint32_t offset_bytes,
               uint32_t stride_bytes) {
  if (rq::Active()) { rq::EnqSetStream(stream, vb_object, offset_bytes, stride_bytes); return; }
  if (stream >= 16) return;
  g_state.streams[stream] = {vb_object, offset_bytes, stride_bytes};
}
void SetIndices(uint32_t ib_object) {
  if (rq::Active()) { rq::EnqSetIndices(ib_object); return; } g_state.index_buffer_object = ib_object; }

void SetShaderConstantsF(bool pixel, uint32_t start_reg, uint32_t guest_data,
                         uint32_t vec4_count) {
  if (rq::Active()) { rq::EnqSetShaderConstantsF(pixel, start_reg, guest_data, vec4_count); return; }
  if (start_reg >= 256) return;
  vec4_count = std::min(vec4_count, 256 - start_reg);
  float* dst = (pixel ? g_state.ps_floats : g_state.vs_floats) + start_reg * 4;
  const uint32_t* src =
      rex::system::kernel_memory()->TranslateVirtual<const uint32_t*>(
          guest_data);
  for (uint32_t i = 0; i < vec4_count * 4; i++) {
    const uint32_t v = __builtin_bswap32(src[i]);
    std::memcpy(&dst[i], &v, 4);
  }
}

// Host-order data variant: replayed command-buffer constant sets carry the
// values captured at record time, since the guest source pointer is gone.
void SetShaderConstantsFHost(bool pixel, uint32_t start_reg,
                             const uint32_t* host_dwords, uint32_t vec4_count) {
  if (rq::Active()) { rq::EnqSetShaderConstantsFHost(pixel, start_reg, host_dwords, vec4_count); return; }
  if (start_reg >= 256) return;
  vec4_count = std::min(vec4_count, 256 - start_reg);
  float* dst = (pixel ? g_state.ps_floats : g_state.vs_floats) + start_reg * 4;
  std::memcpy(dst, host_dwords, size_t(vec4_count) * 16);
}

void PatchShaderFloatDwordsHost(bool pixel, uint32_t dword_index,
                                const uint32_t* host_dwords, uint32_t count) {
  if (rq::Active()) { rq::EnqPatchShaderFloatDwordsHost(pixel, dword_index, host_dwords, count); return; }
  if (dword_index >= 256 * 4) return;
  count = std::min(count, 256 * 4 - dword_index);
  float* dst = (pixel ? g_state.ps_floats : g_state.vs_floats) + dword_index;
  std::memcpy(dst, host_dwords, size_t(count) * 4);
}
void MergeShaderFloatDwordHost(bool pixel, uint32_t dword_index, uint32_t mask,
                               uint32_t orv) {
  if (rq::Active()) { rq::EnqMergeShaderFloatDwordHost(pixel, dword_index, mask, orv); return; }
  if (dword_index >= 256 * 4) return;
  float* dst = (pixel ? g_state.ps_floats : g_state.vs_floats) + dword_index;
  uint32_t v;
  std::memcpy(&v, dst, 4);
  v = (v & mask) | orv;
  std::memcpy(dst, &v, 4);
}

void Clear(uint32_t, uint32_t flags, uint32_t color, float z,
           const int32_t* rects, uint32_t rect_count) {
  if (rq::Active()) { rq::EnqClear(flags, color, z, rects, rect_count); return; }
  EnsureFrameOpen();
  if (!g_frameOpen) return;
  ApplyRenderTargetState();  // clears apply to the bound guest surfaces
  // Bracket-less per-tile loops (CC1 shadow window) interleave full-surface
  // clears between tile resolves, which would wipe later tiles' source in the
  // single-surface model, so they are suppressed on viewport-grown RTs and
  // bracketed tiled RTs at least 2048 wide; rect-scoped clears always pass.
  if (REXCVAR_GET(gpu_vp_grow) &&
      (g_activeRtGrown || (g_tilingWidth && g_activeRtWidth >= 2048)) &&
      g_passResolves > 0 && rect_count == 0) {
    return;
  }
  // Only a clear that executes invalidates the depth-staging plane memo, so the
  // suppressed clears above return before this bump.
  g_rtMutationSeq++;
  auto& commandList = g_commandLists[g_frame];
  // XDK pRects scope the clear, since per-tile command buffers clear only their
  // band.
  RenderRect clear_rects[4];
  const uint32_t n = std::min(rect_count, 4u);
  // Swapchain-targeted clears (no active RT) and full-res variants stay 1:1.
  const auto cc = [&](int32_t v) {
    return (g_activeRtKey && !g_activeRtFullRes) ? HostCoord(v) : v;
  };
  for (uint32_t i = 0; i < n; i++) {
    clear_rects[i] =
        RenderRect(cc(rects[i * 4 + 0]), cc(rects[i * 4 + 1]),
                   cc(rects[i * 4 + 2]), cc(rects[i * 4 + 3]));
  }
  // D3DCLEAR flags: bits 0-3 clear color and 0x30 clears depth (CC2 uses 0x1F
  // for all and 0x10 for z).
  if (flags & 0xF) {
    const RenderColor clear_color(
        ((color >> 16) & 0xFF) / 255.0f, ((color >> 8) & 0xFF) / 255.0f,
        (color & 0xFF) / 255.0f, ((color >> 24) & 0xFF) / 255.0f);
    // Bits 0-3 are color targets 0-3, and the swapchain framebuffer has only
    // one.
    const uint32_t attachment_count = g_activeRtKey ? kMrtCount : 1;
    for (uint32_t i = 0; i < attachment_count; i++) {
      if (flags & (1u << i)) {
        commandList->clearColor(i, clear_color, n ? clear_rects : nullptr, n);
      }
    }
  }
  if (flags & 0x30) {
    commandList->clearDepth(true, z, n ? clear_rects : nullptr, n);
  }
}

void SetTilingExtent(uint32_t width, uint32_t height) {
  if (rq::Active()) { rq::EnqSetTilingExtent(width, height); return; }
  g_tilingWidth = width;
  g_tilingHeight = height;
}

void RegisterSurface(uint32_t surface_obj, uint32_t width, uint32_t height,
                     uint32_t guest_format) {
  if (rq::Active()) { rq::EnqRegisterSurface(surface_obj, width, height, guest_format); return; }
  SurfaceInfo info;
  info.width = width;
  info.height = height;
  info.guest_format = guest_format;
  info.valid = width > 0 && width <= 8192 && height > 0 && height <= 8192;
  g_surfaces[surface_obj] = info;
}

void SetRenderTargetSurface(uint32_t index, uint32_t surface_obj) {
  if (rq::Active()) { rq::EnqSetRenderTargetSurface(index, surface_obj); return; }
  g_state.color_surface[index & 3u] = surface_obj;
}
void SetDepthSurface(uint32_t surface_obj) {
  if (rq::Active()) { rq::EnqSetDepthSurface(surface_obj); return; }
  g_state.depth_surface = surface_obj;
}

// Snapshot/restore of the captured guest state around command-buffer replay,
// since the XDK re-emits device shadow state after Run and CB binds must not
// leak.
static std::vector<GuestState> g_stateStack;
void PushState() {
  if (rq::Active()) { rq::EnqPushState(); return; } g_stateStack.push_back(g_state); }
void PopState() {
  if (rq::Active()) { rq::EnqPopState(); return; }
  if (g_stateStack.empty()) return;
  g_state = g_stateStack.back();
  g_stateStack.pop_back();
}

// XDK Run contract: a command buffer inherits the device's current constants,
// so the replay mirror is seeded from the live device float shadow and recorded
// in-CB sets overlay afterwards.
void SeedReplayFloatConstants(uint32_t device, uint32_t recording_device) {
  if (rq::Active()) { rq::EnqSeedReplayFloatConstants(device, recording_device); return; }
  const uint32_t* vs_src =
      reinterpret_cast<const uint32_t*>(GuestPtr(device + 1920));
  const uint32_t* ps_src =
      reinterpret_cast<const uint32_t*>(GuestPtr(device + 6016));
  uint32_t* vdst = reinterpret_cast<uint32_t*>(g_state.vs_floats);
  uint32_t* pdst = reinterpret_cast<uint32_t*>(g_state.ps_floats);
  for (uint32_t i = 0; i < 256 * 4; i++) {
    vdst[i] = __builtin_bswap32(vs_src[i]);
    pdst[i] = __builtin_bswap32(ps_src[i]);
  }
  if (!recording_device || recording_device == device) return;
  // Registers the run device never touched but the recording device holds
  // execute with the recorded values, as on hardware (see renderer.h).
  const uint32_t* rec[2] = {
      reinterpret_cast<const uint32_t*>(GuestPtr(recording_device + 1920)),
      reinterpret_cast<const uint32_t*>(GuestPtr(recording_device + 6016))};
  uint32_t* dst[2] = {vdst, pdst};
  for (int stage = 0; stage < 2; stage++) {
    for (uint32_t r = 0; r < 256; r++) {
      uint32_t* d = dst[stage] + r * 4;
      if (d[0] | d[1] | d[2] | d[3]) continue;  // live value wins
      const uint32_t* s = rec[stage] + r * 4;
      const uint32_t v0 = __builtin_bswap32(s[0]);
      const uint32_t v1 = __builtin_bswap32(s[1]);
      const uint32_t v2 = __builtin_bswap32(s[2]);
      const uint32_t v3 = __builtin_bswap32(s[3]);
      if (!(v0 | v1 | v2 | v3)) continue;
      d[0] = v0;
      d[1] = v1;
      d[2] = v2;
      d[3] = v3;
    }
  }
}

// Render-state override for replayed draws (see renderer.h).
static uint32_t g_replayDrawState[10];
static uint32_t g_replayDrawStateDirty = 0;
static bool g_replayDrawStateValid = false;
void ApplyReplayStatePersistent(const uint32_t* state10,
                                uint32_t dirty_mask) {
  if (rq::Active()) { rq::EnqApplyReplayStatePersistent(state10, dirty_mask); return; }
  if (!state10) return;
  if (dirty_mask & (1u << 1)) g_fxBlendControl[0] = state10[1];
  if (dirty_mask & (1u << 4)) g_fxColorMask = state10[4];
  if (dirty_mask & (1u << 7)) g_fxBlendControl[1] = state10[7];
  if (dirty_mask & (1u << 8)) g_fxBlendControl[2] = state10[8];
  if (dirty_mask & (1u << 9)) g_fxBlendControl[3] = state10[9];
}


void ApplyBlendControlDirect(uint32_t rt, uint32_t value) {
  if (rq::Active()) { rq::EnqApplyBlendControlDirect(rt, value); return; }
  if (rt < kMrtCount) g_fxBlendControl[rt] = value;
}

// Folds the run device's live blend/mask programming (dev+16 dirty bits and
// register shadows) into the effective register file and drains the bits,
// modelling the XDK Run pre-flush; called at RunCommandBuffer start and at live
// draws, not at replayed draws.
void ConsumeLiveStateDirty(uint32_t device) {
  // The guest dirty bits are read and cleared at call time, since the game
  // re-sets them between calls, and the captured values apply on the worker in
  // order.
  const uint64_t dirty_hi = LoadGuestU32(device + 16);
  const uint64_t dirty_lo = LoadGuestU32(device + 20);
  uint64_t dirty = (dirty_hi << 32) | dirty_lo;
  const uint64_t consumed =
      dirty & ((1ull << 10) | (1ull << 3) | (1ull << 2) | (1ull << 1) | 1ull);
  uint32_t vals[5] = {0, 0, 0, 0, 0};
  if (dirty & (1ull << 10)) vals[0] = LoadGuestU32(device + 10552);
  if (dirty & (1ull << 2)) vals[1] = LoadGuestU32(device + 10584);
  if (dirty & (1ull << 1)) vals[2] = LoadGuestU32(device + 10588);
  if (dirty & (1ull << 0)) vals[3] = LoadGuestU32(device + 10592);
  if (dirty & (1ull << 3)) vals[4] = LoadGuestU32(device + 10580);
  if (consumed) {
    dirty &= ~consumed;
    StoreGuestU32(device + 16, uint32_t(dirty >> 32));
    StoreGuestU32(device + 20, uint32_t(dirty));
  }
  if (rq::Active()) {
    rq::EnqApplyLiveStateDirty(uint32_t(consumed), vals);
    return;
  }
  ApplyLiveStateDirtyHost(uint32_t(consumed), vals);
}

void ApplyLiveStateDirtyHost(uint32_t consumed_mask, const uint32_t vals[5]) {
  if (consumed_mask & (1u << 10)) g_fxBlendControl[0] = vals[0];
  if (consumed_mask & (1u << 2)) g_fxBlendControl[1] = vals[1];
  if (consumed_mask & (1u << 1)) g_fxBlendControl[2] = vals[2];
  if (consumed_mask & (1u << 0)) g_fxBlendControl[3] = vals[3];
  if (consumed_mask & (1u << 3)) g_fxColorMask = vals[4];
}
double PresentInterval() { return g_presentInterval.load(std::memory_order_relaxed); }

void SetReplayDrawState(const uint32_t* state10, uint32_t dirty_mask) {
  if (rq::Active()) { rq::EnqSetReplayDrawState(state10, dirty_mask); return; }
  if (state10) {
    std::memcpy(g_replayDrawState, state10, sizeof(g_replayDrawState));
    g_replayDrawStateDirty = dirty_mask;
    g_replayDrawStateValid = true;
  } else {
    g_replayDrawStateDirty = 0;
    g_replayDrawStateValid = false;
  }
}

// Bool/loop constants captured from the recording device at record time (40
// host-order dwords), since the recompiled SetShaderConstantB/I setters write
// the recording device's shadow block, which the live draw device never sees.
static uint32_t g_replayBoolLoop[40];
static bool g_replayBoolLoopValid = false;
static uint32_t g_replayFetch[32][6];
static uint32_t g_replayFetchValid = 0;  // bit per slot
void SetReplayFetchConstants(const uint32_t* records, uint32_t count) {
  if (rq::Active()) { rq::EnqSetReplayFetchConstants(records, count); return; }
  g_replayFetchValid = 0;
  for (uint32_t r = 0; r < count; r++) {
    const uint32_t slot = records[r * 7];
    if (slot >= 32) continue;
    std::memcpy(g_replayFetch[slot], &records[r * 7 + 1], 24);
    g_replayFetchValid |= 1u << slot;
  }
}

void SetReplayBoolConstants(const uint32_t* dwords40) {
  if (rq::Active()) { rq::EnqSetReplayBoolConstants(dwords40); return; }
  if (dwords40) {
    std::memcpy(g_replayBoolLoop, dwords40, sizeof(g_replayBoolLoop));
    g_replayBoolLoopValid = true;
  } else {
    g_replayBoolLoopValid = false;
  }
}

// FPS overlay (renderer_fps.h): a persistent RGBA8 strip re-uploaded when the
// readout changes and drawn into a top-left viewport box, with g_blitPipeline
// and the swapchain already bound by the caller at the tail of SwapFrontbuffer.
namespace {
void DrawFpsOverlay(std::unique_ptr<RenderCommandList>& commandList) {
  const fps::Overlay overlay = fps::Tick();
  if (!overlay.visible) return;

  bool created = false;
  if (!g_fpsTexture) {
    if (g_nextTextureDescriptor >= kFirstReservedDescriptor) return;
    g_fpsTexture = g_device->createTexture(RenderTextureDesc::Texture2D(
        overlay.width, overlay.height, 1, RenderFormat::R8G8B8A8_UNORM));
    g_fpsView = g_fpsTexture->createTextureView(
        RenderTextureViewDesc::Texture2DArray(RenderFormat::R8G8B8A8_UNORM));
    g_fpsDescriptor = g_nextTextureDescriptor++;
    g_textureDescriptorSet->setTexture(g_fpsDescriptor, g_fpsTexture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       g_fpsView.get());
    created = true;
  }

  if (overlay.dirty || created) {
    const uint32_t row_bytes = overlay.width * 4;
    const uint32_t padded_row_bytes = (row_bytes + 255u) & ~255u;
    UploadAllocation staging = g_upload[g_frame].Allocate(
        (padded_row_bytes * overlay.height + 511) & ~511u, 512);
    for (uint32_t y = 0; y < overlay.height; y++) {
      std::memcpy(staging.memory + y * padded_row_bytes,
                  overlay.pixels + y * row_bytes, row_bytes);
    }
    commandList->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(g_fpsTexture.get(),
                                               RenderTextureLayout::COPY_DEST));
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(g_fpsTexture.get(), 0, 0),
        RenderTextureCopyLocation::PlacedFootprint(
            staging.buffer, RenderFormat::R8G8B8A8_UNORM, overlay.width,
            overlay.height, 1, padded_row_bytes / 4, staging.offset));
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(g_fpsTexture.get(),
                             RenderTextureLayout::SHADER_READ));
  }

  // Top-left of the game image, which is inset from the window when
  // letterboxed; the rasterizer chooses the scale.
  if (g_fpsPipeline) commandList->setPipeline(g_fpsPipeline.get());
  constexpr float kMargin = 8.0f;
  const float box_w = overlay.width * overlay.scale;
  const float box_h = overlay.height * overlay.scale;
  const float box_x = g_presentRect[0] + kMargin;
  const float box_y = g_presentRect[1] + kMargin;
  // The scissor crops the strip to the text's width so short readouts do not
  // trail dead backdrop on the right.
  const float used_w =
      (overlay.used_width ? overlay.used_width : overlay.width) *
      overlay.scale;
  commandList->setViewports(
      RenderViewport(box_x, box_y, box_w, box_h, 0.0f, 1.0f));
  commandList->setScissors(
      RenderRect(int32_t(box_x), int32_t(box_y),
                 int32_t(box_x + used_w), int32_t(box_y + box_h)));
  uint32_t indices[32] = {g_fpsDescriptor};
  UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(indices), 256);
  std::memcpy(b4.memory, indices, sizeof(indices));
  commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                         kRootB4IndicesPs);
  commandList->drawInstanced(3, 1, 0, 0);
  MarkDraw(commandList.get(), 3, false, 0);
}

// ImGui overlay (overlay.h): replays the UI thread's newest recorded frame over
// the whole swap chain image at the tail of SwapFrontbuffer, with its textures
// in the bindless 2D table like any other texture.
void ReleaseOverlayTexture(OverlayHostTexture& host) {
  if (host.descriptor > 0 && host.descriptor < kFirstReservedDescriptor) {
    g_textureDescriptorSet->setTexture(host.descriptor, g_blankTexture.get(),
                                       RenderTextureLayout::SHADER_READ,
                                       g_blankTextureView.get());
    g_freeDescriptors.push_back(host.descriptor);
  }
  host = {};
}

void DrawUiOverlay(std::unique_ptr<RenderCommandList>& commandList) {
  if (!g_overlayPipeline) return;
  std::shared_ptr<const overlay::Frame> frame = overlay::LatestFrame();
  // This slot's previous command list has passed its fence, so the frame it
  // drew and textures only that frame used can go.
  g_overlayInFlight[g_frame] = frame;
  for (auto it = g_overlayTextures.begin(); it != g_overlayTextures.end();) {
    if (it->second.owner.expired()) {
      ReleaseOverlayTexture(it->second);
      it = g_overlayTextures.erase(it);
    } else {
      ++it;
    }
  }
  if (!frame || frame->draws.empty() || frame->vertices.empty() || frame->indices.empty() ||
      frame->width <= 0.0f || frame->height <= 0.0f) {
    return;
  }
  if (!g_overlayWhite) {
    g_overlayWhite = std::make_shared<overlay::TextureData>();
    g_overlayWhite->width = g_overlayWhite->height = 1;
    g_overlayWhite->rgba = {0xFF, 0xFF, 0xFF, 0xFF};
  }

  // Textures this frame uses for the first time are created, uploaded and bound
  // to a slot.
  for (const overlay::Draw& draw : frame->draws) {
    const std::shared_ptr<overlay::TextureData>& data = draw.texture ? draw.texture : g_overlayWhite;
    auto [it, inserted] = g_overlayTextures.try_emplace(data.get());
    if (!inserted) continue;
    OverlayHostTexture& host = it->second;
    const uint32_t row_bytes = data->width * 4;
    const uint32_t padded_row_bytes = (row_bytes + 255u) & ~255u;
    const uint64_t upload_bytes = (uint64_t(padded_row_bytes) * data->height + 511) & ~511ull;
    host.owner = data;
    host.descriptor = upload_bytes <= kUploadBufferSize ? AllocTextureDescriptor() : 0;
    if (host.descriptor) {
      host.texture = g_device->createTexture(RenderTextureDesc::Texture2D(
          data->width, data->height, 1, RenderFormat::R8G8B8A8_UNORM));
    }
    if (!host.texture || data->rgba.size() < size_t(row_bytes) * data->height) {
      REXGPU_WARN("videonative: overlay texture {}x{} unavailable", data->width, data->height);
      ReleaseOverlayTexture(host);
      host.owner = data;  // stays in the table (descriptor 0): not retried every frame
      continue;
    }
    host.view = host.texture->createTextureView(
        RenderTextureViewDesc::Texture2DArray(RenderFormat::R8G8B8A8_UNORM));
    g_textureDescriptorSet->setTexture(host.descriptor, host.texture.get(),
                                       RenderTextureLayout::SHADER_READ, host.view.get());
    UploadAllocation staging = g_upload[g_frame].Allocate(uint32_t(upload_bytes), 512);
    for (uint32_t y = 0; y < data->height; y++) {
      std::memcpy(staging.memory + size_t(y) * padded_row_bytes,
                  data->rgba.data() + size_t(y) * row_bytes, row_bytes);
    }
    commandList->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(host.texture.get(), RenderTextureLayout::COPY_DEST));
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::Subresource(host.texture.get(), 0, 0),
        RenderTextureCopyLocation::PlacedFootprint(staging.buffer, RenderFormat::R8G8B8A8_UNORM,
                                                   data->width, data->height, 1,
                                                   padded_row_bytes / 4, staging.offset));
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(host.texture.get(), RenderTextureLayout::SHADER_READ));
    if (data != g_overlayWhite) std::vector<uint8_t>().swap(data->rgba);
  }

  // Vertices go up already in clip space, since the UI's coordinate space
  // covers the whole window.
  const uint32_t vertex_bytes = uint32_t(frame->vertices.size() * sizeof(rex::ui::ImmediateVertex));
  const uint32_t index_bytes = uint32_t((frame->indices.size() * sizeof(uint16_t) + 3) & ~size_t(3));
  if (uint64_t(vertex_bytes) + index_bytes > kUploadBufferSize / 2) return;
  UploadAllocation vertices = g_upload[g_frame].Allocate(vertex_bytes, 256);
  UploadAllocation indices = g_upload[g_frame].Allocate(index_bytes, 256);
  auto* out = reinterpret_cast<rex::ui::ImmediateVertex*>(vertices.memory);
  const float to_x = 2.0f / frame->width, to_y = -2.0f / frame->height;
  for (size_t i = 0; i < frame->vertices.size(); i++) {
    out[i] = frame->vertices[i];
    out[i].x = out[i].x * to_x - 1.0f;
    out[i].y = out[i].y * to_y + 1.0f;
  }
  std::memcpy(indices.memory, frame->indices.data(), frame->indices.size() * sizeof(uint16_t));

  const float width = float(g_swapChain->getWidth());
  const float height = float(g_swapChain->getHeight());
  commandList->setPipeline(g_overlayPipeline.get());
  const RenderVertexBufferView vertex_view(vertices.buffer->at(vertices.offset), vertex_bytes);
  const RenderInputSlot slot(0, sizeof(rex::ui::ImmediateVertex));
  commandList->setVertexBuffers(0, &vertex_view, 1, &slot);
  const RenderIndexBufferView index_view(indices.buffer->at(indices.offset), index_bytes,
                                         RenderFormat::R16_UINT);
  commandList->setIndexBuffer(&index_view);
  commandList->setViewports(RenderViewport(0.0f, 0.0f, width, height, 0.0f, 1.0f));

  const float sx = width / frame->width, sy = height / frame->height;
  const overlay::TextureData* bound = nullptr;
  for (const overlay::Draw& draw : frame->draws) {
    const overlay::TextureData* data = draw.texture ? draw.texture.get() : g_overlayWhite.get();
    const auto it = g_overlayTextures.find(data);
    if (it == g_overlayTextures.end() || !it->second.descriptor) continue;
    const int32_t left = std::clamp(int32_t(draw.clip[0] * sx), 0, int32_t(width));
    const int32_t top = std::clamp(int32_t(draw.clip[1] * sy), 0, int32_t(height));
    const int32_t right = std::clamp(int32_t(draw.clip[2] * sx + 0.5f), 0, int32_t(width));
    const int32_t bottom = std::clamp(int32_t(draw.clip[3] * sy + 0.5f), 0, int32_t(height));
    if (right <= left || bottom <= top) continue;
    commandList->setScissors(RenderRect(left, top, right, bottom));
    if (data != bound) {
      bound = data;
      // (texture slot, sampler): 1 = linear clamp, 3 = point clamp.
      uint32_t b4_indices[32] = {it->second.descriptor, data->linear ? 1u : 3u};
      UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(b4_indices), 256);
      std::memcpy(b4.memory, b4_indices, sizeof(b4_indices));
      commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset), kRootB4IndicesPs);
    }
    commandList->drawIndexedInstanced(draw.index_count, 1, draw.first_index, draw.base_vertex, 0);
    MarkDraw(commandList.get(), draw.index_count, true, 0);
  }
}
}  // namespace

// Lazily creates the 1x1 all-white R32F texture the shadowwhite debug
// substitutes for the sun shadow map.
void EnsureWhiteTexture() {
  if (g_whiteTexture || !g_frameOpen || !g_device) return;
  g_whiteTexture = g_device->createTexture(
      RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R32_FLOAT));
  if (!g_whiteTexture) return;
  g_whiteTextureView = g_whiteTexture->createTextureView(
      RenderTextureViewDesc::Texture2DArray(RenderFormat::R32_FLOAT));
  if (g_nextTextureDescriptor < kFirstReservedDescriptor) {  // 2045+ reserved blanks
    g_whiteTextureDescriptor = g_nextTextureDescriptor++;
    g_textureDescriptorSet->setTexture(
        g_whiteTextureDescriptor, g_whiteTexture.get(),
        RenderTextureLayout::SHADER_READ, g_whiteTextureView.get());
  }
  auto& commandList = g_commandLists[g_frame];
  commandList->barriers(RenderBarrierStage::COPY,
                        RenderTextureBarrier(g_whiteTexture.get(),
                                             RenderTextureLayout::COPY_DEST));
  UploadAllocation staging = g_upload[g_frame].Allocate(512, 512);
  if (!staging.memory) return;
  const float one = 1.0f;
  std::memcpy(staging.memory, &one, 4);
  commandList->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(g_whiteTexture.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(
          staging.buffer, RenderFormat::R32_FLOAT, 1, 1, 1, 64,
          staging.offset));
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(g_whiteTexture.get(),
                                             RenderTextureLayout::SHADER_READ));
  REXGPU_INFO("videonative: white shadow texture -> desc {}",
              g_whiteTextureDescriptor);
}

// Uploads the guest gamma ramp (3 rows of 256 R16 values) into the LUT texture
// the present blit samples.
void UpdateGammaRamp(const uint16_t* rgb768) {
  if (rq::Active()) { rq::EnqUpdateGammaRamp(rgb768); return; }
  EnsureFrameOpen();
  if (!g_frameOpen || !g_device) return;
  if (!g_gammaRampTexture) {
    g_gammaRampTexture = g_device->createTexture(
        RenderTextureDesc::Texture2D(256, 3, 1, RenderFormat::R16_UNORM));
    if (!g_gammaRampTexture) return;
    g_gammaRampView = g_gammaRampTexture->createTextureView(
        RenderTextureViewDesc::Texture2DArray(RenderFormat::R16_UNORM));
    if (g_nextTextureDescriptor < kFirstReservedDescriptor) {  // 2045+ reserved blanks
      g_gammaRampDescriptor = g_nextTextureDescriptor++;
      g_textureDescriptorSet->setTexture(
          g_gammaRampDescriptor, g_gammaRampTexture.get(),
          RenderTextureLayout::SHADER_READ, g_gammaRampView.get());
    }
    REXGPU_INFO("videonative: gamma ramp LUT created -> desc {}",
                g_gammaRampDescriptor);
  }
  auto& commandList = g_commandLists[g_frame];
  commandList->barriers(RenderBarrierStage::COPY,
                        RenderTextureBarrier(g_gammaRampTexture.get(),
                                             RenderTextureLayout::COPY_DEST));
  // 3 rows of 256 R16 texels = 512 bytes/row, 256-aligned as placed footprints
  // require.
  UploadAllocation staging = g_upload[g_frame].Allocate(512 * 3, 512);
  if (!staging.memory) return;
  std::memcpy(staging.memory, rgb768, 512 * 3);
  commandList->copyTextureRegion(
      RenderTextureCopyLocation::Subresource(g_gammaRampTexture.get(), 0),
      RenderTextureCopyLocation::PlacedFootprint(staging.buffer,
                                                 RenderFormat::R16_UNORM, 256,
                                                 3, 1, 256, staging.offset));
  commandList->barriers(RenderBarrierStage::GRAPHICS,
                        RenderTextureBarrier(g_gammaRampTexture.get(),
                                             RenderTextureLayout::SHADER_READ));
}

void SwapFrontbuffer(uint32_t frontbuffer_header) {
  if (rq::Active()) { rq::EnqSwapFrontbuffer(frontbuffer_header); return; }
  if (!frontbuffer_header || !g_swapChain) return;
  // Frame boundary: the RTT note is retired so a stale one never outlives its
  // frame, and a fresh StartTextureRendering re-arms it.
  g_semRtt.active = false;
  EnsureFrameOpen();
  if (!g_frameOpen) return;

  if (!g_blitTried) {
    g_blitTried = true;
    const auto& vs = detail::BlitVsDxil();
    const auto& ps = detail::BlitPsDxil();
    if (!vs.empty() && !ps.empty()) {
      g_blitVs = g_device->createShader(vs.data(), vs.size(), "main",
                                        RenderShaderFormat::DXIL);
      g_blitPs = g_device->createShader(ps.data(), ps.size(), "main",
                                        RenderShaderFormat::DXIL);
      RenderGraphicsPipelineDesc desc;
      desc.pipelineLayout = g_pipelineLayout.get();
      desc.vertexShader = g_blitVs.get();
      desc.pixelShader = g_blitPs.get();
      desc.renderTargetFormat[0] = kColorFormat;
      desc.renderTargetCount = 1;
      desc.depthTargetFormat = kDepthFormat;
      desc.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
      desc.depthEnabled = false;
      desc.depthWriteEnabled = false;
      desc.cullMode = RenderCullMode::NONE;
      g_blitPipeline = g_device->createGraphicsPipeline(desc);
      // FPS overlay: the same pair with a PS that preserves sampled alpha,
      // since blit.ps forces a = 1, plus alpha blending.
      g_fpsPs = g_device->createShader(kFpsOverlayPsDxil,
                                       sizeof(kFpsOverlayPsDxil), "main",
                                       RenderShaderFormat::DXIL);
      desc.pixelShader = g_fpsPs.get();
      desc.renderTargetBlend[0] = RenderBlendDesc::AlphaBlend();
      g_fpsPipeline = g_device->createGraphicsPipeline(desc);
      // ImGui overlay: alpha-blended like the FPS strip, with vertex input
      // (position, uv, color) instead of the blit's generated triangle.
      g_overlayVs = g_device->createShader(kOverlayVsDxil, sizeof(kOverlayVsDxil), "main",
                                           RenderShaderFormat::DXIL);
      g_overlayPs = g_device->createShader(kOverlayPsDxil, sizeof(kOverlayPsDxil), "main",
                                           RenderShaderFormat::DXIL);
      const RenderInputSlot overlay_slot(0, sizeof(rex::ui::ImmediateVertex));
      const RenderInputElement overlay_elements[] = {
          {"POSITION", 0, 0, RenderFormat::R32G32_FLOAT, 0, offsetof(rex::ui::ImmediateVertex, x)},
          {"TEXCOORD", 0, 1, RenderFormat::R32G32_FLOAT, 0, offsetof(rex::ui::ImmediateVertex, u)},
          {"COLOR", 0, 2, RenderFormat::R8G8B8A8_UNORM, 0,
           offsetof(rex::ui::ImmediateVertex, color)},
      };
      desc.vertexShader = g_overlayVs.get();
      desc.pixelShader = g_overlayPs.get();
      desc.inputSlots = &overlay_slot;
      desc.inputSlotsCount = 1;
      desc.inputElements = overlay_elements;
      desc.inputElementsCount = 3;
      g_overlayPipeline = g_device->createGraphicsPipeline(desc);
    }
  }
  static uint64_t swap_log_budget = 8;
  if (!g_blitPipeline) {
    if (swap_log_budget) {
      swap_log_budget--;
      REXGPU_WARN("videonative: [swaplog] no blit pipeline");
    }
    return;
  }

  CachedTexture* tex = GetOrCreateTexture(frontbuffer_header, true, 0, 0,
                                          RenderFormat::UNKNOWN, nullptr,
                                          /*resolve_scale=*/-1);
  // Presents the last resolve into a texture of the frontbuffer's size, which
  // is the finished image since scene resolves precede the composite resolve,
  // independent of the game's front/back texture-pointer flip.
  if (tex && tex->valid) {
    const uint64_t size_key = SizeKeyOf(tex->width, tex->height);
    auto last = g_lastResolvedBySize.find(size_key);
    if (last != g_lastResolvedBySize.end()) {
      tex = last->second;
      // performance_mode UI sharpness: the surface that produced the presented
      // resolve is the composite+UI surface and renders 1:1 from its next bind
      // on.
      if (Scaled() && g_hsNum < g_hsDen &&
          REXCVAR_GET(gpu_perfui)) {
        auto surf = g_lastResolveSurface.find(size_key);
        if (surf != g_lastResolveSurface.end() && surf->second) {
          // Confirms or learns with the current frame stamp; stale entries age
          // out via kFullResConfirmWindow.
          auto ins = g_fullResSurfaces.insert({surf->second, g_frameIndex});
          if (!ins.second) {
            ins.first->second = g_frameIndex;
          } else {
            REXGPU_INFO(
                "videonative: [perfui] composite+UI surface {:#x} learned — "
                "renders at full resolution from the next bind",
                surf->second);
          }
        }
      }
    }
  }
  // Debug: shows the light-accum or shadow-map resolve instead
  // (lightaccum_toggle cycles the mode).
  if (g_debugShowLightAccum == 1 && g_lastLightAccumResolve &&
      g_lastLightAccumResolve->valid) {
    tex = g_lastLightAccumResolve;
  } else if (g_debugShowLightAccum == 2 && g_lastShadowMapResolve &&
             g_lastShadowMapResolve->valid) {
    tex = g_lastShadowMapResolve;
  } else if (g_debugShowLightAccum == 3 && g_prevFmt19Resolve &&
             g_prevFmt19Resolve->valid) {
    tex = g_prevFmt19Resolve;
  }
  if (!tex || !tex->valid || !tex->descriptor_index) return;
  auto& commandList = g_commandLists[g_frame];
  if (tex->layout != RenderTextureLayout::SHADER_READ) {
    commandList->barriers(
        RenderBarrierStage::GRAPHICS,
        RenderTextureBarrier(tex->texture.get(),
                             RenderTextureLayout::SHADER_READ));
    tex->layout = RenderTextureLayout::SHADER_READ;
  }

  // Back to the swapchain framebuffer for the composite, with all surfaces read
  // as unbound so a lingering depth surface does not route the blit at the
  // depth-only host RT (see ApplyRenderTargetState).
  uint32_t saved_surfaces[kMrtCount];
  std::memcpy(saved_surfaces, g_state.color_surface, sizeof(saved_surfaces));
  const uint32_t saved_depth = g_state.depth_surface;
  std::memset(g_state.color_surface, 0, sizeof(g_state.color_surface));
  g_state.depth_surface = 0;
  ApplyRenderTargetState();
  std::memcpy(g_state.color_surface, saved_surfaces, sizeof(saved_surfaces));
  g_state.depth_surface = saved_depth;

  commandList->setPipeline(g_blitPipeline.get());
  // Aspect-preserving fit by default: the 16:9 game image is scaled to the
  // largest centered box that fits the swapchain, with bars painted by the
  // frame-open clear; gpu_stretch fills instead.
  {
    const float sw = float(g_swapChain->getWidth());
    const float sh = float(g_swapChain->getHeight());
    float vx = 0.0f, vy = 0.0f, vw = sw, vh = sh;
    if (!REXCVAR_GET(gpu_stretch) && tex->height > 0) {
      const float aspect = float(tex->width) / float(tex->height);
      if (sw / sh > aspect) {
        vw = sh * aspect;
        vx = std::floor((sw - vw) * 0.5f);
      } else {
        vh = sw / aspect;
        vy = std::floor((sh - vh) * 0.5f);
      }
    }
    g_presentRect[0] = vx;
    g_presentRect[1] = vy;
    g_presentRect[2] = vw;
    g_presentRect[3] = vh;
    commandList->setViewports(RenderViewport(vx, vy, vw, vh, 0.0f, 1.0f));
    commandList->setScissors(RenderRect(int32_t(vx), int32_t(vy),
                                        int32_t(vx + vw), int32_t(vy + vh)));
  }
  uint32_t indices[32] = {tex->descriptor_index};
  // b4 [0].y is the gamma-ramp LUT descriptor (0 = no ramp), applied on top of
  // the frontbuffer sample in blit.ps.
  indices[1] =
      REXCVAR_GET(gpu_gamma_ramp) ? g_gammaRampDescriptor : 0;
  UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(indices), 256);
  std::memcpy(b4.memory, indices, sizeof(indices));
  commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                         kRootB4IndicesPs);
  commandList->drawInstanced(3, 1, 0, 0);
  MarkDraw(commandList.get(), 3, false, 0);

  DrawFpsOverlay(commandList);
  DrawUiOverlay(commandList);
}

// Copies the bound render target into the destination guest texture's host
// cache entry, which draws sampling the header then hit.
namespace {
// Shared body for color and depth resolves: copies the clamped source region of
// the active RT into the destination texture at the dest point.
void ResolveRegion(uint32_t dest_texture_header, bool depth, uint32_t source,
                   bool has_rect, int32_t src_left, int32_t src_top,
                   int32_t src_right, int32_t src_bottom, int32_t dest_x,
                   int32_t dest_y) {
  if (!dest_texture_header) return;
  g_passResolves++;  // per-tile-loop detector (reset on target change)
  EnsureFrameOpen();
  if (!g_frameOpen) return;
  ApplyRenderTargetState();
  auto& commandList = g_commandLists[g_frame];

  RenderTexture* src = nullptr;
  CachedRenderTarget* rt = nullptr;
  const uint32_t rt_index = std::min(source, kMrtCount - 1);
  uint32_t src_w = 0, src_h = 0;
  // The source is the bound render target, as hardware resolves read EDRAM at
  // the current RB_COLOR_INFO binding, falling back to the last-drawn RT only
  // when nothing is bound.
  const uint64_t src_key = g_activeRtKey ? g_activeRtKey : g_lastDrawnRtKey;
  if (src_key != 0) {
    auto it = g_renderTargets.find(src_key);
    if (it == g_renderTargets.end()) return;
    rt = &it->second;
    src = depth ? rt->depth.get() : rt->color[rt_index].get();
    src_w = rt->width;
    src_h = rt->height;
  } else {
    // Swapchain fallback, which returns early under a downscale so the 1:1 box
    // math after this block is safe for both source kinds.
    if (depth) return;  // swapchain depth copies not supported (never tiled)
    if (Scaled()) {
      static uint64_t sc_skip_logs = 4;
      if (sc_skip_logs) {
        sc_skip_logs--;
        REXGPU_WARN(
            "videonative: swapchain-source resolve skipped under "
            "resolution_scale (unscaled source, scaled dest)");
      }
      return;
    }
    src = const_cast<RenderTexture*>(g_swapChain->getTexture(g_backBufferIndex));
    src_w = g_swapChain->getWidth();
    src_h = g_swapChain->getHeight();
  }

  // The destination is allocated at guest header dimensions and with the
  // source's host format, since CopyTextureRegion needs same-family formats:
  // depth lands in R32_FLOAT through a staging buffer and color in the source
  // RT's format.
  const RenderFormat required =
      depth ? RenderFormat::R32_FLOAT
            : (rt ? rt->formats[rt_index] : kColorFormat);
  // A full-res source (the composite+UI surface) resolves 1:1 into a full-res
  // destination, so the present blit samples a sharp image.
  const bool src_full = rt && rt->full_res;
  CachedTexture* dest = GetOrCreateTexture(dest_texture_header, true, 0, 0,
                                           required, nullptr,
                                           src_full ? 1 : 0);
  if (!dest || !dest->valid) return;
  if (dest->host_format != required) {
    static uint64_t mismatch_logs = 8;
    if (mismatch_logs) {
      mismatch_logs--;
      REXGPU_WARN(
          "videonative: resolve format mismatch (depth={} src={} dest={}) — "
          "skipped",
          depth, uint32_t(required), uint32_t(dest->host_format));
    }
    return;
  }
  // Without a rect the whole source resolves to the dest origin.
  if (!has_rect) {
    src_left = 0;
    src_top = 0;
    src_right = int32_t(src_w);
    src_bottom = int32_t(src_h);
    dest_x = 0;
    dest_y = 0;
  }
  if (depth) {
    // One-shot capture of the frame containing the shadow-tile renders, which
    // fire on player movement and not every frame.
    if (REXCVAR_GET(gpu_capture_on_dres) && src_right >= 2048) {
      static bool dres_fired = false;
      if (!dres_fired) {
        dres_fired = true;
        static auto rdoc_dres = rex::ui::RenderDocAPI::CreateIfConnected();
        if (rdoc_dres) {
          rdoc_dres->api_1_0_0()->TriggerCapture();
          REXGPU_INFO("videonative: [dres] capture triggered (next frame)");
        }
      }
    }
  }
  // Un-bracketed tiled resolves (CC1's course lightmap and shadow tiles): the
  // game re-renders each slice into a tile-height surface and resolves a
  // frame-coord rect whose origin matches the dest origin, so a rect that
  // exceeds the RT is re-based to row/column 0.
  if (has_rect && src_bottom > int32_t(src_h) && dest_y == src_top) {
    src_bottom -= src_top;
    src_top = 0;
  }
  if (has_rect && src_right > int32_t(src_w) && dest_x == src_left) {
    src_right -= src_left;
    src_left = 0;
  }
  src_left = std::clamp(src_left, 0, int32_t(src_w));
  src_top = std::clamp(src_top, 0, int32_t(src_h));
  src_right = std::clamp(src_right, src_left, int32_t(src_w));
  src_bottom = std::clamp(src_bottom, src_top, int32_t(src_h));
  dest_x = std::clamp(dest_x, 0, int32_t(dest->width));
  dest_y = std::clamp(dest_y, 0, int32_t(dest->height));
  const int32_t copy_w =
      std::min(src_right - src_left, int32_t(dest->width) - dest_x);
  const int32_t copy_h =
      std::min(src_bottom - src_top, int32_t(dest->height) - dest_y);
  if (copy_w <= 0 || copy_h <= 0) return;

  // Entry transitions: lazy mode consults the tracked layouts so a chain of
  // resolves from one source RT pays a single source transition, and eager mode
  // (default) transitions unconditionally.
  const bool lazy = REXCVAR_GET(gpu_resolve_lazy);
  const RenderTextureLayout src_now =
      rt ? (depth ? rt->depth_layout : rt->color_layout[rt_index])
         : RenderTextureLayout::UNKNOWN;
  uint32_t entry_count = 0;
  RenderTextureBarrier entry[2];
  if (!lazy || src_now != RenderTextureLayout::COPY_SOURCE ||
      LazyLedgerStale(src, src_now, "resolve-src", src_key, rt_index)) {
    entry[entry_count++] =
        RenderTextureBarrier(src, RenderTextureLayout::COPY_SOURCE);
  }
  if (!lazy || dest->layout != RenderTextureLayout::COPY_DEST ||
      LazyLedgerStale(dest->texture.get(), dest->layout, "resolve-dest",
                      dest->descriptor_index, 0)) {
    entry[entry_count++] = RenderTextureBarrier(
        dest->texture.get(), RenderTextureLayout::COPY_DEST);
  }
  if (entry_count) {
    commandList->barriers(RenderBarrierStage::COPY, entry, entry_count);
  }
  if (rt) {
    if (depth) {
      rt->depth_layout = RenderTextureLayout::COPY_SOURCE;
    } else {
      rt->color_layout[rt_index] = RenderTextureLayout::COPY_SOURCE;
    }
  }
  dest->layout = RenderTextureLayout::COPY_DEST;

  if (depth) {
    // The whole depth plane goes to a staging buffer, as the D3D12 depth-copy
    // rule requires, then the requested region to the guest-sized R32
    // destination; all dims and rects here are host units.
    const uint32_t host_w = src_full ? src_w : HostDim(src_w);
    const uint32_t host_h = src_full ? src_h : HostDim(src_h);
    // Shader resolve: renders the region in one read pass and one write pass
    // instead of staging the plane.
    if (REXCVAR_GET(gpu_depth_resolve_shader) && g_blitVs &&
        dest->resolve_fb == nullptr && dest->rt_capable) {
      const RenderTexture* color = dest->texture.get();
      RenderFramebufferDesc fbDesc;
      fbDesc.colorAttachments = &color;
      fbDesc.colorAttachmentsCount = 1;
      fbDesc.depthAttachment = nullptr;
      dest->resolve_fb = g_device->createFramebuffer(fbDesc);
    }
    if (REXCVAR_GET(gpu_depth_resolve_shader) && rt && g_blitVs &&
        dest->resolve_fb) {
      // Lazy R32_FLOAT SRV + bindless slot for the RT's depth plane.
      if (!rt->depth_descriptor &&
          g_nextTextureDescriptor < kFirstReservedDescriptor) {
        rt->depth_srv = rt->depth->createTextureView(
            RenderTextureViewDesc::Texture2DArray(RenderFormat::R32_FLOAT));
        rt->depth_descriptor = g_nextTextureDescriptor++;
        g_textureDescriptorSet->setTexture(rt->depth_descriptor,
                                           rt->depth.get(),
                                           RenderTextureLayout::SHADER_READ,
                                           rt->depth_srv.get());
      }
      if (!g_depthResolvePs) {
        g_depthResolvePs = g_device->createShader(
            kDepthResolvePsDxil, sizeof(kDepthResolvePsDxil), "main",
            RenderShaderFormat::DXIL);
      }
      if (!g_depthResolvePipeline && g_depthResolvePs) {
        RenderGraphicsPipelineDesc pd;
        pd.pipelineLayout = g_pipelineLayout.get();
        pd.vertexShader = g_blitVs.get();
        pd.pixelShader = g_depthResolvePs.get();
        pd.renderTargetFormat[0] = RenderFormat::R32_FLOAT;
        pd.renderTargetCount = 1;
        pd.depthTargetFormat = RenderFormat::UNKNOWN;
        pd.primitiveTopology = RenderPrimitiveTopology::TRIANGLE_LIST;
        pd.depthEnabled = false;
        pd.depthWriteEnabled = false;
        pd.cullMode = RenderCullMode::NONE;
        g_depthResolvePipeline = g_device->createGraphicsPipeline(pd);
      }
      if (rt->depth_descriptor && g_depthResolvePipeline) {
        const auto rcs = [&](int32_t v) { return src_full ? v : HostCoord(v); };
        // Source must be readable, destination writable.
        if (rt->depth_layout != RenderTextureLayout::SHADER_READ) {
          commandList->barriers(
              RenderBarrierStage::GRAPHICS,
              RenderTextureBarrier(rt->depth.get(),
                                   RenderTextureLayout::SHADER_READ));
          rt->depth_layout = RenderTextureLayout::SHADER_READ;
        }
        commandList->barriers(
            RenderBarrierStage::GRAPHICS,
            RenderTextureBarrier(dest->texture.get(),
                                 RenderTextureLayout::COLOR_WRITE));
        dest->layout = RenderTextureLayout::COLOR_WRITE;
        commandList->setFramebuffer(dest->resolve_fb.get());
        const int32_t dx0 = rcs(dest_x), dy0 = rcs(dest_y);
        const int32_t dw = rcs(dest_x + copy_w) - dx0;
        const int32_t dh = rcs(dest_y + copy_h) - dy0;
        commandList->setViewports(RenderViewport(float(dx0), float(dy0),
                                                 float(dw), float(dh), 0.0f,
                                                 1.0f));
        commandList->setScissors(RenderRect(dx0, dy0, dx0 + dw, dy0 + dh));
        commandList->setPipeline(g_depthResolvePipeline.get());
        commandList->setGraphicsPipelineLayout(g_pipelineLayout.get());
        uint32_t idx[32] = {};
        idx[0] = rt->depth_descriptor;
        const int32_t delta_x = rcs(src_left) - dx0;
        const int32_t delta_y = rcs(src_top) - dy0;
        std::memcpy(&idx[4], &delta_x, 4);   // b4[1].x
        std::memcpy(&idx[5], &delta_y, 4);   // b4[1].y
        const uint32_t src_hw = src_full ? src_w : HostDim(src_w);
        const uint32_t src_hh = src_full ? src_h : HostDim(src_h);
        idx[8] = src_hw;                     // b4[2].x
        idx[9] = src_hh;                     // b4[2].y
        UploadAllocation b4 = g_upload[g_frame].Allocate(sizeof(idx), 256);
        std::memcpy(b4.memory, idx, sizeof(idx));
        commandList->setGraphicsRootDescriptor(b4.buffer->at(b4.offset),
                                               kRootB4IndicesPs);
        commandList->drawInstanced(3, 1, 0, 0);
        // Back to sampleable; the RT rebinds on the next draw.
        commandList->barriers(
            RenderBarrierStage::GRAPHICS,
            RenderTextureBarrier(dest->texture.get(),
                                 RenderTextureLayout::SHADER_READ));
        dest->layout = RenderTextureLayout::SHADER_READ;
        dest->host_rendered = true;
        dest->last_resolve_frame = g_frameIndex + 1;
        g_activeRtKey = ~0ull;  // force a framebuffer rebind before drawing
        return;
      }
    }
    const uint32_t row_pitch = (host_w * 4 + 255) & ~255u;
    const uint64_t needed = uint64_t(row_pitch) * host_h;
    // Plane-copy memo: CC1's shadow map resolves 4 bands from one unchanged
    // render, so the whole-plane copy happens only for the first band, and any
    // draw or clear bumps g_rtMutationSeq and forces a fresh copy.
    RenderBuffer* plane = nullptr;
    if (g_depthStageMemo.src == src && g_depthStageMemo.buffer &&
        g_depthStageMemo.frame == g_frameIndex &&
        g_depthStageMemo.mutation == g_rtMutationSeq &&
        g_depthStageMemo.host_w == host_w &&
        g_depthStageMemo.host_h == host_h &&
        g_depthStageMemo.row_pitch == row_pitch) {
      plane = g_depthStageMemo.buffer;  // already staged this frame
    } else {
      DepthStagingSlot& slot = g_depthStaging[g_depthStagingRot++ % 8];
      if (!slot.buffer || slot.size < needed) {
        slot.buffer =
            g_device->createBuffer(RenderBufferDesc::DefaultBuffer(needed));
        slot.buffer->setName(
            fmt::format("depth staging {}", (g_depthStagingRot - 1) % 8));
        slot.size = needed;
      }
      plane = slot.buffer.get();
      // Explicit write-state barrier: without it the first copy implicitly
      // promotes the buffer to COPY_DEST while plume's tracked state stays
      // stale, and the read barrier below is then ignored by D3D12, leaving the
      // two copies unsynchronized.
      commandList->barriers(RenderBarrierStage::COPY,
                            RenderBufferBarrier(plane,
                                                RenderBufferAccess::WRITE));
      commandList->copyTextureRegion(
          RenderTextureCopyLocation::PlacedFootprint(
              plane, RenderFormat::R32_FLOAT, host_w, host_h,
              1, row_pitch / 4, 0),
          RenderTextureCopyLocation::Subresource(src, 0));
      commandList->barriers(RenderBarrierStage::COPY,
                            RenderBufferBarrier(plane,
                                                RenderBufferAccess::READ));
      g_depthStageMemo = {src,    plane,  g_frameIndex, g_rtMutationSeq,
                          host_w, host_h, row_pitch};
    }
    const auto rc = [&](int32_t v) { return src_full ? v : HostCoord(v); };
    RenderBox box(rc(src_left), rc(src_top), rc(src_left + copy_w),
                  rc(src_top + copy_h));
    // Sub-pixel after a downscale means nothing to copy, and an empty box is a
    // D3D12 error.
    if (box.right > box.left && box.bottom > box.top) {
      commandList->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(dest->texture.get(), 0),
          RenderTextureCopyLocation::PlacedFootprint(
              plane, RenderFormat::R32_FLOAT, host_w, host_h,
              1, row_pitch / 4, 0),
          uint32_t(rc(dest_x)), uint32_t(rc(dest_y)), 0, &box);
    }
  } else {
    const auto rc = [&](int32_t v) { return src_full ? v : HostCoord(v); };
    RenderBox box(rc(src_left), rc(src_top), rc(src_left + copy_w),
                  rc(src_top + copy_h));
    if (box.right > box.left && box.bottom > box.top) {
      commandList->copyTextureRegion(
          RenderTextureCopyLocation::Subresource(dest->texture.get(), 0),
          RenderTextureCopyLocation::Subresource(src, 0),
          uint32_t(rc(dest_x)), uint32_t(rc(dest_y)), 0, &box);
    }
  }

  // Exit transitions: dest -> SHADER_READ stays eager since draw-time sampling
  // has no on-demand transition hook, and the source's return to render state
  // is the lazy candidate.
  uint32_t exit_count = 0;
  RenderTextureBarrier exits[2];
  if (!lazy) {
    exits[exit_count++] =
        RenderTextureBarrier(src, depth ? RenderTextureLayout::DEPTH_WRITE
                                        : RenderTextureLayout::COLOR_WRITE);
    if (rt) {
      if (depth) {
        rt->depth_layout = RenderTextureLayout::DEPTH_WRITE;
      } else {
        rt->color_layout[rt_index] = RenderTextureLayout::COLOR_WRITE;
      }
    }
  } else if (rt && RtKey(rt) == g_activeRtKey) {
    // Mid-pass resolve from the active RT: the layout stays as is and the next
    // draw or same-key rebind repairs it on demand.
    g_activeRtLayoutsDirty = true;
  }
  exits[exit_count++] = RenderTextureBarrier(dest->texture.get(),
                                             RenderTextureLayout::SHADER_READ);
  commandList->barriers(RenderBarrierStage::GRAPHICS, exits, exit_count);
  dest->layout = RenderTextureLayout::SHADER_READ;
  dest->last_resolve_frame = g_frameIndex + 1;
  // Resolve write-back queue for guest-visible resolve bytes (see the flush
  // above EndFrameAndPresent): 32bpp color and optionally depth, delivered at
  // the guest's next fence wait.
  const bool wb_color =
      !depth && (required == RenderFormat::R8G8B8A8_UNORM ||
                 required == RenderFormat::R10G10B10A2_UNORM);
  const bool wb_depth = depth && REXCVAR_GET(gpu_depth_writeback);
  const bool wb_skip =
      dest->gpu_sampled && REXCVAR_GET(gpu_wb_skip_sampled);
  if ((wb_color || wb_depth) && !wb_skip &&
      REXCVAR_GET(gpu_resolve_writeback) && copy_w > 0 &&
      copy_h > 0 && g_resolveWritebacks.size() < 512 && !Scaled()) {
    const uint32_t dw0 = LoadGuestU32(dest_texture_header + 0);
    const uint32_t dw1 = LoadGuestU32(dest_texture_header + 4);
    // The rect is snapshotted into the readback arena here so a deferred flush
    // delivers this frame's bytes; a grow flushes first so pending records
    // never reference a freed buffer.
    const uint32_t snap_pitch = (uint32_t(copy_w) * 4u + 255u) & ~255u;
    const uint32_t snap_need =
        (snap_pitch * uint32_t(copy_h) + 255u) & ~255u;
    if (!g_wbSnapBuffer || g_wbSnapUsed + snap_need > g_wbSnapCap) {
      FlushResolveWritebacksInline();
      if (g_wbSnapCap < snap_need * 2) {
        g_wbSnapCap = std::max(8u * 1024u * 1024u, snap_need * 2);
        g_wbSnapBuffer.reset();
        g_wbSnapBuffer = g_device->createBuffer(
            RenderBufferDesc::ReadbackBuffer(g_wbSnapCap));
      }
    }
    commandList->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(
                              dest->texture.get(),
                              RenderTextureLayout::COPY_SOURCE));
    RenderBox snap_box(int32_t(dest_x), int32_t(dest_y),
                       int32_t(dest_x + copy_w), int32_t(dest_y + copy_h));
    commandList->copyTextureRegion(
        RenderTextureCopyLocation::PlacedFootprint(
            g_wbSnapBuffer.get(), required, uint32_t(copy_w),
            uint32_t(copy_h), 1, snap_pitch / 4, g_wbSnapUsed),
        RenderTextureCopyLocation::Subresource(dest->texture.get(), 0), 0, 0,
        0, &snap_box);
    commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(
                              dest->texture.get(),
                              RenderTextureLayout::SHADER_READ));
    g_resolveWritebacks.push_back(ResolveWriteback{
        required, dw1 & 0xFFFFF000u, ((dw0 >> 22) & 0x1FFu) * 32u,
        (dw1 >> 6) & 3u, ((dw0 >> 31) & 1u) != 0, dest->width, dest->height,
        uint32_t(dest_x), uint32_t(dest_y), uint32_t(copy_w),
        uint32_t(copy_h), g_wbSnapUsed, snap_pitch, wb_depth,
        wb_color && required == RenderFormat::R8G8B8A8_UNORM &&
            ((dw0 >> 2) & 3u) == 3u &&
            REXCVAR_GET(gpu_gamma_writeback)});
    g_wbSnapUsed += snap_need;
    g_resolveWritebacksPending.store(uint32_t(g_resolveWritebacks.size()),
                                     std::memory_order_release);
  }
  if (!depth) {
    g_lastResolvedBySize[SizeKeyOf(dest->width, dest->height)] = dest;
    // The surface bound at this resolve, which is the composite+UI surface if
    // the present pick selects this entry (see SwapFrontbuffer).
    g_lastResolveSurface[SizeKeyOf(dest->width, dest->height)] =
        g_state.color_surface[0];
    if (required == RenderFormat::R10G10B10A2_UNORM) {
      if (g_lastLightAccumResolve != dest) {
        g_prevFmt19Resolve = g_lastLightAccumResolve;  // the GBuffer normals
      }
      g_lastLightAccumResolve = dest;
    }
  } else if (dest->width == dest->height && dest->width >= 512) {
    g_lastShadowMapResolve = dest;  // square depth resolve = the shadow map
  }
}
}  // namespace

void ResolveToTexture(uint32_t dest_texture_header, uint32_t source,
                      bool has_rect, int32_t src_left, int32_t src_top,
                      int32_t src_right, int32_t src_bottom, int32_t dest_x,
                      int32_t dest_y) {
  if (rq::Active()) { rq::EnqResolveToTexture(dest_texture_header, source, has_rect, src_left, src_top, src_right, src_bottom, dest_x, dest_y); return; }
  ResolveRegion(dest_texture_header, false, source, has_rect, src_left,
                src_top, src_right, src_bottom, dest_x, dest_y);
}

void ResolveDepthToTexture(uint32_t dest_texture_header, bool has_rect,
                           int32_t src_left, int32_t src_top,
                           int32_t src_right, int32_t src_bottom,
                           int32_t dest_x, int32_t dest_y) {
  if (rq::Active()) { rq::EnqResolveDepthToTexture(dest_texture_header, has_rect, src_left, src_top, src_right, src_bottom, dest_x, dest_y); return; }
  ResolveRegion(dest_texture_header, true, 0, has_rect, src_left, src_top,
                src_right, src_bottom, dest_x, dest_y);
}

// ---- Draws ----

namespace {

struct DrawSetup {
  const ResolvedShader* vs;
  const ResolvedShader* ps;
  RenderPrimitiveTopology topology;
  bool rect = false;
  bool point = false;
};

bool TranslateTopology(uint32_t prim_type, RenderPrimitiveTopology* out,
                       bool* rect, bool* point) {
  *rect = false;
  *point = false;
  switch (prim_type) {
    case 4: *out = RenderPrimitiveTopology::TRIANGLE_LIST; return true;
    case 6: *out = RenderPrimitiveTopology::TRIANGLE_STRIP; return true;
    case 2: *out = RenderPrimitiveTopology::LINE_LIST; return true;
    case 3: *out = RenderPrimitiveTopology::LINE_STRIP; return true;
    case 1:  // POINTLIST: sprites expanded by the pack's point GS, sized by the
             // VS oPts export.
      *out = RenderPrimitiveTopology::POINT_LIST;
      *point = true;
      return true;
    case 8:  // RECTLIST: triangles expanded by the pack's geometry shader.
      *out = RenderPrimitiveTopology::TRIANGLE_LIST;
      *rect = !REXCVAR_GET(gpu_rect_gs_off);
      return true;
    default: return false;  // quadlist (13) is unsupported
  }
}

// Uploads vertex data for the draw and writes the vertex fetch constant pair
// for vfetch constant 95-stream into the fetch constant array.
void BindStreamData(uint32_t* fetch_dwords, uint32_t stream,
                    const void* vertex_data, uint32_t size_bytes,
                    bool immediate = false, uint32_t stride = 0,
                    uint32_t guest_key = 0) {
  // A VB size that cannot fit the ring is garbage from a stale guest object, so
  // the stream is skipped and the draw reads the blank fetch.
  if (size_bytes == 0 || size_bytes > kUploadBufferSize / 2) {
    static uint64_t bad_stream_logs = 8;
    if (bad_stream_logs) {
      bad_stream_logs--;
      REXGPU_WARN("videonative: stream {} size {} bytes implausible — skipped",
                  stream, size_bytes);
    }
    return;
  }
  // The identity component of the cache key is the guest address when known,
  // since under the render queue vertex_data may be an arena snapshot whose
  // host pointer changes every draw.
  const struct {
    uint64_t ptr;
    uint32_t size;
  } ck{guest_key != 0
           ? uint64_t(guest_key)
           : uint64_t(reinterpret_cast<uintptr_t>(vertex_data)),
       size_bytes};
  // Uploads are keyed by content, since the game rewrites its shared UP scratch
  // (unk_82ECE7F0) and its pooled stream VBs between draws of one frame;
  // buffers over 64 KB hash head, middle and tail windows.
  uint64_t cache_key;
  if (immediate) {
    cache_key = XXH3_64bits(vertex_data, size_bytes);
  } else if (size_bytes <= 64 * 1024) {
    cache_key = XXH3_64bits(vertex_data, size_bytes) ^ ck.ptr;
  } else {
    const uint8_t* p = static_cast<const uint8_t*>(vertex_data);
    constexpr uint32_t kWin = 16 * 1024;
    XXH3_state_t* st = XXH3_createState();
    XXH3_64bits_reset(st);
    XXH3_64bits_update(st, &ck, sizeof(ck));
    XXH3_64bits_update(st, p, kWin);
    XXH3_64bits_update(st, p + (size_bytes / 2 & ~15u), kWin);
    XXH3_64bits_update(st, p + size_bytes - kWin, kWin);
    cache_key = XXH3_64bits_digest(st);
    XXH3_freeState(st);
  }
  uint32_t ring_offset = 0xFFFFFFFFu;
  // Tier 1: persistent region for stream VBs; the cache key embeds the content
  // hash, so a mutated buffer misses here, and the demotion map keeps streaming
  // pools from leaking the region.
  if (!immediate && guest_key && REXCVAR_GET(gpu_vb_persist)) {
    auto git = g_vbPersistByGuest.find(guest_key);
    const bool demoted =
        git != g_vbPersistByGuest.end() && git->second.changes >= 2;
    if (!demoted) {
      auto pit = g_vbPersistCache.find(cache_key);
      if (pit != g_vbPersistCache.end()) {
        ring_offset = pit->second;
      } else {
        PersistGuestInfo& gi = git != g_vbPersistByGuest.end()
                                   ? git->second
                                   : g_vbPersistByGuest[guest_key];
        if (gi.last_key && gi.last_key != cache_key) {
          gi.changes++;
          g_vbPersistOrphans += gi.size;  // that resident alloc is dead
        }
        if (gi.changes < 2) {
          const uint32_t poff = PersistAlloc(size_bytes);
          if (poff != 0xFFFFFFFFu) {
            bool stored = false;
            VertexRing& cur = g_vertexRing[g_frame];
            if (cur.gpu_local) {
              // One staging write is copied into every parity's GPU buffer,
              // which same-queue ordering makes safe since the other parity's
              // in-flight reads complete before this list executes.
              cur.staging_offset = (cur.staging_offset + 15) & ~15u;
              if (cur.staging_offset + size_bytes <= kVbStagingSize) {
                const uint32_t soff = cur.staging_offset;
                std::memcpy(cur.staging_memory + soff, vertex_data,
                            size_bytes);
                cur.staging_offset += size_bytes;
                auto* cl = g_commandLists[g_frame].get();
                for (uint32_t i = 0; i < kNumFrames; i++) {
                  g_vertexRing[i].ToCopyState(cl);
                  cl->copyBufferRegion(g_vertexRing[i].buffer->at(poff),
                                       cur.staging->at(soff), size_bytes);
                }
                for (uint32_t i = 0; i < kNumFrames; i++) {
                  if (i != uint32_t(g_frame)) g_vertexRing[i].ToReadState(cl);
                }
                stored = true;
              }
            } else {
              for (uint32_t i = 0; i < kNumFrames; i++) {
                std::memcpy(g_vertexRing[i].memory + poff, vertex_data,
                            size_bytes);
              }
              stored = true;
            }
            if (stored) {
              g_vbPersistCache.emplace(cache_key, poff);
              gi.last_key = cache_key;
              gi.size = size_bytes;
              ring_offset = poff;
            }
          }
        }
      }
    }
  }
  // Tier 2: the per-frame ring (UP/immediate data, demoted dynamic VBs,
  // persistent-region overflow).
  if (ring_offset == 0xFFFFFFFFu) {
  auto cached = g_vbUploadCache.find(cache_key);
  if (cached != g_vbUploadCache.end()) {
    ring_offset = cached->second;
  } else {
    UploadAllocation alloc = g_vertexRing[g_frame].Allocate(size_bytes);
    if (!alloc.memory) {
      static uint64_t ring_full_logs = 8;
      if (ring_full_logs) {
        ring_full_logs--;
        REXGPU_WARN(
            "videonative: vertex ring exhausted (stream {} size {}) - draw "
            "fetches stale data",
            stream, size_bytes);
      }
      return;
    }
    // Under the render queue the copy happens at draw time, since enqueue
    // snapshots already hold hardware-time bytes and present-time copies would
    // outlive their arena backing.
    if (immediate || rq::Running() ||
        !REXCVAR_GET(gpu_deferred_vb_copy)) {
      std::memcpy(alloc.memory, vertex_data, size_bytes);
      // A/B test (dcc2_fx_alpha_pop_test): zeroes exact-0xFF alpha bytes in
      // small FX UP batches (24-byte verts: pos12 + packed color4 + uv8).
      if (REXCVAR_GET(dcc2_fx_alpha_pop_test) && stride == 24 &&
          size_bytes <= 256 * 24 && immediate) {
        uint8_t* v = static_cast<uint8_t*>(alloc.memory);
        for (uint32_t off = 12; off + 4 <= size_bytes; off += 24) {
          // Packed color u32 at +12, with both byte orders' alpha lanes tested.
          if (v[off + 3] == 0xFF) v[off + 3] = 0;
          if (v[off] == 0xFF) v[off] = 0;
        }
      }
    } else {
      g_deferredCopies.push_back({vertex_data, alloc.memory, size_bytes, 0});
    }
    // In gpu-local mode the bytes above went to staging, so the copy into the
    // GPU buffer is recorded before the draw that fetches them.
    g_vertexRing[g_frame].CommitCopy(g_commandLists[g_frame].get(),
                                     uint32_t(alloc.offset),
                                     g_vertexRing[g_frame].staging_offset,
                                     size_bytes);
    ring_offset = uint32_t(alloc.offset);
    g_vbUploadCache.emplace(cache_key, ring_offset);
  }
  }
  g_drawVfetchWrittenMask |= 1u << stream;
  const uint32_t vfetch_index = 95 - stream;
  // xe_gpu_vertex_fetch_t: dword0 = byte address with the type in the low 2
  // bits, dword1 = endian:2 + size-in-words:24, with endian 2 = 8-in-32 for raw
  // guest big-endian data in the ring.
  fetch_dwords[vfetch_index * 2 + 0] = ring_offset | 3u;
  fetch_dwords[vfetch_index * 2 + 1] = 2u | (((size_bytes + 3) >> 2) << 2);
}

static const void* g_upOverrideVertexData = nullptr;
static const void* g_upOverrideIndexData = nullptr;

bool SetupDraw(uint32_t device, uint32_t prim_type, DrawSetup* setup,
               uint32_t up_vertex_data, uint32_t up_size_bytes,
               uint32_t up_stride_bytes, int32_t vertex_offset) {
  g_currentDevice = device;  // RB shadow fold source (ApplyRenderTargetState)
  EnsureFrameOpen();
  if (!g_frameOpen) {
    return false;
  }
  RepairActiveRtLayouts();  // lazy mid-pass resolves leave WRITE on demand
  g_drawsThisFrame++;
  g_rtMutationSeq++;  // invalidates the depth-staging plane memo

  setup->vs = detail::CurrentResolvedVertexShader();
  setup->ps = detail::CurrentResolvedPixelShader();
  // TDR forensics ring, always on: the last draws' shader pairs, dumped when a
  // present or resize fails, which bounds the in-flight suspects on OS builds
  // without DRED.
  {
    DrawRec& r = g_drawRing[g_drawRingPos++ % kDrawRingSize];
    r.vs = setup->vs ? setup->vs->ucode_hash : 0;
    r.ps = setup->ps ? setup->ps->ucode_hash : 0;
    r.frame = uint32_t(g_frameIndex);
  }
  if (!setup->vs || !setup->vs->pack) {
    return false;
  }
  // TDR triage by subtraction (gpu_skip_vs, comma-separated hex list),
  // with a parse-once cache.
  {
    static std::string skip_str;
    static std::vector<uint64_t> skip_hashes;
    const std::string& skip = REXCVAR_GET(gpu_skip_vs);
    if (skip != skip_str) {
      skip_str = skip;
      skip_hashes.clear();
      size_t pos = 0;
      while (pos < skip.size()) {
        const uint64_t h = strtoull(skip.c_str() + pos, nullptr, 16);
        if (h) skip_hashes.push_back(h);
        const size_t comma = skip.find(',', pos);
        if (comma == std::string::npos) break;
        pos = comma + 1;
      }
    }
    for (const uint64_t h : skip_hashes) {
      if (setup->vs->ucode_hash == h) {
        return false;
      }
    }
  }
  if (!setup->ps || !setup->ps->pack) {
    if (detail::CurrentDrawPixelShaderBound()) {
      // A PS is bound but did not resolve (pack miss), so the draw is skipped.
      return false;
    }
    // No PS bound at all: a deliberate depth-only draw with a PS-less pipeline.
    setup->ps = nullptr;
  }
  if (!TranslateTopology(prim_type, &setup->topology, &setup->rect,
                         &setup->point)) {
    return false;
  }

  // Targets the framebuffer of the bound guest surfaces, ahead of pipeline
  // creation since the PSO's RT format depends on it.
  ApplyRenderTargetState();
  // Bracket-less tiled pass: when the draw's viewport is taller than the bound
  // host RT and width-compatible (CC1's 2048x2048 shadow window into a 2080x544
  // tile surface), the viewport is posted as the wanted extent, the RT is
  // re-applied grown, and the grown RT is cleared to far depth.
  if (REXCVAR_GET(gpu_vp_grow) && g_activeRtKey &&
      g_state.viewport_h > g_activeRtHeight &&
      g_state.viewport_h <= 4096 && g_state.viewport_w <= 4096 &&
      g_state.viewport_x == 0 && g_state.viewport_y == 0) {
    const SurfaceInfo cinfo = ParseSurface(g_state.color_surface[0]);
    const SurfaceInfo dinfo = ParseSurface(g_state.depth_surface);
    const SurfaceInfo& binfo = cinfo.valid ? cinfo : dinfo;
    if (binfo.valid && binfo.width >= g_state.viewport_w &&
        binfo.width <= g_state.viewport_w + 64 &&
        binfo.height < g_state.viewport_h) {
      g_growWidth = g_state.viewport_w;
      g_growHeight = g_state.viewport_h;
      g_growSurface = cinfo.valid ? g_state.color_surface[0]
                                  : g_state.depth_surface;
      g_activeRtKey = ~0ull;  // force the re-apply to run the expansion
      ApplyRenderTargetState();
      auto& cl = g_commandLists[g_frame];
      cl->clearDepth(true, 1.0f, nullptr, 0);
      cl->clearColor(0, RenderColor(0.0f, 0.0f, 0.0f, 0.0f), nullptr, 0);
      g_passResolves = 0;
    }
  }
  g_lastDrawnRtKey = g_activeRtKey;

  // Render state comes from the shadow registers the recompiled XDK setters
  // keep in the guest device block (0x2200-block at dev+10548, 0x2100-block at
  // dev+10444); for command-buffer replays the state captured on the recording
  // device overrides them.
  uint32_t depth_control, blend_control[kMrtCount], color_control, cull_bits,
      color_mask, alpha_ref_bits;
  bool z_enable;
  // Live dirty bits are consumed only on live draws, since RunCommandBuffer
  // already consumed them once at Run start as the XDK pre-flush does, ahead of
  // the CB's own blend writes.
  if (!g_replayDrawStateValid && !rq::OnWorker()) {
    // Non-queue path only: under the queue, consumption happens on the guest
    // side of the enqueue (see DrawVertices), since writing cleared dirty words
    // from the worker would race the guest thread.
    ConsumeLiveStateDirty(device);
  }
  if (g_replayDrawStateValid) {
    depth_control = g_replayDrawState[0];
    blend_control[0] = g_replayDrawState[1];
    color_control = g_replayDrawState[2];
    cull_bits = g_replayDrawState[3] & 7;
    color_mask = g_replayDrawState[4];
    alpha_ref_bits = g_replayDrawState[5];
    z_enable = g_replayDrawState[6] != 0;  // composed at record time
    // Recorded blend/mask slots the game programmed on the recording device
    // (CbCall::st_dirty) update the persistent effective register file and
    // persist after the command buffer ends, like PM4 register writes on
    // hardware.
    if (g_replayDrawStateDirty & (1u << 1))
      g_fxBlendControl[0] = g_replayDrawState[1];
    if (g_replayDrawStateDirty & (1u << 4))
      g_fxColorMask = g_replayDrawState[4];
    if (g_replayDrawStateDirty & (1u << 7))
      g_fxBlendControl[1] = g_replayDrawState[7];
    if (g_replayDrawStateDirty & (1u << 8))
      g_fxBlendControl[2] = g_replayDrawState[8];
    if (g_replayDrawStateDirty & (1u << 9))
      g_fxBlendControl[3] = g_replayDrawState[9];
  } else {
    depth_control = LoadGuestU32(device + 10548);    // RB_DEPTHCONTROL 0x2200
    color_control = LoadGuestU32(device + 10556);   // RB_COLORCONTROL 0x2202
    cull_bits = LoadGuestU32(device + 10568) & 7;   // PA_SU_SC_MODE_CNTL
    alpha_ref_bits = LoadGuestU32(device + 10500);  // RB_ALPHA_REF 0x210E
    // The shadow's z_enable bit (bit1) depends on a DS-binding shadow only the
    // replaced XDK functions maintained, so it is recomposed from the raw
    // request (dev+12308) and the host-side depth-surface binding.
    z_enable = (LoadGuestU32(device + 12308) & 1u) != 0 &&
               g_state.depth_surface != 0;
    // XDK live-draw invariant: the XDK re-dirties every state group after a
    // RunCommandBuffer, so live draws take blend/mask from the live shadow and
    // re-sync the persistent register file to it.
    g_fxBlendControl[0] = LoadGuestU32(device + 10552);
    g_fxBlendControl[1] = LoadGuestU32(device + 10584);
    g_fxBlendControl[2] = LoadGuestU32(device + 10588);
    g_fxBlendControl[3] = LoadGuestU32(device + 10592);
    g_fxColorMask = LoadGuestU32(device + 10580);
  }
  // PA_CL_CLIP_CNTL (dev+10564): low 6 bits enable the user clip planes
  // uploaded via D3DDevice_SetClipPlane (float4 at dev+10272+16*i), read from
  // the live shadow in both branches since command buffers carry no clip-state
  // slot.
  const uint32_t ucp_mask = LoadGuestU32(device + 10564) & 0x3Fu;
  // Both branches consume the effective register file.
  for (uint32_t i = 0; i < kMrtCount; i++) blend_control[i] = g_fxBlendControl[i];
  color_mask = g_fxColorMask;
  depth_control = (depth_control & ~2u) | (z_enable ? 2u : 0u);
  // A raw-zero RB_BLENDCONTROL decodes as src*ZERO + dst*ZERO, so RT1-3 shadows
  // the game never writes take the hardware default 0x00010001 (ONE/ZERO).
  for (uint32_t i = 1; i < kMrtCount; i++) {
    if (blend_control[i] == 0) blend_control[i] = 0x00010001;
  }

  // Depth-only (shadow-map) passes render Y-flipped so the resolved depth
  // texture is stored in guest row order for the lighting shaders' analytic
  // light-space UVs; gpu_shadow_flip drops the flip for A/B.
  const bool flip_y =
      g_depthOnlyTarget && REXCVAR_GET(gpu_shadow_flip);
  if (flip_y) cull_bits |= 8;  // winding flips with the projection

  const CachedPipeline* pipeline =
      GetOrCreatePipeline(setup->vs, setup->ps, blend_control, depth_control,
                          cull_bits, setup->topology, setup->rect,
                          setup->point, ucp_mask != 0, color_mask);
  if (ucp_mask && (setup->rect || setup->point)) {
    // The expansion GS owns the stage, so this draw goes unclipped with a
    // budgeted warning.
    static uint64_t ucp_combo_budget = 8;
    if (ucp_combo_budget) {
      ucp_combo_budget--;
      REXGPU_WARN(
          "videonative: user clip planes on a rect/point expansion draw "
          "(mask={:#x}) — GS stage taken, draw is unclipped",
          ucp_mask);
    }
  }
  if (!pipeline || !pipeline->pipeline) {
    return false;
  }

  auto& commandList = g_commandLists[g_frame];
  commandList->setPipeline(pipeline->pipeline.get());

  // Viewport and scissor: a reversed guest depth range (MinZ > MaxZ) is illegal
  // as a D3D12 viewport, so the sorted range is passed and the reversal folded
  // into the shader-side z transform (ndc_scale.z = -1, ndc_offset.z = 1), as
  // in the ring's GetHostViewportInfo.
  float vp_min_z = std::clamp(g_state.viewport_min_z, 0.0f, 1.0f);
  float vp_max_z = std::clamp(g_state.viewport_max_z, 0.0f, 1.0f);
  // Shadow depth-convention A/B: halving the depth-only pass's viewport range
  // stores map values in the ring's halved float24 convention (live trigger
  // shadowhalf_toggle).
  if (g_depthOnlyTarget && REXCVAR_GET(gpu_shadow_depth_half)) {
    vp_min_z *= 0.5f;
    vp_max_z *= 0.5f;
  }
  float ndc_scale_z = 1.0f;
  float ndc_offset_z = 0.0f;
  if (vp_min_z > vp_max_z) {
    std::swap(vp_min_z, vp_max_z);
    ndc_scale_z = -1.0f;
    ndc_offset_z = 1.0f;
  }
  // Guest-unit viewport and scissor are scaled to the host allocation;
  // swapchain-targeted draws and full-res RT variants stay 1:1.
  const bool rt_scaled = g_activeRtKey && !g_activeRtFullRes;
  const float vsc = rt_scaled ? HostScaleF() : 1.0f;
  const auto sc = [&](int32_t v) { return rt_scaled ? HostCoord(v) : v; };
  commandList->setViewports(RenderViewport(
      float(g_state.viewport_x) * vsc, float(g_state.viewport_y) * vsc,
      float(g_state.viewport_w) * vsc, float(g_state.viewport_h) * vsc,
      vp_min_z, vp_max_z));
  commandList->setScissors(
      RenderRect(sc(g_state.scissor[0]), sc(g_state.scissor[1]),
                 sc(g_state.scissor[2]), sc(g_state.scissor[3])));

  // b0: system constants.
  SystemConstants sys{};
  // W_NOT_RECIPROCAL (1<<3): the XDK standard path exports true clip W
  // (VTX_W0_FMT=1), so the VS must not reciprocate it.
  sys.flags = kSysFlagWNotReciprocal;
  // Alpha test from RB_COLORCONTROL: bit3 = enable, bits 0-2 = compare func (0
  // NEVER .. 7 ALWAYS), mapped onto the pass-if-less/equal/greater flag bits
  // xe_alpha_test consumes, with all three set when the test is disabled.
  float alpha_ref = 0.0f;
  if (color_control & 0x8) {
    const uint32_t func = color_control & 0x7;
    if (func == 1 || func == 3 || func == 5) sys.flags |= kSysFlagAlphaPassIfLess;
    if (func == 2 || func == 3 || func == 6) sys.flags |= kSysFlagAlphaPassIfEqual;
    if (func == 4 || func == 5 || func == 6) sys.flags |= kSysFlagAlphaPassIfGreater;
    if (func == 7) {
      sys.flags |= kSysFlagAlphaPassIfLess | kSysFlagAlphaPassIfEqual |
                   kSysFlagAlphaPassIfGreater;
    }
    std::memcpy(&alpha_ref, &alpha_ref_bits, 4);
  } else {
    sys.flags |= kSysFlagAlphaPassIfLess | kSysFlagAlphaPassIfEqual |
                 kSysFlagAlphaPassIfGreater;
  }
  sys.line_loop_index = 0xFFFFFFFFu;
  sys.vertex_index_endian = 0;  // host-generated / pre-swapped indices
  sys.vertex_index_offset = vertex_offset;
  sys.vertex_index_min = 0;
  sys.vertex_index_max = 0xFFFFFFu;
  // Y is not negated: the present blit does not flip V, unlike the ring path,
  // so the guest's +Y-up NDC maps straight through.
  sys.ndc_scale[0] = 1.0f;
  sys.ndc_scale[1] = flip_y ? -1.0f : 1.0f;
  sys.ndc_scale[2] = ndc_scale_z;
  sys.ndc_offset[0] = 1.0f / float(g_state.viewport_w);
  // Half-pixel Y is -1/h with ndc_scale.y=+1, as on the ring, and Y-flipped
  // depth-only passes mirror the offset with the scale.
  sys.ndc_offset[1] = (flip_y ? 1.0f : -1.0f) / float(g_state.viewport_h);
  sys.ndc_offset[2] = ndc_offset_z;
  sys.alpha_test_reference = alpha_ref;
  // User clip planes: the enabled planes are published, compacted, into c2..c7
  // as M^-T * plane for the ndc_scale/ndc_offset affine M (plane'.xyz =
  // plane.xyz / ndc_scale, plane'.w = plane.w - dot(plane'.xyz, ndc_offset)),
  // so the clip_planes GS can dot them with the transformed position.
  if (ucp_mask) {
    uint32_t out_plane = 0;
    for (uint32_t i = 0; i < 6; ++i) {
      if (!(ucp_mask & (1u << i))) continue;
      float p[4];
      for (uint32_t c = 0; c < 4; ++c) {
        const uint32_t bits = LoadGuestU32(device + 10272 + 16 * i + 4 * c);
        std::memcpy(&p[c], &bits, 4);
      }
      float* out = sys.user_clip_planes[out_plane];
      for (uint32_t c = 0; c < 3; ++c) {
        const float s = sys.ndc_scale[c];
        out[c] = (s != 0.0f) ? p[c] / s : 0.0f;
      }
      out[3] = p[3] - out[0] * sys.ndc_offset[0] -
               out[1] * sys.ndc_offset[1] - out[2] * sys.ndc_offset[2];
      ++out_plane;
    }
  }
  if (setup->point) {
    // PA_SU_POINT_SIZE / PA_SU_POINT_MINMAX shadows (regs 0x2280/0x2281 at
    // dev+11060/+11064), whose fields are half-size 12.4 fixed, so diameter px
    // = field * 2 / 16.
    const uint32_t point_size = LoadGuestU32(device + 11060);
    const uint32_t point_minmax = LoadGuestU32(device + 11064);
    sys.point_constant_diameter[0] = float(point_size >> 16) * 0.125f;
    sys.point_constant_diameter[1] = float(point_size & 0xFFFF) * 0.125f;
    sys.point_vertex_diameter_min = float(point_minmax & 0xFFFF) * 0.125f;
    sys.point_vertex_diameter_max = float(point_minmax >> 16) * 0.125f;
  }
  sys.color_exp_bias[0] = sys.color_exp_bias[1] = sys.color_exp_bias[2] =
      sys.color_exp_bias[3] = 1.0f;
  if (REXCVAR_GET(gpu_write_exp_bias)) {
    for (uint32_t i = 0; i < kMrtCount; i++) {
      const uint32_t surf = g_state.color_surface[i];
      if (!surf) continue;
      const SurfaceInfo si = ParseSurface(surf);
      if (si.valid && si.exp_bias != 0) {
        sys.color_exp_bias[i] = std::exp2f(float(si.exp_bias));
      }
    }
  }
  UploadAllocation b0 = g_upload[g_frame].Allocate(sizeof(sys), 256);
  std::memcpy(b0.memory, &sys, sizeof(sys));

  // b1: float constants, one buffer per stage; live draws read the device float
  // shadow (VS regs 0-255 at dev+1920, PS at dev+6016), replayed CB draws keep
  // the record-time capture in g_state, and an upload is reused within the
  // frame while its source bytes are unchanged.
  struct B1Cache {
    alignas(16) uint8_t src[4096];
    UploadAllocation alloc{};
    uint64_t frame = ~0ull;
    uint32_t device = 0;
    bool replay_src = false;
    bool valid = false;
  };
  static B1Cache b1_cache[2];  // [0] = VS, [1] = PS (render thread only)
  const bool b1_replay_src = g_replayDrawStateValid;
  const bool b1_dedup = REXCVAR_GET(gpu_b1_dedup);
  UploadAllocation b1_allocs[2];
  for (int st = 0; st < 2; st++) {
    const uint8_t* src =
        b1_replay_src
            ? reinterpret_cast<const uint8_t*>(st ? g_state.ps_floats
                                                  : g_state.vs_floats)
            : GuestPtr(device + (st ? 6016u : 1920u));
    B1Cache& c = b1_cache[st];
    if (b1_dedup && c.valid && c.frame == g_frameIndex &&
        c.replay_src == b1_replay_src &&
        (b1_replay_src || c.device == device) &&
        std::memcmp(c.src, src, 4096) == 0) {
      b1_allocs[st] = c.alloc;
      continue;
    }
    UploadAllocation a = g_upload[g_frame].Allocate(4096, 256);
    if (b1_replay_src) {
      std::memcpy(a.memory, src, 4096);
    } else {
      const uint32_t* s32 = reinterpret_cast<const uint32_t*>(src);
      uint32_t* d32 = reinterpret_cast<uint32_t*>(a.memory);
      for (uint32_t i = 0; i < 1024; i++) d32[i] = __builtin_bswap32(s32[i]);
    }
    if (b1_dedup) {
      std::memcpy(c.src, src, 4096);
      c.alloc = a;
      c.frame = g_frameIndex;
      c.device = device;
      c.replay_src = b1_replay_src;
      c.valid = true;
    }
    b1_allocs[st] = a;
  }
  UploadAllocation b1vs = b1_allocs[0];
  UploadAllocation b1ps = b1_allocs[1];

  // b2: bool/loop constants from the device shadow block at dev+10112 (PM4
  // block 0x4900 = SHADER_CONSTANT_BOOL_000_031 x8 + LOOP_00..31 x32, the
  // 40-dword b2 layout, big-endian), which is also valid for CB replays since
  // constants are inherited live.
  UploadAllocation b2 = g_upload[g_frame].Allocate(160, 256);
  if (g_replayBoolLoopValid) {
    // Replayed CB draw: the bools the game set on the recording device at
    // record time, already in host order.
    std::memcpy(b2.memory, g_replayBoolLoop, sizeof(g_replayBoolLoop));
  } else {
    const uint32_t* src =
        reinterpret_cast<const uint32_t*>(GuestPtr(device + 10112));
    uint32_t* dst = reinterpret_cast<uint32_t*>(b2.memory);
    for (uint32_t i = 0; i < 40; i++) dst[i] = __builtin_bswap32(src[i]);
  }

  // b3: fetch constants; texture fetch constants come from the guest device
  // fetch shadow (dev+1152+24*slot), the same bind source the SRV selection
  // below reads, and vertex fetch constants overwrite their slots below
  // (streams 0-15 = vfetch 95..80 = dwords 160-191).
  uint32_t fetch_dwords[192] = {};
  g_drawVfetchWrittenMask = 0;
  for (uint32_t slot = 0; slot < 32; slot++) {
    // Replayed draws use the recording device's shadow at record time, since
    // the run device's live shadow belongs to other passes by replay time.
    if (g_replayFetchValid & (1u << slot)) {
      std::memcpy(&fetch_dwords[slot * 6], g_replayFetch[slot], 24);
      continue;
    }
    const uint32_t slot_addr = device + 1152 + 24 * slot;
    const uint32_t dw0 = LoadGuestU32(slot_addr);
    const uint32_t dw1 = LoadGuestU32(slot_addr + 4);
    if (!(dw0 | dw1)) continue;  // empty slot — keep zeros (matches unbound)
    fetch_dwords[slot * 6 + 0] = dw0;
    fetch_dwords[slot * 6 + 1] = dw1;
    for (uint32_t i = 2; i < 6; i++) {
      fetch_dwords[slot * 6 + i] = LoadGuestU32(slot_addr + i * 4);
    }
  }
  // The vfetch dword range (160-191 = vertex streams 15..0) never carries
  // shadow or replay values, since guest-space vertex pairs are untranslatable
  // in ring space; BindStreamData rewrites bound streams and the blank fill
  // covers the rest.
  std::memset(&fetch_dwords[160], 0, 32 * sizeof(uint32_t));
  if (up_vertex_data) {
    const void* up_src = g_upOverrideVertexData ? g_upOverrideVertexData
                                                : GuestPtr(up_vertex_data);
    BindStreamData(fetch_dwords, 0, up_src, up_size_bytes,
                   /*immediate=*/true,  // XDK copies UP data at call time
                   up_stride_bytes);
  } else {
    // Binds the streams the game set, with offset and stride from
    // SetStreamSource.
    for (uint32_t stream = 0; stream < 16; stream++) {
      const StreamState& ss = g_state.streams[stream];
      // Stride 0 is legal (Avatar_Draw binds the bone-matrix VB and the mesh
      // VBs with stride 0), so only an unbound stream is skipped.
      if (!ss.vb_object) continue;
      // Two VB object flavors reach SetStreamSource: an XDK D3DVertexBuffer
      // (CreateVertexBuffer 0x82528A80) with +24 = data physical address | 3
      // and +28 = (byte size & 0x3FFFFFC) | endian and flags, and plain objects
      // with data ptr at +24 and byte size at +28.
      uint32_t data_base = LoadGuestU32(ss.vb_object + 24);
      uint32_t total = LoadGuestU32(ss.vb_object + 28);
      if ((data_base & 3u) == 3u) {
        data_base &= ~3u;
        total &= 0x3FFFFFCu;
      } else if (total > kUploadBufferSize / 2) {
        // Unrecognized third flavor, dumped for reverse engineering.
        static uint64_t vb_dump_budget = 4;
        if (vb_dump_budget) {
          vb_dump_budget--;
          REXGPU_WARN(
              "videonative: [vblog] unknown VB layout obj={:#x} dw={:#x} "
              "{:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x}",
              ss.vb_object, LoadGuestU32(ss.vb_object),
              LoadGuestU32(ss.vb_object + 4), LoadGuestU32(ss.vb_object + 8),
              LoadGuestU32(ss.vb_object + 12), LoadGuestU32(ss.vb_object + 16),
              LoadGuestU32(ss.vb_object + 20), LoadGuestU32(ss.vb_object + 24),
              LoadGuestU32(ss.vb_object + 28));
        }
      }
      const uint32_t data_ptr = data_base + ss.offset_bytes;
      if (!data_base || total <= ss.offset_bytes) continue;
      // guest_key is the last parameter, and a bare address in the 5th slot
      // would convert silently to `bool immediate`, hence the argument labels.
      BindStreamData(fetch_dwords, stream, GuestPtr(data_ptr),
                     total - ss.offset_bytes, /*immediate=*/false,
                     /*stride=*/0, /*guest_key=*/data_ptr);
    }
  }
  // Vertex-fetch pairs the stream binding did not translate this draw are
  // pointed past the ring, where D3D12 raw-buffer OOB reads return 0, since a
  // guest-space pair or base 0 would fetch another upload's bytes.
  for (uint32_t stream = 0; stream < 16; stream++) {
    if (g_drawVfetchWrittenMask & (1u << stream)) continue;
    const uint32_t vfetch_index = 95 - stream;
    fetch_dwords[vfetch_index * 2 + 0] = kUploadBufferSize | 3u;
    fetch_dwords[vfetch_index * 2 + 1] = 2u;
  }
  UploadAllocation b3 = g_upload[g_frame].Allocate(sizeof(fetch_dwords), 256);
  std::memcpy(b3.memory, fetch_dwords, sizeof(fetch_dwords));

  // b4: descriptor indices, one buffer per stage, since each shader's slots are
  // 0-based within its own cbuffer (translator contract).
  auto upload_bindings = [&](const ResolvedShader* shader) {
    uint32_t descriptor_indices[32] = {};
    if (!shader) {  // PS-less depth-only draw: zeros
      UploadAllocation b4z =
          g_upload[g_frame].Allocate(sizeof(descriptor_indices), 256);
      std::memcpy(b4z.memory, descriptor_indices, sizeof(descriptor_indices));
      return b4z;
    }
    uint32_t slot = 0;
    for (const auto& binding : shader->pack->bindings) {
      if (slot >= 32) break;
      if (binding.is_sampler) {
        // Sampler slot selection: the legacy path uses clamp bits only
        // (pick_sampler below), and the semantics path applies the full ring
        // contract over static slots 0-12 and write-once dynamic slots 13-15.
        uint32_t sampler_index = 0;
        // Xenos clamp mode -> axis-mode index {0 wrap, 1 mirror, 2 clamp}.
        auto axis_mode = [](uint32_t m) -> uint32_t {
          return m == 0 ? 0u : (m == 1 ? 1u : 2u);
        };
        auto pick_sampler = [&](uint32_t dw0) -> uint32_t {
          if (!REXCVAR_GET(gpu_per_axis_clamp)) {
            return ((dw0 >> 10) & 7) >= 2 ? 1u : 0u;  // legacy: clamp_x only
          }
          const uint32_t u = axis_mode((dw0 >> 10) & 7);
          const uint32_t v = axis_mode((dw0 >> 13) & 7);
          return 4 + u * 3 + v;
        };
        if (binding.fetch_constant < 32) {
          // The guest device fetch shadow (dev+1152+24*fc) is the bind source:
          // written by the XDK bind bodies and by inline engine writers no hook
          // sees, and never freed since it lives in the device block.
          uint32_t sdw[6];
          if (g_replayFetchValid & (1u << binding.fetch_constant)) {
            std::memcpy(sdw, g_replayFetch[binding.fetch_constant], 24);
          } else {
            const uint32_t slot_addr =
                device + 1152 + 24 * binding.fetch_constant;
            for (int i = 0; i < 6; i++) {
              sdw[i] = LoadGuestU32(slot_addr + 4 * i);
            }
          }
          if (sdw[0] | sdw[1]) {  // unbound slot keeps sampler 0
            const uint32_t legacy = pick_sampler(sdw[0]);
            if (REXCVAR_GET(gpu_sem_sampler)) {
              semantics::SamplerSpec spec;
              semantics::DecodeSamplerSpec(sdw, &spec);
              sampler_index = semantics::MapToSlot(spec);
            } else {
              sampler_index = legacy;
            }
          }
        }
        descriptor_indices[slot++] = sampler_index;
      } else {
        uint32_t index = 0;
        CachedTexture* tex = nullptr;
        if (binding.fetch_constant < 32) {
          // The fetch constant comes from the guest device fetch shadow, as in
          // the sampler block above; all-zero dwords mean nothing is bound.
          const uint32_t slot_addr =
              device + 1152 + 24 * binding.fetch_constant;
          uint32_t sdw[6];
          if (g_replayFetchValid & (1u << binding.fetch_constant)) {
            std::memcpy(sdw, g_replayFetch[binding.fetch_constant], 24);
          } else {
            for (int i = 0; i < 6; i++) {
              sdw[i] = LoadGuestU32(slot_addr + 4 * i);
            }
          }
          if (sdw[0] | sdw[1]) {
            tex = GetOrCreateTexture(slot_addr, false, 0, 0,
                                     RenderFormat::UNKNOWN, sdw);
            if (tex && tex->valid) index = tex->descriptor_index;
          }
        }
        // shadowwhite debug: replaces the sun shadow map (fc9 on lighting
        // draws) with an all-white texture.
        if (g_debugWhiteShadow && binding.fetch_constant == 9 &&
            shader->is_pixel &&
            g_activeRtFormat == RenderFormat::R10G10B10A2_UNORM) {
          EnsureWhiteTexture();
          if (g_whiteTextureDescriptor) index = g_whiteTextureDescriptor;
        }
        // Dimension-matched descriptors only: a view dimension that mismatches
        // the shader's declared type is D3D12 UB, so a mismatch or empty slot
        // gets the blank of the declared dimension (binding.dimension 0/1 = 2D
        // array, 2 = 3D, 3 = cube).
        if (binding.dimension == 3) {
          if (!tex || !tex->valid || tex->view_dim != 3) {
            index = kBlankCubeDescriptor;
          }
        } else if (binding.dimension == 2) {
          // No 3D textures exist, since guest 3D is treated as 2D at creation,
          // so a Texture3D declaration always gets the 3D blank.
          index = kBlank3dDescriptor;
        } else if (tex && tex->valid && tex->view_dim == 3) {
          index = 0;  // cube texture through the 2D declaration: 2D blank
        }
        descriptor_indices[slot++] = index;
      }
    }
    UploadAllocation b4 =
        g_upload[g_frame].Allocate(sizeof(descriptor_indices), 256);
    std::memcpy(b4.memory, descriptor_indices, sizeof(descriptor_indices));
    return b4;
  };
  UploadAllocation b4vs = upload_bindings(setup->vs);
  UploadAllocation b4ps = upload_bindings(setup->ps);

  commandList->setGraphicsRootDescriptor(b0.buffer->at(b0.offset),
                                         kRootB0System);
  commandList->setGraphicsRootDescriptor(b1vs.buffer->at(b1vs.offset),
                                         kRootB1FloatsVs);
  commandList->setGraphicsRootDescriptor(b1ps.buffer->at(b1ps.offset),
                                         kRootB1FloatsPs);
  commandList->setGraphicsRootDescriptor(b2.buffer->at(b2.offset),
                                         kRootB2BoolLoop);
  commandList->setGraphicsRootDescriptor(b3.buffer->at(b3.offset),
                                         kRootB3Fetch);
  commandList->setGraphicsRootDescriptor(b4vs.buffer->at(b4vs.offset),
                                         kRootB4IndicesVs);
  commandList->setGraphicsRootDescriptor(b4ps.buffer->at(b4ps.offset),
                                         kRootB4IndicesPs);
  auto& ring = g_vertexRing[g_frame];
  if (ring.buffer) {
    ring.ToReadState(commandList.get());  // copies recorded above must land
    commandList->setGraphicsRootDescriptor(ring.buffer->at(0),
                                           kRootSharedMemSrv);
  }
  commandList->setGraphicsRootDescriptor(g_dummyUav->at(0), kRootDummyUav);
  return true;
}

}  // namespace

void DrawVertices(uint32_t device, uint32_t prim_type, uint32_t start_vertex,
                  uint32_t vertex_count) {
  // Live-state dirty consumption runs on the guest side of the enqueue, where
  // read-clear-enqueue is coherent, since clearing the dirty words from the
  // worker would race the guest thread's own sets.
  if (rq::Active()) { ConsumeLiveStateDirty(device); rq::EnqDrawVertices(device, prim_type, start_vertex, vertex_count); return; }
  DrawSetup setup;
  if (!SetupDraw(device, prim_type, &setup, 0, 0, 0, int32_t(start_vertex))) {
    return;
  }
  g_commandLists[g_frame]->drawInstanced(vertex_count, 1, 0, 0);
  MarkDraw(g_commandLists[g_frame].get(), vertex_count, false, prim_type);
}

void DrawIndexedVertices(uint32_t device, uint32_t prim_type,
                         uint32_t base_vertex, uint32_t start_index,
                         uint32_t index_count) {
  if (rq::Active()) { ConsumeLiveStateDirty(device); rq::EnqDrawIndexedVertices(device, prim_type, base_vertex, start_index, index_count); return; }
  DrawSetup setup;
  if (!SetupDraw(device, prim_type, &setup, 0, 0, 0, int32_t(base_vertex))) {
    return;
  }
  // Guest IB object (D3D_CreateIndexBuffer 0x82528B58, 32 bytes): +24 = raw
  // data pointer, +28 = byte size, dword0 bit 31 = 32-bit index format (tested
  // by the XDK draw body 0x82534DF8), big-endian either way.
  const uint32_t ib = g_state.index_buffer_object;
  if (!ib) return;
  const uint32_t data_ptr = LoadGuestU32(ib + 24);
  if (!data_ptr) return;
  const bool idx32 = (LoadGuestU32(ib) & 0x80000000u) != 0;
  MaybeCaptureWatch(device, index_count, prim_type,
                    data_ptr + start_index * (idx32 ? 4 : 2), idx32);
  if (idx32) {
    const uint32_t size_bytes = index_count * 4;
    UploadAllocation alloc = g_upload[g_frame].Allocate(size_bytes, 4);
    const uint32_t* src = reinterpret_cast<const uint32_t*>(
        GuestPtr(data_ptr + start_index * 4));
    if (!rq::Running() && REXCVAR_GET(gpu_deferred_vb_copy)) {
      g_deferredCopies.push_back({src, alloc.memory, size_bytes, 4});
    } else {
      uint32_t* dst = reinterpret_cast<uint32_t*>(alloc.memory);
      for (uint32_t i = 0; i < index_count; i++) {
        dst[i] = __builtin_bswap32(src[i]);
      }
    }
    RenderIndexBufferView view(alloc.buffer->at(alloc.offset), size_bytes,
                               RenderFormat::R32_UINT);
    g_commandLists[g_frame]->setIndexBuffer(&view);
  } else {
    const uint32_t size_bytes = index_count * 2;
    UploadAllocation alloc = g_upload[g_frame].Allocate(size_bytes, 4);
    const uint16_t* src = reinterpret_cast<const uint16_t*>(
        GuestPtr(data_ptr + start_index * 2));
    if (!rq::Running() && REXCVAR_GET(gpu_deferred_vb_copy)) {
      g_deferredCopies.push_back({src, alloc.memory, size_bytes, 2});
    } else {
      uint16_t* dst = reinterpret_cast<uint16_t*>(alloc.memory);
      for (uint32_t i = 0; i < index_count; i++) {
        dst[i] = __builtin_bswap16(src[i]);
      }
    }
    RenderIndexBufferView view(alloc.buffer->at(alloc.offset), size_bytes,
                               RenderFormat::R16_UINT);
    g_commandLists[g_frame]->setIndexBuffer(&view);
  }
  g_commandLists[g_frame]->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  MarkDraw(g_commandLists[g_frame].get(), index_count, true, prim_type);
}

void SetUPDataOverride(const void* vertex_data, const void* index_data) {
  if (rq::Active()) { rq::SetPendingUPOverride(vertex_data, index_data); return; }
  g_upOverrideVertexData = vertex_data;
  g_upOverrideIndexData = index_data;
}

void DrawVerticesUP(uint32_t device, uint32_t prim_type, uint32_t vertex_count,
                    uint32_t vertex_data, uint32_t stride_bytes) {
  if (rq::Active()) { ConsumeLiveStateDirty(device); rq::EnqDrawVerticesUP(device, prim_type, vertex_count, vertex_data, stride_bytes); return; }
  DrawSetup setup;
  if (!SetupDraw(device, prim_type, &setup, vertex_data,
                 vertex_count * stride_bytes, stride_bytes, 0)) {
    return;
  }
  g_commandLists[g_frame]->drawInstanced(vertex_count, 1, 0, 0);
  MarkDraw(g_commandLists[g_frame].get(), vertex_count, false, prim_type);
}

void DrawIndexedVerticesUP(uint32_t device, uint32_t prim_type,
                           uint32_t min_vertex, uint32_t num_vertices,
                           uint32_t index_count, uint32_t index_data,
                           uint32_t index_format, uint32_t vertex_data,
                           uint32_t stride_bytes) {
  if (rq::Active()) { ConsumeLiveStateDirty(device); rq::EnqDrawIndexedVerticesUP(device, prim_type, min_vertex, num_vertices, index_count, index_data, index_format, vertex_data, stride_bytes); return; }
  (void)min_vertex;
  (void)index_format;  // 1 = 16-bit — the only format CC2 uses
  DrawSetup setup;
  if (!SetupDraw(device, prim_type, &setup, vertex_data,
                 num_vertices * stride_bytes, stride_bytes, 0)) {
    return;
  }
  const uint32_t size_bytes = index_count * 2;
  UploadAllocation alloc = g_upload[g_frame].Allocate(size_bytes, 4);
  const uint16_t* src = reinterpret_cast<const uint16_t*>(
      g_upOverrideIndexData ? g_upOverrideIndexData : GuestPtr(index_data));
  uint16_t* dst = reinterpret_cast<uint16_t*>(alloc.memory);
  for (uint32_t i = 0; i < index_count; i++) dst[i] = __builtin_bswap16(src[i]);
  RenderIndexBufferView view(alloc.buffer->at(alloc.offset), size_bytes,
                             RenderFormat::R16_UINT);
  g_commandLists[g_frame]->setIndexBuffer(&view);
  g_commandLists[g_frame]->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  MarkDraw(g_commandLists[g_frame].get(), index_count, true, prim_type);
}

}  // namespace rex::videonative::renderer
