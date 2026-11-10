#pragma once

// Shared bits between the files that talk to the game (CoopGame.cpp,
// CoopPuppets.cpp). Not used by the pure network code.

#include "soh/Network/Coop/Session.h"

#include <mutex>
#include <vector>

struct Actor; // the game's actor struct (z64actor.h)

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

// The other players' Links standing in our scene right now.
std::vector<::Actor*> VisiblePuppets();

// --- flags (CoopFlags.cpp) ---

// Call every frame with the session locked, after Session().Tick().
void UpdateFlags();
// Client: true once we got the host's flags for the save we're playing.
bool FlagsSynced();
void RegisterFlagHooks();

// --- enemies (CoopEnemies.cpp) ---

// Call every frame with the session locked: runs/copies enemies, applies hits and kills.
void UpdateEnemies();
void RegisterEnemyHooks();

// --- items (CoopItems.cpp) ---

// Call every frame with the session locked: gives the items teammates got.
void UpdateItems();
void RegisterItemHooks();
// The host's inventory for the join snapshot, and applying it on a client.
std::vector<uint8_t> BuildSaveBlob();
bool ApplySaveBlob(const std::vector<uint8_t>& blob);

} // namespace CoopGame
