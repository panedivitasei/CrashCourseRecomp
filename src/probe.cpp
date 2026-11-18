// Bodies of the title's guest hooks (guest_hooks.cpp) and the pad filter: the
// light and shadow fixes, the course-exit guard, the render-to-texture build
// retry, the chicken-out and restart-area fixes, the scripted launch input
// and the "Saving..." loop fix.
#include <cstring>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "generated/cc1/cc1_init.h"
#include "legacy_sounds.h"
#include "spectator.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/hook.h>
#include <video_native.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

// Defined in kernel_hooks.cpp: the pad filter on XamInputGetState and
// synthetic VK_PAD_* presses, which CC1's GUI navigates by.
namespace cc1 {
void SetGamepadFilter(void (*fn)(uint32_t, rex::input::X_INPUT_GAMEPAD*));
void InjectKeystroke(uint16_t vk);
}

static uint32_t LoadGuestU32(uint8_t* base, uint32_t ea) {
  uint32_t raw;
  std::memcpy(&raw, base + ea, 4);
  return __builtin_bswap32(raw);
}

// Guarded guest accessors, defined with the force-selection block below.
static bool HostReadable(const void* p, size_t n);
static uint32_t GR32(uint32_t ea);

REXCVAR_DEFINE_BOOL(cc1_force_shadow_rtt, false, "GPU",
                    "Forces the shadow driver's RTT type check from 1 to 11, "
                    "which aborts the shadow phase.");
REXCVAR_DEFINE_BOOL(cc1_light_mask_force, true, "GPU",
                    "Marks every render object as lit and as a caster for "
                    "light cameras by setting the caster-bitmask fill byte "
                    "(cam+2140) to 0xFF before the mask is built; off keeps "
                    "the game's own gating.");

// CAMERA::UpdateMatrices entry (base 0x82270F28 / TU1 0x8294C7C0),
// observe-only: r3 is the camera and +1400 its mode (2/3 shadow-directional,
// 5/6 other light cameras).
void CC1_LightCamFillForce(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cc1_light_mask_force)) return;
  const uint32_t cam = ctx.r3.u32;
  if (!cam) return;
  const uint32_t mode =
      __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + cam + 1400));
  if (mode == 3 || mode == 5 || mode == 6) {
    *reinterpret_cast<uint8_t*>(base + cam + 2140) = 0xFF;
  }
}

// Hooks the manifest names that do nothing here: guest_hooks.cpp and the
// generated code call them.
void CC1_StartTexRenderFailA(PPCContext&, uint8_t*) {}
void CC1_StartTexRenderFailB(PPCContext&, uint8_t*) {}
void CC1_StartTexRenderFailC(PPCContext&, uint8_t*) {}
void CC1_PassLoopTop(PPCContext&, uint8_t*, PPCRegister&) {}
void CC1_PassGateVerdict(PPCContext&, uint8_t*, PPCRegister&) {}
void CC1_RenderPassedProbe(PPCContext&, uint8_t*) {}
void CC1_ShadowBeginProbe(PPCContext&, uint8_t*) {}
void CC1_ShadowCasterDrawProbe(PPCContext&, uint8_t*) {}
void CC1_ShadowEndProbe(PPCContext&, uint8_t*) {}

// Shadow driver gate (TU1 sub_828B9298, SCENEXA::RenderShadowMaps): the
// midasm at 0x828B9410 has the RTT texture type in r3, where type 11 (map not
// created) aborts the whole phase and type 1 is the healthy in-level value.

REXCVAR_DEFINE_BOOL(cc1_shadow_rtt_note, false, "CC1",
                    "Serves the shadow bracket's host render target from the "
                    "engine RTT note instead of the viewport-growth path.");

