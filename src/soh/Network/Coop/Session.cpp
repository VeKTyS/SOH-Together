#include "soh/Network/Coop/Session.h"

#include <algorithm>
#include <utility>

namespace Coop {

std::string DescribeDisconnect(DisconnectReason reason, HandshakeError error) {
    switch (reason) {
        case DisconnectReason::Normal:
            return "Disconnected";
        case DisconnectReason::Refused:
            return std::string("Refused: ") + Describe(error);
        case DisconnectReason::ProtocolViolation:
            return "Refused: the other side is not a SOH-Together game";
        case DisconnectReason::HandshakeTimeout:
            return "The other side did not answer the handshake";
        case DisconnectReason::ConnectionLost:
            return "Connection lost (host unreachable, firewall or timeout)";
    }
    return "Disconnected";
}

const char* TeleportBlocker(const PlayerState& me, const PlayerState& them, int32_t entranceCount) {
    if (!me.inGame) {
        return "Load your save first";
    }
    if (!them.inGame) {
        return "They're not in game";
    }
    if (them.entranceIndex < 0 || them.entranceIndex >= entranceCount || them.roomNum < 0) {
        return "Not sure where they are yet, try again in a sec";
    }
    return nullptr;
}

bool ParseAddress(const std::string& text, std::string& host, uint16_t& port) {
    size_t begin = text.find_first_not_of(" \t");
    size_t end = text.find_last_not_of(" \t");
    if (begin == std::string::npos) {
        return false;
    }
    std::string trimmed = text.substr(begin, end - begin + 1);

    size_t colon = trimmed.rfind(':');
    host = trimmed.substr(0, colon);
    port = kDefaultPort;
    if (colon != std::string::npos) {
        std::string digits = trimmed.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos) {
            return false;
        }
        unsigned long value = std::stoul(digits);
        if (value == 0 || value > 65535) {
            return false;
        }
        port = static_cast<uint16_t>(value);
    }
    return !host.empty() && host.find_first_of(" \t:") == std::string::npos;
}

Session::Session(Logger logger) : mLogger(std::move(logger)) {
}

Session::~Session() {
    // At process exit the logging backend may already be destroyed.
    mLogger = nullptr;
    Stop();
}

void Session::Log(const std::string& line) const {
    if (mLogger) {
        mLogger(line);
    }
}

bool Session::Host(uint16_t port, const Hello& local) {
    Stop();
    TransportConfig config;
    config.localHello = local;
    mTransport = Transport::Listen(port, config);
    if (!mTransport) {
        mLastError = "Cannot open UDP port " + std::to_string(port) + " (already in use?)";
        Log(mLastError);
        return false;
    }
    mRole = SessionRole::Hosting;
    mLastError.clear();
    Log("Hosting on UDP port " + std::to_string(mTransport->LocalPort()));
    return true;
}

bool Session::Join(const std::string& host, uint16_t port, const Hello& local) {
    Stop();
    TransportConfig config;
    config.localHello = local;
    mTransport = Transport::Connect(host, port, config);
    if (!mTransport) {
        mLastError = "Invalid address: " + host;
        Log(mLastError);
        return false;
    }
    mRole = SessionRole::Connecting;
    mLastError.clear();
    Log("Connecting to " + host + ":" + std::to_string(port));
    return true;
}

void Session::Stop() {
    if (mTransport) {
        Log("Session stopped");
    }
    mTransport.reset(); // the destructor notifies every peer
    mPlayers.clear();
    mRole = SessionRole::Off;
    mFlagChangesIn.clear();
    mFlagSnapshotIn.reset();
    mSnapshotRequestsIn.clear();
}

uint16_t Session::Port() const {
    return mRole == SessionRole::Hosting && mTransport ? mTransport->LocalPort() : 0;
}

RemotePlayer* Session::Find(PeerId peer) {
    auto it = std::find_if(mPlayers.begin(), mPlayers.end(), [&](const RemotePlayer& p) { return p.peer == peer; });
    return it == mPlayers.end() ? nullptr : &*it;
}

void Session::HandleEvent(TransportEvent& ev) {
    switch (ev.type) {
        case TransportEvent::Type::Connected: {
            RemotePlayer player;
            player.peer = ev.peer;
            player.name = ev.remoteHello.playerName;
            mPlayers.push_back(std::move(player));
            if (mRole == SessionRole::Connecting) {
                mRole = SessionRole::Connected;
            }
            Log("Player joined: " + ev.remoteHello.playerName);
            break;
        }
        case TransportEvent::Type::Disconnected: {
            RemotePlayer* player = Find(ev.peer);
            std::string who = player != nullptr ? player->name : "peer";
            std::string why = DescribeDisconnect(ev.reason, ev.handshakeError);
            Log(who + " left: " + why);
            std::erase_if(mPlayers, [&](const RemotePlayer& p) { return p.peer == ev.peer; });
            // A client only has one peer, the host: losing it ends the session.
            if (mRole == SessionRole::Connecting || mRole == SessionRole::Connected) {
                mLastError = why;
                mTransport.reset();
                mRole = SessionRole::Off;
            }
            break;
        }
        case TransportEvent::Type::Message: {
            if (RemotePlayer* player = Find(ev.peer)) {
                HandleMessage(*player, ev.payload);
            }
            break;
        }
    }
}

