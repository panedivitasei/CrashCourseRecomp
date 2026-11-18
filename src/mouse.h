// mouse.h: drives the game's own menu pointer (GUIINPUT's joystick pointer) with the mouse, left button as A and
// right as B.

#pragma once

#include <cstdint>

struct PPCContext;
namespace rex::ui {
class Window;
}

namespace cc1::mouse {

// Called once the window is up, on the UI thread.
void Initialize(rex::ui::Window* window);
void Shutdown();

// True while a course is being played (no menu up, not paused) as of the last GUI update; any thread.
bool InCourse();

// Runs once per GUI update on the game's main thread (game_lua.cpp's hook).
void Tick(PPCContext& ctx, uint8_t* base);

// True when a point that missed the click box of 'control' is on a picture of the control (the hit test's hook).
bool OnControlHook(PPCContext& ctx, uint8_t* base, uint32_t control, double x, double y);

}  // namespace cc1::mouse
