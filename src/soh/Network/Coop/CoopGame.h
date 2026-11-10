#pragma once

// Shared bits between the files that talk to the game (CoopGame.cpp,
// CoopPuppets.cpp). Not used by the pure network code.

#include "soh/Network/Coop/Session.h"

#include <ship/utils/color.h>

#include <functional>
#include <mutex>
#include <span>
#include <string>
#include <vector>

struct Actor; // the game's actor struct (z64actor.h)


#define CVAR_COOP(var) CVAR_REMOTE("Coop." var)

namespace CoopGame {

// The session lives as long as the game. Lock Mutex() before touching it.
// Recursive on purpose: what we do with the lock held (give an item, set a flag)
// fires game hooks that lock it again on the same thread.
Coop::Session& Session();
std::recursive_mutex& Mutex();

// Client: reload the scene at the host's spot next frame (same as the Teleport button).
void RequestTeleportToHost();

// --- plumbing for the feature files ---
// Every feature lives in its own Coop*.cpp and registers itself at startup
// (static RegisterShipInitFunc), with these. Registering the same name again
// replaces the previous one (SoH re-runs init functions when presets load).

// Runs every game frame with the session locked, after the network tick and
// the built-in updates. Only while a session is on (hosting or connected).
void OnEveryFrame(const std::string& name, std::function<void()> fn);
// Called (session locked) for every event of that kind a teammate sent us.
void OnEvent(Coop::EventKind kind, const std::string& name, std::function<void(const Coop::Event&)> fn);
// Send an event to everyone else. Safe to call from any hook (locks itself).
// Snapshot channel = may be lost but never waits (voice).
void SendEvent(Coop::EventKind kind, std::vector<uint8_t> data, Coop::Channel channel = Coop::Channel::Ordered);

// --- menu / connect plumbing (Crests of Courage features) ---
enum class MenuWhere : uint8_t {
    NotConnected, // under the Host / Join buttons
    InSession,    // hosting or connected, under the room settings
    Always,       // both
};
// Adds a block to the Co-op menu. Drawn with the session locked. Same name again replaces it.
void AddMenuSection(const std::string& name, MenuWhere where, std::function<void()> draw);
// Start hosting / joining like the menu buttons do (our name, build, seed...). Join runs
// the OnBeforeJoin callbacks first (save backup). Session locked inside.
bool StartHost(uint16_t port);
bool StartJoin(const std::string& host, uint16_t port);
void OnBeforeJoin(const std::string& name, std::function<void()> fn);

// --- notifications (CoopNotify.cpp, Crests of Courage notify.cpp) ---
enum class NoticeKind : uint8_t { Item, Players, Randomizer, Teleport, Other };
// Shows a pop-up on screen (queued, stacked). Any thread, locks itself.
void Notify(NoticeKind kind, const std::string& text);
// Our player id (same numbers as RemotePlayer::id / Event::origin), 0 if none yet.
uint32_t SelfId();
// Hosting or connected.
bool SessionActive();
// Player name for an id, for messages ("Malon found the Kokiri Sword").
std::string PlayerName(uint32_t id);

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
// Who this puppet belongs to (nullptr if it isn't one). Session locked.
const Coop::RemotePlayer* PuppetOwner(::Actor* puppet);
// Dead / downed puppet (still listed by VisiblePuppets). Session locked.
bool PuppetIsDead(::Actor* puppet);
// Colors: ours from the menu, theirs from their state (same default as Anchor)
Color_RGB8 OwnColor();
Color_RGB8 PlayerColor(const Coop::RemotePlayer& player);

// --- puppet fairies (CoopFairy.cpp) ---
// True for the cosmetic fairies following puppets: never sync / count them.
bool IsPuppetFairy(::Actor* actor);

// --- minimap arrows (CoopMinimap.cpp) ---
void RegisterMinimapHooks();

// --- room settings: PvP, who sees who, teleport rules, teams (CoopRoom.cpp) ---
// Host: the settings controls. Client: shows them. Drawn inside the Co-op menu.
void DrawRoomSettings();
// Why we can't teleport to them because of the room rules, nullptr if we can.
const char* RoomTeleportBlocker(const Coop::RemotePlayer& them);
uint8_t RoomPvpMode();           // Anchor roomState.pvpMode: 0 off, 1 other teams, 2 everyone
uint8_t RoomShowLocationsMode(); // 0 nobody, 1 my team, 2 everyone
uint8_t OwnTeam();               // CVAR_COOP("Team"), 0-7

// --- randomizer (CoopRando.cpp) ---
// Hash of the seed + settings for the handshake; 0 = vanilla.
uint64_t SeedHash();
// rando check statuses in the join snapshot (CoopItems.cpp puts them in the save blob)
std::vector<uint8_t> BuildRandoSnapshot();
bool RandoSnapshotValid(std::span<const uint8_t> data);
void ApplyRandoSnapshot(std::span<const uint8_t> data);

// --- save / team state (CoopFlags.cpp / CoopItems.cpp) ---
// Client: ask the host for its whole world again (Anchor's "Request Team State").
void RequestResync();

// --- life / death (CoopLife.cpp) ---
enum class LifeState : uint8_t { Alive = 0, Reviving = 1, Downed = 2, GameOver = 3 };
LifeState LocalLifeState();
// A teammate's last known life state (Alive if unknown).
LifeState PlayerLifeState(uint32_t playerId);

// --- warps queued until it's safe (CoopWarp.cpp, TrueCoop CoopWarp) ---
// Go through an entrance (keeps entrance cutscenes; cutsceneIndex -1 = none).
void RequestEntranceWarp(int16_t entranceIndex, int32_t cutsceneIndex);
// Respawn right where they stand (same as the Teleport button).
void RequestPositionWarp(const Coop::PlayerState& them);

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