void CC1_ShadowGateProbe(PPCContext& ctx, uint8_t* base) {
  const uint32_t shadow_tb =
      __builtin_bswap32(*reinterpret_cast<uint32_t*>(base + ctx.r1.u32 + 100));
  // At this site [r1+100] is the shadow map's TEXTUREBASEDX9 and r3 its kind,
  // and only the 2D map is noted (kind 11, the R32F cube, is the dir-light
  // abort path), off by default because the bracket's host RT needs the pitch
  // width.
  if (REXCVAR_GET(cc1_shadow_rtt_note) && rex::videonative::IsActive() &&
      shadow_tb && ctx.r3.u32 != 11) {
    const uint32_t w = GR32(shadow_tb + 8), h = GR32(shadow_tb + 12);
    if (w >= 16 && w <= 4096 && h >= 16 && h <= 4096) {
      rex::videonative::NoteRttBegin(
          shadow_tb, w, h, GR32(shadow_tb + 44), GR32(shadow_tb + 72),
          GR32(shadow_tb + 28), GR32(shadow_tb + 96), GR32(shadow_tb + 84),
          GR32(shadow_tb + 88));
    }
  }
  // cc1_force_shadow_rtt forces 1 -> 11, the abort-all path, as a diagnostic
  // that disables the shadow phase.
  if (REXCVAR_GET(cc1_force_shadow_rtt) && ctx.r3.u32 == 1) {
    ctx.r3.u64 = 11;
  }
}

void CC1_CasterMaskForce(PPCContext& ctx, uint8_t* base) {
  const uint32_t cam = ctx.r3.u32;
  if (!cam) return;
  if (REXCVAR_GET(cc1_light_mask_force)) {
    *reinterpret_cast<uint8_t*>(base + cam + 2140) = 0xFF;
  }
}

// ---- course-exit crash guard -----------------------------------------
// Midasm at 0x82269778 in the render-object bucket remove (sub_82269450,
// TU1): a node with null links (r11/r10) faults on the unconditional
// unlink, so both links are pointed at the node's own pair (r9) and the
// guest's own sanity branch skips the unlink.
REXCVAR_DEFINE_BOOL(cc1_unlink_guard, true, "CC1",
                    "Guards the render-object bucket remove against "
                    "null-linked nodes, which fault on course exit.");

void CC1_UnlinkGuard(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (!REXCVAR_GET(cc1_unlink_guard)) return;
  if (ctx.r11.u32 == 0 || ctx.r10.u32 == 0) {
    static std::atomic<int> guard_logs{12};
    if (guard_logs.fetch_sub(1, std::memory_order_relaxed) > 0) {
      REXKRNL_INFO(
          "[unlinkguard] null-linked node pair={:#x} r10={:#x} r11={:#x} "
          "lr={:#x}",
          ctx.r9.u32, ctx.r10.u32, ctx.r11.u32, uint32_t(ctx.lr));
    }
    ctx.r10.u64 = ctx.r9.u64;
    ctx.r11.u64 = ctx.r9.u64;
  }
}

REXCVAR_DEFINE_BOOL(cc1_chicken_keeps_timer, false, "CC1",
                    "Keeps the run timer running after a chicken-out (RB "
                    "skip) by blocking the chickened script event.")
    .debug_only();

// ---- RTT build retry ---------------------------------------------------
// The game builds each render-to-texture surface once at level load and
// fails silently when the named RENDERTEXTURE actor has not spawned yet, so
// each attempt's TEXTUREMASTER is recorded and the game's own build (TU1
// sub_82930F48) is re-invoked from the per-frame hook until it succeeds.
REXCVAR_DEFINE_BOOL(cc1_rtt_build_retry, true, "CC1",
                    "Retries failed render-to-texture builds once their "
                    "target actor exists.");

struct BrtRetryEntry {
  uint32_t texmaster;
  uint32_t attempts;
  std::chrono::steady_clock::time_point next_try;
};
static std::mutex g_brt_mutex;
static std::vector<BrtRetryEntry> g_brt_retry;
static thread_local bool g_brt_in_pump = false;  // nested-call re-entry guard

// Entry of TU1 TEXTUREMASTER::BuildRenderToTexture: records every attempt so
// the pump can retry any whose built-interface field (+136, set only on
// success) is still zero.
void CC1_BrtEntryProbe(PPCContext& ctx, uint8_t* base) {
  (void)base;
  if (g_brt_in_pump) return;  // the pump's retry call re-entering the hook
  const uint32_t tm = ctx.r3.u32;
  if (!REXCVAR_GET(cc1_rtt_build_retry) || !tm) return;
  std::lock_guard<std::mutex> lk(g_brt_mutex);
  for (auto& e : g_brt_retry) {
    if (e.texmaster == tm) return;
  }
  if (g_brt_retry.size() < 24) {
    g_brt_retry.push_back(
        {tm, 0, std::chrono::steady_clock::now() + std::chrono::seconds(2)});
  }
}

