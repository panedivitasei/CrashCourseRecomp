// mouse.cpp: the mouse on the game's own menus (see mouse.h).

#include "mouse.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define NOMINMAX
#include <windows.h>

#include <commctrl.h>
#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>
#include <rexglue_guide/guide.h>

#include "controls.h"
#include "settings.h"

REXCVAR_DEFINE_BOOL(cc1_mouse, true, "Input",
                    "The mouse works the game's menus: left click is A, right click is B.");
REXCVAR_DEFINE_STRING(cc1_mouse_panel,
                      "0.888330,-0.047559,0.023437,-0.055473,0.822677,0.014858,-0.114722,-0.106751", "Input",
                      "The first eight numbers, row by row, of the 3x3 map from the menu layer's space to the "
                      "pointer's (the ninth is 1); empty takes the layer as flat.");
REXCVAR_DECLARE(bool, gpu_stretch);  // src/gpu/renderer.cpp

// GUIINPUT's joystick listener (GUIINPUT + 8) takes a 20-byte event by value in r4..r6: {device, button, -, x, y},
// with x and y as floats from 0 to 1 over the game's screen from its top left.
REX_EXTERN(__imp__sub_82952F00);  // OnJoystickPointerMove(listener, event)
REX_EXTERN(__imp__sub_82953D20);  // JoystickButtonDown(listener, event)
REX_EXTERN(__imp__sub_82953B08);  // JoystickButtonUp(listener, event)
REX_EXTERN(__imp__sub_82964390);  // takes the screen's safe area out of a point (x*, y*), as INPUT::OnMouseMove does
REX_EXTERN(__imp__sub_82919C60);  // GUICLICKABLE: whether the point (f1, f2) is in the control's click area
REX_EXTERN(sub_82919C60);         // the same, with the control's pictures taken in (the hook below)
REX_EXTERN(__imp__sub_829FC768);  // GUILAYER: a point of the pointer's into the layer's space (layer, x*, y*)
REX_EXTERN(__imp__sub_829481D0);  // GUICONTROL: shown, its parents too
REX_EXTERN(__imp__sub_829482D0);  // GUICONTROL: opacity with its parents' (f1), 0 when hidden
REX_EXTERN(__imp__sub_82948AC0);  // GUICONTROL: its centre on the screen (float[2]*)
REX_EXTERN(__imp__sub_82948458);  // GUICONTROL: its size with its parents' scale, before the master's Size (float[2]*)

bool CC1_SaveInputBlocked();  // probe.cpp: a profile save holds A

