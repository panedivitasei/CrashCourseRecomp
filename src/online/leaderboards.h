// The game's leaderboard page, fed read-only with speedrun.com's verified level times from the server.

#pragma once

#include <cstdint>

namespace cc1::boards {

void Initialize();
void Shutdown();
// Completes the enumeration the game is waiting on; game's main thread, every GUI frame.
void Tick();
// Answers XUserReadStats for the page's friends view by writing the board's top rows as the results, or returns
// false to leave it to the caller.
bool FriendsView(uint32_t specs_count, uint32_t specs_ptr, uint32_t results_size, uint32_t results_ptr);

}  // namespace cc1::boards