void CC1_StartTexRenderProbe(PPCContext& ctx, uint8_t* base) {
  // Retry pump: runs per frame on the guest render thread, the only safe
  // context for calling generated code, with a full context save/restore
  // around the nested call.
  if (REXCVAR_GET(cc1_rtt_build_retry)) {
    uint32_t tm = 0;
    {
      std::lock_guard<std::mutex> lk(g_brt_mutex);
      const auto now = std::chrono::steady_clock::now();
      for (auto it = g_brt_retry.begin(); it != g_brt_retry.end();) {
        // Built (interface at +136 nonzero), so dropped silently.
        if (LoadGuestU32(base, it->texmaster + 136) != 0) {
          it = g_brt_retry.erase(it);
          continue;
        }
        if (now >= it->next_try) {
          tm = it->texmaster;
          if (++it->attempts >= 30) {
            REXLOG_INFO("[brtheal] tm={:08X} giving up after 30 attempts",
                        tm);
            g_brt_retry.erase(it);
          } else {
            it->next_try = now + std::chrono::seconds(1);
          }
          break;
        }
        ++it;
      }
    }
    if (tm) {
      PPCContext saved = ctx;
      g_brt_in_pump = true;
      ctx.r3.u64 = tm;
      sub_82930F48(ctx, base);  // TEXTUREMASTER::BuildRenderToTexture (TU1)
      g_brt_in_pump = false;
      ctx = saved;
      const uint32_t built = LoadGuestU32(base, tm + 136);
      if (built) {
        REXLOG_INFO("[brtheal] tm={:08X} rebuilt (iface={:08X})", tm,
                    built);
      }
    }
  }
  // Engine-authoritative RTT note: reports the built TEXTUREBASEDX9 this pass
  // is about to render into (+8/+12 dims, +44/+72 EDRAM color/depth surfaces,
  // +28 resolve dest, +96 format, +84 MSAA, +88 tiling config).
  if (rex::videonative::IsActive()) {
    const uint32_t tm_note = ctx.r4.u32;
    if (tm_note) {
      // Target dims are at tm+96/+100 and tm+136 is an 8-byte registry handle
      // that sub_829715A0 resolves to the backing rtt master, whose +136 is
      // the built TEXTUREBASEDX9 (anim fallback +140/+144).
      const uint32_t w = GR32(tm_note + 96);
      const uint32_t h = GR32(tm_note + 100);
      PPCContext saved_note = ctx;
      ctx.r3.u64 =
          (uint64_t(GR32(tm_note + 136)) << 32) | GR32(tm_note + 140);
      sub_829715A0(ctx, base);
      const uint32_t master = ctx.r3.u32;
      ctx = saved_note;
      uint32_t tb = 0;
      if (master) {
        tb = GR32(master + 136);
        if (!tb) {
          const uint32_t frames = GR32(master + 140);
          if (frames) tb = GR32(frames + 4 * GR32(master + 144));
        }
      }
      // TEXTUREBASEDX9 TU1 field map from its Bind body (sub_8287EB08): +44
      // goes to the SetRenderTarget thunk 0x82247D40, +72 to
      // SetDepthStencilSurface 0x82247A68 and +88 tiling to BeginTiling
      // 0x8225B4B8, with the viewport from +8/+12.
      if (tb && w >= 16 && w <= 4096 && h >= 16 && h <= 4096) {
        rex::videonative::NoteRttBegin(tb, w, h, GR32(tb + 44),
                                       GR32(tb + 72), GR32(tb + 28),
                                       GR32(tb + 96), GR32(tb + 84),
                                       GR32(tb + 88));
      }
    }
  }
}

// Entry of sub_821B41E8, which sets the per-run chickened flag (race-slot
// byte +57) that cancels the HUD timer: the chicken executor's call (return
// address 0x829A5644) has its argument forced to false so the run keeps its
// timer.
void CC1_ChickenFlagKeep(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cc1_chicken_keeps_timer)) {
    return;
  }
  if (ctx.r4.u32 != 0 && uint32_t(ctx.lr) == 0x829A5644) {
    ctx.r4.u64 = 0;
  }
}

// Entry of sub_8248C870 (script event dispatch): when the chicken-out poster
// sub_829C9BC8 calls it (return address 0x829C9C44) with the toggle on, the
// record's channel is set to 255 so event 132 never reaches the script.
void CC1_ScriptEventFilter(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(cc1_chicken_keeps_timer)) {
    return;
  }
  if (uint32_t(ctx.lr) != 0x829C9C44) {
    return;
  }
  const uint32_t rec = ctx.r3.u32;
  const uint32_t bad = __builtin_bswap32(255);
  std::memcpy(base + rec, &bad, 4);
}

