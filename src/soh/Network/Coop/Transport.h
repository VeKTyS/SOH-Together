#pragma once

// Sends and receives packets over UDP. ENet does the heavy lifting, but the
// rest of the code never sees it (easier to swap later if we need to).
// Call Poll() once per frame, from the game thread only.
//
// Someone only counts as "connected" once both sides swapped a matching
// Hello. Before that, we don't accept anything else from them.

#include "soh/Network/Coop/Handshake.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
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

// An IPv4 address and a UDP port, like 203.0.113.5:27720
struct UdpEndpoint {
    std::string ip;
    uint16_t port = 0;

    std::string Text() const {
        return ip + ":" + std::to_string(port);
    }
    bool operator==(const UdpEndpoint&) const = default;
};

// "1.2.3.4:5" -> endpoint. Numbers only (no host names), port 1-65535.
bool ParseUdpEndpoint(const std::string& text, UdpEndpoint& out);

// Little helpers for room codes / same wifi (CoopLobby.cpp). They're here and not in
// CoopLobby.cpp so the tests can reach them without the game.
namespace Nat {

// STUN (RFC 5389): ask a public server "what address do you see me as?".
// Copied from Crests of Courage online.cpp (send_stun_requests / parse_stun).
inline constexpr uint32_t kStunCookie = 0x2112A442u;
using StunTxn = std::array<uint8_t, 12>;
std::vector<uint8_t> StunRequest(const StunTxn& txn);
// Cookie in the right spot and top bits clear. Doesn't check the transaction.
bool LooksLikeStun(std::span<const uint8_t> data);
StunTxn StunTxnOf(std::span<const uint8_t> data); // call only when LooksLikeStun()
// Reads a binding success reply: XOR-MAPPED-ADDRESS if there, else MAPPED-ADDRESS. IPv4 only.
bool ParseStunReply(std::span<const uint8_t> data, UdpEndpoint& mapped);

// Hole punching packets (Crests of Courage online.cpp send_punch / on_punch):
// "CPH1" + token (8 bytes, little endian) + kind. Exactly 13 bytes.
enum class PunchKind : uint8_t { Probe = 1, Answer = 2 };
inline constexpr size_t kPunchSize = 4 + 8 + 1;
std::vector<uint8_t> PunchPacket(uint64_t token, PunchKind kind);
bool ParsePunch(std::span<const uint8_t> data, uint64_t& token, PunchKind& kind);

// 16 hex digits -> token (online.cpp parse_hex_token)
bool ParseHexToken(const std::string& text, uint64_t& out);
// Upper case letters and digits only, the rest is dropped (mod.cpp normalize_room_code)
std::string NormalizeRoomCode(const std::string& typed);
inline constexpr size_t kRoomNameMin = 4;
inline constexpr size_t kRoomNameMax = 24;

// 10.x, 172.16-31.x, 192.168.x
bool IsPrivateIPv4(const std::string& ip);
// This PC's address on the home network (the one used to reach the internet), "" if it's
// not a private one. Sends nothing. Copied from Crests of Courage lan.cpp.
std::string LanAddress();
// Our Tailscale address (100.64.0.0/10), "" if Tailscale is off (lan.cpp too).
std::string TailscaleAddress();

// "Find games on my wifi": a broadcast probe, the host answers with its port and name.
// Our own (Crests of Courage only finds same-wifi players through the room server).
std::vector<uint8_t> LanProbe();
bool IsLanProbe(std::span<const uint8_t> data);
std::vector<uint8_t> LanAnswer(uint16_t protocol, uint16_t port, const std::string& name);
bool ParseLanAnswer(std::span<const uint8_t> data, uint16_t& protocol, uint16_t& port, std::string& name);

} // namespace Nat

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

    // --- our own little packets on the game socket (room codes, same wifi: CoopLobby.cpp) ---
    // Crests of Courage (online.cpp) asks STUN and punches the NAT from the same UDP socket
    // the game then plays on, because a NAT hole only works for the socket that made it.
    // So these let CoopLobby send and read its packets on the ENet socket, next to ENet's.

    // Gets every datagram that lands on a game socket, before ENet. Return true if it was
    // yours (ENet never sees it then). Runs inside Poll() / PollJoinSocket(), game thread.
    using RawHandler = std::function<bool(const UdpEndpoint& from, std::span<const uint8_t> data)>;
    static void SetRawHandler(RawHandler handler);
    // Sends from the join socket if it's open, else from the newest Transport. False if none.
    static bool SendRaw(const UdpEndpoint& to, std::span<const uint8_t> data);
    // Same, from this transport's socket.
    bool SendRawFrom(const UdpEndpoint& to, std::span<const uint8_t> data);

    // Joiner side: opens the socket the next Connect() will use (any free port), so STUN
    // and the punch happen on it before we know where to connect. Can broadcast (same wifi).
    // Returns its port, 0 on failure. Opening it twice keeps the first one.
    static uint16_t OpenJoinSocket();
    static void CloseJoinSocket();
    static bool JoinSocketOpen();
    // Reads what landed on the join socket and gives it to the raw handler (call every frame).
    static void PollJoinSocket();

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