namespace cc1::mouse {
namespace {

constexpr uint32_t kGuiInput = 0x82C52A40;  // GUIINPUT*
constexpr uint32_t kCursors = 0x82C0281C;   // CURSORMANAGER*: a CURSORMASTER* per device at +0x38, pads first
constexpr uint32_t kGame = 0x82C0479C;      // GAMEXA*, null outside a course; its state at +0x140
constexpr uint32_t kGui = 0x82C04798;       // GUI*: the pause menu's handle at +0x3DA8, -1 while it is down
constexpr uint32_t kPlaying = 2;
constexpr uint32_t kButtonA = 2, kButtonB = 3;

constexpr uint8_t kCursorRgba[] = {
#include "cursor_image.inc"
};
constexpr int kCursorSize = 128;    // the picture's side; its tip is at the centre
constexpr float kCursorScreen = 1080.0f;  // the screen height the picture is drawn for

struct ButtonEvent {
  uint32_t button;
  bool down;
  int32_t x, y;
};

std::atomic<rex::ui::Window*> g_window{nullptr};
std::atomic<bool> g_menu{true};  // a menu is up: the pointer shows and works

std::mutex g_mutex;  // guards the three below, passed from the UI thread to the game thread
int32_t g_x = 0, g_y = 0;
bool g_moved = false;
std::deque<ButtonEvent> g_buttons;

// Owned by the UI thread.
HCURSOR g_cursor = nullptr;
int g_cursor_side = 0;
HWND g_subclassed = nullptr;

// The game's picture in the window: the largest 16:9 box that fits, centred, as the renderer presents it.
struct Box {
  float x, y, w, h;
};
Box Picture(rex::ui::Window* window) {
  const float width = float(window->GetActualPhysicalWidth()), height = float(window->GetActualPhysicalHeight());
  Box box{0.0f, 0.0f, width, height};
  if (REXCVAR_GET(gpu_stretch) || width <= 0.0f || height <= 0.0f) return box;
  constexpr float kAspect = 16.0f / 9.0f;
  if (width / height > kAspect) {
    box.w = height * kAspect;
    box.x = std::floor((width - box.w) * 0.5f);
  } else {
    box.h = width / kAspect;
    box.y = std::floor((height - box.h) * 0.5f);
  }
  return box;
}

// Builds the pointer's picture at 'side' pixels as a cursor with its hot spot at the centre.
HCURSOR MakeCursor(int side) {
  std::vector<uint8_t> pixels(size_t(side) * side * 4);
  const float step = float(kCursorSize) / float(side);
  auto texel = [](int x, int y, int channel) {
    x = std::clamp(x, 0, kCursorSize - 1);
    y = std::clamp(y, 0, kCursorSize - 1);
    return float(kCursorRgba[(y * kCursorSize + x) * 4 + channel]);
  };
  for (int y = 0; y < side; ++y) {
    for (int x = 0; x < side; ++x) {
      const float sx = (float(x) + 0.5f) * step - 0.5f, sy = (float(y) + 0.5f) * step - 0.5f;
      const int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
      const float fx = sx - float(x0), fy = sy - float(y0);
      // Colour is weighted by alpha, so the edge takes none from the clear pixels beside it.
      float sum[4] = {};
      for (int corner = 0; corner < 4; ++corner) {
        const int cx = x0 + (corner & 1), cy = y0 + (corner >> 1);
        const float weight = ((corner & 1) ? fx : 1.0f - fx) * ((corner >> 1) ? fy : 1.0f - fy);
        const float alpha = texel(cx, cy, 3) * weight;
        for (int channel = 0; channel < 3; ++channel) sum[channel] += texel(cx, cy, channel) * alpha;
        sum[3] += alpha;
      }
      uint8_t* out = &pixels[(size_t(y) * side + x) * 4];
      for (int channel = 0; channel < 3; ++channel) {  // blue first, as a Windows bitmap has it
        out[2 - channel] = sum[3] > 0.0f ? uint8_t(std::clamp(sum[channel] / sum[3] + 0.5f, 0.0f, 255.0f)) : 0;
      }
      out[3] = uint8_t(std::clamp(sum[3] + 0.5f, 0.0f, 255.0f));
    }
  }
  BITMAPV5HEADER header = {};
  header.bV5Size = sizeof(header);
  header.bV5Width = side;
  header.bV5Height = -side;  // top row first
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!color || !bits) return nullptr;
  std::memcpy(bits, pixels.data(), pixels.size());
  HBITMAP mask = CreateBitmap(side, side, 1, 1, nullptr);
  ICONINFO info = {};
  info.fIcon = FALSE;
  info.xHotspot = DWORD(side / 2);
  info.yHotspot = DWORD(side / 2);
  info.hbmMask = mask;
  info.hbmColor = color;
  HCURSOR cursor = mask ? static_cast<HCURSOR>(CreateIconIndirect(&info)) : nullptr;
  if (mask) DeleteObject(mask);
  DeleteObject(color);
  return cursor;
}

bool Showing() { return g_menu.load() || rexglue_guide::IsOpen(); }

// Answers WM_SETCURSOR with the game's pointer while a menu is up and with no cursor while a course is played.
LRESULT CALLBACK CursorProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR) {
  if (message == WM_SETCURSOR && LOWORD(lparam) == HTCLIENT && g_cursor && REXCVAR_GET(cc1_mouse)) {
    SetCursor(Showing() ? g_cursor : nullptr);
    return TRUE;
  }
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(hwnd, CursorProc, id);
    g_subclassed = nullptr;
  }
  return DefSubclassProc(hwnd, message, wparam, lparam);
}

