// controls.cpp: key and button bindings behind the Controls window (see controls.h).

#include "controls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rexglue_guide/guide.h>

#include "game_lua.h"
#include "mouse.h"
#include "settings.h"

REXCVAR_DECLARE(bool, mnk_mode);  // the runtime's keyboard and mouse as a pad

namespace cc1 {
void InjectKeystroke(uint16_t vk);  // kernel_hooks.cpp
}

REXCVAR_DEFINE_STRING(cc1_pad_jump, "A", "Input/Gamepad", "The pad button that jumps");
REXCVAR_DEFINE_STRING(cc1_pad_sprint, "RT", "Input/Gamepad", "The pad button that sprints");
REXCVAR_DEFINE_STRING(cc1_pad_skip, "RB", "Input/Gamepad", "The pad button that skips ahead (Chicken Out)");
REXCVAR_DEFINE_STRING(cc1_pad_restart, "Y", "Input/Gamepad", "The pad button that restarts the area");

namespace cc1::controls {
namespace {

constexpr char kMenu[] = "cc1_controls", kPrompt[] = "cc1_controls_prompt";
constexpr auto kWait = std::chrono::seconds(8);  // pad-button wait timeout; a key wait ends on Esc

// ---- the keyboard and mouse: a key for each pad button, the runtime's settings ------------------------------------

struct KeyAction {
  const char* label;
  const char* setting;  // keybind_*
  const char* fallback;
};
constexpr KeyAction kKeys[] = {
    {"Move Left", "keybind_lstick_left", "A"},
    {"Move Right", "keybind_lstick_right", "D"},
    {"Up", "keybind_lstick_up", "W"},
    {"Down / Slide", "keybind_lstick_down", "S"},
    {"Jump / Select", "keybind_a", "Space"},
    {"Sprint", "keybind_right_trigger", "Shift"},
    {"Skip Ahead", "keybind_right_shoulder", "F"},
    {"Restart Area", "keybind_y", "Tab"},
    {"Back", "keybind_b", "Backspace"},
    {"Menu Action", "keybind_x", "X"},  // X: a menu's other choice (Change Filter, Reset To Defaults)
    {"Pause", "keybind_start", "Escape"},
};

// ---- the gamepad: a button of the player's for each of the game's ---------------------------------------------------

constexpr uint16_t kTriggerDown = 64;
struct PadButton {
  const char* name;
  uint16_t mask;  // 0: a trigger
  bool right;
};
constexpr PadButton kButtons[] = {
    {"A", 0x1000, false},     {"B", 0x2000, false},      {"X", 0x4000, false},    {"Y", 0x8000, false},
    {"LB", 0x0100, false},    {"RB", 0x0200, false},     {"LT", 0, false},        {"RT", 0, true},
    {"Back", 0x0020, false},  {"Start", 0x0010, false},  {"LS", 0x0040, false},   {"RS", 0x0080, false},
    {"D-pad Up", 0x0001, false}, {"D-pad Down", 0x0002, false}, {"D-pad Left", 0x0004, false},
    {"D-pad Right", 0x0008, false},
};
constexpr int kButtonCount = int(std::size(kButtons));

struct PadAction {
  const char* label;
  const char* setting;  // cc1_pad_*
  int game;             // index in kButtons of the game's own button, the default
};
constexpr PadAction kPad[] = {
    {"Jump", "cc1_pad_jump", 0},
    {"Sprint", "cc1_pad_sprint", 7},
    {"Skip Ahead", "cc1_pad_skip", 5},
    {"Restart Area", "cc1_pad_restart", 3},
};
constexpr int kPadCount = int(std::size(kPad));

int ButtonNamed(const std::string& name) {
  for (int i = 0; i < kButtonCount; ++i) {
    if (name == kButtons[i].name) return i;
  }
  return -1;
}

// Bound button of each action as an index into kButtons, read on the game's threads.
std::atomic<int> g_bound[kPadCount];
std::atomic<bool> g_remapped{false};  // any action is off its default

void LoadPad() {
  bool moved = false;
  for (int i = 0; i < kPadCount; ++i) {
    const int button = ButtonNamed(rex::cvar::Query<std::string>(kPad[i].setting));
    g_bound[i] = button < 0 ? kPad[i].game : button;
    moved = moved || g_bound[i] != kPad[i].game;
  }
  g_remapped = moved;
}

bool Down(int button, uint16_t buttons, uint8_t lt, uint8_t rt) {
  const PadButton& b = kButtons[button];
  return b.mask ? (buttons & b.mask) != 0 : (b.right ? rt : lt) >= kTriggerDown;
}

// ---- the keyboard, read here ------------------------------------------------------------------------------------------

// The runtime treats Shift, Ctrl and Alt as modifiers only, so keys are tracked from the window and every keybind_*
// setting is evaluated again without that rule and added to player 1's pad.

std::array<std::atomic<bool>, 256> g_key{};

constexpr uint8_t kShift = 1, kCtrl = 2, kAlt = 4;
struct Chord {
  uint16_t key = 0;
  uint8_t with = 0;      // modifiers that must be held too
  bool modifier = false;  // the key is Shift, Ctrl or Alt: the runtime never fires it
};

enum class Out { kButton, kLeftTrigger, kRightTrigger, kLeftX, kLeftY, kRightX, kRightY };
struct KeyBind {
  const char* setting;
  Out out;
  int value;        // the button's mask, or the stick's direction
  uint16_t stroke;  // the pad keystroke the menus read for it (VK_PAD_*), 0 for none
};
constexpr KeyBind kBinds[] = {
    {"keybind_a", Out::kButton, 0x1000, 0x5800},
    {"keybind_b", Out::kButton, 0x2000, 0x5801},
    {"keybind_x", Out::kButton, 0x4000, 0x5802},
    {"keybind_y", Out::kButton, 0x8000, 0x5803},
    {"keybind_right_shoulder", Out::kButton, 0x0200, 0x5804},
    {"keybind_left_shoulder", Out::kButton, 0x0100, 0x5805},
    {"keybind_left_trigger", Out::kLeftTrigger, 255, 0x5806},
    {"keybind_right_trigger", Out::kRightTrigger, 255, 0x5807},
    {"keybind_dpad_up", Out::kButton, 0x0001, 0x5810},
    {"keybind_dpad_down", Out::kButton, 0x0002, 0x5811},
    {"keybind_dpad_left", Out::kButton, 0x0004, 0x5812},
    {"keybind_dpad_right", Out::kButton, 0x0008, 0x5813},
    {"keybind_start", Out::kButton, 0x0010, 0x5814},
    {"keybind_back", Out::kButton, 0x0020, 0x5815},
    {"keybind_lstick_press", Out::kButton, 0x0040, 0x5816},
    {"keybind_rstick_press", Out::kButton, 0x0080, 0x5817},
    {"keybind_lstick_up", Out::kLeftY, 1, 0x5820},
    {"keybind_lstick_down", Out::kLeftY, -1, 0x5821},
    {"keybind_lstick_right", Out::kLeftX, 1, 0x5822},
    {"keybind_lstick_left", Out::kLeftX, -1, 0x5823},
    {"keybind_rstick_up", Out::kRightY, 1, 0},
    {"keybind_rstick_down", Out::kRightY, -1, 0},
    {"keybind_rstick_right", Out::kRightX, 1, 0},
    {"keybind_rstick_left", Out::kRightX, -1, 0},
};
constexpr int kBindCount = int(std::size(kBinds));

std::mutex g_chord_mutex;
std::vector<Chord> g_chords[kBindCount];
std::chrono::steady_clock::time_point g_chords_read;

uint8_t ModifierOf(rex::ui::VirtualKey key) {
  using Key = rex::ui::VirtualKey;
  if (key == Key::kShift || key == Key::kLShift || key == Key::kRShift) return kShift;
  if (key == Key::kControl || key == Key::kLControl || key == Key::kRControl) return kCtrl;
  if (key == Key::kMenu || key == Key::kLMenu || key == Key::kRMenu) return kAlt;
  return 0;
}

// Parses the alternatives of one setting, such as "Semicolon,Space" or "Shift+Up".
std::vector<Chord> ParseChords(const std::string& value) {
  std::vector<Chord> chords;
  for (size_t at = 0; at <= value.size();) {
    size_t end = value.find(',', at);
    if (end == std::string::npos) end = value.size();
    std::string token = value.substr(at, end - at);
    at = end + 1;
    Chord chord;
    for (;;) {
      while (!token.empty() && token.front() == ' ') token.erase(0, 1);
      while (!token.empty() && token.back() == ' ') token.pop_back();
      const size_t plus = token.find('+');
      if (plus == std::string::npos || plus == 0) break;
      const std::string head = token.substr(0, plus);
      const uint8_t bit = head == "Shift" ? kShift : head == "Ctrl" || head == "Control" ? kCtrl : head == "Alt" ? kAlt : 0;
      if (!bit) break;
      chord.with |= bit;
      token.erase(0, plus + 1);
    }
    const rex::ui::VirtualKey key = rex::ui::ParseVirtualKey(token);
    if (key == rex::ui::VirtualKey::kNone || uint16_t(key) >= 256) continue;
    chord.key = uint16_t(key);
    chord.modifier = ModifierOf(key) != 0;
    chords.push_back(chord);
  }
  return chords;
}

// Re-reads the settings as chords, at most every 500 ms unless 'now' is set, with g_chord_mutex held.
void ReadChords(bool now) {
  const auto time = std::chrono::steady_clock::now();
  if (!now && time - g_chords_read < std::chrono::milliseconds(500)) return;
  g_chords_read = time;
  for (int i = 0; i < kBindCount; ++i) g_chords[i] = ParseChords(rex::cvar::Query<std::string>(kBinds[i].setting));
}

bool Held(rex::ui::VirtualKey key) { return g_key[uint16_t(key)].load(std::memory_order_relaxed); }

bool Held(const Chord& chord) {
  using Key = rex::ui::VirtualKey;
  if (!g_key[chord.key].load(std::memory_order_relaxed)) return false;
  if ((chord.with & kShift) && !Held(Key::kShift)) return false;
  if ((chord.with & kCtrl) && !Held(Key::kControl)) return false;
  if ((chord.with & kAlt) && !Held(Key::kMenu)) return false;
  return true;
}

void NoteKey(rex::ui::VirtualKey key, bool down) {
  using Key = rex::ui::VirtualKey;
  if (uint16_t(key) >= 256) return;
  g_key[uint16_t(key)] = down;
  // Left and right modifiers also set the generic key.
  switch (ModifierOf(key)) {
    case kShift: g_key[uint16_t(Key::kShift)] = down || Held(Key::kLShift) || Held(Key::kRShift); break;
    case kCtrl: g_key[uint16_t(Key::kControl)] = down || Held(Key::kLControl) || Held(Key::kRControl); break;
    case kAlt: g_key[uint16_t(Key::kMenu)] = down || Held(Key::kLMenu) || Held(Key::kRMenu); break;
    default: break;
  }
}

// ---- a binding waiting for its key or button ------------------------------------------------------------------------

enum class Wait { kNone, kKey, kPad };
std::atomic<Wait> g_wait{Wait::kNone};
std::atomic<int> g_wait_action{0};
std::chrono::steady_clock::time_point g_wait_since;
std::atomic<uint32_t> g_pad_held{0};  // buttons down when the wait began, not counted as presses
std::atomic<bool> g_pad_seen{false};

void Set(const char* setting, const std::string& value) {
  rex::cvar::SetFlagByName(setting, value);
  {
    std::lock_guard lock(g_chord_mutex);
    ReadChords(true);
  }
  settings::SaveValues({{setting, "\"" + value + "\""}}, "Controls, saved by Help & Options > Controls.");
  REXLOG_INFO("[controls] {} = {}", setting, value);
}

void Begin(Wait kind, int action) {
  g_wait_action = action;
  g_wait_since = std::chrono::steady_clock::now();
  g_pad_seen = false;
  g_wait = kind;
  rexglue_guide::ShowMenu(kMenu, false);
  rexglue_guide::ShowMenu(kPrompt, true);
}

void End() {
  if (g_wait.exchange(Wait::kNone) == Wait::kNone) return;
  rexglue_guide::ShowMenu(kPrompt, false);
  rexglue_guide::ShowMenu(kMenu, true);
}

class Listener final : public rex::ui::WindowInputListener, public rex::ui::WindowListener {
 public:
  void OnKeyUp(rex::ui::KeyEvent& e) override { NoteKey(e.virtual_key(), false); }
  void OnMouseUp(rex::ui::MouseEvent& e) override { NoteMouse(e, false); }
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    for (auto& key : g_key) key = false;
  }
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    NoteKey(e.virtual_key(), true);
    if (g_wait.load() != Wait::kKey) return;
    e.set_handled(true);
    if (e.virtual_key() == rex::ui::VirtualKey::kEscape) {
      End();
      return;
    }
    Take(e.virtual_key());
  }
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    NoteMouse(e, true);
    if (g_wait.load() != Wait::kKey) return;
    using Button = rex::ui::MouseEvent::Button;
    if (e.button() == Button::kLeft) {
      Take(rex::ui::VirtualKey::kLButton);
    } else if (e.button() == Button::kRight) {
      Take(rex::ui::VirtualKey::kRButton);
    } else if (e.button() == Button::kMiddle) {
      Take(rex::ui::VirtualKey::kMButton);
    }
  }

 private:
  static void NoteMouse(rex::ui::MouseEvent& e, bool down) {
    using Button = rex::ui::MouseEvent::Button;
    using Key = rex::ui::VirtualKey;
    if (e.button() == Button::kLeft) NoteKey(Key::kLButton, down);
    if (e.button() == Button::kRight) NoteKey(Key::kRButton, down);
    if (e.button() == Button::kMiddle) NoteKey(Key::kMButton, down);
  }
  void Take(rex::ui::VirtualKey key) {
    using Key = rex::ui::VirtualKey;
    // Left and right modifiers map to the generic key that bindings name.
    switch (ModifierOf(key)) {
      case kShift: key = Key::kShift; break;
      case kCtrl: key = Key::kControl; break;
      case kAlt: key = Key::kMenu; break;
      default: break;
    }
    const std::string name = rex::ui::VirtualKeyToString(key);
    if (name.empty()) return;  // no runtime name for the key: keeps waiting
    Set(kKeys[g_wait_action.load()].setting, name);
    End();
  }
};
Listener g_listener;
rex::ui::Window* g_window = nullptr;

