// rexvideonative — see video_native.h and LICENSE (GPL-3.0).
// Guest API surface and shader resolution; the plume backend (frame flow,
// pipelines, texture cache, draw submission) lives in renderer.cpp.

#include "video_native.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>  // SetEnvironmentVariableA (DRED arming)
#endif

#include <fmt/format.h>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>

#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>

#include <plume_render_interface.h>

#include "renderer.h"
#include "shader_cache.h"
#include "video_native_internal.h"
#include "render_queue.h"

namespace plume {
// Defined in plume_d3d12.cpp; not declared in the public headers.
extern std::unique_ptr<RenderInterface> CreateD3D12Interface();
}  // namespace plume

REXCVAR_DEFINE_BOOL(gpu_native, true, "GPU",
                    "Draws the game's Direct3D calls with the native renderer");
REXCVAR_DEFINE_BOOL(gpu_xenia_gpu, false, "GPU",
                    "With gpu_native on, also creates the Xenia graphics "
                    "system (second D3D12 device, PM4 command processor, "
                    "pipeline-cache threads, vblank thread), which the native "
                    "path does not use.");
REXCVAR_DEFINE_BOOL(gpu_swap_bookkeeping, true, "GPU",
                    "Per present, writes the two per-frame fence-ring slots "
                    "and the VdSwap writeback with an advancing counter and "
                    "advances the device frame counters (dev+22036, "
                    "dev+22032), as the body of the XDK Swap does.");
REXCVAR_DEFINE_BOOL(gpu_device_ring_scratch, true, "GPU",
                    "Gives the replacement guest device a command-buffer "
                    "scratch segment (dev+48/+56) and a fence pool "
                    "(dev+11024), so recompiled XDK PM4 emitters do not "
                    "write through a null pointer into guest page zero.");
REXCVAR_DEFINE_INT32(gpu_heap_pad, 0, "GPU",
                     "Guest-heap padding in bytes allocated at native-video "
                     "init, which shifts the game's later allocation layout "
                     "(0 = off).");
REXCVAR_DEFINE_INT32(gpu_gamma_ramp_offset, 0, "GPU",
                     "Device-block offset of the D3DGAMMARAMP; 0 picks "
                     "+15408 or +15216, whichever holds a valid ramp.");
REXCVAR_DEFINE_BOOL(gpu_fence_pool_max, false, "GPU",
                    "Pins the device fence pool's completed counter at its "
                    "maximum (0xFFFFFF00), which satisfies every guest fence "
                    "wait at once.");
REXCVAR_DEFINE_BOOL(gpu_fence_emulation, false, "GPU",
                    "Emulates GPU resource-fence progression: binds stamp "
                    "the resource Fence field (obj+8) with the device submit "
                    "counter (dev+11036) and Swap advances the counter and "
                    "writes completed = cur-1 to *(dev+11024), so "
                    "D3DResource_Release defers frees of in-flight textures.");
REXCVAR_DEFINE_BOOL(gpu_dred, false, "Diagnostics",
                    "Arms D3D12 DRED before device creation, so the "
                    "present-failure dump in plume_present_dbg.txt carries "
                    "auto-breadcrumbs after a device removal.");
REXCVAR_DEFINE_INT32(gpu_d3d_debug, 0, "Diagnostics",
                     "D3D12 debug layer, which needs the Graphics Tools "
                     "optional feature: 1 = validation layer, 2 = validation "
                     "layer and GPU-based validation.");
REXCVAR_DEFINE_STRING(gpu_adapter, "", "GPU",
                      "Selects the GPU of the native video layer by a "
                      "case-insensitive name fragment ('intel', 'radeon') or "
                      "a hex DXGI vendor id (8086=Intel, 1002=AMD, 10de=NVIDIA); "
                      "empty or unmatched = the first hardware adapter.");
REXCVAR_DEFINE_STRING(gpu_shader_pack, "", "GPU",
                      "The XenosRecomp native shader pack used by the native "
                      "video layer: its directory (manifest.csv + DXIL), or "
                      "the pack in one file (cc1.pack)");
REXCVAR_DEFINE_STRING(gpu_dump_misses, "", "Diagnostics",
                      "Directory to dump pack-missed runtime ucode into, in "
                      "the shader corpus convention (shader_<hash>.ucode.bin."
                      "vert/frag, host-order words) for pack regeneration");
REXCVAR_DEFINE_STRING(gpu_heal_tool, "", "GPU",
                      "Path of XenosRecomp.exe, with which (given "
                      "gpu_heal_header and gpu_dump_misses) a shader the "
                      "pack lacks is built while the game runs and added to "
                      "the pack; empty skips its draws.");
REXCVAR_DEFINE_STRING(gpu_heal_header, "", "GPU",
                      "Path of rexglue_shader_common.h for gpu_heal_tool.");
REXCVAR_DEFINE_INT32(gpu_cb_bools, 2, "GPU",
                     "Bool and loop constant model for command-buffer "
                     "replay: 0 = the full record-time block at every "
                     "replayed draw, 1 = live values overlaid with those "
                     "changed during recording, 2 = 1 plus the values "
                     "pending at Begin.");
REXCVAR_DEFINE_BOOL(gpu_cb_const_diffs, true, "GPU",
                    "Replays command-buffer draws with the float constants "
                    "the recording device dirtied between Begin and the "
                    "draw; off replays with inherited constants only.");
REXCVAR_DEFINE_BOOL(gpu_defaults_win, false, "Diagnostics",
                    "Bind-time shader float defaults overwrite constants set "
                    "before the bind, dirty registers included; off keeps "
                    "dirty registers, as the XDK draw flush does.");