// UI thread: rebuilds the cursor at the size the window calls for and subclasses the window to serve it.
void Present() {
  rex::ui::Window* window = g_window.load();
  if (!window || !REXCVAR_GET(cc1_mouse)) return;
  const HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle());
  if (!hwnd) return;
  const Box box = Picture(window);
  const int side = std::clamp(int(std::lround(float(kCursorSize) * box.h / kCursorScreen)) & ~1, 24, 256);
  if (side != g_cursor_side || !g_cursor) {
    if (HCURSOR cursor = MakeCursor(side)) {
      if (g_cursor) DestroyCursor(g_cursor);
      g_cursor = cursor;
      g_cursor_side = side;
    }
  }
  if (g_subclassed != hwnd && SetWindowSubclass(hwnd, CursorProc, 0xCC1, 0)) g_subclassed = hwnd;
}

// True while the Guide or another overlay window has the mouse; UI thread.
bool OverlayHasMouse() {
  if (rexglue_guide::IsOpen()) return true;
  return ImGui::GetCurrentContext() && ImGui::GetIO().WantCaptureMouse;
}

class Listener final : public rex::ui::WindowInputListener {
 public:
  void OnMouseMove(rex::ui::MouseEvent& e) override {
    Present();
    if (OverlayHasMouse()) return;
    std::lock_guard lock(g_mutex);
    g_moved = g_moved || e.x() != g_x || e.y() != g_y;
    g_x = e.x();
    g_y = e.y();
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override { Button(e, true); }
  void OnMouseWheel(rex::ui::MouseEvent& e) override {
    if (!OverlayHasMouse() && e.scroll_y()) settings::Scroll(e.scroll_y() > 0 ? -1 : 1);
  }
  void OnMouseUp(rex::ui::MouseEvent& e) override { Button(e, false); }

 private:
  void Button(rex::ui::MouseEvent& e, bool down) {
    // A release goes through whoever has the mouse, so nothing stays held.
    if (down && (OverlayHasMouse() || controls::Capturing())) return;
    uint32_t button = 0;
    if (e.button() == rex::ui::MouseEvent::Button::kLeft) {
      button = kButtonA;
    } else if (e.button() == rex::ui::MouseEvent::Button::kRight) {
      button = kButtonB;
    } else {
      return;
    }
    std::lock_guard lock(g_mutex);
    if (g_buttons.size() < 16) g_buttons.push_back({button, down, e.x(), e.y()});
  }
};
Listener g_listener;

uint32_t Bits(float value) { return std::bit_cast<uint32_t>(value); }

// Game-thread side: moves and presses one pad's pointer.
struct Pointer {
  PPCContext& ctx;
  uint8_t* base;
  uint32_t listener, device, scratch;
  Box box;

  void Move(int32_t x, int32_t y) {
    const float nx = std::clamp((float(x) - box.x) / box.w, 0.0f, 1.0f);
    const float ny = std::clamp((float(y) - box.y) / box.h, 0.0f, 1.0f);
    rex::memory::store_and_swap<uint32_t>(base + scratch, Bits(nx));
    rex::memory::store_and_swap<uint32_t>(base + scratch + 4, Bits(ny));
    ctx.r3.u64 = scratch;
    ctx.r4.u64 = scratch + 4;
    __imp__sub_82964390(ctx, base);
    ctx.r3.u64 = listener;
    ctx.r4.u64 = uint64_t(device) << 32;
    ctx.r5.u64 = rex::memory::load_and_swap<uint32_t>(base + scratch);
    ctx.r6.u64 = uint64_t(rex::memory::load_and_swap<uint32_t>(base + scratch + 4)) << 32;
    __imp__sub_82952F00(ctx, base);
  }
  void Button(uint32_t button, bool down) {
    ctx.r3.u64 = listener;
    ctx.r4.u64 = (uint64_t(device) << 32) | button;
    if (down) {
      __imp__sub_82953D20(ctx, base);
    } else {
      __imp__sub_82953B08(ctx, base);
    }
  }
};

// ---- where a control takes the pointer ---------------------------------------------------------------------------

// The game's click test is a box from each control's master (ClickArea), so the hook also accepts any picture of the
// control, its own or one in a group inside it, that the click box's centre lies on.
constexpr uint32_t kGroupClass = 0x8216147C, kPictureClass = 0x82168714;  // GUIGROUP and GUIPICTURE vtables
constexpr uint32_t kMaster = 0x78, kDead = 0x80, kDecal = 0xB8, kChildCount = 0x380, kChildren = 0x384;
constexpr uint32_t kSize = 0x58, kAreaCentre = 0xD4, kAreaSize = 0xDC;  // in a master
constexpr float kLargestPicture = 0.6f;  // of the screen: anything bigger is a backdrop, not a button

struct Point {
  float x, y;
};

// Reads the float[2] the game returns a pointer to in r3.
Point Pair(PPCContext& ctx, uint8_t* base) {
  const uint32_t at = ctx.r3.u32;
  return {std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(base + at)),
          std::bit_cast<float>(rex::memory::load_and_swap<uint32_t>(base + at + 4))};
}

