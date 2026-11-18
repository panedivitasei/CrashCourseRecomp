// CC1's display settings on the game's own Help & Options > Settings page
// (settings.cpp).

#pragma once

namespace rex::ui {
class Window;
}

#include <string>
#include <utility>
#include <vector>

namespace cc1::settings {

// Reads the saved settings; call once the window exists (UI thread).
void Initialize(rex::ui::Window* window);
void Shutdown();

// Writes 'values' (key and file-format value pairs) into the config the game boots from, changing only their own
// lines and adding a missing key under 'heading'.
void SaveValues(const std::vector<std::pair<std::string, std::string>>& values, const char* heading);

// Scrolls the settings page by 'steps' entries (up when negative) for the mouse wheel; any thread.
void Scroll(int steps);

// Pointer handling for the page's bars (mouse.cpp) on the game's main thread: PageOpen is true while the page is
// up, Drag holds the bar of row 'slot' (1-3) at 'fraction' of its length from the left, and DragEnd releases it.
bool PageOpen();
void Drag(int slot, float fraction);
void DragEnd();

// Row label line spacing (settings_menu_lua.h) on the game's main thread: TakeRowSpacing is true when a change was
// requested since the last call and fills each row's factor, and RowSpacingSet marks it applied so the texts are
// set again.
bool TakeRowSpacing(float factors[3]);
void RowSpacingSet();

}  // namespace cc1::settings