// The action-35 block in sub_829AB7C0 is the chicken-out handler, whose
// action state asserts spuriously in this runtime, so the branch is gated on
// the physical RB state captured from the pad each poll.
REXCVAR_DEFINE_BOOL(cc1_fix_chickenout, true, "CC1",
                    "Gates the chicken-out branch on the physical RB button.");

// The game binds physical Y to the same action (35) as chicken-out, so a Y
// press edge invokes the game's own forced respawn at the last checkpoint,
// FIGHTER::Respawn(this, 0, 1).
REXCVAR_DEFINE_BOOL(cc1_y_restart_area, true, "CC1",
                    "Physical Y restarts the area with a forced respawn at "
                    "the last checkpoint.");

static std::atomic<uint16_t> g_pad_buttons{0};

// ---- headless auto-launch ----
// Scripted pad injection at the XamInputGetState filter from a
// comma-separated token list: wN waits N polls, a button name (start back a
// b x y up down left right lb rb) presses it and force stamps the course.
REXCVAR_DEFINE_BOOL(cc1_auto_launch, false, "CC1",
                    "Injects the pad sequence in cc1_auto_launch_seq once at "
                    "boot.");
REXCVAR_DEFINE_STRING(cc1_auto_launch_seq, "w1800,start,w300,a", "CC1",
                      "Auto-launch token list: wN waits N polls and a button "
                      "name presses that button.");
REXCVAR_DEFINE_INT32(cc1_auto_launch_hold, 6, "CC1",
                     "Polls each auto-launch press is held.");
REXCVAR_DEFINE_INT32(cc1_auto_launch_gap, 60, "CC1",
                     "Idle polls after each auto-launch press.");
REXCVAR_DEFINE_INT32(cc1_force_track_id, -1, "CC1",
                     "Course id the auto-launch 'force' token stamps into "
                     "the level-config selection (-1 = the Japan course with "
                     "the lowest id).");

namespace {
struct AlToken {
  uint16_t bit;    // 0 = wait token
  uint32_t polls;  // wait length (wait) or per-token hold override (press)
  const char* name;
};

uint16_t AlButtonBit(const std::string& t, const char** name) {
  static constexpr struct { const char* n; uint16_t bit; } kMap[] = {
      {"up", 0x0001},   {"down", 0x0002}, {"left", 0x0004}, {"right", 0x0008},
      {"start", 0x0010}, {"back", 0x0020}, {"lb", 0x0100},  {"rb", 0x0200},
      {"a", 0x1000},    {"b", 0x2000},    {"x", 0x4000},    {"y", 0x8000},
  };
  for (const auto& m : kMap) {
    if (t == m.n) {
      *name = m.n;
      return m.bit;
    }
  }
  *name = "?";
  return 0;
}

// Appends the tokens of a comma-separated list to 's'.
void AlParse(const std::string& raw, std::vector<AlToken>& s) {
    size_t i = 0;
    while (i < raw.size()) {
      size_t j = raw.find(',', i);
      if (j == std::string::npos) j = raw.size();
      std::string tok = raw.substr(i, j - i);
      i = j + 1;
      while (!tok.empty() && tok.front() == ' ') tok.erase(tok.begin());
      while (!tok.empty() && tok.back() == ' ') tok.pop_back();
      if (tok.empty()) continue;
      if (tok == "force") {
        s.push_back({0, 0, "force"});
      } else if (tok[0] == 'w' && tok.size() > 1 &&
          tok.find_first_not_of("0123456789", 1) == std::string::npos) {
        s.push_back({0, uint32_t(std::stoul(tok.substr(1))), "wait"});
      } else {
        // press token, optional per-token hold: "right:45"
        uint32_t hold_override = 0;
        size_t colon = tok.find(':');
        if (colon != std::string::npos) {
          const std::string h = tok.substr(colon + 1);
          if (!h.empty() &&
              h.find_first_not_of("0123456789") == std::string::npos) {
            hold_override = uint32_t(std::stoul(h));
          }
          tok = tok.substr(0, colon);
        }
        const char* nm = nullptr;
        uint16_t bit = AlButtonBit(tok, &nm);
        if (bit) {
          s.push_back({bit, hold_override, nm});
        } else {
          REXLOG_WARN("[cc1launch] unknown token '{}' skipped", tok);
        }
      }
    }
}

// Parsed once on first poll, when the cvars from toml and CLI are final.
std::vector<AlToken>& AlSequence() {
  static std::vector<AlToken> seq = [] {
    std::vector<AlToken> s;
    AlParse(REXCVAR_GET(cc1_auto_launch_seq), s);
    return s;
  }();
  return seq;
}

// More input for a running session: once the sequence is done, a token list
// dropped into cc1_input.txt beside the exe is appended and the file removed.
bool AlReadDropFile(std::vector<AlToken>& seq) {
  static uint32_t polls = 0;
  if (++polls % 30) return false;
  const auto path = rex::filesystem::GetExecutableFolder() / "cc1_input.txt";
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) return false;
  std::string raw;
  {
    std::ifstream in(path);
    std::getline(in, raw);
  }
  std::filesystem::remove(path, ec);
  if (ec) return false;  // still being written: try again later
  while (!raw.empty() && std::isspace(static_cast<unsigned char>(raw.back()))) {
    raw.pop_back();
  }
  const size_t before = seq.size();
  AlParse(raw, seq);
  REXLOG_INFO("[cc1launch] cc1_input.txt: {} more token(s)", seq.size() - before);
  return seq.size() > before;
}
}  // namespace

