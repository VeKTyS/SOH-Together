#include "soh/Network/Coop/Session.h"

#include <algorithm>

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
            if (PeekType(ev.payload) != MessageType::PlayerState) {
                break; // unknown types are ignored: forward compatibility within a protocol version
            }
            RemotePlayer* player = Find(ev.peer);
            std::optional<PlayerState> state = DecodePlayerState(ev.payload);
            if (player != nullptr && state.has_value()) {
                player->state = *state;
                player->lastStateAt = std::chrono::steady_clock::now();
            }
            break;
        }
    }
}

void Session::Tick(const PlayerState& local) {
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
    for (RemotePlayer& player : mPlayers) {
        mTransport->Send(player.peer, Channel::Snapshot, bytes);
        player.pingMs = mTransport->RoundTripMs(player.peer);
    }
    // Without this, this frame's state would wait for next frame's Poll(): +50 ms.
    mTransport->Flush();
}

} // namespace Coop