bool OnPicture(PPCContext& ctx, uint8_t* base, uint32_t group, Point anchor, Point point, int depth) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  auto real = [&](uint32_t at) { return std::bit_cast<float>(word(at)); };
  const uint32_t count = std::min<uint32_t>(word(group + kChildCount), 32);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t child = word(group + kChildren + 4 * i);
    if (!child || base[child + kDead]) continue;
    const uint32_t kind = word(child);
    if (kind == kGroupClass) {
      if (depth < 3 && OnPicture(ctx, base, child, anchor, point, depth + 1)) return true;
      continue;
    }
    const uint32_t master = word(child + kMaster);
    if (kind != kPictureClass || !master || word(child + kDecal) == 0xFFFFFFFFu) continue;
    ctx.r3.u64 = child;
    __imp__sub_829481D0(ctx, base);
    if (!(ctx.r3.u32 & 0xFF)) continue;
    ctx.r3.u64 = child;
    __imp__sub_829482D0(ctx, base);
    if (!(ctx.f1.f64 > 0.01)) continue;
    ctx.r3.u64 = child;
    __imp__sub_82948AC0(ctx, base);
    const Point centre = Pair(ctx, base);
    ctx.r3.u64 = child;
    __imp__sub_82948458(ctx, base);
    const Point scale = Pair(ctx, base);
    const float half_w = std::fabs(scale.x * real(master + kSize)) * 0.5f;
    const float half_h = std::fabs(scale.y * real(master + kSize + 4)) * 0.5f;
    if (half_w * 2.0f > kLargestPicture || half_h * 2.0f > kLargestPicture) continue;
    auto inside = [&](Point p) { return std::fabs(p.x - centre.x) <= half_w && std::fabs(p.y - centre.y) <= half_h; };
    if (inside(anchor) && inside(point)) return true;
  }
  return false;
}

// True when 'point', which missed the control's click box, is on a visible picture of the control.
bool OnControl(PPCContext& ctx, uint8_t* base, uint32_t control, Point point) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  auto real = [&](uint32_t at) { return std::bit_cast<float>(word(at)); };
  const uint32_t master = control ? word(control + kMaster) : 0;
  if (!master || word(control) != kGroupClass) return false;
  // A control with no click box, such as a whole screen's group kept for its key bindings, takes no pointer.
  if (!(real(master + kAreaSize) > 0.0f) || !(real(master + kAreaSize + 4) > 0.0f)) return false;
  ctx.r3.u64 = control;
  __imp__sub_82948AC0(ctx, base);
  const Point centre = Pair(ctx, base);
  ctx.r3.u64 = control;
  __imp__sub_82948458(ctx, base);
  const Point scale = Pair(ctx, base);
  const Point anchor{centre.x + scale.x * real(master + kSize) * real(master + kAreaCentre),
                     centre.y + scale.y * real(master + kSize + 4) * real(master + kAreaCentre + 4)};
  return OnPicture(ctx, base, control, anchor, point, 0);
}

// ---- the settings page's bars ------------------------------------------------------------------------------------

// The three bars of Help & Options > Settings (settings_menu_lua.h) are GUISCROLLBARs the game gives no pointer
// handling, so the left button held on one slides its row's setting (settings.cpp).
constexpr uint32_t kScrollbarClass = 0x821682E4;  // GUISCROLLBAR vtable
constexpr uint32_t kParent = 0x68;                // in a control

