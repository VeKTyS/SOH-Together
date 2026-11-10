#pragma once

// Shared bits between the files that talk to the game (CoopGame.cpp,
// CoopPuppets.cpp). Not used by the pure network code.

#include "soh/Network/Coop/Session.h"

#include <mutex>

namespace CoopGame {

// The session lives as long as the game. Lock Mutex() before touching it.
Coop::Session& Session();
std::mutex& Mutex();

// What we read from our own Link this frame (updated in the frame hook).
const Coop::PlayerState& LocalState();

// --- puppets (the other players' Links, CoopPuppets.cpp) ---

// Fills `pose` from our Link. False when there's no Link (title screen etc).
bool ReadLocalPose(Coop::PlayerPose& pose);

// Call every frame with the session locked: respawns puppets when someone
// joins/leaves, or after a scene load.
void UpdatePuppets();

// Hooks the puppets need, called once at startup (safe to call again).
void RegisterPuppetHooks();

} // namespace CoopGame
