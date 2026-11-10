#pragma once

// One co-op game: a host and a few clients.
// This file doesn't know anything about Zelda: you give it where your Link is
// every frame (Tick), it gives you the list of other players and where they are.
//
// For now: clients see the host and the host sees everyone, but two clients
// don't see each other yet (the host would need to forward their stuff).

#include "soh/Network/Coop/Messages.h"
#include "soh/Network/Coop/Transport.h"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Coop {

inline constexpr uint16_t kDefaultPort = 27720;

enum class SessionRole : uint8_t {
    Off,
    Hosting,    // listening, 0..N clients
    Connecting, // client, waiting for the handshake
    Connected,  // client, handshake done
};

struct RemotePlayer {
    PeerId peer = 0;
    std::string name;
    uint32_t pingMs = 0;
    std::optional<PlayerState> state; // nullopt until the first PlayerState arrives
    std::optional<PlayerPose> pose;   // nullopt until they're in game
    std::chrono::steady_clock::time_point lastStateAt;
};

// "Refused: different game build...", "Connection lost", ...
std::string DescribeDisconnect(DisconnectReason reason, HandshakeError error);

// Why I can't teleport to `them` right now, or nullptr if I can.
// entranceCount = how many entrances the game has (ENTR_MAX), anything outside
// is a broken packet. The text is shown as-is in the menu.
const char* TeleportBlocker(const PlayerState& me, const PlayerState& them, int32_t entranceCount);

// Accepts "host" or "host:port" (surrounding spaces ignored); port defaults to
// kDefaultPort. False on an empty host or an invalid port. IPv4 / host names only.
bool ParseAddress(const std::string& text, std::string& host, uint16_t& port);

class Session {
  public:
    using Logger = std::function<void(const std::string&)>;

    explicit Session(Logger logger = nullptr);
    ~Session();

    // false (and LastError() set) if the port cannot be opened.
    bool Host(uint16_t port, const Hello& local);
    // false only on an invalid address; the outcome arrives through Tick().
    bool Join(const std::string& host, uint16_t port, const Hello& local);
    void Stop();

    // Call once per game frame: processes the network and sends `local` to
    // everyone (plus `pose` when we have one, i.e. when we're in game).
    void Tick(const PlayerState& local, const PlayerPose* pose = nullptr);

    SessionRole Role() const {
        return mRole;
    }
    const std::vector<RemotePlayer>& Players() const {
        return mPlayers;
    }
    // Why the last session ended or failed; empty if it did not.
    const std::string& LastError() const {
        return mLastError;
    }
    uint16_t Port() const; // listening port when hosting, 0 otherwise

    // --- flags (chests, switches, story events...) ---
    // How it works: when a client loads their save it asks the host for all of
    // its flags (snapshot) and takes them. After that every single change is
    // sent to everyone; the host forwards a client's change to the other clients.

    // We changed a flag: tell everyone. Sent at the next Tick().
    void SendFlagChange(const FlagChange& change);
    // Client only: ask the host for its flags.
    void RequestFlagSnapshot();
    // Host only: answer someone's request.
    void SendFlagSnapshot(PeerId peer, const FlagSnapshot& snapshot);

    // What arrived since last time (each call empties the list).
    std::vector<FlagChange> TakeFlagChanges();
    std::optional<FlagSnapshot> TakeFlagSnapshot();
    std::vector<PeerId> TakeSnapshotRequests();

    // --- enemies ---
    // We killed an enemy: tell everyone (host relays, like flags).
    void SendEnemyKill(const EnemyKill& kill);
    std::vector<EnemyKill> TakeEnemyKills();
    // Host only: where our enemies are this frame (clients copy them).
    void SendEnemyStates(const EnemyStates& states);
    // Client: the newest enemy states from the host (older ones are dropped).
    std::optional<EnemyStates> TakeEnemyStates();
    // We hit an enemy someone else runs (client -> host), or an enemy whose
    // health we share (bosses). Relayed by the host like kills.
    void SendEnemyHit(const EnemyHit& hit);
    std::vector<EnemyHit> TakeEnemyHits();

  private:
    // host: pass a client's message on to the other clients (not back to the sender)
    void RelayToOthers(PeerId from, std::span<const uint8_t> payload);
    void SendToAll(Channel channel, std::span<const uint8_t> bytes);

    void HandleEvent(TransportEvent& ev);
    void HandleMessage(RemotePlayer& from, std::span<const uint8_t> payload);
    void SendTo(PeerId peer, Channel channel, std::span<const uint8_t> bytes);
    RemotePlayer* Find(PeerId peer);
    void Log(const std::string& line) const;

    Logger mLogger;
    std::unique_ptr<Transport> mTransport;
    SessionRole mRole = SessionRole::Off;
    std::vector<RemotePlayer> mPlayers;
    std::string mLastError;
    std::vector<TransportEvent> mEvents; // reused every frame to avoid allocations

    std::vector<FlagChange> mFlagChangesIn;
    std::optional<FlagSnapshot> mFlagSnapshotIn;
    std::vector<PeerId> mSnapshotRequestsIn;
    std::vector<EnemyKill> mEnemyKillsIn;
    std::optional<EnemyStates> mEnemyStatesIn;
    std::vector<EnemyHit> mEnemyHitsIn;
};

} // namespace Coop