struct Bar {
  Point centre;
  float half_w, half_h;
};

void FindBars(PPCContext& ctx, uint8_t* base, uint32_t group, int depth, std::vector<Bar>& bars) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  auto real = [&](uint32_t at) { return std::bit_cast<float>(word(at)); };
  const uint32_t count = std::min<uint32_t>(word(group + kChildCount), 32);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t child = word(group + kChildren + 4 * i);
    if (!child || base[child + kDead]) continue;
    const uint32_t kind = word(child), master = word(child + kMaster);
    if (kind == kGroupClass) {
      if (depth < 2) FindBars(ctx, base, child, depth + 1, bars);
    } else if (kind == kScrollbarClass && master) {
      ctx.r3.u64 = child;
      __imp__sub_82948AC0(ctx, base);
      const Point centre = Pair(ctx, base);
      ctx.r3.u64 = child;
      __imp__sub_82948458(ctx, base);
      const Point scale = Pair(ctx, base);
      bars.push_back({centre, std::fabs(scale.x * real(master + kSize)) * 0.5f,
                      std::fabs(scale.y * real(master + kSize + 4)) * 0.5f});
    }
  }
}

// Returns the settings page's three bars, top row first, or none when the pointer's controls are not that page's,
// and stores the group they were found in to 'page'.
std::vector<Bar> Bars(PPCContext& ctx, uint8_t* base, uint32_t* page = nullptr) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const uint32_t cursors = word(kCursors);
  uint32_t cursor = 0;
  for (uint32_t pad = 0; cursors && pad < 4 && !cursor; ++pad) cursor = word(cursors + 0x38 + 4 * pad);
  if (!cursor) return {};
  const uint32_t first = word(cursor + 0x8C), last = word(cursor + 0x90);
  const uint32_t count = first && last > first ? std::min<uint32_t>((last - first) / 4, 60) : 0;
  for (uint32_t i = 0; i < count; ++i) {
    // A row's button is in the page, or a group or two down from it.
    uint32_t group = word(first + 4 * i);
    for (int up = 0; up < 3 && group; ++up) {
      group = word(group + kParent);
      if (!group || word(group) != kGroupClass) break;
      std::vector<Bar> bars;
      FindBars(ctx, base, group, 0, bars);
      if (bars.size() != 3) continue;
      std::sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.centre.y < b.centre.y; });
      if (page) *page = group;
      return bars;
    }
  }
  return {};
}

// ---- a settings row's line spacing --------------------------------------------------------------------------------

// A row's text is a GUILABEL whose line spacing is its master's LineSpacingFactor (GUILABELMASTER +0x78), out of
// reach of scripts, so the factor settings.cpp asks for is applied to the row's label here.
constexpr uint32_t kLabelClass = 0x82168524;  // GUILABEL vtable
constexpr uint32_t kLineSpacing = 0x78;       // in a label's master

uint32_t FindLabel(uint8_t* base, uint32_t group, int depth) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const uint32_t count = std::min<uint32_t>(word(group + kChildCount), 32);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t child = word(group + kChildren + 4 * i);
    if (!child || base[child + kDead]) continue;
    if (word(child) == kLabelClass && word(child + kMaster)) return child;
    if (word(child) == kGroupClass && depth < 2) {
      if (const uint32_t label = FindLabel(base, child, depth + 1)) return label;
    }
  }
  return 0;
}