namespace rex::videonative {

namespace {

// Size of the guest-visible device block returned by CreateDevice, into which
// unhooked recompiled XDK state setters write the shadow registers the renderer
// reads at draw time.
constexpr uint32_t kGuestDeviceSize = 0x8000;

std::mutex g_initMutex;
std::atomic<bool> g_initialized{false};
bool g_initFailed = false;

std::unique_ptr<plume::RenderInterface> g_interface;
std::unique_ptr<plume::RenderDevice> g_device;
std::unique_ptr<plume::RenderCommandQueue> g_queue;

std::atomic<uint64_t> g_frameCount{0};

void StoreGuestU32(GuestAddr addr, uint32_t value) {
  auto* p = rex::system::kernel_memory()->TranslateVirtual<uint32_t*>(addr);
  *p = __builtin_bswap32(value);
}

uint32_t LoadGuestU32(GuestAddr addr) {
  return __builtin_bswap32(
      *rex::system::kernel_memory()->TranslateVirtual<const uint32_t*>(addr));
}

// --- shader resolution -------------------------------------------------------

ShaderCache g_shaderCache;
std::once_flag g_shaderPackLoadOnce;
uint64_t g_shaderPackHits = 0;
uint64_t g_shaderPackMisses = 0;

VfetchPatchFn g_vfetchPatcher = nullptr;

// Current bind state relevant to VS resolution (render thread only).
GuestAddr g_currentVertexShader = 0;
GuestAddr g_currentPixelShader = 0;
GuestAddr g_currentVertexDecl = 0;
uint8_t g_streamStrideDwords[16] = {};

const ResolvedShader* g_currentResolvedVs = nullptr;
const ResolvedShader* g_currentResolvedPs = nullptr;

// VS/PS resolution caches, insert-only with nodes never rewritten in place
// because the render-queue worker dereferences ResolvedShader pointers from
// in-flight draw records, so a re-resolve lands in a node keyed by its content
// hash.
struct VsKey {
  uint64_t k0, k1;
  bool operator==(const VsKey& o) const { return k0 == o.k0 && k1 == o.k1; }
};
struct VsKeyHash {
  size_t operator()(const VsKey& k) const {
    return size_t(k.k0 ^ (k.k1 * 0x9E3779B97F4A7C15ull));
  }
};
std::unordered_map<VsKey, ResolvedShader, VsKeyHash> g_resolvedVertexShaders;
// PS keyed by (object, code ptr) + content hash.
std::unordered_map<VsKey, ResolvedShader, VsKeyHash> g_resolvedPixelShaders;

// Guest scratch used for the patched microcode copy + the stride table.
uint32_t g_patchScratch = 0;
uint32_t g_patchScratchSize = 0;
uint32_t g_strideTableGuest = 0;

void EnsureShaderPackLoaded() {
  std::call_once(g_shaderPackLoadOnce, [] {
    std::string pack = REXCVAR_GET(gpu_shader_pack);
    // Unset, the pack is the one an installed game has beside its exe.
    if (pack.empty()) {
      const auto beside = rex::filesystem::GetExecutableFolder() / "shaderpack" / "cc1.pack";
      std::error_code ec;
      if (std::filesystem::is_regular_file(beside, ec)) pack = beside.string();
    }
    if (!pack.empty()) {
      g_shaderCache.Load(pack);
    } else {
      REXGPU_WARN(
          "videonative: gpu_shader_pack not set — all shaders will "
          "miss");
    }
  });
}

// Corpus dump convention (Shader::DumpUcode): the file name carries the XXH3 of
// the guest-order ucode and the contents are host-order words.
void DumpMissedUcode(const ResolvedShader& resolved, const void* ucode_guest) {
  const std::string& dir = REXCVAR_GET(gpu_dump_misses);
  if (dir.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path =
      std::filesystem::path(dir) /
      fmt::format("shader_{:016X}.ucode.bin.{}", resolved.ucode_hash,
                  resolved.is_pixel ? "frag" : "vert");
  if (std::filesystem::exists(path, ec)) return;

  const uint32_t words = resolved.code_size / 4;
  std::vector<uint32_t> swapped(words);
  auto* src = static_cast<const uint32_t*>(ucode_guest);
  for (uint32_t i = 0; i < words; i++) swapped[i] = __builtin_bswap32(src[i]);
  std::ofstream file(path, std::ios::binary);
  file.write(reinterpret_cast<const char*>(swapped.data()), words * 4);
  REXGPU_INFO("videonative: dumped missed {} ucode -> {}",
              resolved.is_pixel ? "ps" : "vs", path.string());
}

// --- pack misses built while the game runs ----------------------------------
// With gpu_heal_tool set, a shader the pack lacks is built by
// XenosRecomp on its own thread and lands in the pack directory and the live
// cache.
struct HealJob {
  uint64_t hash;
  bool pixel;
};
struct Healed {
  uint64_t hash;
  bool pixel;
  PackShader shader;
};
std::mutex g_healMutex;
std::condition_variable g_healWake;
std::deque<HealJob> g_healJobs;
std::unordered_set<uint64_t> g_healQueued;
std::vector<Healed> g_healed;
std::atomic<bool> g_healedReady{false};
std::once_flag g_healThreadOnce;

// Runs the tool and waits for it; false when it did not start or end in time.
bool RunHealTool(const std::filesystem::path& tool, const std::wstring& args) {
  std::wstring command = L"\"" + tool.wstring() + L"\" " + args;
  STARTUPINFOW startup = {};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process = {};
  const std::wstring directory = tool.parent_path().wstring();
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup,
                      &process)) {
    return false;
  }
  const bool done = WaitForSingleObject(process.hProcess, 120000) == WAIT_OBJECT_0;
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return done;
}

void HealOne(const HealJob& job) {
  namespace fs = std::filesystem;
  const fs::path tool = REXCVAR_GET(gpu_heal_tool);
  const fs::path header = REXCVAR_GET(gpu_heal_header);
  const fs::path pack = REXCVAR_GET(gpu_shader_pack);
  const fs::path dumps = REXCVAR_GET(gpu_dump_misses);
  const std::string hash = fmt::format("{:016X}", job.hash);
  const std::string dump = fmt::format("shader_{}.ucode.bin.{}", hash,
                                       job.pixel ? "frag" : "vert");
  std::error_code ec;
  const fs::path stage = fs::temp_directory_path(ec) / "cc1_heal" / hash;
  fs::remove_all(stage, ec);
  fs::create_directories(stage / "dumps", ec);
  fs::create_directories(stage / "none", ec);
  fs::create_directories(stage / "out", ec);
  fs::copy_file(dumps / dump, stage / "dumps" / dump, ec);
  if (ec) {
    REXGPU_WARN("videonative: missed shader {} has no dump to build from", hash);
    return;
  }
  {
    std::ofstream map(stage / "map.csv");
    map << "runtime_ucode_hash,type,match,container_hash,fingerprint,recompiles_ok\n";
  }
  auto quoted = [](const fs::path& path) { return L"\"" + path.wstring() + L"\""; };
  const std::wstring args = L"--rexglue-pack " + quoted(stage / "none") + L" " +
                            quoted(stage / "dumps") + L" " + quoted(stage / "map.csv") +
                            L" " + quoted(stage / "out") + L" " + quoted(header);
  if (!RunHealTool(tool, args)) {
    REXGPU_WARN("videonative: could not run {} for missed shader {}", tool.string(), hash);
    return;
  }
  std::string row;
  {
    std::ifstream built(stage / "out" / "manifest.csv");
    for (std::string line; std::getline(built, line);) {
      if (line.rfind(hash + ",", 0) == 0) row = line;
    }
  }
  if (!row.empty() && row.back() == '\r') row.pop_back();
  if (row.empty()) {
    REXGPU_WARN("videonative: the tool built nothing for missed shader {}", hash);
    return;
  }
  for (const auto& file : fs::directory_iterator(stage / "out", ec)) {
    const fs::path name = file.path().filename();
    if (name.string().rfind(hash, 0) == 0) {
      fs::copy_file(file.path(), pack / name, fs::copy_options::overwrite_existing, ec);
    }
  }
  {
    std::ofstream manifest(pack / "manifest.csv", std::ios::app);
    manifest << row << "\n";
  }
  Healed healed;
  if (!ShaderCache::LoadRow(pack, row, &healed.hash, &healed.pixel, &healed.shader)) {
    REXGPU_WARN("videonative: missed shader {} could not be built: {}", hash, row);
    return;
  }
  std::lock_guard lock(g_healMutex);
  g_healed.push_back(std::move(healed));
  g_healedReady = true;
}

void HealThread() {
  for (;;) {
    HealJob job;
    {
      std::unique_lock lock(g_healMutex);
      g_healWake.wait(lock, [] { return !g_healJobs.empty(); });
      job = g_healJobs.front();
      g_healJobs.pop_front();
    }
    HealOne(job);
  }
}

// Queues a dumped miss for building, once per hash.
void QueueHeal(const ResolvedShader& resolved) {
  if (REXCVAR_GET(gpu_heal_tool).empty() ||
      REXCVAR_GET(gpu_heal_header).empty() ||
      REXCVAR_GET(gpu_dump_misses).empty() ||
      REXCVAR_GET(gpu_shader_pack).empty()) {
    return;
  }
  // What is built lands in the pack directory; a pack in one file takes none.
  std::error_code ec;
  if (!std::filesystem::is_directory(REXCVAR_GET(gpu_shader_pack), ec)) return;
  std::call_once(g_healThreadOnce, [] { std::thread(HealThread).detach(); });
  std::lock_guard lock(g_healMutex);
  if (!g_healQueued.insert(resolved.ucode_hash).second) return;
  g_healJobs.push_back({resolved.ucode_hash, resolved.is_pixel});
  g_healWake.notify_one();
}

// Render thread: moves what the heal thread built into the cache and into the
// resolutions that missed it, writing only their null pack pointer, which the
// render-queue worker reads as either value.
void ApplyHealed() {
  if (!g_healedReady.exchange(false)) return;
  std::vector<Healed> healed;
  {
    std::lock_guard lock(g_healMutex);
    healed.swap(g_healed);
  }
  for (Healed& built : healed) {
    const PackShader* pack =
        g_shaderCache.Add(built.hash, built.pixel, std::move(built.shader));
    uint32_t bound = 0;
    for (auto& [key, resolved] :
         built.pixel ? g_resolvedPixelShaders : g_resolvedVertexShaders) {
      if (!resolved.pack && resolved.ucode_hash == built.hash) {
        resolved.pack = pack;
        ++bound;
      }
    }
    REXGPU_INFO("videonative: built missed {} {:016X} while running ({} in use)",
                built.pixel ? "ps" : "vs", built.hash, bound);
  }
}

void NoteResolveOutcome(const ResolvedShader& resolved,
                        const void* ucode_guest) {
  if (resolved.pack) {
    g_shaderPackHits++;
  } else {
    g_shaderPackMisses++;
    REXGPU_WARN("videonative: pack miss {} ucode {:016X} ({} bytes)",
                resolved.is_pixel ? "ps" : "vs", resolved.ucode_hash,
                resolved.code_size);
    DumpMissedUcode(resolved, ucode_guest);
    QueueHeal(resolved);
  }
  if ((g_shaderPackHits + g_shaderPackMisses) % 64 == 0) {
    REXGPU_INFO("videonative: shader resolves {} hit / {} miss",
                g_shaderPackHits, g_shaderPackMisses);
  }
}

// Pixel shaders hash the object's microcode directly, with the ucode offset and
// size taken from the container Shader struct at +40 because the physical blob
// (pointer at +0x18) can carry default constants before the ucode.
const ResolvedShader* ResolvePixelShader(GuestAddr shader_obj) {
  EnsureShaderPackLoaded();
  ApplyHealed();
  const uint32_t phys_base = LoadGuestU32(shader_obj + 0x18);
  const uint32_t container = shader_obj + 40;
  const uint32_t shader_off = LoadGuestU32(container + 24);
  if (!phys_base || !shader_off || shader_off > 0x8000) {
    // Logs the object layout of shaders that resolution refuses.
    static int diag = 0;
    if (diag < 12) {
      diag++;
      REXGPU_WARN(
          "videonative: ps resolve early-out obj={:#x} phys={:#x} "
          "shader_off={:#x} hdr={:#x} {:#x} {:#x} {:#x} {:#x} {:#x} {:#x} "
          "{:#x}",
          shader_obj, phys_base, shader_off, LoadGuestU32(shader_obj),
          LoadGuestU32(shader_obj + 4), LoadGuestU32(shader_obj + 8),
          LoadGuestU32(shader_obj + 12), LoadGuestU32(shader_obj + 16),
          LoadGuestU32(shader_obj + 20), LoadGuestU32(shader_obj + 40),
          LoadGuestU32(shader_obj + 44));
    }
    return nullptr;
  }
  const uint32_t code_off = LoadGuestU32(container + shader_off);
  const uint32_t code_size = LoadGuestU32(container + shader_off + 4);
  const uint32_t code_ptr = phys_base + code_off;
  if (!code_size || code_size > 0x100000) return nullptr;
  // The game link-patches shader microcode in place after load, so the cache
  // keys on content as well as the code pointer.
  const void* code_host =
      rex::system::kernel_memory()->TranslateVirtual<const void*>(code_ptr);
  const uint64_t raw_hash = XXH3_64bits(code_host, code_size);
  VsKey key;
  key.k0 = (uint64_t(shader_obj) << 32) | code_ptr;
  key.k1 = raw_hash;
  auto it = g_resolvedPixelShaders.find(key);
  if (it != g_resolvedPixelShaders.end()) {
    return &it->second;
  }

  ResolvedShader resolved;
  resolved.guest_object = shader_obj;
  resolved.code_ptr = code_ptr;
  resolved.code_size = code_size;
  resolved.is_pixel = true;
  resolved.ucode_hash = raw_hash;
  resolved.pack = g_shaderCache.Find(resolved.ucode_hash, true);
  NoteResolveOutcome(resolved, code_host);
  return &(g_resolvedPixelShaders[key] = resolved);
}

// Vertex shaders: the XDK's per-binding vfetch patch is reproduced (via the
// recompiled patcher) on a guest scratch copy and the patched bytes are hashed,
// which is what the pack is keyed on.
const ResolvedShader* ResolveVertexShaderForDraw() {
  ApplyHealed();
  const GuestAddr shader_obj = g_currentVertexShader;
  if (!shader_obj || !g_vfetchPatcher) {
    static uint64_t novs_diag = 16;
    if (novs_diag) {
      novs_diag--;
      REXGPU_WARN("videonative: [novslog] vs resolve fail obj={:#x} patcher={}",
                  shader_obj, g_vfetchPatcher != nullptr);
    }
    return nullptr;
  }
  EnsureShaderPackLoaded();

  auto* mem = rex::system::kernel_memory();
  // Code pointer and size as the XDK flush reads them (sub_82537000):
  // descriptor offset at obj[224], code offset/size at obj+desc+872/876, code
  // base at obj dword8.
  const uint32_t desc = LoadGuestU32(shader_obj + 224 * 4);
  const uint32_t code_off = LoadGuestU32(shader_obj + desc + 872);
  const uint32_t code_size = LoadGuestU32(shader_obj + desc + 876);
  const uint32_t code_ptr = LoadGuestU32(shader_obj + 8 * 4) + code_off;
  if (!code_ptr || !code_size || code_size > 0x100000) {
    static uint64_t novs_diag2 = 16;
    if (novs_diag2) {
      novs_diag2--;
      REXGPU_WARN(
          "videonative: [novslog] vs code fail obj={:#x} desc={:#x} "
          "code_ptr={:#x} size={:#x} decl={:#x}",
          shader_obj, desc, code_ptr, code_size, g_currentVertexDecl);
    }
    return nullptr;
  }

  // The raw (pre-vfetch-patch) code content is mixed into the cache key because
  // the game link-patches constant registers in the microcode after load.
  const uint64_t raw_hash = XXH3_64bits(
      mem->TranslateVirtual<const void*>(code_ptr), code_size);

  VsKey key;
  key.k0 = (uint64_t(shader_obj) << 32) | g_currentVertexDecl;
  key.k1 = XXH3_64bits(g_streamStrideDwords, sizeof(g_streamStrideDwords)) ^
           raw_hash;
  auto it = g_resolvedVertexShaders.find(key);
  if (it != g_resolvedVertexShaders.end()) return &it->second;

  if (g_patchScratchSize < code_size) {
    // Grow-only scratch; the outgrown allocation is leaked.
    g_patchScratch = mem->SystemHeapAlloc((code_size + 0xFFF) & ~0xFFFu, 0x100);
    g_patchScratchSize = (code_size + 0xFFF) & ~0xFFFu;
  }
  if (!g_strideTableGuest) {
    g_strideTableGuest = mem->SystemHeapAlloc(16, 0x10);
  }
  if (!g_patchScratch || !g_strideTableGuest) return nullptr;

  std::memcpy(mem->TranslateVirtual<void*>(g_patchScratch),
              mem->TranslateVirtual<const void*>(code_ptr), code_size);
  std::memcpy(mem->TranslateVirtual<void*>(g_strideTableGuest),
              g_streamStrideDwords, sizeof(g_streamStrideDwords));

  if (g_currentVertexDecl) {
    g_vfetchPatcher(shader_obj, g_patchScratch, g_currentVertexDecl,
                    g_strideTableGuest, 0);
  }

  ResolvedShader resolved;
  resolved.guest_object = shader_obj;
  resolved.code_ptr = code_ptr;
  resolved.code_size = code_size;
  resolved.is_pixel = false;
  resolved.ucode_hash = XXH3_64bits(
      mem->TranslateVirtual<const void*>(g_patchScratch), code_size);
  resolved.pack = g_shaderCache.Find(resolved.ucode_hash, false);
  // The XDK draw flush (sub_82537348) selects between two code bodies per VS
  // object, descriptor obj[224] (variant 0) or obj[226] (variant 1) by
  // obj_dword[218] & 0x20; only variant 0 is resolved and variant 1 is hashed
  // for the log.
  {
    static std::unordered_set<uint64_t> vsvar_seen;
    const uint32_t d218 = LoadGuestU32(shader_obj + 218 * 4);
    const uint32_t desc1 = LoadGuestU32(shader_obj + 226 * 4);
    const bool vsvar_fresh =
        vsvar_seen.size() < 256 && vsvar_seen.insert(key.k0).second;
    if (vsvar_fresh && desc1 && desc1 != desc && desc1 < 0x10000) {
      const uint32_t off1 = LoadGuestU32(shader_obj + desc1 + 872);
      const uint32_t size1 = LoadGuestU32(shader_obj + desc1 + 876);
      uint64_t hash1 = 0;
      if (off1 && size1 && size1 <= g_patchScratchSize) {
        const uint32_t ptr1 = LoadGuestU32(shader_obj + 8 * 4) + off1;
        std::memcpy(mem->TranslateVirtual<void*>(g_patchScratch),
                    mem->TranslateVirtual<const void*>(ptr1), size1);
        if (g_currentVertexDecl) {
          g_vfetchPatcher(shader_obj, g_patchScratch, g_currentVertexDecl,
                          g_strideTableGuest, 1);
        }
        hash1 = XXH3_64bits(mem->TranslateVirtual<const void*>(g_patchScratch),
                            size1);
        // Restores the variant-0 patched bytes the caller's hash was made of.
        std::memcpy(mem->TranslateVirtual<void*>(g_patchScratch),
                    mem->TranslateVirtual<const void*>(code_ptr), code_size);
        if (g_currentVertexDecl) {
          g_vfetchPatcher(shader_obj, g_patchScratch, g_currentVertexDecl,
                          g_strideTableGuest, 0);
        }
      }
      REXGPU_INFO(
          "videonative: [vsvar] obj={:#x} d218={:#x} sel1={} h0={:016X} "
          "h1={:016X} pack1={}",
          shader_obj, d218, (d218 & 0x20) ? 1 : 0, resolved.ucode_hash, hash1,
          g_shaderCache.Find(hash1, false) != nullptr);
    }
  }
  // On a miss the patched ucode is dumped, since that is what the hash keys.
  NoteResolveOutcome(resolved,
                     mem->TranslateVirtual<const void*>(g_patchScratch));
  return &(g_resolvedVertexShaders[key] = resolved);
}

void OnDraw() {
  g_currentResolvedVs = ResolveVertexShaderForDraw();
  if (!g_currentPixelShader) {
    static int diag = 0;
    if (diag < 4) {
      diag++;
      REXGPU_WARN("videonative: draw with no pixel shader bound (vs obj "
                  "{:#x})",
                  g_currentVertexShader);
    }
  }
  g_currentResolvedPs =
      g_currentPixelShader ? ResolvePixelShader(g_currentPixelShader) : nullptr;
}

// Per-device float-constant dirty bitmaps (bit = ALU register, [0] = VS, [1] =
// PS) mirroring the XDK's dirty tracking: set by the hooked Set*ShaderConstantF
// entries, cleared by every draw, and consulted so bind-time shader defaults do
// not override dirty registers.
struct FloatDirtyMap {
  uint32_t bits[2][8];
};
std::unordered_map<uint32_t, FloatDirtyMap> g_floatDirty;

void MarkFloatsDirty(GuestAddr device, bool pixel, uint32_t start,
                     uint32_t count) {
  if (!device) return;
  auto& m = g_floatDirty[device];
  const uint32_t end = std::min(start + count, 256u);
  for (uint32_t r = start; r < end; r++) {
    m.bits[pixel ? 1 : 0][r >> 5] |= 1u << (r & 31);
  }
}
bool FloatRegDirty(GuestAddr device, bool pixel, uint32_t reg) {
  auto it = g_floatDirty.find(device);
  if (it == g_floatDirty.end()) return false;
  return (it->second.bits[pixel ? 1 : 0][reg >> 5] >> (reg & 31)) & 1u;
}
void ClearFloatDirty(GuestAddr device) {
  auto it = g_floatDirty.find(device);
  if (it != g_floatDirty.end()) {
    std::memset(it->second.bits, 0, sizeof(it->second.bits));
  }
}

// Applies shader default constants at bind time from group 1 of the runtime
// definition table ((aluRegister:16 | countFloats:16, physicalOffset:32)
// records until a zero count, data in the physical blob), writing the device
// float shadow for live binds and g_state for replayed binds.
void ApplyShaderFloatDefaults(GuestAddr device, bool pixel,
                              GuestAddr container, uint32_t phys_base,
                              bool replaying) {
  const uint32_t def_off = LoadGuestU32(container + 20);
  if (!def_off || def_off > 0x8000) return;
  GuestAddr def = container + def_off + 20;
  // Table extent: the dword at (container+def_off)+16 is the byte size of the
  // record area starting at (container+def_off)+20.
  const GuestAddr def_end =
      container + def_off + 20 + LoadGuestU32(container + def_off + 16);
  for (uint32_t guard = 0; guard < 256; guard++, def += 8) {
    const uint32_t head = LoadGuestU32(def);
    const uint32_t count_floats = head & 0xFFFF;
    if (!count_floats) break;  // group-1 terminator (count == 0)
    if (!phys_base) continue;  // group-1 data lives in the physical blob
    const uint32_t reg = head >> 16;  // ALU-space index (PS regs are 256+)
    const uint32_t phys_off = LoadGuestU32(def + 4);
    const uint32_t reg_base = pixel ? reg - 256 : reg;
    if (reg_base >= 256) continue;
    const uint32_t vec4s = std::min((count_floats + 3) / 4, 256 - reg_base);
    if (replaying) {
      renderer::SetShaderConstantsF(pixel, reg_base, phys_base + phys_off,
                                    vec4s);
    } else if (device) {
      // m_Constants.Alu at dev+1920 spans VS 0-255 and PS 256-511, big-endian
      // on both sides so a raw copy, skipping registers the game has set since
      // the last draw.
      auto* mem = rex::system::kernel_memory();
      const bool defaults_win = REXCVAR_GET(gpu_defaults_win);
      for (uint32_t v = 0; v < vec4s; v++) {
        if (!defaults_win && FloatRegDirty(device, pixel, reg_base + v)) {
          continue;
        }
        std::memcpy(
            mem->TranslateVirtual<void*>(device + 1920 + (reg + v) * 16),
            mem->TranslateVirtual<const void*>(phys_base + phys_off + v * 16),
            16);
      }
    }
  }
  if (!replaying) return;
  // Groups 2 and 3 (raw dword copies and (mask, or) dword merges into
  // m_Constants, each list ended by a zero count) are applied by the recompiled
  // Set*Shader body for live binds only, so replayed binds reproduce them here
  // for ALU-float destinations.
  GuestAddr p = def + 4;  // past the group-1 terminator
  for (uint32_t guard = 0; guard < 1024 && p + 4 <= def_end; guard++) {
    const uint32_t head = LoadGuestU32(p);
    const uint32_t byte_off = head >> 16;
    const uint32_t dwords = head & 0xFFFF;
    p += 4;
    if (!dwords) break;
    for (uint32_t i = 0; i < dwords; i++) {
      const uint32_t dst = byte_off + i * 4;
      const uint32_t value = LoadGuestU32(p + i * 4);
      if (dst < 4096) {
        renderer::PatchShaderFloatDwordsHost(false, dst / 4, &value, 1);
      } else if (dst < 8192) {
        renderer::PatchShaderFloatDwordsHost(true, (dst - 4096) / 4, &value,
                                             1);
      }
    }
    p += dwords * 4;
  }
  for (uint32_t guard = 0; guard < 1024 && p + 4 <= def_end; guard++) {
    const uint32_t head = LoadGuestU32(p);
    const uint32_t byte_off = head >> 16;
    const uint32_t n = head & 0xFFFF;
    p += 4;
    if (!n) break;
    for (uint32_t pair = 0; pair < n / 2 && p + 8 <= def_end; pair++) {
      const uint32_t mask = LoadGuestU32(p);
      const uint32_t orv = LoadGuestU32(p + 4);
      p += 8;
      const uint32_t dst = byte_off + pair * 4;
      if (dst < 4096) {
        renderer::MergeShaderFloatDwordHost(false, dst / 4, mask, orv);
      } else if (dst < 8192) {
        renderer::MergeShaderFloatDwordHost(true, (dst - 4096) / 4, mask, orv);
      }
    }
  }
}

// --- command buffers ---------------------------------------------------------
// Public D3DDevice_* calls recorded on the secondary device between Begin and
// End are captured per CB object and re-invoked at Run, with run-varying state
// (constants, textures, viewport) inherited from live state.

enum class CbOp : uint8_t {
  kSetRenderTarget,
  kSetDepthStencilSurface,
  kSetSurfaces,
  kClear,
  kSetViewport,
  kSetScissorRect,
  kSetTexture,
  kSetStreamSource,
  kSetIndices,
  kSetVertexDeclaration,
  kSetVertexShader,
  kSetPixelShader,
  kSetVertexShaderConstantsF,
  kSetPixelShaderConstantsF,
  kResolve,
  kDrawVertices,
  kDrawIndexedVertices,
  kDrawVerticesUP,
  kDrawIndexedVerticesUP,
  kPredicationMark,
  kRunNested,
  kSetBlendControlDirect,
};

struct CbCall {
  CbOp op;
  bool is_draw;     // render-state snapshot in st[]/bool_loop[] is valid
  uint32_t a[9];
  // Render-state shadows of the recording device at record time:
  // RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL, PA_SU_SC_MODE_CNTL,
  // RB_COLOR_MASK, RB_ALPHA_REF, composed ZEnable (raw request at dev+12308),
  // RB_BLENDCONTROL1-3.
  uint32_t st[10];
  // Bool/loop constant block (dev+10112, 40 dwords, host order) of the
  // recording device at record time.
  uint32_t bool_loop[40];
  // Which bool_loop dwords changed on the recording device since Begin or the
  // last recorded draw; replay applies only these over the run device's
  // bool/loop state.
  uint64_t bool_dirty;
  // UP-draw payload captured at record time: raw guest vertex bytes, then for
  // the indexed variant raw index bytes at up_index_offset.
  std::vector<uint8_t> up_data;
  uint32_t up_index_offset = 0;
  // Recording-device fetch-constant shadow slots that changed since Begin or
  // the last recorded draw, as 7-dword records {slot, dw0..dw5}, which also
  // carries the game's direct pokes of the fetch shadow (sub_82A49948 on
  // dev+1152/+1176).
  std::vector<uint32_t> fetch_data;
  // Which st[] slots the game programmed on the recording device (bit i = slot
  // i authoritative), tracked for the blend/mask slots {1,4,7,8,9}; clear slots
  // inherit the run device's live state at replay.
  uint16_t st_dirty;
  // Payload in host-order dwords copied at record time: the raw vec4 data for
  // constant-set ops, and for draws the record-time dirty float constants as
  // 5-dword records {reg | (pixel << 16), v0, v1, v2, v3}.
  std::vector<uint32_t> data;
  double f;
};

std::unordered_map<uint32_t, std::vector<CbCall>> g_commandBuffers;
uint32_t g_recordingCb = 0;
GuestAddr g_recordingDevice = 0;
// Last device used for recording, whose float shadow run-time seeding consults
// for registers the run device never set (see
// renderer::SeedReplayFloatConstants).
GuestAddr g_lastRecordingDevice = 0;
// CB scratch segment per guest device, and the main (type-1) guest device block
// that owns the D3DGAMMARAMP kept by the recompiled D3DDevice_SetGammaRamp.
std::unordered_map<uint32_t, uint32_t> g_deviceScratch;
GuestAddr g_mainGuestDevice = 0;
// Record-time float-constant baseline (host order; [0] = VS regs at dev+1920,
// [1] = PS regs at dev+6016), snapshotted at BeginCommandBuffer and advanced as
// recorded draws capture their dirty registers.
uint32_t g_recordFloatBaseline[2][256 * 4];
// Bool/loop baseline for the recorded draws' dirty capture (see
// CbCall::bool_dirty): the recording device's dev+10112 block at Begin,
// advanced at each recorded draw.
uint32_t g_recordBoolBaseline[40];
// Fetch-constant shadow baseline (dev+1152, 32 slots x 6 dwords) for the
// recorded draws' dirty capture (see CbCall::fetch_data).
uint32_t g_recordFetchBaseline[32 * 6];
// Per-device bool/loop flush shadow: the block as of the last flush point (draw
// or Run pre-flush) on that device, diffed against live values at Begin to
// recover pending dwords because the inlined bool setters cannot be hooked.
std::unordered_map<uint32_t, std::array<uint32_t, 40>> g_boolFlushShadow;
uint64_t g_recordBoolPending = 0;
// Persistent effective bool/loop file: live flush points fold in only the
// shadow's pending dwords and recorded dirty overlays write through, as a CB's
// PM4 bool writes persist in ring state on hardware.
uint32_t g_boolPersist[40];
bool g_boolPersistInit = false;

// Folds the device shadow's pending (changed-since-last-flush) dwords into the
// persist file and advances the flush shadow, the bool analog of
// ConsumeLiveStateDirty.
void FoldBoolPendingIntoPersist(GuestAddr device) {
  if (!device) return;
  auto it = g_boolFlushShadow.find(device);
  const bool have_shadow = it != g_boolFlushShadow.end();
  auto& s = g_boolFlushShadow[device];
  for (uint32_t d = 0; d < 40; d++) {
    const uint32_t live = LoadGuestU32(device + 10112 + d * 4);
    if (!g_boolPersistInit || !have_shadow || it->second[d] != live) {
      g_boolPersist[d] = live;
    }
    s[d] = live;
  }
  g_boolPersistInit = true;
}


// Registers dirty on the recording device at Begin, which the XDK's
// recorded-draw flush bakes into the CB with their record-time values; cleared
// once the first recorded draw captures them.
uint32_t g_recordPendingDirty[2][8];

void SnapshotRecordFloatBaseline(GuestAddr device) {
  auto* mem = rex::system::kernel_memory();
  for (int stage = 0; stage < 2; stage++) {
    const uint32_t* src = mem->TranslateVirtual<const uint32_t*>(
        device + (stage ? 6016 : 1920));
    uint32_t* dst = g_recordFloatBaseline[stage];
    for (uint32_t i = 0; i < 256 * 4; i++) dst[i] = __builtin_bswap32(src[i]);
  }
  auto it = g_floatDirty.find(device);
  if (it != g_floatDirty.end()) {
    std::memcpy(g_recordPendingDirty, it->second.bits,
                sizeof(g_recordPendingDirty));
  } else {
    std::memset(g_recordPendingDirty, 0, sizeof(g_recordPendingDirty));
  }
  for (uint32_t i = 0; i < 32 * 6; i++) {
    g_recordFetchBaseline[i] = LoadGuestU32(device + 1152 + i * 4);
  }
  g_recordBoolPending = 0;
  auto shadow_it = g_boolFlushShadow.find(device);
  for (uint32_t d = 0; d < 40; d++) {
    g_recordBoolBaseline[d] = LoadGuestU32(device + 10112 + d * 4);
    // Pending at Begin: dwords set since the last flush on this device, none
    // when the device has no flush shadow.
    if (shadow_it != g_boolFlushShadow.end() &&
        shadow_it->second[d] != g_recordBoolBaseline[d]) {
      g_recordBoolPending |= 1ull << d;
    }
  }
}
// Depth-stencil binding per guest device, standing in for the XDK's dev+12832
// shadow whose recompiled setters are hooked.
std::unordered_map<uint32_t, uint32_t> g_dsBoundPerDevice;

bool Recording(GuestAddr device) {
  return g_recordingCb != 0 && device == g_recordingDevice;
}

void Record(CbOp op, std::initializer_list<uint32_t> args, double f = 0.0,
            bool is_draw = false, std::vector<uint32_t> payload = {}) {
  CbCall call{};
  call.op = op;
  call.f = f;
  call.is_draw = is_draw;
  call.data = std::move(payload);
  size_t i = 0;
  for (uint32_t v : args) call.a[i++] = v;
  if ((is_draw || op == CbOp::kRunNested) && g_recordingDevice) {
    call.st[0] = LoadGuestU32(g_recordingDevice + 10548);  // RB_DEPTHCONTROL
    call.st[1] = LoadGuestU32(g_recordingDevice + 10552);  // RB_BLENDCONTROL0
    call.st[2] = LoadGuestU32(g_recordingDevice + 10556);  // RB_COLORCONTROL
    call.st[3] = LoadGuestU32(g_recordingDevice + 10568);  // PA_SU_SC_MODE_CNTL
    call.st[4] = LoadGuestU32(g_recordingDevice + 10580);  // RB_COLOR_MASK
    call.st[5] = LoadGuestU32(g_recordingDevice + 10500);  // RB_ALPHA_REF
    // Effective z-enable composed at record time as the XDK does: raw request
    // and a DS surface bound on the recording device.
    const uint32_t raw_z = LoadGuestU32(g_recordingDevice + 12308);
    call.st[6] = (raw_z & 1u) && g_dsBoundPerDevice[g_recordingDevice] ? 1 : 0;
    call.st[7] = LoadGuestU32(g_recordingDevice + 10584);  // RB_BLENDCONTROL1
    call.st[8] = LoadGuestU32(g_recordingDevice + 10588);  // RB_BLENDCONTROL2
    call.st[9] = LoadGuestU32(g_recordingDevice + 10592);  // RB_BLENDCONTROL3
    // Authoritative-slot mask (see CbCall::st_dirty): the recording device's
    // live dev+16 dirty bits at capture time, drained at capture as the XDK's
    // recorded flush does.
    call.st_dirty = 0xFFFFu & ~((1u << 1) | (1u << 4) | (1u << 7) |
                                (1u << 8) | (1u << 9));
    {
      const uint64_t q = (uint64_t(LoadGuestU32(g_recordingDevice + 16)) << 32) |
                         LoadGuestU32(g_recordingDevice + 20);
      if (q & (1ull << 10)) call.st_dirty |= 1u << 1;  // RB_BLENDCONTROL0
      if (q & (1ull << 3)) call.st_dirty |= 1u << 4;   // RB_COLOR_MASK
      if (q & (1ull << 2)) call.st_dirty |= 1u << 7;   // RB_BLENDCONTROL1
      if (q & (1ull << 1)) call.st_dirty |= 1u << 8;   // RB_BLENDCONTROL2
      if (q & (1ull << 0)) call.st_dirty |= 1u << 9;   // RB_BLENDCONTROL3
      const uint64_t drained =
          q & ~((1ull << 10) | (1ull << 3) | (1ull << 2) | (1ull << 1) | 1ull);
      StoreGuestU32(g_recordingDevice + 16, uint32_t(drained >> 32));
      StoreGuestU32(g_recordingDevice + 20, uint32_t(drained));
    }
    // Bool/loop constants of the recording device (host order) with a dirty
    // mask of pending-at-Begin plus changed-since-baseline dwords (see
    // CbCall::bool_dirty, g_recordBoolPending).
    call.bool_dirty = REXCVAR_GET(gpu_cb_bools) >= 2
                          ? g_recordBoolPending
                          : 0;
    for (uint32_t d = 0; d < 40; d++) {
      call.bool_loop[d] = LoadGuestU32(g_recordingDevice + 10112 + d * 4);
      if (call.bool_loop[d] != g_recordBoolBaseline[d]) {
        call.bool_dirty |= 1ull << d;
        g_recordBoolBaseline[d] = call.bool_loop[d];
      }
    }
    // Fetch-constant shadow capture: slots whose current recording-device
    // shadow differs from the baseline, including the game's direct pokes.
    for (uint32_t slot = 0; slot < 32; slot++) {
      uint32_t cur[6];
      bool changed = false;
      for (uint32_t i = 0; i < 6; i++) {
        cur[i] = LoadGuestU32(g_recordingDevice + 1152 + (slot * 6 + i) * 4);
        if (cur[i] != g_recordFetchBaseline[slot * 6 + i]) changed = true;
      }
      if (!changed) continue;
      call.fetch_data.push_back(slot);
      call.fetch_data.insert(call.fetch_data.end(), cur, cur + 6);
      std::memcpy(&g_recordFetchBaseline[slot * 6], cur, sizeof(cur));
    }
    // The recorded flush emitted the pending set, which is cleared, and it is
    // also a flush point for the recording device's shadow.
    g_recordBoolPending = 0;
    {
      auto& s = g_boolFlushShadow[g_recordingDevice];
      std::memcpy(s.data(), call.bool_loop, sizeof(call.bool_loop));
    }
    // Dirty float constants (see CbCall::data): every register of the recording
    // device's float shadow that changed since Begin or the last recorded draw,
    // with the baseline advanced.
    auto* mem = rex::system::kernel_memory();
    for (int stage = 0; stage < 2; stage++) {
      const uint32_t* shadow = mem->TranslateVirtual<const uint32_t*>(
          g_recordingDevice + (stage ? 6016 : 1920));
      uint32_t* baseline = g_recordFloatBaseline[stage];
      for (uint32_t reg = 0; reg < 256; reg++) {
        uint32_t v[4];
        // Dirty means changed since Begin or the last recorded draw, or carried
        // dirty into Begin (see g_recordPendingDirty).
        bool dirty =
            (g_recordPendingDirty[stage][reg >> 5] >> (reg & 31)) & 1u;
        for (int j = 0; j < 4; j++) {
          v[j] = __builtin_bswap32(shadow[reg * 4 + j]);
          dirty |= v[j] != baseline[reg * 4 + j];
        }
        if (!dirty) continue;
        call.data.push_back(reg | (stage ? 0x10000u : 0u));
        call.data.insert(call.data.end(), v, v + 4);
        std::memcpy(&baseline[reg * 4], v, 16);
      }
    }
    // The draw's flush emitted every pending range, so they are cleared like
    // the XDK's.
    std::memset(g_recordPendingDirty, 0, sizeof(g_recordPendingDirty));
  }
  g_commandBuffers[g_recordingCb].push_back(call);
}

void ReplayCall(GuestAddr device, const CbCall& c);  // after the public API

bool g_replaying = false;

}  // namespace