// ---- the window -----------------------------------------------------------------------------------------------------

void Build(rexglue_guide::Menu& menu) {
  static int tab = 0;
  menu.Choice("Bindings for", &tab, {"Keyboard & Mouse", "Gamepad"});
  if (tab == 0) {
    for (int i = 0; i < int(std::size(kKeys)); ++i) {
      if (menu.Button(kKeys[i].label, rex::cvar::Query<std::string>(kKeys[i].setting))) Begin(Wait::kKey, i);
    }
    if (menu.Button("Reset To Defaults##keys")) {
      for (const KeyAction& key : kKeys) Set(key.setting, key.fallback);
    }
  } else {
    for (int i = 0; i < kPadCount; ++i) {
      if (menu.Button(kPad[i].label, kButtons[g_bound[i].load()].name)) Begin(Wait::kPad, i);
    }
    if (menu.Button("Reset To Defaults##pad")) {
      for (const PadAction& action : kPad) Set(action.setting, kButtons[action.game].name);
      LoadPad();
    }
  }
}

void BuildPrompt(rexglue_guide::Menu& menu) {
  const Wait wait = g_wait.load();
  if (wait == Wait::kNone) return;
  const int action = g_wait_action.load();
  if (wait == Wait::kKey) {
    menu.Header(kKeys[action].label);
    menu.Text("Press a key or a mouse button.");
    menu.Text("Esc to cancel.", rexglue_guide::Menu::Tone::kDim);
  } else {
    menu.Header(kPad[action].label);
    menu.Text("Press a button on the gamepad.");
    menu.Text("Esc to cancel, or wait a few seconds.", rexglue_guide::Menu::Tone::kDim);
    if (std::chrono::steady_clock::now() - g_wait_since > kWait) End();
  }
}

