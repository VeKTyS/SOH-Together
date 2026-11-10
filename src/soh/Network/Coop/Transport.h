#pragma once

// Sends and receives packets over UDP. ENet does the heavy lifting, but the
// rest of the code never sees it (easier to swap later if we need to).
// Call Poll() once per frame, from the game thread only.
//
// Someone only counts as "connected" once both sides swapped a matching
// Hello. Before that, we don't accept anything else from them.

#include "soh/Network/Coop/Handshake.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer ENetPeer;

namespace Coop {

// 3 lanes so a slow one doesn't hold up the others
enum class Channel : uint8_t {
    Ordered = 0,  // always arrives, in order: story progress, spawns, cutscenes
    Snapshot = 1, // can get lost, old ones are dropped: positions (a new one comes next frame anyway)
    Rpc = 2,      // always arrives, but doesn't wait behind Ordered: hits, requests
};
inline constexpr uint8_t kChannelCount = 3;

// 1 MB max per message, ENet throws away anything bigger
inline constexpr size_t kMaxMessageBytes = 1 << 20;

// Just a number to tell peers apart, never reused
using PeerId = uint32_t;

enum class DisconnectReason : uint8_t {
    ConnectionLost = 0,    // timeout or no answer (ENet sends no reason)
    Normal = 1,            // Disconnect() called by either side
    Refused = 2,           // incompatible Hello, see handshakeError
    ProtocolViolation = 3, // something other than a Hello sent before the handshake
    HandshakeTimeout = 4,  // connected but no Hello received in time
};

struct TransportEvent {
    enum class Type : uint8_t { Connected, Disconnected, Message };

    Type type = Type::Message;
    PeerId peer = 0;
    Channel channel = Channel::Ordered;                   // Message only
    std::vector<uint8_t> payload;                         // Message only
    Hello remoteHello;                                    // Connected only
    DisconnectReason reason = DisconnectReason::Normal;   // Disconnected only
    HandshakeError handshakeError = HandshakeError::None; // Disconnected with Refused
};

struct TransportConfig {
    Hello localHello;
    std::chrono::milliseconds handshakeTimeout{ 5000 };
    size_t maxPeers = 4;
};

// IPv4 addresses of this machine (loopback excluded), to show the host which
// address to give to friends. Empty if the lookup fails. LAN addresses only:
// the public (internet) address is only known to the router.
std::vector<std::string> LocalIPv4Addresses();

class Transport {
  public:
    // port 0 lets the OS pick a free port (see LocalPort()). nullptr on failure.
    static std::unique_ptr<Transport> Listen(uint16_t port, const TransportConfig& config);
    // Returns immediately; the outcome arrives later as Connected or Disconnected.
    static std::unique_ptr<Transport> Connect(const std::string& host, uint16_t port, const TransportConfig& config);

    ~Transport();
    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    // Appends every event received since the last call. Never blocks.
    void Poll(std::vector<TransportEvent>& out);

    // False if the peer is unknown or has not completed the handshake.
    // Queued only: packets leave on the next Poll() or Flush().
    bool Send(PeerId peer, Channel channel, std::span<const uint8_t> payload);

    // Sends queued packets now (call after the frame's Send()s).
    void Flush();

    // Graceful: queued messages are delivered first. The local Disconnected
    // event (reason Normal) is returned by the next Poll().
    void Disconnect(PeerId peer);

    uint16_t LocalPort() const;
    size_t ReadyPeerCount() const;

    // Most recent round-trip time measured by ENet, in ms. ENet pings every
    // 500 ms and starts at a default of 500 ms, so the first second is not
    // meaningful. 0 for an unknown peer.
    uint32_t RoundTripMs(PeerId peer) const;
    // Everything sent / received since this transport started, in bytes (for the Network tab)
    uint64_t BytesSent() const;
    uint64_t BytesReceived() const;

    // ENet's disconnect payload is a single integer: reason in the low byte,
    // handshake error in the next one. Exposed for tests and diagnostics.
    static uint32_t EncodeDisconnectData(DisconnectReason reason, HandshakeError error);
    static void DecodeDisconnectData(uint32_t data, DisconnectReason& reason, HandshakeError& error);

  private:
    struct PeerState {
        ENetPeer* enet = nullptr;
        bool ready = false;   // handshake completed, Connected emitted
        bool closing = false; // Disconnected already emitted locally
        std::chrono::steady_clock::time_point connectedAt;
    };

    Transport(ENetHost* host, const TransportConfig& config);

    PeerId Track(ENetPeer* enet);
    PeerId IdOf(ENetPeer* enet) const;
    void SendHello(ENetPeer* enet);
    void CloseLocally(PeerId id, DisconnectReason reason, HandshakeError error, std::vector<TransportEvent>& out);
    void OnReceive(PeerId id, uint8_t channel, std::span<const uint8_t> data, std::vector<TransportEvent>& out);

    ENetHost* mHost = nullptr;
    TransportConfig mConfig;
    PeerId mNextId = 1;
    std::unordered_map<PeerId, PeerState> mPeers;
    std::vector<TransportEvent> mPending; // produced outside Poll(), delivered by it
};

} // namespace Coop
