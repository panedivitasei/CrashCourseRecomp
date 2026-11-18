// The avatars' poses in an online lobby, kept the same for every member (lobby_pose.cpp).

#pragma once

namespace cc1::pose {

// Starts every box over for a new session.
void Reset();
// Runs on the game's main thread every GUI frame.
void Tick();
// Has the lobby load every player's avatar again, since a race empties the list it loads them from; game's main
// thread, with the lobby up.
void Requeue();
// Has the host's game send the level configuration (the course) to the session's other members, as when the lobby
// forms; game's main thread.
void SendCourse();

}  // namespace cc1::pose
