// Online Multiplayer: the Game Jolt account as the game's Xbox LIVE profile, the lobbies behind the main menu's
// online button, and the hand-off from a lobby to the game's own LIVE session.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex {
class Runtime;
}
namespace rex::system {
class KernelState;
}

namespace cc1::online {

constexpr size_t kSessionInfoSize = 60;  // XSESSION_INFO: session id, host address, key

// Called from the app's setup before the Guide is installed, with 'server_url' the JMstudios CC1 server.
void Initialize(std::string server_url);
void Shutdown();

// The runtime's kernel bring-up with this title's session and LIVE apps; set as RuntimeConfig::kernel_init.
void InitializeKernel(rex::Runtime* runtime, rex::system::KernelState* kernel_state);

// True while a Game Jolt account is signed in, when player 1 reads as signed in to LIVE; any thread.
bool Live();
// The account's XUID (LIVE format, from its name) and name, or 0 and empty when not Live; any thread.
uint64_t Xuid();
std::string Name();

void RandomBytes(void* out, size_t size);

// GETs 'path' on the server as the signed-in player and returns the HTTP status (0 for no answer or nobody signed
// in) with the reply's JSON in 'body'; blocks on the network, so not for the game's threads.
int Get(const std::string& path, std::string* body);

// The members of the lobby this game is in, oldest first (empty when in none), with the number of spectator seats
// the lobby allows in 'spectator_seats'; any thread.
struct Player {
  std::string name;
  uint64_t xuid = 0;
  int pid = 0;  // the member's seat at the relay (online/relay.h)
  bool host = false, spectator = false, local = false;
};
std::vector<Player> Players(int* spectator_seats = nullptr);
// Takes player 1's pad as read: Y on a lobby match's results returns the host to the lobby, and in the lobby RB or
// RT is the host's Kick and LB or LT their Change Course; any thread.
void NotePad(uint32_t user, uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger);

// Asks the server to have this player watch the lobby's match, or race in it again; any thread.
void Spectate(bool watch);

// The game's LIVE session, as its XSession calls report it (guest threads).
void OnSessionCreated(bool host, const uint8_t* info);
void OnSessionDeleted();
void OnHostMigrated(bool local, const uint8_t* info);
// The id (8 bytes) of the session this game is in; zeroes when in none.
void CurrentSessionId(uint8_t* id);
// The session this game is in; false when in none.
bool CurrentSession(uint8_t* info);
// The session the lobby's host published, for a member to join; false while there is none.
bool HostSession(uint8_t* info);

}  // namespace cc1::online
