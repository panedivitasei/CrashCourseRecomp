// controls.h: replaces the Help & Options > Controls page with a window for rebinding keyboard, mouse and gamepad
// actions.

#pragma once

#include <cstdint>

namespace rex::input {
struct X_INPUT_GAMEPAD;
}
namespace rex::ui {
class Window;
}

namespace cc1::controls {

// Called once the window is up, on the UI thread.
void Initialize(rex::ui::Window* window);
void Shutdown();

// True while a binding waits for its key or button, when the game and the Guide get no input; any thread.
bool Capturing();

// Feeds a pad state as read (kernel_hooks.cpp, host byte order) to a waiting binding.
void NotePad(uint32_t user, uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger);

// Moves the player's bound buttons onto the game's own while a course is played.
void Remap(rex::input::X_INPUT_GAMEPAD* pad);

// Adds held keys to player 1's pad per the keybind_* settings, with Shift, Ctrl and Alt as ordinary keys.
void ApplyKeyboard(uint32_t user, rex::input::X_INPUT_GAMEPAD* pad);

}  // namespace cc1::controls