// ---- the game's Controls button -------------------------------------------------------------------------------------

// Intercepts the Transition to the controller-picture page (main or pause menu), opens the window instead and pops
// the menu option the click pushed.
constexpr char kScript[] = R"lua(
if rawget(_G, "CC1C") then return "have" end
if not (CC1_Hook and Transition) then return "wait" end
local C = { events = "" }
rawset(_G, "CC1C", C)
local PAGES = { ["GGRP:menus/help_and_options/controls"] = true, ["GGRP:pause/controls"] = true }
CC1_Hook("Transition", function(next, target, ...)
  if PAGES[target] then
    local pop = rawget(_G, "RemoveMenuOptionFromStack") or RemoveMenuOptionFromStack
    if pop then pcall(pop) end
    C.events = C.events .. "open;"
    return
  end
  return next(target, ...)
end)
rawset(_G, "CC1C_Poll", function()
  local events = C.events
  C.events = ""
  return events
end)
return "installed"
)lua";

void OnEvents(const std::string& events) {
  if (events.find("open;") != std::string::npos) rexglue_guide::ShowMenu(kMenu, true);
}

}  // namespace

// Applies the list's default keys to every setting the config leaves unset and strips them from the runtime's
// defaults for unlisted buttons, such as Tab and F.
void ApplyKeyDefaults() {
  auto listed = [](const char* setting) {
    for (const KeyAction& key : kKeys) {
      if (std::string_view(key.setting) == setting) return true;
    }
    return false;
  };
  auto taken = [](std::string_view name) {
    for (const KeyAction& key : kKeys) {
      if (name.size() == std::strlen(key.fallback) && _strnicmp(name.data(), key.fallback, name.size()) == 0) {
        return true;
      }
    }
    return false;
  };
  for (const KeyBind& bind : kBinds) {
    if (rex::cvar::GetFlagSource(bind.setting) != rex::cvar::Source::kDefault) continue;
    if (listed(bind.setting)) {
      for (const KeyAction& key : kKeys) {
        if (std::string_view(key.setting) == bind.setting) rex::cvar::SetFlagByName(key.setting, key.fallback);
      }
      continue;
    }
    const std::string keys = rex::cvar::Query<std::string>(bind.setting);
    std::string left;
    for (size_t at = 0; at <= keys.size();) {
      const size_t end = std::min(keys.find(',', at), keys.size());
      const std::string_view name(keys.data() + at, end - at);
      if (!name.empty() && !taken(name)) left += (left.empty() ? "" : ",") + std::string(name);
      at = end + 1;
    }
    if (left != keys) rex::cvar::SetFlagByName(bind.setting, left);
  }
  std::lock_guard lock(g_chord_mutex);
  ReadChords(true);
}

