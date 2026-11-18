// local_players.h: the other players of local multiplayer, signed in through the Guide with a local profile or a
// Game Jolt account.

#pragma once

#include <cstdint>
#include <string>

namespace cc1 {

constexpr uint32_t kLocalPlayers = 2;  // local players per machine

// True when the player (index 1 and up) is a local player signed in on the Guide; any thread.
bool LocalPlayer(uint32_t user);
uint64_t LocalPlayerXuid(uint32_t user);
std::string LocalPlayerName(uint32_t user);

// Called from the app's setup, before the game starts.
void InitializeLocalPlayers();

}  // namespace cc1
