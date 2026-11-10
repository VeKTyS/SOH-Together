#pragma once

// Shared bits between the files that talk to the game (CoopGame.cpp,
// CoopPuppets.cpp). Not used by the pure network code.

#include "soh/Network/Coop/Session.h"

#include <mutex>
#include <vector>

struct Actor; // the game's actor struct (z64actor.h)

namespace CoopGame {

// The session lives as long as the game. Lock Mutex() before touching it.
// Recursive on purpose: what we do with the lock held (give an item, set a flag)
// fires game hooks that lock it again on the same thread.
Coop::Session& Session();
std::recursive_mutex& Mutex();

// Client: reload the scene at the host's spot next frame (same as the Teleport button).
void RequestTeleportToHost();

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
// "The rest of an actor's memory", the same copy enemies use (CoopEnemies.cpp)
void ReadActorBody(::Actor* actor, std::vector<uint8_t>& body, std::vector<uint8_t>& kinds);
void WriteActorBody(::Actor* actor, const std::vector<uint8_t>& body, const std::vector<uint8_t>& kinds);
// pots, bushes, rocks, crates
bool IsBreakable(int16_t actorId);

// --- things carried / thrown / bombs (CoopThings.cpp) ---
void UpdateThings();
void RegisterThingHooks();

// --- items (CoopItems.cpp) ---

// Call every frame with the session locked: gives the items teammates got.
void UpdateItems();
void RegisterItemHooks();
// The host's inventory for the join snapshot, and applying it on a client.
std::vector<uint8_t> BuildSaveBlob();
bool ApplySaveBlob(const std::vector<uint8_t>& blob);

// Where soh.exe is loaded (CoopModule.cpp). False if unknown on this platform.
bool ExeRange(uintptr_t& start, uintptr_t& end);

// --- world (CoopWorld.cpp): loaded things react to teammates' flags right away ---
void RegisterWorldHooks();

} // namespace CoopGame