void SpaceRows(PPCContext& ctx, uint8_t* base) {
  // The page's buttons become the pointer's a few frames after the page opens, so the lookup is retried until then.
  static float factors[3];
  static int tries = 0;
  if (settings::TakeRowSpacing(factors)) tries = 180;
  if (!tries) return;
  --tries;
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const PPCContext saved = ctx;
  uint32_t page = 0;
  Bars(ctx, base, &page);
  // The rows' buttons: the pointer's controls in the page, top one first.
  struct Row {
    uint32_t button;
    float y;
  };
  std::vector<Row> rows;
  const uint32_t cursors = word(kCursors);
  uint32_t cursor = 0;
  for (uint32_t pad = 0; cursors && pad < 4 && !cursor; ++pad) cursor = word(cursors + 0x38 + 4 * pad);
  const uint32_t first = cursor ? word(cursor + 0x8C) : 0, last = cursor ? word(cursor + 0x90) : 0;
  const uint32_t count = page && first && last > first ? std::min<uint32_t>((last - first) / 4, 60) : 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t control = word(first + 4 * i);
    if (!control || word(control) != kGroupClass) continue;
    bool inside = false;
    uint32_t up = control;
    for (int level = 0; level < 3 && up && !inside; ++level) {
      up = word(up + kParent);
      inside = up == page;
    }
    if (!inside) continue;
    ctx.r3.u64 = control;
    __imp__sub_82948AC0(ctx, base);
    rows.push_back({control, Pair(ctx, base).y});
  }
  ctx = saved;
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.y < b.y; });
  if (rows.size() != 3) {
    if (!tries) {
      REXLOG_WARN("[mouse] settings page: {} row buttons found for its line spacing (page {:08X})", rows.size(),
                  page);
    }
    return;
  }
  tries = 0;
  static std::vector<std::pair<uint32_t, float>> own;  // each master with its original spacing
  for (int i = 0; i < 3; ++i) {
    const uint32_t label = FindLabel(base, rows[i].button, 0);
    if (!label) {
      REXLOG_WARN("[mouse] settings row {}: no label in its button {:08X}", i + 1, rows[i].button);
      continue;
    }
    const uint32_t master = word(label + kMaster);
    const float now = std::bit_cast<float>(word(master + kLineSpacing));
    auto known = std::find_if(own.begin(), own.end(), [&](const auto& entry) { return entry.first == master; });
    if (known == own.end()) known = own.insert(own.end(), {master, now});
    const float wanted = known->second * factors[i];
    rex::memory::store_and_swap<uint32_t>(base + master + kLineSpacing, Bits(wanted));
    REXLOG_DEBUG("[mouse] settings row {}: label {:08X} master {:08X} line spacing {:.3f} -> {:.3f}", i + 1, label,
                master, now, wanted);
  }
  settings::RowSpacingSet();
}

// Returns how far along 'bar' the point is, 0 at its left end, with half a knob left off each end.
float Along(const Bar& bar, Point point) {
  const float inset = bar.half_w > 2.0f * bar.half_h ? bar.half_h : 0.0f;
  const float left = bar.centre.x - bar.half_w + inset, length = 2.0f * (bar.half_w - inset);
  return length > 0.0f ? std::clamp((point.x - left) / length, 0.0f, 1.0f) : 0.0f;
}

// ---- the menus' panel ---------------------------------------------------------------------------------------------

// The front end's menus (GUI layer guiScreens) are drawn on a tilted, curved panel, so the layer's homography from
// the pointer's space (+0x1C, inverse at +0x40, enabled by the byte at +0x64) is set here from cc1_mouse_panel.
constexpr uint32_t kScreensLayer = 0x3D94, kInputLayer = 0x1E58;  // in the GUI
constexpr uint32_t kToLayer = 0x1C, kToPointer = 0x40, kLayerMapped = 0x64;

