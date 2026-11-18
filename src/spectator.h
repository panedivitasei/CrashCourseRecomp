// Spectating: watching a match instead of racing in it, in lobbies with spectator seats.

#pragma once

#include <cstdint>
#include <string>

namespace cc1::spectator {

// Called from the app's setup, after cc1::online::Initialize.
void Initialize();

// Handles a request for the lobby's Gamer Card on the game's main thread: true in a lobby with spectator seats,
// where X toggles watching and no card is shown.
bool OnGamercard();
// Reports that player 1 pressed X, from any thread.
void PressX();

// True while the watching player's fighter steps as the racer's did (puppet.cpp), when no local pad input may reach
// it; game thread only.
bool PuppetStep();

// The racer this player is watching in the running race (puppet.cpp), or empty when not watching; game thread only.
std::string Watched();

// Number of bars on the HUD's map in a watched race, one per racer with the first racer's first, or -1 when the map
// is the game's own; game thread only.
int Bars();

// Takes player 1's pad as read, where LB and RB step through the racers to watch; any thread.
void NotePad(uint32_t user, uint16_t buttons);

}  // namespace cc1::spectator