namespace detail {
plume::RenderDevice* Device() { return g_device.get(); }
plume::RenderCommandQueue* Queue() { return g_queue.get(); }
// Render-queue worker override, thread-local so the worker never writes the
// guest-thread globals.
thread_local const ResolvedShader* t_ovVs = nullptr;
thread_local const ResolvedShader* t_ovPs = nullptr;
thread_local uint32_t t_ovPsBound = 0;
thread_local bool t_ovValid = false;
const ResolvedShader* CurrentResolvedVertexShader() {
  return t_ovValid ? t_ovVs : g_currentResolvedVs;
}
const ResolvedShader* CurrentResolvedPixelShader() {
  return t_ovValid ? t_ovPs : g_currentResolvedPs;
}
bool CurrentDrawPixelShaderBound() {
  return t_ovValid ? t_ovPsBound != 0 : g_currentPixelShader != 0;
}
void OverrideResolvedShaders(const ResolvedShader* vs,
                             const ResolvedShader* ps, bool ps_bound) {
  t_ovVs = vs;
  t_ovPs = ps;
  t_ovPsBound = ps_bound ? 1u : 0u;
  t_ovValid = true;
}
void ClearResolvedShaderOverride() { t_ovValid = false; }
const std::vector<uint8_t>& RectExpandGsDxil() {
  EnsureShaderPackLoaded();
  return g_shaderCache.rect_expand_gs();
}
const std::vector<uint8_t>& PointExpandGsDxil() {
  EnsureShaderPackLoaded();
  return g_shaderCache.point_expand_gs();
}
const std::vector<uint8_t>& ClipGsDxil() {
  EnsureShaderPackLoaded();
  return g_shaderCache.clip_gs();
}
const std::vector<uint8_t>& BlitVsDxil() {
  EnsureShaderPackLoaded();
  return g_shaderCache.blit_vs();
}
const std::vector<uint8_t>& BlitPsDxil() {
  EnsureShaderPackLoaded();
  return g_shaderCache.blit_ps();
}
}  // namespace detail