void Initialize(rex::ui::Window* window) {
  ApplyKeyDefaults();
  LoadPad();
  lua::Register({"controls", kScript, "return CC1C_Poll()", nullptr, OnEvents});
  rexglue_guide::AddMenu({.id = kMenu,
                          .title = "Controls",
                          .place = rexglue_guide::MenuPlace::kDetached,
                          .window = {.width = 620.0f, .height = 720.0f, .hold_input = true},
                          .build = Build});
  rexglue_guide::AddMenu({.id = kPrompt,
                          .title = "Controls",
                          .place = rexglue_guide::MenuPlace::kDetached,
                          .window = {.width = 520.0f, .height = 240.0f, .movable = false, .hold_input = false},
                          .build = BuildPrompt});
  g_window = window;
  if (window) {
    window->AddInputListener(&g_listener, 200);
    window->AddListener(&g_listener);
  }
}

void Shutdown() {
  if (g_window) {
    g_window->RemoveInputListener(&g_listener);
    g_window->RemoveListener(&g_listener);
  }
  g_window = nullptr;
}

bool Capturing() { return g_wait.load() != Wait::kNone; }

void NotePad(uint32_t user, uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger) {
  if (g_wait.load() != Wait::kPad || user != 0) return;
  uint32_t down = 0;
  for (int i = 0; i < kButtonCount; ++i) {
    if (Down(i, buttons, left_trigger, right_trigger)) down |= 1u << i;
  }
  if (!g_pad_seen.exchange(true)) {
    g_pad_held = down;  // the A that chose the action is still down
    return;
  }
  const uint32_t held = g_pad_held.load() & down;
  g_pad_held = held;
  const uint32_t pressed = down & ~held;
  if (!pressed) return;
  int button = 0;
  while (!(pressed & (1u << button))) ++button;
  Set(kPad[g_wait_action.load()].setting, kButtons[button].name);
  LoadPad();
  End();
}

