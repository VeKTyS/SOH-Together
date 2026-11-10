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

    // Call once per game frame: processes the network and sends `local` to everyone.
    void Tick(const PlayerState& local);

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

  private:
    void HandleEvent(TransportEvent& ev);
    RemotePlayer* Find(PeerId peer);
    void Log(const std::string& line) const;

    Logger mLogger;
    std::unique_ptr<Transport> mTransport;
    SessionRole mRole = SessionRole::Off;
    std::vector<RemotePlayer> mPlayers;
    std::string mLastError;
    std::vector<TransportEvent> mEvents; // reused every frame to avoid allocations
};

} // namespace Coop