// Sets the map of the menus' layer when it is not set, or the numbers given have changed.
void PlacePanel(uint8_t* base) {
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const uint32_t gui = word(kGui), layer = gui ? word(gui + kScreensLayer) : 0;
  if (!layer) return;
  static std::string placed;
  const std::string numbers = REXCVAR_GET(cc1_mouse_panel);
  if (numbers == placed && (numbers.empty() || base[layer + kLayerMapped])) return;
  placed = numbers;
  double h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  int count = 0;
  for (size_t at = 0; at < numbers.size() && count < 8;) {
    size_t used = 0;
    try {
      h[count] = std::stod(numbers.substr(at), &used);
    } catch (...) {
      break;
    }
    ++count;
    at += used;
    while (at < numbers.size() && (numbers[at] == ',' || numbers[at] == ' ')) ++at;
  }
  if (count != 8) {
    base[layer + kLayerMapped] = 0;
    return;
  }
  // The inverse, by cofactors.
  const double det = h[0] * (h[4] * h[8] - h[5] * h[7]) - h[1] * (h[3] * h[8] - h[5] * h[6]) +
                     h[2] * (h[3] * h[7] - h[4] * h[6]);
  if (std::fabs(det) < 1e-9) return;
  const double inverse[9] = {
      (h[4] * h[8] - h[5] * h[7]) / det, (h[2] * h[7] - h[1] * h[8]) / det, (h[1] * h[5] - h[2] * h[4]) / det,
      (h[5] * h[6] - h[3] * h[8]) / det, (h[0] * h[8] - h[2] * h[6]) / det, (h[2] * h[3] - h[0] * h[5]) / det,
      (h[3] * h[7] - h[4] * h[6]) / det, (h[1] * h[6] - h[0] * h[7]) / det, (h[0] * h[4] - h[1] * h[3]) / det};
  for (uint32_t i = 0; i < 9; ++i) {
    rex::memory::store_and_swap<uint32_t>(base + layer + kToLayer + 4 * i, Bits(float(inverse[i])));
    rex::memory::store_and_swap<uint32_t>(base + layer + kToPointer + 4 * i, Bits(float(h[i])));
  }
  base[layer + kLayerMapped] = 1;
  REXLOG_DEBUG("[mouse] the menus' panel is placed: {}", numbers);
}

}  // namespace

bool OnControlHook(PPCContext& ctx, uint8_t* base, uint32_t control, double x, double y) {
  if (!REXCVAR_GET(cc1_mouse)) return false;
  const PPCContext saved = ctx;
  const bool on = OnControl(ctx, base, control, {float(x), float(y)});
  ctx = saved;
  return on;
}

bool InCourse() { return !g_menu.load(); }

void Initialize(rex::ui::Window* window) {
  if (!window) return;
  g_window = window;
  window->AddInputListener(&g_listener, 100);
  Present();
}

void Shutdown() {
  if (rex::ui::Window* window = g_window.exchange(nullptr)) window->RemoveInputListener(&g_listener);
  if (g_subclassed) RemoveWindowSubclass(g_subclassed, CursorProc, 0xCC1);
  g_subclassed = nullptr;
  if (g_cursor) DestroyCursor(g_cursor);
  g_cursor = nullptr;
}