void Remap(rex::input::X_INPUT_GAMEPAD* pad) {
  if (!pad || !g_remapped.load() || !mouse::InCourse()) return;
  const uint16_t buttons = uint16_t(pad->buttons);
  const uint8_t lt = pad->left_trigger, rt = pad->right_trigger;
  // Clears both the game's and the player's buttons for these actions and leaves the rest of the pad as read.
  uint16_t out = buttons;
  uint8_t out_lt = lt, out_rt = rt;
  auto clear = [&](int button) {
    const PadButton& b = kButtons[button];
    if (b.mask) {
      out = uint16_t(out & ~b.mask);
    } else {
      (b.right ? out_rt : out_lt) = 0;
    }
  };
  for (int i = 0; i < kPadCount; ++i) {
    clear(kPad[i].game);
    clear(g_bound[i].load());
  }
  for (int i = 0; i < kPadCount; ++i) {
    const int bound = g_bound[i].load();
    if (!Down(bound, buttons, lt, rt)) continue;
    const PadButton& game = kButtons[kPad[i].game];
    if (game.mask) {
      out = uint16_t(out | game.mask);
    } else {
      // A game trigger takes the analogue value of the bound trigger, or 255 for a bound button.
      const PadButton& own = kButtons[bound];
      (game.right ? out_rt : out_lt) = own.mask ? uint8_t(255) : (own.right ? rt : lt);
    }
  }
  pad->buttons = out;
  pad->left_trigger = out_lt;
  pad->right_trigger = out_rt;
}