// ---- 'force' token: stamp the level-config selection directly ----
// Replicates KERNELCOMMANDSXA CMD_SetLvlConfTrack (base 0x821A35D8) with
// plain catalog data writes: id (+52), flat index (+56), index in world
// (+60), world (+64), selection type (+236, +240) and playlist mask (+244).
static bool HostReadable(const void* p, size_t n) {
  // Guest holes are unmapped host memory, so raw reads are
  // VirtualQuery-guarded, with a one-entry region cache that keeps a bulk
  // scan to one query per committed region.
  static const uint8_t* s_lo = nullptr;
  static const uint8_t* s_hi = nullptr;
  static bool s_ok = false;
  const uint8_t* q = static_cast<const uint8_t*>(p);
  if (q >= s_lo && q + n <= s_hi) return s_ok;
  MEMORY_BASIC_INFORMATION mbi;
  if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
  s_lo = static_cast<const uint8_t*>(mbi.BaseAddress);
  s_hi = s_lo + mbi.RegionSize;
  s_ok = mbi.State == MEM_COMMIT &&
         !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
  return s_ok && q + n <= s_hi;
}
static uint32_t GR32(uint32_t ea) {
  if (ea < 0x1000u || ea >= 0xC0000000u) return 0u;
  const uint32_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint32_t*>(ea);
  if (!p || !HostReadable(p, 4)) return 0u;
  return __builtin_bswap32(*p);
}
static uint8_t GR8(uint32_t ea) {
  if (ea < 0x1000u || ea >= 0xC0000000u) return 0u;
  const uint8_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<const uint8_t*>(ea);
  if (!p || !HostReadable(p, 1)) return 0u;
  return *p;
}
static void GW32(uint32_t ea, uint32_t v) {
  uint32_t* p = REX_KERNEL_MEMORY()->TranslateVirtual<uint32_t*>(ea);
  if (p && HostReadable(p, 4)) *p = __builtin_bswap32(v);
}
static bool GStrHas(uint32_t ea, const char* needle) {
  if (ea < 0x1000u || ea >= 0xC0000000u) return false;
  const char* p = REX_KERNEL_MEMORY()->TranslateVirtual<const char*>(ea);
  if (!p || !HostReadable(p, 64)) return false;
  char buf[64];
  int n = 0;
  for (; n < 63; n++) {
    const char c = p[n];
    if (!c) break;
    if (static_cast<unsigned char>(c) < 0x20 ||
        static_cast<unsigned char>(c) > 0x7E) {
      return false;
    }
    buf[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  buf[n] = 0;
  return n > 0 && std::strstr(buf, needle) != nullptr;
}

// The catalog global moved in TU1 (base dword_82B55084), so it is resolved at
// runtime by structure: a .data pointer whose pointee has a flat course list
// at +68/+72 with a plausible count and sane course id (+96) and world (+108)
// fields.
static uint32_t CC1_FindCatalog(uint32_t* out_global) {
  auto validate = [](uint32_t c) -> bool {
    if (c < 0x100000u || c >= 0xC0000000u) return false;
    const uint32_t fb = GR32(c + 68), fe = GR32(c + 72);
    if (!fb || fe < fb || ((fb | fe) & 3)) return false;
    const uint32_t n = (fe - fb) >> 2;
    if (n < 10 || n > 40) return false;
    for (uint32_t i = 0; i < 3; i++) {
      const uint32_t course = GR32(fb + 4 * i);
      if (course < 0x100000u || course >= 0xC0000000u) return false;
      if (GR32(course + 96) > 100000u) return false;
      if (GR32(course + 108) > 7u) return false;
    }
    const uint32_t wb = GR32(c + 84), we = GR32(c + 88);
    if (we < wb || ((we - wb) >> 2) > 10) return false;
    return true;
  };
  static const struct { uint32_t lo, hi; } kWindows[] = {
      {0x82B40000u, 0x82C80000u},  // base global 0x82B55084 + TU1 drift
      {0x82A00000u, 0x82B40000u},
      {0x82C80000u, 0x82E00000u},
  };
  for (const auto& w : kWindows) {
    for (uint32_t g = w.lo; g < w.hi; g += 4) {
      const uint32_t c = GR32(g);
      if (c < 0x100000u || c >= 0xC0000000u) continue;
      if (validate(c)) {
        *out_global = g;
        return c;
      }
    }
  }
  return 0;
}

static void CC1_ForceTrackSelection() {
  uint32_t gaddr = 0;
  const uint32_t cat = CC1_FindCatalog(&gaddr);
  if (!cat) {
    REXLOG_WARN("[cc1launch] force: catalog singleton not found/invalid");
    return;
  }
  const uint32_t fb = GR32(cat + 68), fe = GR32(cat + 72);
  const uint32_t count = (fe - fb) >> 2;
  const int target = REXCVAR_GET(cc1_force_track_id);
  uint32_t best_id = 0xFFFFFFFFu, best_idx = 0, best_world = 0;
  uint32_t w2_id = 0xFFFFFFFFu, w2_idx = 0;  // fallback: world 2 (Japan slot)
  for (uint32_t i = 0; i < count; i++) {
    const uint32_t course = GR32(fb + 4 * i);
    if (!course) continue;
    const uint32_t id = GR32(course + 96);
    const uint32_t world = GR32(course + 108);
    bool jap = false;
    uint32_t jat = 0;
    for (uint32_t off = 0; off <= 252; off += 4) {
      if (off == 96 || off == 108) continue;
      if (GStrHas(GR32(course + off), "japan")) {
        jap = true;
        jat = off;
        break;
      }
    }
    REXLOG_INFO("[cc1launch] course[{:2}] @{:08X} id={:<4} world={} japstr={}{}",
                i, course, id, world, (int)jap,
                jap ? (" @+" + std::to_string(jat)) : std::string());
    if (target >= 0) {
      if (id == uint32_t(target)) {
        best_id = id;
        best_idx = i;
        best_world = world;
      }
    } else if (jap && id < best_id) {
      best_id = id;
      best_idx = i;
      best_world = world;
    }
    if (world == 2 && id < w2_id) {
      w2_id = id;
      w2_idx = i;
    }
  }
  if (best_id == 0xFFFFFFFFu && target < 0 && w2_id != 0xFFFFFFFFu) {
    REXLOG_INFO("[cc1launch] force: no japan-string course; falling back to "
                "world 2 first course (catalog order USA/EU/JAPAN)");
    best_id = w2_id;
    best_idx = w2_idx;
    best_world = 2;
  }
  if (best_id == 0xFFFFFFFFu) {
    REXLOG_WARN("[cc1launch] force: no target course resolved");
    return;
  }
  // index within the world list (cat+84+16*w vector), as sub_821CFB08 does
  uint32_t widx = 0;
  const uint32_t wb = GR32(cat + 84 + 16 * best_world);
  const uint32_t we = GR32(cat + 88 + 16 * best_world);
  const uint32_t wn = we >= wb ? ((we - wb) >> 2) : 0;
  for (uint32_t j = 0; j < wn; j++) {
    if (GR32(GR32(wb + 4 * j) + 96) == best_id) {
      widx = j;
      break;
    }
  }
  GW32(cat + 52, best_id);
  GW32(cat + 56, best_idx);
  GW32(cat + 60, widx);
  GW32(cat + 64, best_world);
  GW32(cat + 236, 0);  // selection type SINGLE
  GW32(cat + 240, 0);
  if (GR8(cat + 232)) GW32(cat + 244, 1);  // playlist mask (sub_821D0170)
  REXLOG_INFO(
      "[cc1launch] force: stamped id={} flat={} world={} widx={} "
      "(catalog @{:08X} via {:08X})",
      best_id, best_idx, best_world, widx, cat, gaddr);
}

static void CC1_AutoLaunchInject(rex::input::X_INPUT_GAMEPAD* pad) {
  if (!REXCVAR_GET(cc1_auto_launch)) return;
  auto& seq = AlSequence();
  static size_t s_idx = 0;
  static uint32_t s_phase = 0;  // polls spent inside the current token
  static bool s_done_logged = false;
  if (s_idx >= seq.size()) {
    if (!s_done_logged) {
      s_done_logged = true;
      REXLOG_INFO("[cc1launch] sequence complete ({} tokens)", seq.size());
    }
    if (!AlReadDropFile(seq)) return;
    s_done_logged = false;
    s_phase = 0;
  }
  const AlToken& t = seq[s_idx];
  if (!t.bit && t.polls == 0) {  // 'force' token: stamp selection, advance
    REXLOG_INFO("[cc1launch] token {}/{}: force", s_idx + 1, seq.size());
    CC1_ForceTrackSelection();
    s_phase = 0;
    ++s_idx;
    return;
  }
  const uint32_t hold =
      t.bit && t.polls
          ? t.polls
          : uint32_t(std::max(1, REXCVAR_GET(cc1_auto_launch_hold)));
  const uint32_t gap = uint32_t(std::max(1, REXCVAR_GET(cc1_auto_launch_gap)));
  const uint32_t span = t.bit ? (hold + gap) : std::max(1u, t.polls);
  if (s_phase == 0) {
    REXLOG_INFO("[cc1launch] token {}/{}: {} {}", s_idx + 1, seq.size(),
                t.name, t.bit ? hold : t.polls);
    if (t.bit) {
      // The CC1 GUI reads VK_PAD_* keystrokes (XamInputGetKeystroke), not
      // GetState, so the matching keystroke is injected on the press edge,
      // plus the left-thumb keystroke for directions.
      static constexpr struct { uint16_t bit; uint16_t vk; uint16_t vk2; } kVk[] = {
          {0x0001, 0x5810, 0x5820}, {0x0002, 0x5811, 0x5821},
          {0x0004, 0x5812, 0x5823}, {0x0008, 0x5813, 0x5822},
          {0x0010, 0x5814, 0},      {0x0020, 0x5815, 0},
          {0x0100, 0x5805, 0},      {0x0200, 0x5804, 0},
          {0x1000, 0x5800, 0},      {0x2000, 0x5801, 0},
          {0x4000, 0x5802, 0},      {0x8000, 0x5803, 0},
      };
      for (const auto& m : kVk) {
        if (m.bit == t.bit) {
          cc1::InjectKeystroke(m.vk);
          if (m.vk2) cc1::InjectKeystroke(m.vk2);
          break;
        }
      }
    }
  }
  if (t.bit && s_phase < hold) {
    pad->buttons = uint16_t(uint16_t(pad->buttons) | t.bit);
    // Directions also drive the left stick for analog readers.
    if (t.bit == 0x0004) pad->thumb_lx = -30000;
    if (t.bit == 0x0008) pad->thumb_lx = 30000;
    if (t.bit == 0x0001) pad->thumb_ly = 30000;
    if (t.bit == 0x0002) pad->thumb_ly = -30000;
  }
  if (++s_phase >= span) {
    s_phase = 0;
    ++s_idx;
  }
}

bool CC1_SaveInputBlocked();  // defined below: the save popup owns A

static void CC1_PadCapture(uint32_t user_index, rex::input::X_INPUT_GAMEPAD* pad) {
  if (pad && CC1_SaveInputBlocked()) pad->buttons = uint16_t(pad->buttons) & ~uint16_t(0x1000);  // A
  if (user_index == 0 && pad) {
    CC1_AutoLaunchInject(pad);
    g_pad_buttons.store(uint16_t(pad->buttons), std::memory_order_relaxed);
  }
}

static void CC1_EnsurePadCapture() {
  static std::atomic<bool> registered{false};
  bool expected = false;
  if (registered.compare_exchange_strong(expected, true)) {
    cc1::SetGamepadFilter(&CC1_PadCapture);
  }
}

// Registers at exe start so the filter is live from the title screen, since
// the in-level CC1_EnsurePadCapture call is too late for boot navigation.
static const bool g_cc1_pad_filter_boot = [] {
  cc1::SetGamepadFilter(&CC1_PadCapture);
  return true;
}();

// At 0x829AB900, right after the chicken-out action-state query
// (sub_8295EE98(input, 35)) inside sub_829AB7C0, with r31 the FIGHTER passed
// explicitly.
void CC1_SkipDemoFence(PPCContext& ctx, uint8_t* base, PPCRegister& r31) {
  CC1_EnsurePadCapture();
  if (cc1::spectator::PuppetStep()) {  // a racer's step replayed: this machine's RB and Y are not the racer's
    ctx.r3.u64 = 0;
    return;
  }
  if (!REXCVAR_GET(cc1_fix_chickenout)) {
    return;
  }
  static std::atomic<bool> y_was_down{false};
  constexpr uint16_t kRightShoulder = 0x0200;  // XINPUT_GAMEPAD_RIGHT_SHOULDER
  constexpr uint16_t kYButton = 0x8000;        // XINPUT_GAMEPAD_Y
  const uint16_t buttons = g_pad_buttons.load(std::memory_order_relaxed);
  const bool rb_held = (buttons & kRightShoulder) != 0;
  const bool y_held = (buttons & kYButton) != 0;
  // Y edge with no cooldown: re-fires as fast as press edges are detected.
  if (y_held && !y_was_down.exchange(true)) {
    if (REXCVAR_GET(cc1_y_restart_area)) {
      // this->Respawn(0, 1) is a plain forced respawn at the last checkpoint,
      // safe here because the game calls Respawn from this spot (0x829AB9B4)
      // and the code after the hook reloads everything it uses.
      const uint32_t fighter = r31.u32;
      ctx.r3.u64 = fighter;
      ctx.r4.u64 = 0;
      ctx.r5.u64 = 1;
      sub_829A3740(ctx, base);
      ctx.r3.u64 = 0;  // suppress the fence branch this frame (Y wins over RB)
      return;
    }
  } else if (!y_held) {
    y_was_down.store(false);
  }
  if (!rb_held) {
    ctx.r3.u64 = 0;  // RB not physically held -> chicken-out branch skipped
  }
}

// Entry of the pak registrar sub_829E37E8 (AddPack): the path of each pak the
// game registers, for the legacy sounds.
void CC1_AddPackProbe(PPCContext& ctx, uint8_t* base) {
  const uint32_t path_ptr = ctx.r3.u32;
  char buf[128] = {};
  if (path_ptr) {
    std::memcpy(buf, base + path_ptr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
  }
  cc1::legacy_sounds::NotePack(buf);
}

// ---- "Saving..." loop fix ---------------------------------------------
// The profile write finishes in milliseconds, so the results screen takes
// the next A press and starts another save (sub_829DFD20 -> sub_8297BC38 ->
// sub_82988308); A is dropped for cc1_save_input_block_ms after each save.
REXCVAR_DEFINE_INT32(cc1_save_input_block_ms, 3500, "CC1",
                     "Milliseconds for which A presses are ignored after a "
                     "profile save starts (0 = off).");

static std::atomic<uint64_t> g_save_block_until_ms{0};

static uint64_t CC1_NowMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

bool CC1_SaveInputBlocked() {
  return CC1_NowMs() < g_save_block_until_ms.load(std::memory_order_relaxed);
}

// Called by the XamInputGetKeystroke(Ex) overrides in kernel_hooks.cpp: true
// drops the keystroke (A down while the Saving popup owns input).
bool CC1_KeystrokeFilter(uint32_t /*user*/, uint16_t vk, uint16_t flags) {
  constexpr uint16_t kVkPadA = 0x5800, kKeyUp = 0x0002;
  if (vk != kVkPadA || (flags & kKeyUp) || !CC1_SaveInputBlocked()) return false;
  return true;
}

REX_EXTERN(__imp__sub_8297BC38);
REX_EXTERN(sub_8297BC38) {  // NETPLAYERPROFILEXA: save + broadcast
  const int32_t block = REXCVAR_GET(cc1_save_input_block_ms);
  if (block > 0)
    g_save_block_until_ms.store(CC1_NowMs() + uint64_t(block), std::memory_order_relaxed);
  __imp__sub_8297BC38(ctx, base);
}