void SetVfetchPatcher(VfetchPatchFn fn) { g_vfetchPatcher = fn; }

bool Enabled() { return REXCVAR_GET(gpu_native); }

namespace {

// GPU register block (0x7FC80000, 64 KB) when no Xenia graphics system owns it:
// writes are latched and read back, and the status registers the XDK polls
// return the values hardware gives.
uint32_t g_gpuRegs[0x10000 / 4];

uint32_t GpuRegRead(void*, void*, uint32_t addr) {
  const uint32_t r = (addr & 0xFFFF) / 4;
  switch (r) {
    case 0x0F00: return 0x08100748;  // RB_EDRAM_TIMING
    case 0x0F01: return 0x0000200E;  // RB_BC_CONTROL
    case 0x194C: return 720;         // D1MODE_V_COUNTER
    case 0x1951: return 1;           // interrupt status: vblank
    case 0x1961: return (1280u << 16) | 720u;  // D1MODE_VIEWPORT_SIZE
    default: return g_gpuRegs[r];
  }
}

void GpuRegWrite(void*, void*, uint32_t addr, uint32_t value) {
  g_gpuRegs[(addr & 0xFFFF) / 4] = value;
}

void EnsureGpuRegisterStub() {
  auto* mem = rex::system::kernel_memory();
  if (!mem || mem->LookupVirtualMappedRange(0x7FC80000)) return;
  mem->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, nullptr,
                             GpuRegRead, GpuRegWrite);
  REXGPU_INFO("videonative: fully native (no Xenia GPU); register stub at "
              "0x7FC80000");
}

}  // namespace