void ApplyKeyboard(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad) {
  if (user != 0 || !pad || !REXCVAR_GET(mnk_mode)) return;
  uint16_t buttons = 0;
  int lx = 0, ly = 0, rx = 0, ry = 0;
  bool lt = false, rt = false;
  static bool was[kBindCount] = {};  // held last time by a key the runtime never fires
  {
    std::lock_guard lock(g_chord_mutex);
    ReadChords(false);
    for (int i = 0; i < kBindCount; ++i) {
      bool held = false, by_modifier = false;
      for (const Chord& chord : g_chords[i]) {
        if (!Held(chord)) continue;
        held = true;
        by_modifier = by_modifier || chord.modifier;
      }
      // Menus read keystrokes, so one the runtime cannot produce from a modifier key is injected here outside a course.
      if (by_modifier && !was[i] && kBinds[i].stroke && !mouse::InCourse()) InjectKeystroke(kBinds[i].stroke);
      was[i] = by_modifier;
      if (!held) continue;
      switch (kBinds[i].out) {
        case Out::kButton: buttons = uint16_t(buttons | kBinds[i].value); break;
        case Out::kLeftTrigger: lt = true; break;
        case Out::kRightTrigger: rt = true; break;
        case Out::kLeftX: lx += kBinds[i].value; break;
        case Out::kLeftY: ly += kBinds[i].value; break;
        case Out::kRightX: rx += kBinds[i].value; break;
        case Out::kRightY: ry += kBinds[i].value; break;
      }
    }
  }
  pad->buttons = uint16_t(uint16_t(pad->buttons) | buttons);
  if (lt) pad->left_trigger = 255;
  if (rt) pad->right_trigger = 255;
  auto push = [](int direction) { return int16_t(direction > 0 ? 32767 : -32767); };
  if (lx) pad->thumb_lx = push(lx);
  if (ly) pad->thumb_ly = push(ly);
  if (rx) pad->thumb_rx = push(rx);
  if (ry) pad->thumb_ry = push(ry);
}

}  // namespace cc1::controls
