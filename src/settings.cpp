// CC1's display settings on the game's own Help & Options > Settings page, below Sound FX, Music and Vibration:
// applied as they change, written to crashcourse1.toml on Apply Changes and reverted when the page is left
// without applying.

#include "settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include "game_lua.h"
#include "settings_menu_lua.h"
#include "zoom.h"

REXCVAR_DECLARE(int32_t, gpu_resolution_scale);  // src/gpu/renderer.cpp
REXCVAR_DECLARE(int32_t, gpu_fps_cap);       // src/gpu/renderer.cpp
REXCVAR_DECLARE(bool, gpu_fps);         // src/gpu/renderer_fps.cpp
REXCVAR_DECLARE(bool, gpu_vsync);       // src/gpu/renderer.cpp
REXCVAR_DECLARE(int32_t, cc1_zoom);              // src/zoom.cpp
REXCVAR_DECLARE(bool, cc1_legacy_sounds);        // src/legacy_sounds.cpp
REXCVAR_DEFINE_INT32(cc1_window_width, 0, "Display",
                     "Windowed-mode width in pixels; 0 leaves the default window");
REXCVAR_DEFINE_INT32(cc1_window_height, 0, "Display",
                     "Windowed-mode height in pixels; 0 leaves the default window");

namespace cc1::settings {
namespace {

// ---- The settings -------------------------------------------------------------

constexpr std::array<std::array<int, 2>, 11> kSizes{{{640, 480},   {800, 600},   {1024, 768},
                                                     {1280, 800},  {1280, 720},  {1600, 900},
                                                     {1680, 1050}, {1920, 1080}, {1920, 1200},
                                                     {2560, 1440}, {2560, 1600}}};
constexpr std::array<int, 8> kLimits{30, 60, 90, 120, 144, 165, 240, 0};  // 0 = no cap

struct Settings {
  int scale = 100;  // percent, 50-200 in steps of 25
  int size = 4;     // index into kSizes
  int fullscreen = 0;
  int limit = 1;  // index into kLimits
  int vsync = 1;
  int fps = 0;
  int legacy = 0;  // the sounds the game shipped with (legacy_sounds.cpp)
  int zoom = 0;  // steps of 20%, closer when positive
  bool operator==(const Settings&) const = default;
};

Settings g_current, g_opened;
bool g_zoom_row = false;  // the page was opened where Zoom applies: it has the row
int g_boot_scale = 100;  // the scale the renderer latched at launch
int g_boot_legacy = 0;   // the sounds the packs were registered with at launch
std::atomic<rex::ui::Window*> g_window{nullptr};
std::filesystem::path g_config_path;

// Pointer drag state for a bar; game thread only.
std::atomic<bool> g_page_open{false};
int g_drag_slot = 0;
float g_drag_fraction = 0.0f;
bool g_drag_asked = false, g_drag_ended = false;
bool g_drag_window = false;  // the drag changed the window setting, applied when the bar is let go

// Line spacing factor for each row's label, relative to its own (mouse.cpp sets it); game thread only.
float g_row_spacing[3] = {1.0f, 1.0f, 1.0f};
bool g_spacing_asked = false, g_spacing_set = false;

void Clamp(Settings& s) {
  s.scale = std::clamp(s.scale / 25 * 25, 50, 200);
  s.size = std::clamp(s.size, 0, int(kSizes.size()) - 1);
  s.limit = std::clamp(s.limit, 0, int(kLimits.size()) - 1);
  s.fullscreen = !!s.fullscreen;
  s.vsync = !!s.vsync;
  s.fps = !!s.fps;
  s.legacy = !!s.legacy;
  s.zoom = std::clamp(s.zoom, -zoom::kSteps, zoom::kSteps);
}

// Puts the current settings into effect, where the resolution scale and the legacy sounds only reach their
// cvars because the renderer sizes its surfaces and the game registers its packs at launch.
void Publish(bool change_window) {
  Clamp(g_current);
  rex::cvar::SetFlagByName("gpu_resolution_scale", std::to_string(g_current.scale));
  rex::cvar::SetFlagByName("gpu_fps_cap", std::to_string(kLimits[g_current.limit]));
  rex::cvar::SetFlagByName("gpu_vsync", g_current.vsync ? "true" : "false");
  rex::cvar::SetFlagByName("gpu_fps", g_current.fps ? "true" : "false");
  rex::cvar::SetFlagByName("cc1_zoom", std::to_string(g_current.zoom));
  rex::cvar::SetFlagByName("cc1_legacy_sounds", g_current.legacy ? "true" : "false");
  if (!change_window) return;
  auto* window = g_window.load();
  if (!window) return;
  const Settings s = g_current;
  window->app_context().CallInUIThreadDeferred([window, s] {
    if (g_window.load() != window) return;
    if (window->IsFullscreen() != (s.fullscreen != 0)) window->SetFullscreen(s.fullscreen != 0);
    if (!s.fullscreen) {
      window->SetDesiredLogicalSize(window->SizeToLogical(kSizes[s.size][0]),
                                    window->SizeToLogical(kSizes[s.size][1]));
    }
  });
}

// The end of a toml value on a key line: the first '#' outside quotes, less the
// spaces before it.
size_t ValueEnd(const std::string& line, size_t from) {
  char quote = 0;
  size_t end = line.size();
  for (size_t i = from; i < line.size(); ++i) {
    const char c = line[i];
    if (quote) {
      if (c == '\\' && quote == '"') {
        ++i;
      } else if (c == quote) {
        quote = 0;
      }
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '#') {
      end = i;
      break;
    }
  }
  while (end > from && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
  return end;
}

// Writes the settings into the config the game boots from, changing only their own lines and appending a key
// that is missing.
void Save() {
  SaveValues({
      {"gpu_resolution_scale", std::to_string(g_current.scale)},
      {"cc1_window_width", std::to_string(kSizes[g_current.size][0])},
      {"cc1_window_height", std::to_string(kSizes[g_current.size][1])},
      {"fullscreen", g_current.fullscreen ? "true" : "false"},
      {"gpu_fps_cap", std::to_string(kLimits[g_current.limit])},
      {"gpu_vsync", g_current.vsync ? "true" : "false"},
      {"gpu_fps", g_current.fps ? "true" : "false"},
      {"cc1_legacy_sounds", g_current.legacy ? "true" : "false"},
      {"cc1_zoom", std::to_string(g_current.zoom)}},
             "Display settings, saved by the in-game settings page.");
}

}  // namespace

void SaveValues(const std::vector<std::pair<std::string, std::string>>& values, const char* heading) {
  std::string text;
  {
    std::ifstream in(g_config_path, std::ios::binary);
    if (in) text.assign(std::istreambuf_iterator<char>(in), {});
  }
  const std::string eol = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  std::vector<std::string> lines;
  for (size_t at = 0; at < text.size();) {
    const size_t end = text.find('\n', at);
    std::string line = text.substr(at, end == std::string::npos ? std::string::npos : end - at);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    lines.push_back(std::move(line));
    if (end == std::string::npos) break;
    at = end + 1;
  }
  std::vector<std::string> added;
  for (const auto& [key, value] : values) {
    bool found = false;
    for (auto& line : lines) {
      const size_t start = line.find_first_not_of(" \t");
      if (start == std::string::npos || line.compare(start, key.size(), key) != 0) continue;
      const size_t equals = line.find_first_not_of(" \t", start + key.size());
      if (equals == std::string::npos || line[equals] != '=') continue;
      const size_t from = std::min(line.find_first_not_of(" \t", equals + 1), line.size());
      line = line.substr(0, from) + (from == equals + 1 ? " " : "") + value +
             line.substr(ValueEnd(line, from));
      found = true;
    }
    if (!found) added.push_back(key + " = " + value);
  }
  if (!added.empty()) {
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    lines.push_back("");
    lines.push_back(std::string("# ") + heading);
    for (auto& line : added) lines.push_back(std::move(line));
  }
  const auto temporary = g_config_path.string() + ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    for (const auto& line : lines) out << line << eol;
    if (!out) {
      REXLOG_ERROR("[settings] cannot write {}", temporary);
      return;
    }
  }
  std::error_code ec;
  std::filesystem::copy_file(temporary, g_config_path,
                             std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    REXLOG_ERROR("[settings] cannot save settings: {}", ec.message());
  } else {
    std::filesystem::remove(temporary, ec);
  }
}

namespace {

std::atomic<int> g_wheel{0};

// One step left (-1) or right (+1) on host entry 1-8.
void Adjust(int entry, int direction) {
  auto cycle = [direction](int value, int count) { return (value + count + direction) % count; };
  switch (entry) {
    case 1: g_current.scale += direction * 25; break;
    case 2: g_current.size = cycle(g_current.size, int(kSizes.size())); break;
    case 3: g_current.fullscreen = !g_current.fullscreen; break;
    case 4: g_current.limit = cycle(g_current.limit, int(kLimits.size())); break;
    case 5: g_current.vsync = !g_current.vsync; break;
    case 6: g_current.fps = !g_current.fps; break;
    case 7: g_current.legacy = !g_current.legacy; break;
    case 8: g_current.zoom += direction; break;
    default: return;
  }
  Publish(entry == 2 || entry == 3);
}

// Sets host entry 1-8 to what its bar shows at 'fraction' of its length and returns false when unchanged,
// leaving the window alone so the picture does not move under the pointer.
bool SetFraction(int entry, double fraction) {
  const Settings before = g_current;
  auto step = [fraction](size_t count) { return int(std::lround(fraction * double(count - 1))); };
  const int on = fraction >= 0.5 ? 1 : 0;
  switch (entry) {
    case 1: g_current.scale = 50 + 25 * step(7); break;
    case 2: g_current.size = step(kSizes.size()); break;
    case 3: g_current.fullscreen = on; break;
    case 4: g_current.limit = step(kLimits.size()); break;
    case 5: g_current.vsync = on; break;
    case 6: g_current.fps = on; break;
    case 7: g_current.legacy = on; break;
    case 8: g_current.zoom = step(size_t(2 * zoom::kSteps + 1)) - zoom::kSteps; break;
    default: return false;
  }
  Clamp(g_current);
  if (g_current == before) return false;
  g_drag_window = g_drag_window || entry == 2 || entry == 3;
  Publish(false);
  return true;
}

// CC1S_Set(...) with the entries as Lua: { name, value text, bar fraction }.
std::string EntriesLua() {
  const Settings& s = g_current;
  char text[1024];
  const char* on_off[] = {"OFF", "ON"};
  char limit[24];
  if (kLimits[s.limit]) {
    std::snprintf(limit, sizeof(limit), "%d FPS", kLimits[s.limit]);
  } else {
    std::snprintf(limit, sizeof(limit), "Unlimited");
  }
  // Each entry is two short lines, like the game's own rows ("Sound FX" / "Volume").
  char scale[2][24];
  if (s.scale == g_boot_scale) {
    std::snprintf(scale[0], sizeof(scale[0]), "Resolution");
    std::snprintf(scale[1], sizeof(scale[1]), "Scale: %d%%", s.scale);
  } else {  // takes effect at the next launch
    std::snprintf(scale[0], sizeof(scale[0]), "Scale: %d%%", s.scale);
    std::snprintf(scale[1], sizeof(scale[1]), "(restart)");
  }
  char legacy[2][24];
  if (s.legacy == g_boot_legacy) {
    std::snprintf(legacy[0], sizeof(legacy[0]), "Legacy Sounds");
    std::snprintf(legacy[1], sizeof(legacy[1]), "%s", on_off[s.legacy]);
  } else {  // takes effect at the next launch
    std::snprintf(legacy[0], sizeof(legacy[0]), "Legacy: %s", on_off[s.legacy]);
    std::snprintf(legacy[1], sizeof(legacy[1]), "(restart)");
  }
  // Zoom is the last entry, shown only where it applies, with its bar centred at the game's own.
  char zoom_row[64];
  const int percent = 100 + 20 * std::abs(s.zoom);
  if (s.zoom) {
    std::snprintf(zoom_row, sizeof(zoom_row), ",{'Zoom','%s %d%%',%.4f}", s.zoom > 0 ? "In" : "Out", percent,
                  (s.zoom + zoom::kSteps) / double(2 * zoom::kSteps));
  } else {
    std::snprintf(zoom_row, sizeof(zoom_row), ",{'Zoom','Default',0.5}");
  }
  std::snprintf(text, sizeof(text),
                "CC1S_Set({{'%s','%s',%.4f},{'Window Size','%d x %d',%.4f},"
                "{'Fullscreen','%s',%d},{'Frame Limit','%s',%.4f},{'V-Sync','%s',%d},"
                // The Legacy Sounds row has its lines further apart so the second clears the tail of the y.
                "{'FPS Counter','%s',%d},{'%s','%s',%d,1.35}%s})",
                scale[0], scale[1], (s.scale - 50) / 150.0,
                kSizes[s.size][0], kSizes[s.size][1], s.size / double(kSizes.size() - 1),
                on_off[s.fullscreen], s.fullscreen, limit, s.limit / double(kLimits.size() - 1),
                on_off[s.vsync], s.vsync, on_off[s.fps], s.fps, legacy[0], legacy[1], s.legacy,
                g_zoom_row ? zoom_row : "");
  return text;
}

// ---- The menu script -------------------------------------------------------------

void PushEntries() {
  std::string out;
  if (!lua::Run(EntriesLua(), &out)) lua::LogError("settings entries", out);
}

// What the player did on the page since the last frame.
void OnEvents(const std::string& out) {
  if (const int steps = g_wheel.exchange(0)) lua::Run("CC1S_Scroll(" + std::to_string(steps) + ")");
  bool changed = false;
  if (std::exchange(g_drag_asked, false)) {
    char call[64];
    std::snprintf(call, sizeof(call), "return CC1S_Drag(%d,%.4f)", g_drag_slot, double(g_drag_fraction));
    std::string entry;
    if (!lua::Run(call, &entry)) {
      lua::LogError("settings drag", entry);
    } else if (SetFraction(std::atoi(entry.c_str()), g_drag_fraction)) {
      changed = true;
    }
  }
  if (std::exchange(g_drag_ended, false) && std::exchange(g_drag_window, false)) Publish(true);
  if (std::exchange(g_spacing_set, false)) changed = true;  // texts are set again with the new spacing
  if (out.empty()) {
    if (changed) PushEntries();
    return;
  }

  for (size_t at = 0; at < out.size();) {
    const size_t end = std::min(out.find(';', at), out.size());
    const std::string event = out.substr(at, end - at);
    at = end + 1;
    int entry = 0, direction = 0;
    if (event == "open") {
      g_page_open = true;
      g_opened = g_current;
      g_zoom_row = zoom::Available();
      changed = true;
    } else if (std::sscanf(event.c_str(), "adj,%d,%d", &entry, &direction) == 2) {
      Adjust(entry, direction < 0 ? -1 : 1);
      changed = true;
    } else if (event == "apply") {
      Save();
      g_opened = g_current;
      REXLOG_INFO("[settings] saved: scale {}% window {}x{} fullscreen {} limit {} vsync {} fps {}",
                  g_current.scale, kSizes[g_current.size][0], kSizes[g_current.size][1],
                  g_current.fullscreen, kLimits[g_current.limit], g_current.vsync, g_current.fps);
      REXLOG_INFO("[settings] saved: zoom {} legacy sounds {}", g_current.zoom, g_current.legacy);
    } else if (event == "reset") {
      const bool window = g_current.size != Settings{}.size || g_current.fullscreen;
      g_current = Settings{};
      Publish(window);
      changed = true;
    } else if (event == "cancel" || event == "close") {
      if (event == "close") g_page_open = false;
      if (!(g_current == g_opened)) {
        const bool window = g_current.size != g_opened.size ||
                            g_current.fullscreen != g_opened.fullscreen;
        g_current = g_opened;
        Publish(window);
        changed = true;
      }
    } else if (event.rfind("space,", 0) == 0) {
      int row = 0;
      float factor = 1.0f;
      if (std::sscanf(event.c_str(), "space,%d,%f", &row, &factor) == 2 && row >= 1 && row <= 3) {
        g_row_spacing[row - 1] = factor;
        g_spacing_asked = true;
      }
    } else if (event.rfind("err,", 0) == 0) {
      lua::LogError("settings menu", event.substr(4));
    }
  }
  if (changed) PushEntries();
}

}  // namespace

void Initialize(rex::ui::Window* window) {
  g_window.store(window);
  lua::Register({"settings", kMenuScript, "return CC1S_Poll()", PushEntries, OnEvents});
  g_config_path = rex::filesystem::GetExecutableFolder() / "crashcourse1.toml";
  g_current.scale = REXCVAR_GET(gpu_resolution_scale);
  g_current.fullscreen = window && window->IsFullscreen();
  g_current.vsync = REXCVAR_GET(gpu_vsync) ? 1 : 0;
  g_current.fps = REXCVAR_GET(gpu_fps) ? 1 : 0;
  g_current.zoom = REXCVAR_GET(cc1_zoom);
  g_current.legacy = REXCVAR_GET(cc1_legacy_sounds) ? 1 : 0;
  g_boot_legacy = g_current.legacy;
  for (size_t i = 0; i < kLimits.size(); ++i) {
    if (kLimits[i] == REXCVAR_GET(gpu_fps_cap)) g_current.limit = int(i);
  }
  bool sized = false;
  for (size_t i = 0; i < kSizes.size(); ++i) {
    if (kSizes[i][0] != REXCVAR_GET(cc1_window_width) ||
        kSizes[i][1] != REXCVAR_GET(cc1_window_height)) {
      continue;
    }
    g_current.size = int(i);
    sized = true;
  }
  Clamp(g_current);
  g_boot_scale = g_current.scale;
  g_opened = g_current;
  // A saved window size is applied at launch; everything else is already in effect.
  if (sized && !g_current.fullscreen) Publish(true);
}

void Shutdown() { g_window.store(nullptr); }

void Scroll(int steps) { g_wheel += steps; }

bool PageOpen() { return g_page_open.load(); }

void Drag(int slot, float fraction) {
  g_drag_slot = slot;
  g_drag_fraction = std::clamp(fraction, 0.0f, 1.0f);
  g_drag_asked = true;
}

void DragEnd() { g_drag_ended = true; }

bool TakeRowSpacing(float factors[3]) {
  if (!std::exchange(g_spacing_asked, false)) return false;
  for (int i = 0; i < 3; ++i) factors[i] = g_row_spacing[i];
  return true;
}

void RowSpacingSet() { g_spacing_set = true; }

}  // namespace cc1::settings