bool Init() {
  if (g_initialized.load()) return true;

  std::lock_guard lock(g_initMutex);
  if (g_initialized.load()) return true;
  if (g_initFailed) return false;  // failure is latched, no retry per call
  EnsureGpuRegisterStub();

  // Shifts the guest heap ahead of everything the game allocates after this
  // point (gpu_heap_pad).
  const int32_t pad = REXCVAR_GET(gpu_heap_pad);
  if (pad > 0) {
    auto* mem = rex::system::kernel_memory();
    const uint32_t block = mem->SystemHeapAlloc(uint32_t(pad), 0x100);
    REXGPU_INFO("videonative: [heappad] {} bytes at {:#x}", pad, block);
  }

  // DRED, the adapter pick and the debug layer are armed through the
  // environment before the first D3D12CreateDevice, and device creation is
  // retried because the first attempt can fail with a transient
  // DXGI_ERROR_DEVICE_RESET.
#ifdef _WIN32
  if (REXCVAR_GET(gpu_dred)) {
    SetEnvironmentVariableA("REXGLUE_DRED", "1");
  }
  const std::string& adapter_pick = REXCVAR_GET(gpu_adapter);
  if (!adapter_pick.empty()) {
    SetEnvironmentVariableA("REXGLUE_ADAPTER", adapter_pick.c_str());
  }
  const int32_t d3d_debug = REXCVAR_GET(gpu_d3d_debug);
  if (d3d_debug > 0) {
    SetEnvironmentVariableA("REXGLUE_D3D_DEBUG", d3d_debug >= 2 ? "2" : "1");
  }
#endif
  for (int attempt = 0; attempt < 6 && !g_device; attempt++) {
    if (attempt > 0) {
      REXGPU_WARN(
          "videonative: plume device creation retry {} (possible transient "
          "adapter reset)",
          attempt);
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    g_interface = plume::CreateD3D12Interface();
    if (!g_interface) {
      REXGPU_ERROR("videonative: plume D3D12 interface creation failed");
      continue;
    }
    g_device = g_interface->createDevice();
    if (!g_device) g_interface.reset();
  }
#ifdef _WIN32
  // An adapter filter that matches nothing falls back to the default adapter
  // with a warning.
  if (!g_device && !adapter_pick.empty()) {
    REXGPU_WARN(
        "videonative: gpu_adapter '{}' matched no adapter — "
        "falling back to the default adapter",
        adapter_pick);
    SetEnvironmentVariableA("REXGLUE_ADAPTER", nullptr);
    g_interface = plume::CreateD3D12Interface();
    if (g_interface) {
      g_device = g_interface->createDevice();
      if (!g_device) g_interface.reset();
    }
  }
#endif
  if (g_device) {
    const std::string& adapter_name = g_device->getDescription().name;
    REXGPU_INFO("videonative: adapter '{}'", adapter_name);
    // Per-vendor gates (gpu_vs_trim mode 2).
    renderer::SetAdapterIsIntel(adapter_name.find("Intel") !=
                                std::string::npos);
  }
  if (!g_device) {
    REXGPU_ERROR("videonative: plume device creation failed");
    g_interface.reset();
    g_initFailed = true;
    return false;
  }
  g_queue = g_device->createCommandQueue(plume::RenderCommandListType::DIRECT);

  if (!renderer::Init()) {
    REXGPU_ERROR("videonative: renderer init failed");
    g_queue.reset();
    g_device.reset();
    g_interface.reset();
    g_initFailed = true;
    return false;
  }
  rq::Start();  // render-thread offload (no-op unless the cvar is on)

  const auto& desc = g_device->getDescription();
  REXGPU_INFO("videonative: plume D3D12 device up on '{}' ({} MB VRAM)",
              desc.name, desc.dedicatedVideoMemory >> 20);

  // The plume device, queue and interface are leaked at CRT exit to avoid a
  // DXGI teardown deadlock (see the renderer's swapchain exit guard).
  static bool vn_exit_guard = false;
  if (!vn_exit_guard) {
    vn_exit_guard = true;
    std::atexit([] {
      g_queue.release();
      g_device.release();
      g_interface.release();
    });
  }

  g_initialized.store(true);
  return true;
}

void Shutdown() {
  std::lock_guard lock(g_initMutex);
  if (!g_initialized.load()) return;
  renderer::Shutdown();
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
  g_initialized.store(false);
}

bool IsActive() { return Enabled() && g_initialized.load(); }

// --- device / swap ---

uint32_t CreateDevice(GuestAddr, uint32_t type, uint32_t, uint32_t,
                      GuestAddr present_params, GuestAddr out_device) {
  if (!Init()) return 0x80004005u;  // E_FAIL — caller falls back to guest path

  auto* mem = rex::system::kernel_memory();
  uint32_t guest_device = mem->SystemHeapAlloc(kGuestDeviceSize, 0x100);
  if (!guest_device) return 0x8007000Eu;  // E_OUTOFMEMORY
  std::memset(mem->TranslateVirtual<void*>(guest_device), 0, kGuestDeviceSize);

  // The real Direct3D_CreateDevice stores the device pointer through the
  // xex-header slots at 0x82000864/0x82000868, a page some titles' runtimes
  // protect read-only, so it is unprotected before seeding.
  if (auto* heap = const_cast<rex::memory::BaseHeap*>(
          mem->LookupHeap(0x82000864u))) {
    heap->Protect(0x82000000u, 0x1000,
                  rex::memory::kMemoryProtectRead |
                      rex::memory::kMemoryProtectWrite,
                  nullptr);
  }
  // Double indirection: the header slots hold pointers to the XDK's
  // g_pDevice/g_pDeviceAux variables (readers do [slot] -> [ptr] -> device).
  {
    const uint32_t devvars = mem->SystemHeapAlloc(16, 0x10);
    StoreGuestU32(devvars + 0, guest_device);
    StoreGuestU32(devvars + 4, guest_device);
    StoreGuestU32(0x82000864u, devvars + 0);
    StoreGuestU32(0x82000868u, devvars + 4);
  }
  StoreGuestU32(guest_device + 10580, 0xFFFF);  // RB_COLOR_MASK: RT0-3 RGBA on
  // RB_BLENDCONTROL0-3 default to ONE/ADD/ZERO on both pipes (blending
  // disabled), since a zeroed shadow decodes as srcblend ZERO.
  StoreGuestU32(guest_device + 10552, 0x00010001);  // RB_BLENDCONTROL0
  StoreGuestU32(guest_device + 10584, 0x00010001);  // RB_BLENDCONTROL1
  StoreGuestU32(guest_device + 10588, 0x00010001);  // RB_BLENDCONTROL2
  StoreGuestU32(guest_device + 10592, 0x00010001);  // RB_BLENDCONTROL3

  // CB scratch segment and fence pool: the XDK appender writes through
  // *(dev+48), which on a zeroed device lands stray packets in guest page zero.
  if (REXCVAR_GET(gpu_device_ring_scratch)) {
    constexpr uint32_t kScratchSize = 64 * 1024;
    const uint32_t scratch = mem->SystemHeapAlloc(kScratchSize, 0x100);
    const uint32_t pool = mem->SystemHeapAlloc(256, 0x100);
    if (scratch && pool) {
      std::memset(mem->TranslateVirtual<void*>(scratch), 0, kScratchSize);
      std::memset(mem->TranslateVirtual<void*>(pool), 0, 256);
      StoreGuestU32(guest_device + 48, scratch);
      StoreGuestU32(guest_device + 56, scratch + kScratchSize - 0x200);
      StoreGuestU32(guest_device + 11024, pool);
      // XDK init idiom: current fence 3, completed = current-2.
      StoreGuestU32(guest_device + 11036, 3);
      // With gpu_fence_pool_max the completed counter stays above any
      // fence value, so loaders blocking on 'completed >= resource fence'
      // resume at once; otherwise pool[0]=1 and advances per present.
      StoreGuestU32(pool, REXCVAR_GET(gpu_fence_pool_max)
                              ? 0xFFFFFF00u
                              : 1u);
      g_deviceScratch[guest_device] = scratch;
      REXGPU_INFO(
          "videonative: [ringscratch] dev={:#x} scratch={:#x} pool={:#x}",
          guest_device, scratch, pool);
    }
  }
  StoreGuestU32(out_device, guest_device);
  if (type == 1) g_mainGuestDevice = guest_device;
  REXGPU_INFO(
      "videonative: CreateDevice type={} presentParams={:#x} -> guest device "
      "{:#x}",
      type, present_params, guest_device);
  if (present_params) {
    REXGPU_INFO(
        "videonative: [ppdump] bb={}x{} fmt={:#x} count={} swap={} fs={} "
        "msaa={} flags={:#x} hz={} pi={:#x}",
        LoadGuestU32(present_params + 0), LoadGuestU32(present_params + 4),
        LoadGuestU32(present_params + 8), LoadGuestU32(present_params + 12),
        LoadGuestU32(present_params + 16), LoadGuestU32(present_params + 20),
        LoadGuestU32(present_params + 24), LoadGuestU32(present_params + 28),
        LoadGuestU32(present_params + 32), LoadGuestU32(present_params + 36));
  }
  return 0;  // S_OK
}

// GPU fence writeback emulation: writebacks queued by the hooked XDK
// fence-packet writer (base 0x8253FDD8 / TU1 0x8254F4A8) are written at the
// next Swap with an incrementing counter, a one-frame completion lag.
std::mutex g_fenceMutex;
std::vector<GuestAddr> g_fencePendingThisFrame;
std::vector<GuestAddr> g_fenceDueNextSwap;
uint32_t g_fenceCounter = 0x1000;

void QueueFenceWriteback(GuestAddr addr) {
  if (!addr) return;
  std::lock_guard lock(g_fenceMutex);
  g_fencePendingThisFrame.push_back(addr);
}

void Swap(GuestAddr device, GuestAddr front_buffer_tex, GuestAddr) {
  g_frameCount.fetch_add(1);
  // Frame bookkeeping of the skipped Swap body: the 8-slot per-frame fence
  // ring, the VdSwap writeback and the frame counters at dev+22036/+22032.
  if (device && REXCVAR_GET(gpu_swap_bookkeeping)) {
    const uint32_t pool = LoadGuestU32(device + 11024);
    const uint32_t f = LoadGuestU32(device + 22036);
    if (pool) {
      uint32_t cur = LoadGuestU32(device + 11036);
      StoreGuestU32(pool + 4 * (16 + (f & 7)), ++cur);
      StoreGuestU32(pool + 4 * (16 + ((f + 1) & 7)), ++cur);
      StoreGuestU32(pool + 8, cur);      // VdSwap frontbuffer writeback slot
      StoreGuestU32(device + 11036, cur);
      if (!REXCVAR_GET(gpu_fence_pool_max)) {
        StoreGuestU32(pool, cur - 1);    // completed-fence idiom
      }
    }
    StoreGuestU32(device + 22036, f + 2);
    StoreGuestU32(device + 22032, f + 2);  // complete instantly (no GPU lag)
  }
  // Re-arms the CB scratch write pointer each present, since the scratch only
  // absorbs stray PM4 appends.
  if (device) {
    auto it_scratch = g_deviceScratch.find(device);
    if (it_scratch != g_deviceScratch.end()) {
      StoreGuestU32(device + 48, it_scratch->second);
    }
  }
  if (device && REXCVAR_GET(gpu_fence_emulation)) {
    const uint32_t cur = LoadGuestU32(device + 11036) + 1;
    StoreGuestU32(device + 11036, cur);
    const uint32_t wb = LoadGuestU32(device + 11024);
    // Writes through wb only when it is plausibly a guest pointer, since the
    // Swap body that initializes it is skipped.
    if (wb >= 0x10000 && wb < 0xC0000000u) StoreGuestU32(wb, cur - 1);
  }
  {
    // Completes last frame's fences and promotes this frame's to the next.
    std::lock_guard lock(g_fenceMutex);
    for (GuestAddr a : g_fenceDueNextSwap) {
      StoreGuestU32(a & ~3u, ++g_fenceCounter);
    }
    g_fenceDueNextSwap.swap(g_fencePendingThisFrame);
    g_fencePendingThisFrame.clear();
  }
  // Display gamma ramp: the recompiled D3DDevice_SetGammaRamp keeps a
  // 256x3-word D3DGAMMARAMP in the device block at a title-dependent offset
  // (+15408 on CC2, +15216 on CC1), so both candidates are scored as a ramp and
  // the winner is forwarded to the present blit's LUT whenever it changes.
  if (g_mainGuestDevice) {
    static std::array<uint16_t, 768> cached{};
    static bool have_ramp = false;
    static uint32_t chosen_offset = 0;
    auto read_ramp = [&](uint32_t off, std::array<uint16_t, 768>& out) -> bool {
      const uint8_t* p = rex::system::kernel_memory()
                             ->TranslateVirtual<const uint8_t*>(
                                 g_mainGuestDevice + off);
      if (!p) return false;
      for (uint32_t i = 0; i < 768; i++) {
        out[i] = uint16_t((p[i * 2] << 8) | p[i * 2 + 1]);
      }
      return true;
    };
    // A valid ramp has each 256-entry channel non-decreasing and ending far
    // above where it starts.
    auto score_ramp = [](const std::array<uint16_t, 768>& r) -> int {
      int score = 0;
      for (uint32_t ch = 0; ch < 3; ch++) {
        const uint16_t* c = r.data() + ch * 256;
        uint32_t monotonic = 0;
        for (uint32_t i = 1; i < 256; i++) {
          if (c[i] >= c[i - 1]) monotonic++;
        }
        if (monotonic >= 250) score += 2;      // sorted within tolerance
        if (c[255] > c[0] + 4096) score += 1;  // real dynamic range
      }
      return score;  // 9 = perfect on all three channels
    };
    if (!chosen_offset) {
      const int32_t forced = REXCVAR_GET(gpu_gamma_ramp_offset);
      if (forced > 0) {
        chosen_offset = uint32_t(forced);
        REXGPU_INFO("videonative: gamma ramp offset forced to +{}", forced);
      } else {
        std::array<uint16_t, 768> a{}, b{};
        const bool ok_a = read_ramp(15408, a);
        const bool ok_b = read_ramp(15216, b);
        const int sa = ok_a ? score_ramp(a) : -1;
        const int sb = ok_b ? score_ramp(b) : -1;
        if (sa >= 6 || sb >= 6) {
          chosen_offset = (sb > sa) ? 15216u : 15408u;
          REXGPU_INFO(
              "videonative: gamma ramp offset auto -> +{} (score +15408={} "
              "+15216={})",
              chosen_offset, sa, sb);
        }
        // Neither scores while the game has not written a ramp, so the choice
        // is retried next present.
      }
    }
    std::array<uint16_t, 768> host{};
    bool nonzero = false;
    if (chosen_offset && read_ramp(chosen_offset, host)) {
      for (uint32_t i = 0; i < 768; i++) nonzero |= host[i] != 0;
    }
    // An all-zero block means the game has not set a ramp, so the LUT stays
    // unbound.
    if (nonzero && (!have_ramp || host != cached)) {
      cached = host;
      have_ramp = true;
      renderer::UpdateGammaRamp(host.data());
      REXGPU_INFO(
          "videonative: gamma ramp updated (frame {}): R[32]={} R[64]={} "
          "R[128]={} R[192]={} R[255]={}",
          g_frameCount.load(), host[32], host[64], host[128], host[192],
          host[255]);
    }
  }
  // The frontbuffer texture (fetch header at +0x1C) carries the frame the game
  // resolved its final composite into, which is blitted onto the swapchain.
  if (front_buffer_tex) {
    renderer::SwapFrontbuffer(front_buffer_tex + 0x1C);
  }
  renderer::EndFrameAndPresent();
}

void AcquireThreadOwnership(GuestAddr) {}
void ReleaseThreadOwnership(GuestAddr) {}

// --- render targets / tiling ---
void SetRenderTarget(GuestAddr device, uint32_t index, GuestAddr surface) {
  if (Recording(device)) {
    Record(CbOp::kSetRenderTarget, {index, surface});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  // All four slots are forwarded, since the LightingPass binds its 8888
  // specular accumulation on RT1.
  renderer::SetRenderTargetSurface(index, surface);
}
void SetDepthStencilSurface(GuestAddr device, GuestAddr surface, uint32_t) {
  g_dsBoundPerDevice[device] = surface;
  if (Recording(device)) {
    Record(CbOp::kSetDepthStencilSurface, {surface});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  renderer::SetDepthSurface(surface);
}
// D3DDevice_SetSurfaces(device, {DS, RT0..RT3}, flags) — CC2's unbind idiom
// is SetSurfaces(all-NULL, 1) after ResolveAndUnbind.
void SetSurfaces(GuestAddr device, GuestAddr surfaces, uint32_t) {
  const uint32_t ds = surfaces ? LoadGuestU32(surfaces + 0) : 0;
  uint32_t rt[4] = {};
  if (surfaces) {
    for (uint32_t i = 0; i < 4; i++) rt[i] = LoadGuestU32(surfaces + 4 + i * 4);
  }
  g_dsBoundPerDevice[device] = ds;
  if (Recording(device)) {
    Record(CbOp::kSetSurfaces, {ds, rt[0], rt[1], rt[2], rt[3]});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  renderer::SetDepthSurface(ds);
  for (uint32_t i = 0; i < 4; i++) renderer::SetRenderTargetSurface(i, rt[i]);
}
// D3DDevice_CreateSurface (0x8252BB30): (width, height, format, msaa, params)
// -> surface pointer, with the object given the XDK field layout that GetDesc
// and the engine's EDRAM math read back.
uint32_t CreateSurface(uint32_t width, uint32_t height, uint32_t format,
                       uint32_t msaa, GuestAddr params) {
  if (!width || !height || width > 8192 || height > 8192) return 0;
  auto* mem = rex::system::kernel_memory();
  const uint32_t obj = mem->SystemHeapAlloc(48, 0x10);
  if (!obj) return 0;
  std::memset(mem->TranslateVirtual<void*>(obj), 0, 48);
  StoreGuestU32(obj + 0, 0x100000u | (params ? 0 : 0x80000000u));
  StoreGuestU32(obj + 4, 1);           // refcount
  StoreGuestU32(obj + 20, 0xFFFF0000u);
  StoreGuestU32(obj + 24, msaa & 3);   // pitch/msaa dword (msaa bits only)
  StoreGuestU32(obj + 36, ((width - 1) << 18) | ((height - 1) << 3));
  StoreGuestU32(obj + 40, format);
  StoreGuestU32(obj + 44, 5120);
  renderer::RegisterSurface(obj, width, height, format);
  return obj;
}
// D3DDevice_BeginTiling (0x82531398): r4=flags, r5=tile count, r6=D3DRECT array
// (frame coords), r7=float* clearColor, f1=clearZ; the tile rects are kept for
// the resolves EndTiling performs into its dest texture.
uint32_t g_tilingRectCount = 0;
GuestAddr g_tilingRects = 0;

void BeginTiling(GuestAddr, uint32_t flags, uint32_t count, GuestAddr rects,
                 GuestAddr clear_color, float clear_z, uint32_t) {
  g_tilingRectCount = count;
  g_tilingRects = rects;
  uint32_t max_right = 0, max_bottom = 0;
  for (uint32_t i = 0; i < count && i < 16; i++) {
    const uint32_t right = LoadGuestU32(rects + i * 16 + 8);
    const uint32_t bottom = LoadGuestU32(rects + i * 16 + 12);
    if (right > max_right && right <= 8192) max_right = right;
    if (bottom > max_bottom && bottom <= 8192) max_bottom = bottom;
  }
  renderer::SetTilingExtent(max_right, max_bottom);
  if ((flags & 1) == 0) {
    uint32_t color = 0;
    if (clear_color) {
      // D3DVECTOR4 {r,g,b,a} floats -> packed ARGB.
      float rgba[4];
      for (int i = 0; i < 4; i++) {
        const uint32_t bits = LoadGuestU32(clear_color + i * 4);
        std::memcpy(&rgba[i], &bits, 4);
      }
      auto to8 = [](float f) {
        return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
      };
      color = (to8(rgba[3]) << 24) | (to8(rgba[0]) << 16) |
              (to8(rgba[1]) << 8) | to8(rgba[2]);
    }
    renderer::Clear(0, 0x3F, color, clear_z);
  }
}
void EndTiling(GuestAddr device, uint32_t flags, GuestAddr rects,
               GuestAddr dest_texture) {
  // XDK D3DDevice_EndTiling (0x82531708) resolves every tile rect into the dest
  // texture, when one is given, before ending the bracket.
  if (dest_texture) {
    if (!rects) rects = g_tilingRects;
    const uint32_t count = g_tilingRectCount ? g_tilingRectCount : 1;
    for (uint32_t i = 0; i < count && i < 16; i++) {
      const GuestAddr rect = rects ? rects + i * 16 : 0;
      Resolve(device, flags, rect, dest_texture, rect);
    }
  }
  g_tilingRectCount = 0;
  g_tilingRects = 0;
  renderer::SetTilingExtent(0, 0);
}
// D3DDevice_SetPredication(mask) writes SET_BIN_MASK_LO; with a single
// full-frame pass the live mask at Run time is the effective bin select for
// that Run's spans, and 0 resets it to all-ones.
uint32_t g_predicationSelectLo = 0xFFFFFFFFu;

// force=true skips the pending gate, because under the render queue the push
// happens on the worker and a guest-side pending check right after enqueuing a
// resolve races to zero.
void FlushResolveWritebacksSyncEx(bool force) {
  if (!force && !renderer::ResolveWritebacksPending()) return;
  if (rq::Active()) {
    const uint64_t ticket = rq::EnqFlushResolveWriteback();
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (rq::WbFlushDone() < ticket) {
      std::this_thread::yield();
      if (std::chrono::steady_clock::now() > deadline) {
        static uint64_t timeout_logs = 8;
        if (timeout_logs) {
          timeout_logs--;
          REXGPU_WARN(
              "videonative: [writeback] sync flush timed out (worker busy)");
        }
        break;
      }
    }
  } else {
    renderer::FlushResolveWritebacksInline();
  }
}

void FlushResolveWritebacksSync() { FlushResolveWritebacksSyncEx(false); }

bool WritebackSyncEnabled() { return renderer::WritebackSyncEnabled(); }

// Pack prover access (renderer.cpp ProvePack) to the cache object in the
// anonymous namespace above.
const ShaderCache& PackCacheForProver() { return g_shaderCache; }

void NoteRawDevice(uint32_t device_ea) {
  renderer::NoteRawDevice(device_ea);
}

void InvalidateTextureByHeader(uint32_t header_addr) {
  renderer::InvalidateTextureByHeader(header_addr);
}

void NoteRttBegin(uint32_t texbase, uint32_t width, uint32_t height,
                  uint32_t color_surf, uint32_t depth_surf, uint32_t dest_tex,
                  uint32_t format, uint32_t msaa, uint32_t tiling) {
  renderer::NoteRttBegin(texbase, width, height, color_surf, depth_surf,
                         dest_tex, format, msaa, tiling);
}

void SetPredication(GuestAddr device, uint32_t mask) {
  if (Recording(device)) {
    // While recording, SetPredication writes SET_BIN_MASK_LO into the CB, the
    // same effect as a SetCommandBufferPredication tile mark.
    Record(CbOp::kPredicationMark, {mask, 0});
    return;
  }
  g_predicationSelectLo = mask ? mask : 0xFFFFFFFFu;
}

namespace {
// Shared resolve body taking parameters already read out of guest memory, since
// the recorded path captures the rect and point by value.
void ResolveParsed(uint32_t flags, GuestAddr dest_texture, bool has_rect,
                   int32_t l, int32_t t, int32_t r, int32_t b, int32_t dx,
                   int32_t dy) {
  if (!dest_texture) return;
  const uint32_t source = flags & 7;
  if (source == 4) {
    // Depth resolve, which feeds the depth-restore draw's VS fetch.
    renderer::ResolveDepthToTexture(dest_texture + 0x1C, has_rect, l, t, r, b,
                                    dx, dy);
    return;
  }
  // Sources 0-3 are the MRT color attachments, resolved separately by the
  // lighting/AO bakes that write oC0 and oC1.
  renderer::ResolveToTexture(dest_texture + 0x1C, source, has_rect, l, t, r,
                             b, dx, dy);
  // Write-back delivery at the resolve itself, as hardware's resolve writes
  // RAM, forced because under the render queue the push happens later on the
  // worker.
  if (renderer::ResolveWritebackEnabled() && renderer::WritebackSyncEnabled()) {
    FlushResolveWritebacksSyncEx(true);
  }
}
}  // namespace

// D3DDevice_Resolve: flags bits 0-2 select the source (0-3 = color RT0-3, 4 =
// depth), r5 = D3DRECT* srcRect (frame coords), r6 = dest texture object (fetch
// header at +0x1C), r7 = D3DPOINT* destPoint.
void Resolve(GuestAddr device, uint32_t flags, GuestAddr rect,
             GuestAddr dest_texture, GuestAddr dest_point) {
  const bool has_rect = rect != 0;
  int32_t l = 0, t = 0, r = 0, b = 0, dx = 0, dy = 0;
  if (rect) {
    l = int32_t(LoadGuestU32(rect + 0));
    t = int32_t(LoadGuestU32(rect + 4));
    r = int32_t(LoadGuestU32(rect + 8));
    b = int32_t(LoadGuestU32(rect + 12));
  }
  if (dest_point) {
    dx = int32_t(LoadGuestU32(dest_point + 0));
    dy = int32_t(LoadGuestU32(dest_point + 4));
  }
  if (Recording(device)) {
    Record(CbOp::kResolve,
           {flags, dest_texture, uint32_t(has_rect), uint32_t(l), uint32_t(t),
            uint32_t(r), uint32_t(b), uint32_t(dx), uint32_t(dy)});
    return;
  }
  ResolveParsed(flags, dest_texture, has_rect, l, t, r, b, dx, dy);
}

// --- clear / viewport / scissor ---
// pRects scope the clear, since the per-tile setup command buffers clear only
// their band of the full-size RT.
void Clear(GuestAddr device, uint32_t count, GuestAddr rects, uint32_t flags,
           uint32_t color, double z, uint32_t) {
  int32_t rect_vals[16] = {};
  uint32_t n = rects ? std::min(count, 4u) : 0;
  for (uint32_t i = 0; i < n * 4; i++) {
    rect_vals[i] = int32_t(LoadGuestU32(rects + i * 4));
  }
  if (Recording(device)) {
    // Captured by value because the guest rect array may be a stack temporary;
    // only the first rect fits the record.
    Record(CbOp::kClear,
           {flags, color, n, uint32_t(rect_vals[0]), uint32_t(rect_vals[1]),
            uint32_t(rect_vals[2]), uint32_t(rect_vals[3])},
           z);
    return;
  }
  renderer::Clear(device, flags, color, float(z), rect_vals, n);
}
void SetViewport(GuestAddr device, GuestAddr viewport) {
  if (!viewport) return;
  const uint32_t x = LoadGuestU32(viewport + 0);
  const uint32_t y = LoadGuestU32(viewport + 4);
  const uint32_t w = LoadGuestU32(viewport + 8);
  const uint32_t h = LoadGuestU32(viewport + 12);
  const uint32_t minz_bits = LoadGuestU32(viewport + 16);
  const uint32_t maxz_bits = LoadGuestU32(viewport + 20);
  if (Recording(device)) {
    // Captured by value like the XDK's PM4 write.
    Record(CbOp::kSetViewport, {x, y, w, h, minz_bits, maxz_bits});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  float min_z, max_z;
  std::memcpy(&min_z, &minz_bits, 4);
  std::memcpy(&max_z, &maxz_bits, 4);
  renderer::SetViewport(x, y, w, h, min_z, max_z);
}
void SetScissorRect(GuestAddr device, GuestAddr rect) {
  if (!rect) return;
  const uint32_t l = LoadGuestU32(rect + 0);
  const uint32_t t = LoadGuestU32(rect + 4);
  const uint32_t r = LoadGuestU32(rect + 8);
  const uint32_t b = LoadGuestU32(rect + 12);
  if (Recording(device)) {
    Record(CbOp::kSetScissorRect, {l, t, r, b});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  renderer::SetScissor(int32_t(l), int32_t(t), int32_t(r), int32_t(b));
}

// --- resources ---
uint32_t CreateTexture(GuestAddr, uint32_t, uint32_t, uint32_t, uint32_t,
                       uint32_t, uint32_t, uint32_t, uint32_t) {
  return 0;
}
uint32_t CreateVertexBuffer(uint32_t, uint32_t) { return 0; }
uint32_t CreateIndexBuffer(uint32_t, uint32_t, uint32_t) { return 0; }
void ReleaseResource(GuestAddr) {}
void ReleaseDevice(GuestAddr) {}

// --- binding ---
void SetTexture(GuestAddr device, uint32_t sampler, GuestAddr texture) {
  if (Recording(device)) {
    Record(CbOp::kSetTexture, {sampler, texture});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  // Stamps the resource fence (obj+8) so Release defers the free while in
  // flight, then binds the GPUTEXTURE_FETCH constant at +0x1C of the XDK
  // texture object.
  if (texture && REXCVAR_GET(gpu_fence_emulation)) {
    StoreGuestU32(texture + 8, LoadGuestU32(device + 11036));
  }
  renderer::SetTexture(sampler, texture ? texture + 0x1C : 0);
  // Mirrors the 6 fetch dwords into the guest device fetch shadow
  // (dev+1152+24*sampler) as the skipped XDK SetTexture body does, since draws
  // read the shadow and some engine code writes it directly.
  if (device && sampler < 32) {
    const uint32_t slot = device + 1152 + 24 * sampler;
    if (texture) {
      auto* mem = rex::system::kernel_memory();
      const uint32_t* src =
          mem->TranslateVirtual<const uint32_t*>(texture + 0x1C);
      uint32_t* dst = mem->TranslateVirtual<uint32_t*>(slot);
      if (src && dst) std::memcpy(dst, src, 24);  // raw copy, same endianness
    } else {
      auto* mem = rex::system::kernel_memory();
      uint32_t* dst = mem->TranslateVirtual<uint32_t*>(slot);
      if (dst) std::memset(dst, 0, 24);
    }
  }
}
void SetStreamSource(GuestAddr device, uint32_t stream, GuestAddr vb,
                     uint32_t offset, uint32_t unk, uint32_t stride) {
  if (Recording(device)) {
    Record(CbOp::kSetStreamSource, {stream, vb, offset, unk, stride});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  if (stream < 16) g_streamStrideDwords[stream] = uint8_t(stride >> 2);
  renderer::SetStream(stream, vb, offset, stride);
}
void SetIndices(GuestAddr device, GuestAddr ib) {
  if (Recording(device)) {
    Record(CbOp::kSetIndices, {ib});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  renderer::SetIndices(ib);
}
void SetVertexDeclaration(GuestAddr device, GuestAddr decl) {
  if (Recording(device)) {
    Record(CbOp::kSetVertexDeclaration, {decl});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  g_currentVertexDecl = decl;
}

// --- shaders ---
uint32_t CreateVertexShader(GuestAddr) { return 0; }
uint32_t CreatePixelShader(GuestAddr) { return 0; }
void SetVertexShader(GuestAddr device, GuestAddr shader) {
  if (Recording(device)) {
    Record(CbOp::kSetVertexShader, {shader});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  g_currentVertexShader = shader;
  // Defaults are not applied while recording, since the recorded bind reapplies
  // them at replay.
  if (shader && !Recording(device)) {
    // VS object: 872-byte header (physical blob ptr at +32), container at +872.
    ApplyShaderFloatDefaults(device, false, shader + 872,
                             LoadGuestU32(shader + 32), g_replaying);
  }
}
void SetPixelShader(GuestAddr device, GuestAddr shader) {
  if (Recording(device)) {
    Record(CbOp::kSetPixelShader, {shader});
    // Falls through to apply live too, since the Beenox wrapper caches treat
    // recorded binds as current.
  }
  g_currentPixelShader = shader;
  if (shader && !Recording(device)) {
    // PS object: 40-byte header (physical blob ptr at +0x18), container at +40.
    ApplyShaderFloatDefaults(device, true, shader + 40,
                             LoadGuestU32(shader + 0x18), g_replaying);
  }
}
namespace {
std::vector<uint32_t> CopyConstPayload(GuestAddr data, uint32_t count) {
  std::vector<uint32_t> payload(size_t(count) * 4);
  for (uint32_t i = 0; i < count * 4; i++) {
    payload[i] = LoadGuestU32(data + i * 4);
  }
  return payload;
}
}  // namespace

void SetVertexShaderConstantF(GuestAddr device, uint32_t start_reg,
                              GuestAddr data, uint32_t count) {
  MarkFloatsDirty(device, false, start_reg, count);
  // While recording, the values are recorded because the guest pointer is a
  // stack temporary and the XDK records constant sets inline into the command
  // buffer.
  if (Recording(device)) {
    if (!count || count > 256) return;
    Record(CbOp::kSetVertexShaderConstantsF, {start_reg, count}, 0.0, false,
           CopyConstPayload(data, count));
    return;
  }
  renderer::SetShaderConstantsF(false, start_reg, data, count);
}
void SetPixelShaderConstantF(GuestAddr device, uint32_t start_reg,
                             GuestAddr data, uint32_t count) {
  MarkFloatsDirty(device, true, start_reg, count);
  if (Recording(device)) {
    if (!count || count > 256) return;
    Record(CbOp::kSetPixelShaderConstantsF, {start_reg, count}, 0.0, false,
           CopyConstPayload(data, count));
    return;
  }
  renderer::SetShaderConstantsF(true, start_reg, data, count);
}
void SetShaderGPRAllocation(GuestAddr, uint32_t, uint32_t, uint32_t) {
  /* no-op by design */
}

// --- render states (read back from device shadows at draw time) ---
void SetRenderState_ZEnable(GuestAddr, uint32_t) {}
void SetRenderState_ZWriteEnable(GuestAddr, uint32_t) {}
void SetRenderState_ZFunc(GuestAddr, uint32_t) {}
void SetRenderState_CullMode(GuestAddr, uint32_t) {}
void SetRenderState_AlphaTestEnable(GuestAddr, uint32_t) {}
void SetRenderState_AlphaRef(GuestAddr, uint32_t) {}
void SetRenderState_AlphaFunc(GuestAddr, uint32_t) {}
void SetBlendControl(GuestAddr, uint32_t, uint32_t) {}

// --- draws ---
void DrawVertices(GuestAddr device, uint32_t prim_type, uint32_t start,
                  uint32_t count) {
  if (Recording(device)) {
    Record(CbOp::kDrawVertices, {prim_type, start, count}, 0.0, true);
    ClearFloatDirty(device);  // the XDK recorded-draw flush clears dirty too
    return;
  }
  OnDraw();
  renderer::DrawVertices(device, prim_type, start, count);
  ClearFloatDirty(device);  // draw flush emitted + cleared the dirty ranges
  FoldBoolPendingIntoPersist(device);  // bool flush point (persist fold)
}
void DrawIndexedVertices(GuestAddr device, uint32_t prim_type,
                         uint32_t base_vertex, uint32_t start,
                         uint32_t count) {
  if (Recording(device)) {
    Record(CbOp::kDrawIndexedVertices, {prim_type, base_vertex, start, count},
           0.0, true);
    ClearFloatDirty(device);
    return;
  }
  OnDraw();
  renderer::DrawIndexedVertices(device, prim_type, base_vertex, start, count);
  ClearFloatDirty(device);
  FoldBoolPendingIntoPersist(device);
}
// UP draws feed stream 0's vfetch stride from the draw argument, as the XDK
// binds the inline vertex data as a temporary stream 0.
void DrawVerticesUP(GuestAddr device, uint32_t prim_type, uint32_t count,
                    GuestAddr vertex_data, uint32_t stride) {
  if (Recording(device)) {
    // XDK semantics: UP data is copied into the CB at record time.
    Record(CbOp::kDrawVerticesUP, {prim_type, count, vertex_data, stride}, 0.0,
           true);
    {
      CbCall& call = g_commandBuffers[g_recordingCb].back();
      const uint32_t bytes = count * stride;
      const uint8_t* src =
          bytes && vertex_data
              ? rex::system::kernel_memory()->TranslateVirtual<const uint8_t*>(
                    vertex_data)
              : nullptr;
      if (src && bytes < (1u << 22)) {
        call.up_data.assign(src, src + bytes);
      }
    }
    ClearFloatDirty(device);
    return;
  }
  if (stride) g_streamStrideDwords[0] = uint8_t(stride >> 2);
  OnDraw();
  renderer::DrawVerticesUP(device, prim_type, count, vertex_data, stride);
  ClearFloatDirty(device);
  FoldBoolPendingIntoPersist(device);
}
void DrawIndexedVerticesUP(GuestAddr device, uint32_t prim_type,
                           uint32_t min_vertex, uint32_t num_vertices,
                           uint32_t index_count, GuestAddr index_data,
                           uint32_t index_format, GuestAddr vertex_data,
                           uint32_t stride) {
  if (Recording(device)) {
    Record(CbOp::kDrawIndexedVerticesUP,
           {prim_type, min_vertex, num_vertices, index_count, index_data,
            index_format, vertex_data, stride},
           0.0, true);
    {
      CbCall& call = g_commandBuffers[g_recordingCb].back();
      const uint32_t vbytes = num_vertices * stride;
      const uint32_t ibytes = index_count * 2;  // 16-bit -- CC2's only format
      auto* mem = rex::system::kernel_memory();
      const uint8_t* vsrc =
          vbytes && vertex_data
              ? mem->TranslateVirtual<const uint8_t*>(vertex_data)
              : nullptr;
      const uint8_t* isrc =
          ibytes && index_data
              ? mem->TranslateVirtual<const uint8_t*>(index_data)
              : nullptr;
      if (vsrc && isrc && vbytes + ibytes < (1u << 22)) {
        call.up_data.reserve(vbytes + ibytes);
        call.up_data.assign(vsrc, vsrc + vbytes);
        call.up_data.insert(call.up_data.end(), isrc, isrc + ibytes);
        call.up_index_offset = vbytes;
      }
    }
    ClearFloatDirty(device);
    return;
  }
  if (stride) g_streamStrideDwords[0] = uint8_t(stride >> 2);
  OnDraw();
  renderer::DrawIndexedVerticesUP(device, prim_type, min_vertex, num_vertices,
                                  index_count, index_data, index_format,
                                  vertex_data, stride);
  ClearFloatDirty(device);
  FoldBoolPendingIntoPersist(device);
}

// --- command buffers ---
// Nested-recording stack: Avatar_Draw (0x829DFBF0) records its avatar CB while
// the outer per-tile scene CB is being recorded (Begin/End on the same aux
// device, flag 0x10).
struct RecordingFrame {
  uint32_t cb;
  GuestAddr device;
  uint32_t float_baseline[2][256 * 4];
  uint32_t pending_dirty[2][8];
  uint32_t bool_baseline[40];
  uint64_t bool_pending;
  uint32_t fetch_baseline[32 * 6];
};
std::vector<std::unique_ptr<RecordingFrame>> g_recordingStack;

uint32_t BeginCommandBuffer(GuestAddr device, GuestAddr cb, uint32_t flags) {
  if (g_recordingCb) {
    auto frame = std::make_unique<RecordingFrame>();
    frame->cb = g_recordingCb;
    frame->device = g_recordingDevice;
    std::memcpy(frame->float_baseline, g_recordFloatBaseline,
                sizeof(g_recordFloatBaseline));
    std::memcpy(frame->pending_dirty, g_recordPendingDirty,
                sizeof(g_recordPendingDirty));
    std::memcpy(frame->bool_baseline, g_recordBoolBaseline,
                sizeof(g_recordBoolBaseline));
    frame->bool_pending = g_recordBoolPending;
    std::memcpy(frame->fetch_baseline, g_recordFetchBaseline,
                sizeof(g_recordFetchBaseline));
    g_recordingStack.push_back(std::move(frame));
  }
  g_recordingCb = cb;
  g_recordingDevice = device;
  if (device) g_lastRecordingDevice = device;
  g_commandBuffers[cb].clear();
  // Baseline for the recorded draws' dirty-float capture (see CbCall::data).
  if (device) SnapshotRecordFloatBaseline(device);
  return 0;
}

uint32_t EndCommandBuffer(GuestAddr) {
  if (!g_recordingStack.empty()) {
    auto frame = std::move(g_recordingStack.back());
    g_recordingStack.pop_back();
    g_recordingCb = frame->cb;
    g_recordingDevice = frame->device;
    std::memcpy(g_recordFloatBaseline, frame->float_baseline,
                sizeof(g_recordFloatBaseline));
    std::memcpy(g_recordPendingDirty, frame->pending_dirty,
                sizeof(g_recordPendingDirty));
    std::memcpy(g_recordBoolBaseline, frame->bool_baseline,
                sizeof(g_recordBoolBaseline));
    g_recordBoolPending = frame->bool_pending;
    std::memcpy(g_recordFetchBaseline, frame->fetch_baseline,
                sizeof(g_recordFetchBaseline));
  } else {
    g_recordingCb = 0;
    g_recordingDevice = 0;
  }
  return 0;  // S_OK
}

void SetCommandBufferPredication(GuestAddr device, uint32_t tile_pred,
                                 uint32_t run_pred) {
  if (Recording(device)) {
    Record(CbOp::kPredicationMark, {tile_pred, run_pred});
  }
}

// Direct RB_BLENDCONTROL[rt] PM4 emit (see video_native.h): a predication-gated
// span while recording, and live a change to the effective register file that
// leaves the device shadow and dirty bits untouched.
void SetBlendControlDirect(GuestAddr device, uint32_t rt, uint32_t value) {
  if (Recording(device)) {
    Record(CbOp::kSetBlendControlDirect, {rt, value});
    return;
  }
  renderer::ApplyBlendControlDirect(rt, value);
}

void RunCommandBuffer(GuestAddr device, GuestAddr cb,
                      uint32_t predication_select) {
  if (Recording(device)) {
    Record(CbOp::kRunNested, {cb, predication_select});
    return;
  }
  auto it = g_commandBuffers.find(cb);
  if (it == g_commandBuffers.end()) return;
  const bool was_replaying = g_replaying;
  g_replaying = true;

  // XDK Run pre-flush: live register programming issued since the last flush
  // reaches hardware before the CB's PM4, so it is folded into the effective
  // register file first and recorded direct-blend spans override it.
  renderer::ConsumeLiveStateDirty(device);
  // State isolation: the XDK dirties every non-persisted state group after a
  // Run and re-emits device shadows, so replayed binds do not leak.
  renderer::PushState();
  // The device's live float constants are inherited at Run (XDK contract) with
  // recorded in-CB constant sets overlaid as encountered, while nested runs
  // keep the outer replay's accumulated state.
  if (!was_replaying) {
    renderer::SeedReplayFloatConstants(device, g_lastRecordingDevice);
    // Run pre-flush for bool/loop constants: the run device's pending dwords
    // are folded into the persistent file, so bool state from earlier Runs
    // survives and only live writes since the last flush override it.
    FoldBoolPendingIntoPersist(device);
  }
  const GuestAddr saved_vs = g_currentVertexShader;
  const GuestAddr saved_ps = g_currentPixelShader;
  const GuestAddr saved_decl = g_currentVertexDecl;
  uint8_t saved_strides[16];
  std::memcpy(saved_strides, g_streamStrideDwords, sizeof(saved_strides));

  // Xenos bin predication: a recorded SetCommandBufferPredication(tile, run)
  // emits SET_BIN_MASK_LO=tile / SET_BIN_MASK_HI=run ((0,0) resets to
  // LO=~0/HI=0), Run emits SET_BIN_SELECT_HI=PredicationSelect, SELECT_LO is
  // the live SetPredication mask, and a span executes iff (select & mask) != 0.
  const uint32_t select_lo = g_predicationSelectLo;
  uint32_t tile_tag = 0xFFFFFFFFu;  // reset state: mask LO = ~0
  uint32_t run_tag = 0;             // reset state: mask HI = 0
  for (const CbCall& c : it->second) {
    if (c.op == CbOp::kPredicationMark) {
      if (c.a[0] == 0 && c.a[1] == 0) {
        tile_tag = 0xFFFFFFFFu;
        run_tag = 0;
      } else {
        tile_tag = c.a[0];
        run_tag = c.a[1];
      }
      continue;
    }
    if ((tile_tag & select_lo) == 0 &&
        (run_tag & predication_select) == 0) {
      continue;
    }
    // Replayed draws use the render state and bool/loop constants captured at
    // record time from the recording device's shadow block instead of the live
    // main-device shadows.
    if (c.is_draw) {
      // Record-time dirty float constants go first and persist into g_state for
      // the rest of this replay, like the CB's PM4 stream on hardware.
      if (REXCVAR_GET(gpu_cb_const_diffs)) {
        for (size_t i = 0; i + 5 <= c.data.size(); i += 5) {
          renderer::SetShaderConstantsFHost((c.data[i] & 0x10000u) != 0,
                                            c.data[i] & 0xFFFFu,
                                            &c.data[i + 1], 1);
        }
      }
      renderer::SetReplayDrawState(c.st, c.st_dirty);
      // Recorded dirty bool/loop dwords write through to the persistent file,
      // and gpu_cb_bools mode 0 forces the full record-time block
      // instead.
      if (REXCVAR_GET(gpu_cb_bools) == 0) {
        renderer::SetReplayBoolConstants(c.bool_loop);
      } else {
        for (uint32_t d = 0; d < 40; d++) {
          if (c.bool_dirty & (1ull << d)) g_boolPersist[d] = c.bool_loop[d];
        }
        renderer::SetReplayBoolConstants(g_boolPersist);
      }
      renderer::SetReplayFetchConstants(
          c.fetch_data.empty() ? nullptr : c.fetch_data.data(),
          uint32_t(c.fetch_data.size() / 7));
    }
    ReplayCall(device, c);
    if (c.is_draw) {
      renderer::SetReplayDrawState(nullptr, 0);
      renderer::SetReplayBoolConstants(nullptr);
      renderer::SetReplayFetchConstants(nullptr, 0);
    }
  }

  g_currentVertexShader = saved_vs;
  g_currentPixelShader = saved_ps;
  g_currentVertexDecl = saved_decl;
  std::memcpy(g_streamStrideDwords, saved_strides, sizeof(saved_strides));
  renderer::PopState();
  g_replaying = was_replaying;
}

namespace {
void ReplayCall(GuestAddr device, const CbCall& c) {
  switch (c.op) {
    case CbOp::kSetRenderTarget:
      SetRenderTarget(device, c.a[0], c.a[1]);
      break;
    case CbOp::kSetDepthStencilSurface:
      SetDepthStencilSurface(device, c.a[0], 0);
      break;
    case CbOp::kSetSurfaces:
      renderer::SetDepthSurface(c.a[0]);
      for (uint32_t i = 0; i < 4; i++) {
        renderer::SetRenderTargetSurface(i, c.a[1 + i]);
      }
      break;
    case CbOp::kClear: {
      const int32_t rect_vals[4] = {int32_t(c.a[3]), int32_t(c.a[4]),
                                    int32_t(c.a[5]), int32_t(c.a[6])};
      renderer::Clear(device, c.a[0], c.a[1], float(c.f), rect_vals, c.a[2]);
      break;
    }
    case CbOp::kSetViewport: {
      float min_z, max_z;
      std::memcpy(&min_z, &c.a[4], 4);
      std::memcpy(&max_z, &c.a[5], 4);
      renderer::SetViewport(c.a[0], c.a[1], c.a[2], c.a[3], min_z, max_z);
      break;
    }
    case CbOp::kSetScissorRect:
      renderer::SetScissor(int32_t(c.a[0]), int32_t(c.a[1]), int32_t(c.a[2]),
                           int32_t(c.a[3]));
      break;
    case CbOp::kSetTexture:
      SetTexture(device, c.a[0], c.a[1]);
      break;
    case CbOp::kSetStreamSource:
      SetStreamSource(device, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4]);
      break;
    case CbOp::kSetIndices:
      SetIndices(device, c.a[0]);
      break;
    case CbOp::kSetVertexDeclaration:
      SetVertexDeclaration(device, c.a[0]);
      break;
    case CbOp::kSetVertexShader:
      SetVertexShader(device, c.a[0]);
      break;
    case CbOp::kSetPixelShader:
      SetPixelShader(device, c.a[0]);
      break;
    case CbOp::kSetVertexShaderConstantsF:
      renderer::SetShaderConstantsFHost(false, c.a[0], c.data.data(), c.a[1]);
      break;
    case CbOp::kSetPixelShaderConstantsF:
      renderer::SetShaderConstantsFHost(true, c.a[0], c.data.data(), c.a[1]);
      break;
    case CbOp::kResolve:
      ResolveParsed(c.a[0], c.a[1], c.a[2] != 0, int32_t(c.a[3]),
                    int32_t(c.a[4]), int32_t(c.a[5]), int32_t(c.a[6]),
                    int32_t(c.a[7]), int32_t(c.a[8]));
      break;
    case CbOp::kDrawVertices:
      DrawVertices(device, c.a[0], c.a[1], c.a[2]);
      break;
    case CbOp::kDrawIndexedVertices:
      DrawIndexedVertices(device, c.a[0], c.a[1], c.a[2], c.a[3]);
      break;
    case CbOp::kDrawVerticesUP:
      if (!c.up_data.empty()) {
        renderer::SetUPDataOverride(c.up_data.data(), nullptr);
      }
      DrawVerticesUP(device, c.a[0], c.a[1], c.a[2], c.a[3]);
      renderer::SetUPDataOverride(nullptr, nullptr);
      break;
    case CbOp::kDrawIndexedVerticesUP:
      if (!c.up_data.empty()) {
        renderer::SetUPDataOverride(c.up_data.data(),
                                    c.up_data.data() + c.up_index_offset);
      }
      DrawIndexedVerticesUP(device, c.a[0], c.a[1], c.a[2], c.a[3], c.a[4],
                            c.a[5], c.a[6], c.a[7]);
      renderer::SetUPDataOverride(nullptr, nullptr);
      break;
    case CbOp::kRunNested:
      // XDK semantics: state pending on the recording device is flushed into
      // the outer CB before a nested-run token, so the captured authoritative
      // slots are applied to the persistent file for the nested CB's draws to
      // inherit.
      renderer::ApplyReplayStatePersistent(c.st, c.st_dirty);
      RunCommandBuffer(device, c.a[0], c.a[1]);
      break;
    case CbOp::kPredicationMark:
      break;  // handled by the replay loop
    case CbOp::kSetBlendControlDirect:
      // Predication-gated by the replay loop, and persistent in the register
      // file like the PM4 write it models, where the following draws consume
      // it.
      renderer::ApplyBlendControlDirect(c.a[0], c.a[1]);
      break;
  }
}
}  // namespace

}  // namespace rex::videonative