void Tick(PPCContext& ctx, uint8_t* base) {
  rex::ui::Window* window = g_window.load();
  if (!window || !REXCVAR_GET(cc1_mouse)) return;
  auto word = [&](uint32_t at) { return rex::memory::load_and_swap<uint32_t>(base + at); };
  const uint32_t game = word(kGame), gui = word(kGui);
  const bool playing = game && word(game + 0x140) == kPlaying && gui && word(gui + 0x3DA8) == 0xFFFFFFFFu;
  if (g_menu.exchange(!playing) == playing) {
    if (const HWND hwnd = static_cast<HWND>(window->GetNativeWindowHandle())) {
      PostMessageW(hwnd, WM_SETCURSOR, WPARAM(hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
    }
  }
  PlacePanel(base);
  SpaceRows(ctx, base);

  bool moved = false;
  int32_t x = 0, y = 0;
  std::deque<ButtonEvent> buttons;
  {
    std::lock_guard lock(g_mutex);
    moved = std::exchange(g_moved, false);
    x = g_x;
    y = g_y;
    buttons.swap(g_buttons);
  }
  static bool down[2] = {};  // A and B as the game has them from here
  static uint32_t down_device = 0;
  static int drag = 0;  // the settings row (1-3) whose bar the left button holds
  static Bar drag_bar = {};
  if (drag && (playing || rexglue_guide::IsOpen() || !settings::PageOpen())) {
    drag = 0;
    settings::DragEnd();
  }
  // A press belongs to a menu, while a release always goes through so nothing stays held.
  const bool usable = !playing && !rexglue_guide::IsOpen();
  if (!usable) {
    moved = false;
    std::erase_if(buttons, [](const ButtonEvent& event) { return event.down; });
    for (uint32_t i = 0; i < 2; ++i) {
      if (down[i]) buttons.push_back({kButtonA + i, false, x, y});
    }
  }
  if (!moved && buttons.empty()) return;
  const uint32_t input = word(kGuiInput), cursors = word(kCursors);
  if (!input || !cursors) return;
  uint32_t device = 4;
  for (uint32_t pad = 0; pad < 4 && device == 4; ++pad) {
    if (word(cursors + 0x38 + 4 * pad)) device = pad;
  }
  if (down[0] || down[1]) device = down_device;  // a release goes to the pad that has the press
  if (device >= 4) return;
  static uint32_t scratch = 0;
  if (!scratch) scratch = REX_KERNEL_MEMORY()->SystemHeapAlloc(16);
  if (!scratch) return;

  const PPCContext saved = ctx;
  Pointer pointer{ctx, base, input + 8, device, scratch, Picture(window)};
  // Maps a window point into the controls' space along the pointer's path, without the listener.
  auto placed = [&](int32_t px, int32_t py) {
    const Box& box = pointer.box;
    rex::memory::store_and_swap<uint32_t>(base + scratch, Bits(std::clamp((float(px) - box.x) / box.w, 0.0f, 1.0f)));
    rex::memory::store_and_swap<uint32_t>(base + scratch + 4,
                                          Bits(std::clamp((float(py) - box.y) / box.h, 0.0f, 1.0f)));
    ctx.r3.u64 = scratch;
    ctx.r4.u64 = scratch + 4;
    __imp__sub_82964390(ctx, base);
    if (const uint32_t layer = gui ? word(gui + kInputLayer) : 0) {
      ctx.r3.u64 = layer;
      ctx.r4.u64 = scratch;
      ctx.r5.u64 = scratch + 4;
      __imp__sub_829FC768(ctx, base);
    }
    return Point{std::bit_cast<float>(word(scratch)), std::bit_cast<float>(word(scratch + 4))};
  };
  if (moved && drag) {
    settings::Drag(drag, Along(drag_bar, placed(x, y)));  // the pointer stays the bar's: no other row lights up
  } else if (moved) {
    pointer.Move(x, y);
  }
  for (const ButtonEvent& event : buttons) {
    if (event.button == kButtonA && !event.down && drag) {
      drag = 0;
      settings::DragEnd();
      continue;
    }
    // A click is A, which the Saving popup holds as it does on the pad.
    if (event.button == kButtonA && event.down && CC1_SaveInputBlocked()) continue;
    if (event.button == kButtonA && event.down && !down[0] && !drag && settings::PageOpen()) {
      const Point point = placed(event.x, event.y);
      const std::vector<Bar> bars = Bars(ctx, base);
      for (size_t i = 0; i < bars.size() && !drag; ++i) {
        const Bar& bar = bars[i];
        if (std::fabs(point.y - bar.centre.y) > std::max(bar.half_h * 1.5f, 0.02f) ||
            std::fabs(point.x - bar.centre.x) > bar.half_w + 0.01f) {
          continue;
        }
        drag = int(i) + 1;
        drag_bar = bar;
        REXLOG_DEBUG("[mouse] settings bar {} held: centre {:.4f},{:.4f} half {:.4f}x{:.4f}, pointer {:.4f},{:.4f}",
                    drag, bar.centre.x, bar.centre.y, bar.half_w, bar.half_h, point.x, point.y);
        settings::Drag(drag, Along(bar, point));
      }
      if (drag) continue;  // the press is the bar's, not the page's A
      if (bars.empty()) REXLOG_WARN("[mouse] settings page: its bars were not found");
    }
    bool& held = down[event.button - kButtonA];
    if (event.down == held) continue;  // nothing to pair it with
    if (event.down) pointer.Move(event.x, event.y);
    pointer.Button(event.button, event.down);
    held = event.down;
    down_device = device;
  }
  ctx = saved;
}

}  // namespace cc1::mouse

REX_HOOK_RAW(sub_82919C60) {
  const uint32_t control = ctx.r3.u32;
  const double x = ctx.f1.f64, y = ctx.f2.f64;
  __imp__sub_82919C60(ctx, base);
  if (!(ctx.r3.u32 & 0xFF) && cc1::mouse::OnControlHook(ctx, base, control, x, y)) ctx.r3.u64 = 1;
}