void Session::HandleMessage(RemotePlayer& from, std::span<const uint8_t> payload) {
    switch (PeekType(payload).value_or(static_cast<MessageType>(0))) {
        case MessageType::PlayerState:
            if (std::optional<PlayerState> state = DecodePlayerState(payload)) {
                from.state = *state;
                from.lastStateAt = std::chrono::steady_clock::now();
                if (!state->inGame) {
                    from.pose.reset(); // back on the title screen: nothing to draw
                }
            }
            break;
        case MessageType::PlayerPose:
            if (std::optional<PlayerPose> pose = DecodePlayerPose(payload)) {
                from.pose = *pose;
            }
            break;
        case MessageType::FlagChange:
            if (std::optional<FlagChange> change = DecodeFlagChange(payload)) {
                mFlagChangesIn.push_back(*change);
                // host: pass it on to the other clients (not back to the sender)
                if (mRole == SessionRole::Hosting) {
                    for (const RemotePlayer& other : mPlayers) {
                        if (other.peer != from.peer) {
                            SendTo(other.peer, Channel::Ordered, payload);
                        }
                    }
                }
            }
            break;
        case MessageType::FlagSnapshotRequest:
            if (mRole == SessionRole::Hosting) {
                mSnapshotRequestsIn.push_back(from.peer);
            }
            break;
        case MessageType::FlagSnapshot:
            // only the host's flags count
            if (mRole == SessionRole::Connected) {
                if (std::optional<FlagSnapshot> snapshot = DecodeFlagSnapshot(payload)) {
                    mFlagSnapshotIn = std::move(*snapshot);
                }
            }
            break;
        default:
            break; // unknown: ignore (shouldn't happen within one protocol version)
    }
}

void Session::SendTo(PeerId peer, Channel channel, std::span<const uint8_t> bytes) {
    if (mTransport) {
        mTransport->Send(peer, channel, bytes);
    }
}

void Session::SendFlagChange(const FlagChange& change) {
    std::vector<uint8_t> bytes = EncodeFlagChange(change);
    for (const RemotePlayer& p : mPlayers) {
        SendTo(p.peer, Channel::Ordered, bytes);
    }
}

void Session::RequestFlagSnapshot() {
    if (mRole == SessionRole::Connected && !mPlayers.empty()) {
        SendTo(mPlayers[0].peer, Channel::Ordered, EncodeFlagSnapshotRequest());
    }
}

void Session::SendFlagSnapshot(PeerId peer, const FlagSnapshot& snapshot) {
    if (mRole == SessionRole::Hosting) {
        SendTo(peer, Channel::Ordered, EncodeFlagSnapshot(snapshot));
    }
}

std::vector<FlagChange> Session::TakeFlagChanges() {
    return std::exchange(mFlagChangesIn, {});
}

std::optional<FlagSnapshot> Session::TakeFlagSnapshot() {
    return std::exchange(mFlagSnapshotIn, std::nullopt);
}

std::vector<PeerId> Session::TakeSnapshotRequests() {
    return std::exchange(mSnapshotRequestsIn, {});
}

void Session::Tick(const PlayerState& local, const PlayerPose* pose) {
    if (!mTransport) {
        return;
    }
    mEvents.clear();
    mTransport->Poll(mEvents);
    for (TransportEvent& ev : mEvents) {
        HandleEvent(ev);
        if (!mTransport) {
            return; // the session ended while handling this event
        }
    }

    std::vector<uint8_t> bytes = EncodePlayerState(local);
    std::vector<uint8_t> poseBytes;
    if (pose != nullptr) {
        poseBytes = EncodePlayerPose(*pose);
    }
    for (RemotePlayer& player : mPlayers) {
        mTransport->Send(player.peer, Channel::Snapshot, bytes);
        if (!poseBytes.empty()) {
            mTransport->Send(player.peer, Channel::Snapshot, poseBytes);
        }
        player.pingMs = mTransport->RoundTripMs(player.peer);
    }
    // Without this, this frame's state would wait for next frame's Poll(): +50 ms.
    mTransport->Flush();
}

} // namespace Coop
