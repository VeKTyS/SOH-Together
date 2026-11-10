// Room codes, opening the port with UPnP, same wifi games.
// Ported from Crests of Courage (MIT): online.cpp (room codes + hole punching), upnp.cpp
// (open the port), lan.cpp (our local address) and the texts of menu.cpp.
//
// How a room code connects two players without port forwarding (same as Crests of Courage):
//  1. The host keeps a WebSocket open to the room server (server/ folder), which gives it a code.
//  2. A joiner types the code. Both ask a public STUN server "what address do you see me as?",
//     from the game socket, and tell the room server. The server gives each side the other's
//     addresses (public, local wifi, Tailscale...) and a token.
//  3. Both fire tiny "punch" packets at each other for a few seconds. A router lets answers in
//     once we sent something out, so the packets end up getting through. As soon as the joiner
//     hears the host, it starts the normal ENet join to that address, from that same socket.
// No game traffic goes through the server, there's no relay. If both networks are too strict
// (two symmetric NATs), it fails and the help text points to Tailscale, like Crests of Courage.
//
// Sockets first: winsock2.h must come before anything that pulls windows.h.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// UPnP needs miniupnpc (cmake/coop-inject.cmake). Without it, "Open the port for me" just says so.
#if __has_include(<miniupnpc/miniupnpc.h>)
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#define COOP_HAVE_UPNP 1
#elif __has_include(<miniupnpc.h>)
#include <miniupnpc.h>
#include <upnpcommands.h>
#define COOP_HAVE_UPNP 1
#else
#define COOP_HAVE_UPNP 0
#endif

#include "soh/Network/Coop/CoopGame.h"

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/SohGui/SohGui.hpp"
#include "soh/SohGui/UIWidgets.hpp"

#include <libultraship/bridge/consolevariablebridge.h>
#include <nlohmann/json.hpp>
#include <ship/Context.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---- settings (Crests of Courage mod.cpp register_var, same defaults) ----
// The port to host on is the one the Host button already uses: CVAR_COOP("Port").
#define LOBBY_CVAR(x) CVAR_COOP("Lobby." x)
const char* const kServerCVar = LOBBY_CVAR("Server");       // room_server. Empty = no room codes
const char* const kRoomCVar = LOBBY_CVAR("Room");           // room_code
const char* const kRoomCodesCVar = LOBBY_CVAR("RoomCodes"); // room_codes, on
const char* const kOpenPortCVar = LOBBY_CVAR("OpenPort");   // open_port_automatically, on
const char* const kOnLaunchCVar = LOBBY_CVAR("OnLaunch");   // auto_connect, off
const char* const kModeCVar = LOBBY_CVAR("Mode");           // mode: host / join / join_code
const char* const kHostKeyCVar = LOBBY_CVAR("HostKey");     // room_host_key (lets you get your room back)

void SaveCVars() {
    Ship::Context::GetRawInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::mt19937_64& Rng() {
    static std::mt19937_64 engine{ std::random_device{}() ^ (NowMs() << 21) };
    return engine;
}

// =====================================================================================
// A tiny WebSocket client (ws:// only, no TLS). Crests of Courage gets one from its mod
// loader; SoH has none, so here's the minimum: one thread per connection doing the
// blocking bits (name lookup, connect, reads), the game thread only swaps messages.
// =====================================================================================

#ifdef _WIN32
using RawSocket = SOCKET;
const RawSocket kNoSocket = INVALID_SOCKET;
void CloseSocket(RawSocket s) {
    closesocket(s);
}
bool SetNonBlocking(RawSocket s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
bool WouldBlock() {
    int e = WSAGetLastError();
    return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS;
}
#else
using RawSocket = int;
const RawSocket kNoSocket = -1;
void CloseSocket(RawSocket s) {
    close(s);
}
bool SetNonBlocking(RawSocket s) {
    int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
bool WouldBlock() {
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINPROGRESS;
}
#endif

const size_t kWsMaxMessage = 16 * 1024; // like online.cpp maxMessageBytes
const int kWsConnectTimeoutMs = 8000;   // online.cpp connectTimeoutMs

struct WsEvent {
    enum class Type : uint8_t { Open, Text, Closed };
    Type type = Type::Text;
    std::string text;   // Text: the message. Closed: why
    int httpStatus = 0; // Closed: the server's answer to the upgrade, if it said no
};

struct WsShared {
    std::mutex mutex;
    std::deque<WsEvent> inbox;
    std::deque<std::string> outbox;
    bool stop = false;
};

// 1 = ready, 0 = not yet, -1 = error. Windows reports a failed connect in "except".
int WaitSocket(RawSocket s, bool forWrite, int ms) {
    fd_set set;
    fd_set except;
    FD_ZERO(&set);
    FD_ZERO(&except);
    FD_SET(s, &set);
    FD_SET(s, &except);
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    int r = select(static_cast<int>(s) + 1, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &except, &tv);
    if (r < 0 || FD_ISSET(s, &except)) {
        return -1;
    }
    return r > 0 ? 1 : 0;
}

std::string Base64(const uint8_t* data, size_t size) {
    static const char* const kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        uint32_t n = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) {
            n |= static_cast<uint32_t>(data[i + 1]) << 8;
        }
        if (i + 2 < size) {
            n |= data[i + 2];
        }
        out += kAlphabet[(n >> 18) & 63];
        out += kAlphabet[(n >> 12) & 63];
        out += i + 1 < size ? kAlphabet[(n >> 6) & 63] : '=';
        out += i + 2 < size ? kAlphabet[n & 63] : '=';
    }
    return out;
}

class WsWorker {
  public:
    WsWorker(std::shared_ptr<WsShared> shared, std::string host, uint16_t port, std::string path)
        : mShared(std::move(shared)), mHost(std::move(host)), mPort(port), mPath(std::move(path)) {
    }

    void Run() {
#ifdef _WIN32
        WSADATA wsa;
        bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
        WsEvent closed;
        closed.type = WsEvent::Type::Closed;
        closed.text = "could not reach the room server";
        if (Connect() && Upgrade(closed)) {
            Push({ WsEvent::Type::Open, "", 0 });
            Loop(closed);
        }
        if (mSocket != kNoSocket) {
            CloseSocket(mSocket);
        }
#ifdef _WIN32
        if (wsaOk) {
            WSACleanup();
        }
#endif
        if (!Stopped()) {
            Push(std::move(closed));
        }
    }

  private:
    bool Stopped() {
        std::lock_guard lock(mShared->mutex);
        return mShared->stop;
    }

    void Push(WsEvent ev) {
        std::lock_guard lock(mShared->mutex);
        mShared->inbox.push_back(std::move(ev));
    }

    bool Connect() {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* list = nullptr;
        if (getaddrinfo(mHost.c_str(), std::to_string(mPort).c_str(), &hints, &list) != 0 || list == nullptr) {
            return false;
        }
        mSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool ok = mSocket != kNoSocket && SetNonBlocking(mSocket);
        if (ok && connect(mSocket, list->ai_addr, static_cast<int>(list->ai_addrlen)) != 0) {
            ok = WouldBlock();
            int waited = 0;
            int ready = 0;
            while (ok && ready == 0 && waited < kWsConnectTimeoutMs && !Stopped()) {
                ready = WaitSocket(mSocket, true, 100);
                waited += 100;
            }
            int error = 0;
#ifdef _WIN32
            int length = sizeof(error);
#else
            socklen_t length = sizeof(error);
#endif
            ok = ok && ready == 1 &&
                 getsockopt(mSocket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &length) == 0 && error == 0;
        }
        freeaddrinfo(list);
        return ok;
    }

    bool SendAll(const std::string& data) {
        size_t sent = 0;
        int waited = 0;
        while (sent < data.size()) {
            int n = send(mSocket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
            if (n > 0) {
                sent += static_cast<size_t>(n);
                continue;
            }
            if (!WouldBlock() || waited > kWsConnectTimeoutMs || WaitSocket(mSocket, true, 100) < 0) {
                return false;
            }
            waited += 100;
        }
        return true;
    }

    // HTTP "please switch to WebSocket". True once the server said 101.
    bool Upgrade(WsEvent& closed) {
        uint8_t keyBytes[16];
        for (uint8_t& b : keyBytes) {
            b = static_cast<uint8_t>(mRng());
        }
        std::string request = "GET " + mPath + " HTTP/1.1\r\nHost: " + mHost + ":" + std::to_string(mPort) +
                              "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
                              Base64(keyBytes, sizeof(keyBytes)) + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!SendAll(request)) {
            return false;
        }
        int waited = 0;
        size_t end;
        while ((end = mBuffer.find("\r\n\r\n")) == std::string::npos) {
            if (mBuffer.size() > 8192 || waited > kWsConnectTimeoutMs || Stopped()) {
                return false;
            }
            int ready = WaitSocket(mSocket, false, 100);
            waited += 100;
            if (ready < 0 || (ready > 0 && !ReadSome())) {
                return false;
            }
        }
        // "HTTP/1.1 101 Switching Protocols"
        size_t space = mBuffer.find(' ');
        int status = space == std::string::npos ? 0 : std::atoi(mBuffer.c_str() + space + 1);
        mBuffer.erase(0, end + 4); // anything after the headers is already WebSocket data
        if (status != 101) {
            closed.httpStatus = status;
            closed.text = "the room server answered " + std::to_string(status);
            return false;
        }
        return true;
    }

    bool ReadSome() {
        char chunk[4096];
        int n = recv(mSocket, chunk, sizeof(chunk), 0);
        if (n > 0) {
            mBuffer.append(chunk, static_cast<size_t>(n));
            return true;
        }
        return n < 0 && WouldBlock();
    }

    // Client frames must be masked (RFC 6455)
    bool SendFrame(uint8_t opcode, const std::string& payload) {
        std::string frame;
        frame += static_cast<char>(0x80 | opcode);
        size_t n = payload.size();
        if (n < 126) {
            frame += static_cast<char>(0x80 | n);
        } else if (n < 65536) {
            frame += static_cast<char>(0x80 | 126);
            frame += static_cast<char>(n >> 8);
            frame += static_cast<char>(n & 0xFF);
        } else {
            frame += static_cast<char>(0x80 | 127);
            for (int i = 7; i >= 0; i--) {
                frame += static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF);
            }
        }
        uint8_t mask[4];
        for (uint8_t& b : mask) {
            b = static_cast<uint8_t>(mRng());
            frame += static_cast<char>(b);
        }
        for (size_t i = 0; i < n; i++) {
            frame += static_cast<char>(payload[i] ^ mask[i % 4]);
        }
        return SendAll(frame);
    }

    // Reads whole frames out of mBuffer. False when the connection is over.
    bool ParseFrames(WsEvent& closed) {
        while (mBuffer.size() >= 2) {
            const uint8_t b0 = static_cast<uint8_t>(mBuffer[0]);
            const uint8_t b1 = static_cast<uint8_t>(mBuffer[1]);
            const bool fin = (b0 & 0x80) != 0;
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            uint64_t length = b1 & 0x7F;
            size_t at = 2;
            if (length == 126) {
                if (mBuffer.size() < 4) {
                    return true;
                }
                length =
                    (static_cast<uint64_t>(static_cast<uint8_t>(mBuffer[2])) << 8) | static_cast<uint8_t>(mBuffer[3]);
                at = 4;
            } else if (length == 127) {
                if (mBuffer.size() < 10) {
                    return true;
                }
                length = 0;
                for (int i = 0; i < 8; i++) {
                    length = (length << 8) | static_cast<uint8_t>(mBuffer[2 + i]);
                }
                at = 10;
            }
            if (length > kWsMaxMessage || mFragment.size() + length > kWsMaxMessage) {
                closed.text = "the room server sent too much";
                return false;
            }
            const size_t maskAt = at;
            if (masked) {
                at += 4;
            }
            if (mBuffer.size() < at + length) {
                return true;
            }
            std::string payload = mBuffer.substr(at, static_cast<size_t>(length));
            if (masked) {
                for (size_t i = 0; i < payload.size(); i++) {
                    payload[i] = static_cast<char>(payload[i] ^ mBuffer[maskAt + (i % 4)]);
                }
            }
            mBuffer.erase(0, at + static_cast<size_t>(length));

            switch (opcode) {
                case 0x0: // rest of a split message
                case 0x1: // text
                    mFragment += payload;
                    if (fin) {
                        Push({ WsEvent::Type::Text, std::move(mFragment), 0 });
                        mFragment.clear();
                    }
                    break;
                case 0x8: { // the server closes
                    std::string reason = payload.size() > 2 ? payload.substr(2) : "";
                    SendFrame(0x8, payload.substr(0, std::min<size_t>(payload.size(), 2)));
                    closed.text = reason.empty() ? "the room server closed the connection" : reason;
                    return false;
                }
                case 0x9: // ping
                    SendFrame(0xA, payload);
                    break;
                default: // pong, binary: nothing to do
                    break;
            }
        }
        return true;
    }

    void Loop(WsEvent& closed) {
        uint64_t lastPing = NowMs();
        // Crests of Courage holds a sleeping socket; we ping every 30 s so home routers
        // don't forget an idle connection while the host waits for players.
        while (!Stopped()) {
            std::deque<std::string> out;
            {
                std::lock_guard lock(mShared->mutex);
                out.swap(mShared->outbox);
            }
            for (const std::string& text : out) {
                if (!SendFrame(0x1, text)) {
                    return;
                }
            }
            if (NowMs() - lastPing > 30000) {
                lastPing = NowMs();
                if (!SendFrame(0x9, "")) {
                    return;
                }
            }
            int ready = WaitSocket(mSocket, false, 50);
            if (ready < 0) {
                return;
            }
            if (ready > 0) {
                if (!ReadSome()) {
                    closed.text = "the room server closed the connection";
                    return;
                }
                if (!ParseFrames(closed)) {
                    return;
                }
            }
        }
        // we're the ones leaving: say goodbye properly (code 1000, "done")
        SendFrame(0x8, std::string("\x03\xE8", 2) + "done");
    }

    std::shared_ptr<WsShared> mShared;
    std::string mHost;
    uint16_t mPort;
    std::string mPath;
    RawSocket mSocket = kNoSocket;
    std::string mBuffer;
    std::string mFragment;
    std::mt19937 mRng{ std::random_device{}() }; // our own: Rng() belongs to the game thread
};

// "ws://host:port/prefix" -> parts. wss:// needs TLS, which we don't have.
bool ParseWsUrl(std::string url, std::string& host, uint16_t& port, std::string& prefix, std::string& error) {
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) {
        url.pop_back();
    }
    while (!url.empty() && url.front() == ' ') {
        url.erase(url.begin());
    }
    if (url.rfind("wss://", 0) == 0 || url.rfind("https://", 0) == 0) {
        error = "This version can't use secure (wss://) room servers yet. Use a ws:// address.";
        return false;
    }
    if (url.rfind("ws://", 0) == 0) {
        url.erase(0, 5);
    } else if (url.rfind("http://", 0) == 0) {
        url.erase(0, 7);
    } else if (url.find("://") != std::string::npos) {
        error = "The room server address should look like ws://203.0.113.5:8787";
        return false;
    }
    size_t slash = url.find('/');
    std::string hostPort = url.substr(0, slash);
    prefix = slash == std::string::npos ? "" : url.substr(slash);
    size_t colon = hostPort.rfind(':');
    host = hostPort.substr(0, colon);
    port = 80;
    if (colon != std::string::npos) {
        std::string digits = hostPort.substr(colon + 1);
        if (digits.empty() || digits.size() > 5 || digits.find_first_not_of("0123456789") != std::string::npos ||
            std::stoul(digits) == 0 || std::stoul(digits) > 65535) {
            error = "The room server address has a bad port";
            return false;
        }
        port = static_cast<uint16_t>(std::stoul(digits));
    }
    if (host.empty()) {
        error = "The room server address is empty";
        return false;
    }
    return true;
}

// The game thread's side of a connection. Never blocks.
class WebSocket {
  public:
    // base = the room server address, path = "/host?..." or "/join/CODE"
    bool Open(const std::string& base, const std::string& path, std::string& error) {
        Close();
        std::string host, prefix;
        uint16_t port = 0;
        if (!ParseWsUrl(base, host, port, prefix, error)) {
            return false;
        }
        mShared = std::make_shared<WsShared>();
        auto worker = std::make_shared<WsWorker>(mShared, host, port, prefix + path);
        // detached: Close() never waits for a slow name lookup or connect
        std::thread([worker] { worker->Run(); }).detach();
        return true;
    }

    void Close() {
        if (mShared != nullptr) {
            std::lock_guard lock(mShared->mutex);
            mShared->stop = true;
        }
        mShared = nullptr;
    }

    explicit operator bool() const {
        return mShared != nullptr;
    }

    void SendText(std::string text) {
        if (mShared != nullptr) {
            std::lock_guard lock(mShared->mutex);
            mShared->outbox.push_back(std::move(text));
        }
    }

    bool Poll(WsEvent& out) {
        if (mShared == nullptr) {
            return false;
        }
        std::lock_guard lock(mShared->mutex);
        if (mShared->inbox.empty()) {
            return false;
        }
        out = std::move(mShared->inbox.front());
        mShared->inbox.pop_front();
        return true;
    }

  private:
    std::shared_ptr<WsShared> mShared;
};

// =====================================================================================
// UPnP: ask the router to open our port while we host. Crests of Courage upnp.cpp talks
// SSDP/SOAP by hand; we let miniupnpc do that, on a thread because it blocks (~2 s).
// Same steps, same texts, same 1 hour lease renewed every 45 minutes. UDP only (ENet).
// =====================================================================================

struct UpnpShared {
    std::mutex mutex;
    std::condition_variable wake;
    bool release = false;
    bool ready = false;
    std::string status;
    std::string external; // "ip:port" others can use, once ready
};
std::shared_ptr<UpnpShared> gUpnp;

#if COOP_HAVE_UPNP
void RunUpnp(std::shared_ptr<UpnpShared> s, uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    bool wsaOk = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
    auto fail = [&](const char* why) {
        std::lock_guard lock(s->mutex);
        s->status = why;
        SPDLOG_INFO("[Coop] [UPNP] {}", why);
    };
    const std::string portText = std::to_string(port);
    int error = 0;
    SPDLOG_INFO("[Coop] [UPNP] looking for a gateway on the network");
    UPNPDev* devices = upnpDiscover(2000, nullptr, nullptr, 0, 0, 2, &error);
    if (devices == nullptr) {
        fail("No router here answered. Open the port yourself, or use Tailscale.");
    } else {
        UPNPUrls urls{};
        IGDdatas data{};
        char lan[64] = {};
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
        char wan[64] = {};
        int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof(lan), wan, sizeof(wan));
#else
        int igd = UPNP_GetValidIGD(devices, &urls, &data, lan, sizeof(lan));
#endif
        freeUPNPDevlist(devices);
        auto map = [&] {
            return UPNP_AddPortMapping(urls.controlURL, data.first.servicetype, portText.c_str(), portText.c_str(), lan,
                                       "SOH-Together", "UDP", nullptr, "3600");
        };
        char external[64] = {};
        if (igd != 1) {
#if defined(MINIUPNPC_API_VERSION) && MINIUPNPC_API_VERSION >= 18
            // 2 = the router itself is behind another one (or the ISP's): opening ours won't help
            fail(igd == 2 ? "Your router is behind another one, so opening the port won't help. Use Tailscale."
                          : "Your router does not offer to open ports.");
#else
            fail("Your router does not offer to open ports.");
#endif
        } else if (map() != UPNPCOMMAND_SUCCESS) {
            fail("Your router refused to open the port.");
        } else if (UPNP_GetExternalIPAddress(urls.controlURL, data.first.servicetype, external) !=
                       UPNPCOMMAND_SUCCESS ||
                   external[0] == '\0') {
            fail("The port is open, but your router would not say what your address is.");
            UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, portText.c_str(), "UDP", nullptr);
        } else {
            std::unique_lock lock(s->mutex);
            s->external = std::string(external) + ":" + portText;
            s->ready = true;
            s->status = "Your router opened the port.";
            SPDLOG_INFO("[Coop] [UPNP] port {} open", port);
            // keep it open until we stop hosting (the lease is 1 hour, renew every 45 minutes)
            while (!s->wake.wait_for(lock, std::chrono::minutes(45), [&] { return s->release; })) {
                lock.unlock();
                SPDLOG_INFO("[Coop] [UPNP] renewing mapping");
                map();
                lock.lock();
            }
            lock.unlock();
            // Crests of Courage only removes its TCP one; ours is UDP
            UPNP_DeletePortMapping(urls.controlURL, data.first.servicetype, portText.c_str(), "UDP", nullptr);
            SPDLOG_INFO("[Coop] [UPNP] port {} closed again", port);
        }
        if (igd != 0) {
            FreeUPNPUrls(&urls);
        }
    }
#ifdef _WIN32
    if (wsaOk) {
        WSACleanup();
    }
#endif
}
#endif

void UpnpRelease() {
    if (gUpnp != nullptr) {
        std::lock_guard lock(gUpnp->mutex);
        gUpnp->release = true;
        gUpnp->wake.notify_all();
    }
    gUpnp = nullptr;
}

void UpnpBegin(uint16_t port) {
    UpnpRelease();
    gUpnp = std::make_shared<UpnpShared>();
#if COOP_HAVE_UPNP
    gUpnp->status = "Asking your router to open the port...";
    std::thread([s = gUpnp, port] { RunUpnp(s, port); }).detach();
#else
    gUpnp->status = "This version can't open the port by itself.";
#endif
}

std::string UpnpExternalAddress() {
    if (gUpnp == nullptr) {
        return "";
    }
    std::lock_guard lock(gUpnp->mutex);
    return gUpnp->ready ? gUpnp->external : "";
}

std::string UpnpStatus() {
    if (gUpnp == nullptr) {
        return "";
    }
    std::lock_guard lock(gUpnp->mutex);
    return gUpnp->status;
}

// =====================================================================================
// Room codes: port of Crests of Courage online.cpp. Same phases, timings, messages.
// =====================================================================================

const char* const kTailscaleHint =
    "Your networks are too strict to connect directly. Use Tailscale instead. See How to play together.";

const uint64_t kWelcomeTimeoutMs = 10000;
const uint64_t kStunRetryMs = 300;
const uint64_t kStunGiveUpMs = 2500;
const uint64_t kStunEnoughMs = 1200;
const uint64_t kPeerTimeoutMs = 15000;
const uint64_t kPunchEveryMs = 100;
const uint64_t kJoinPunchMs = 8000;
const uint64_t kHostPunchMs = 15000;

enum class Phase {
    Idle,
    Connecting,
    Stun,
    Waiting,
    Punching,
    Done,
    Failed,
};

struct PendingPeer {
    uint64_t token = 0;
    std::vector<Coop::UdpEndpoint> candidates;
    std::optional<Coop::UdpEndpoint> heardFrom;
    uint64_t startedMs = 0;
    uint64_t lastProbeMs = 0;
};

struct StunAsk {
    Coop::Nat::StunTxn txn;
    Coop::UdpEndpoint server;
};

struct Online {
    bool host = false;
    bool named = false; // the host typed a room name (must get exactly that one)
    Phase phase = Phase::Idle;
    std::string status;
    std::string code;
    uint16_t localPort = 0;
    WebSocket ws;
    uint64_t phaseStartMs = 0;

    std::vector<Coop::UdpEndpoint> stunServers;
    std::vector<StunAsk> stunAsks;
    std::string mapped;
    std::string mappedOther;
    uint64_t lastStunSendMs = 0;
    uint64_t firstStunAnswerMs = 0;

    std::vector<PendingPeer> peers; // host: joiners on the way
    std::string sentUpnp;
    int reconnects = 0;
    uint64_t reconnectAtMs = 0;

    PendingPeer target; // joiner: the host
    std::string hostUpnp;
    bool sameNet = false;
    int punchLogs = 0;
};
Online gOn;
bool gJoinFromLobby = false; // the next "Connecting" comes from us (room code), not the Join button

void SetPhase(Phase phase, std::string status) {
    gOn.phase = phase;
    gOn.status = std::move(status);
    gOn.phaseStartMs = NowMs();
}

void CloseWs() {
    if (gOn.ws) {
        gOn.ws.Close();
    }
}

void OnlineStop() {
    CloseWs();
    gOn.phase = Phase::Idle;
    gOn.status.clear();
    gOn.peers.clear();
    gOn.stunAsks.clear();
    gOn.target = PendingPeer{};
    gOn.reconnectAtMs = 0;
    gOn.code.clear();
}

void SetMode(const char* mode) {
    CVarSetString(kModeCVar, mode);
    SaveCVars();
}

// coop_online_punched (mod.cpp): start the real join, from the socket that made the hole
void JoinPunched(const Coop::UdpEndpoint& host) {
    if (CoopGame::Session().Role() != Coop::SessionRole::Off) {
        return;
    }
    gJoinFromLobby = true;
    if (!CoopGame::StartJoin(host.ip, host.port)) {
        gJoinFromLobby = false;
        Coop::Transport::CloseJoinSocket();
        SetPhase(Phase::Failed, "Couldn't start networking");
    }
}

// coop_online_failed (mod.cpp): last try straight at the address the host's router opened
void JoinFailed(const std::string& why, const std::string& upnpFallback) {
    if (CoopGame::Session().Role() != Coop::SessionRole::Off) {
        return;
    }
    Coop::UdpEndpoint direct;
    if (!upnpFallback.empty() && Coop::ParseUdpEndpoint(upnpFallback, direct)) {
        SPDLOG_INFO("[Coop] [ONLINE] room punch failed, trying the host's opened port");
        OnlineStop();
        gJoinFromLobby = true;
        if (CoopGame::StartJoin(direct.ip, direct.port)) {
            return;
        }
        gJoinFromLobby = false;
    }
    Coop::Transport::CloseJoinSocket();
    SetPhase(Phase::Failed, why);
}

void JoinerGiveUp(const std::string& why) {
    CloseWs();
    const std::string upnp = gOn.hostUpnp;
    SetPhase(Phase::Failed, why);
    SPDLOG_INFO("[Coop] [ONLINE] giving up on the room: {}", why);
    JoinFailed(why, upnp);
}

void HostLostServer(const std::string& why) {
    CloseWs();
    if (gOn.reconnects < 6) {
        const uint64_t delays[] = { 2000, 5000, 10000, 20000, 30000, 60000 };
        gOn.reconnectAtMs = NowMs() + delays[gOn.reconnects];
        ++gOn.reconnects;
        SetPhase(Phase::Failed, "Lost the room server (" + why + "). Trying again...");
    } else {
        gOn.reconnectAtMs = 0;
        SetPhase(Phase::Failed, "Could not reach the room server. Players can still join by address.");
    }
    SPDLOG_INFO("[Coop] [ONLINE] {}", gOn.status);
}

void Fail(const std::string& why) {
    if (gOn.host) {
        HostLostServer(why);
    } else {
        JoinerGiveUp(why);
    }
}

std::string RoomServer() {
    std::string url = CVarGetString(kServerCVar, "");
    while (!url.empty() && (url.back() == '/' || url.back() == ' ')) {
        url.pop_back();
    }
    return url;
}

bool OpenWs(const std::string& path) {
    const std::string server = RoomServer();
    if (server.empty()) {
        SetPhase(Phase::Failed, "Room codes need a room server address.");
        return false;
    }
    std::string error;
    if (!gOn.ws.Open(server, path, error)) {
        SetPhase(Phase::Failed, error);
        return false;
    }
    SetPhase(Phase::Connecting, gOn.host ? "Getting a room code..." : "Looking for room " + gOn.code + "...");
    // not the full path: the host's one carries its room key
    SPDLOG_INFO("[Coop] [ONLINE] connecting to the room server");
    return true;
}

void SendJson(const nlohmann::json& msg) {
    if (gOn.ws) {
        gOn.ws.SendText(msg.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace));
    }
}

void SendStunRequests() {
    gOn.lastStunSendMs = NowMs();
    gOn.stunAsks.clear();
    for (const Coop::UdpEndpoint& server : gOn.stunServers) {
        StunAsk ask;
        for (uint8_t& b : ask.txn) {
            b = static_cast<uint8_t>(Rng()());
        }
        ask.server = server;
        Coop::Transport::SendRaw(server, Coop::Nat::StunRequest(ask.txn));
        gOn.stunAsks.push_back(ask);
    }
}

void SayHello() {
    nlohmann::json hello = {
        { "op", "hello" }, { "v", Coop::kProtocolVersion }, { "ep", gOn.mapped }, { "port", gOn.localPort }
    };
    const std::string lan = Coop::Nat::LanAddress();
    if (!lan.empty()) {
        hello["lan"] = lan;
    }
    SPDLOG_INFO("[Coop] [ONLINE] local address {}", lan.empty() ? "unknown" : "known");
    const std::string ts = Coop::Nat::TailscaleAddress();
    if (!ts.empty()) {
        hello["ts"] = ts;
    }
    SPDLOG_INFO("[Coop] [ONLINE] tailscale {}", ts.empty() ? "off" : "on");
    if (gOn.host) {
        gOn.sentUpnp = UpnpExternalAddress();
        hello["upnp"] = gOn.sentUpnp;
    }
    SendJson(hello);
    if (!gOn.mappedOther.empty() && gOn.mappedOther != gOn.mapped) {
        // two STUN servers saw two different ports: a strict ("symmetric") NAT, punching will likely fail
        SPDLOG_INFO("[Coop] [ONLINE] strict NAT, STUN servers saw two different ports");
    }
    if (gOn.host) {
        SetPhase(Phase::Waiting, "");
    } else {
        SetPhase(Phase::Waiting, "Found room " + gOn.code + ". Introducing you...");
    }
}

bool OnStun(std::span<const uint8_t> data) {
    const Coop::Nat::StunTxn txn = Coop::Nat::StunTxnOf(data);
    const auto ask =
        std::find_if(gOn.stunAsks.begin(), gOn.stunAsks.end(), [&](const StunAsk& a) { return a.txn == txn; });
    if (ask == gOn.stunAsks.end()) {
        return true;
    }
    Coop::UdpEndpoint mapped;
    if (!Coop::Nat::ParseStunReply(data, mapped)) {
        return true;
    }
    gOn.stunAsks.erase(ask);
    if (gOn.phase != Phase::Stun) {
        return true;
    }
    if (gOn.mapped.empty()) {
        gOn.mapped = mapped.Text();
        gOn.firstStunAnswerMs = NowMs();
        SPDLOG_INFO("[Coop] [ONLINE] a STUN server answered");
    } else if (gOn.mappedOther.empty()) {
        gOn.mappedOther = mapped.Text();
        SayHello();
    }
    return true;
}

void UpdateStun() {
    const uint64_t now = NowMs();
    if (!gOn.mapped.empty() && now - gOn.firstStunAnswerMs >= kStunEnoughMs) {
        SayHello();
        return;
    }
    if (now - gOn.phaseStartMs >= kStunGiveUpMs) {
        // the server still knows our public address from the WebSocket, it'll guess the port
        SPDLOG_INFO("[Coop] [ONLINE] no STUN answer, using server view");
        SayHello();
        return;
    }
    if (now - gOn.lastStunSendMs >= kStunRetryMs) {
        SendStunRequests();
    }
}

void SendPunch(const Coop::UdpEndpoint& to, uint64_t token, Coop::Nat::PunchKind kind) {
    Coop::Transport::SendRaw(to, Coop::Nat::PunchPacket(token, kind));
}

void Probe(PendingPeer& peer) {
    const uint64_t now = NowMs();
    if (now - peer.lastProbeMs < kPunchEveryMs) {
        return;
    }
    if (peer.lastProbeMs == 0) {
        SPDLOG_INFO("[Coop] [ONLINE] +{}ms probing {} addresses{}", now - peer.startedMs, peer.candidates.size(),
                    peer.heardFrom ? " + the one we heard from" : "");
    }
    // (Crests of Courage also drops 127.x when its socket keeps getting closed by bounces;
    // we told Windows to ignore bounces instead, see Transport.cpp CreateHost)
    peer.lastProbeMs = now;
    for (const Coop::UdpEndpoint& to : peer.candidates) {
        SendPunch(to, peer.token, Coop::Nat::PunchKind::Probe);
    }
    if (peer.heardFrom &&
        std::find(peer.candidates.begin(), peer.candidates.end(), *peer.heardFrom) == peer.candidates.end()) {
        SendPunch(*peer.heardFrom, peer.token, Coop::Nat::PunchKind::Probe);
    }
}

bool OnPunch(const Coop::UdpEndpoint& from, std::span<const uint8_t> data) {
    uint64_t token = 0;
    Coop::Nat::PunchKind kind;
    if (!Coop::Nat::ParsePunch(data, token, kind)) {
        return false;
    }
    if (gOn.punchLogs < 12) {
        ++gOn.punchLogs;
        SPDLOG_INFO("[Coop] [ONLINE] +{}ms punch {}", NowMs() - gOn.phaseStartMs,
                    kind == Coop::Nat::PunchKind::Probe ? "probe" : "answer");
    }

    if (gOn.host) {
        for (PendingPeer& p : gOn.peers) {
            if (p.token != token) {
                continue;
            }
            if (kind == Coop::Nat::PunchKind::Probe) {
                SendPunch(from, token, Coop::Nat::PunchKind::Answer);
            }
            if (!p.heardFrom) {
                SPDLOG_INFO("[Coop] [ONLINE] reached a joiner");
            }
            p.heardFrom = from;
        }
        return true;
    }

    if (gOn.phase != Phase::Punching || token != gOn.target.token) {
        return true;
    }
    if (kind == Coop::Nat::PunchKind::Probe) {
        SendPunch(from, token, Coop::Nat::PunchKind::Answer);
    }
    SPDLOG_INFO("[Coop] [ONLINE] reached the host");
    CloseWs();
    SetPhase(Phase::Done, "Found the host. Connecting...");
    JoinPunched(from);
    return true;
}

std::string ErrorText(const std::string& why, const std::string& detail) {
    if (why == "no_room") {
        return "There is no room " + gOn.code + ". Check the code with the host.";
    }
    if (why == "version") {
        const int theirs = std::atoi(detail.c_str());
        if (theirs > Coop::kProtocolVersion) {
            return "The host has a newer SOH-Together. Update to join.";
        }
        return "The host has an older SOH-Together. They need to update.";
    }
    if (why == "busy") {
        return "That room is busy. Try again in a moment.";
    }
    if (why == "taken") {
        return "Somebody is already using the room name " + gOn.code +
               ". Pick another, then stop hosting and host again.";
    }
    return "The room server said no (" + why + ").";
}

std::string Str(const nlohmann::json& msg, const char* key) {
    auto it = msg.find(key);
    return it != msg.end() && it->is_string() ? it->get<std::string>() : std::string();
}

void OnServerMessage(const std::string& text) {
    nlohmann::json msg = nlohmann::json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) {
        SPDLOG_WARN("[Coop] [ONLINE] unreadable message from the room server");
        return;
    }
    const std::string op = Str(msg, "op");

    if (op == "welcome") {
        if (gOn.host) {
            gOn.code = Str(msg, "code");
            gOn.reconnects = 0;
            SPDLOG_INFO("[Coop] [ONLINE] got a room code");
        }
        gOn.stunServers.clear();
        if (auto stun = msg.find("stun"); stun != msg.end() && stun->is_array()) {
            for (const auto& entry : *stun) {
                Coop::UdpEndpoint server;
                if (entry.is_string() && Coop::ParseUdpEndpoint(entry.get<std::string>(), server)) {
                    gOn.stunServers.push_back(server);
                }
            }
        }
        gOn.mapped.clear();
        gOn.mappedOther.clear();
        SetPhase(Phase::Stun, gOn.host ? "" : "Found room " + gOn.code + ". Checking your network...");
        SendStunRequests();
        return;
    }

    if (op == "error") {
        const std::string why = ErrorText(Str(msg, "why"), Str(msg, "detail"));
        if (gOn.host && Str(msg, "why") == "taken" && gOn.reconnects == 0) {
            // somebody else has that name: no point retrying
            CloseWs();
            SetPhase(Phase::Failed, why);
            gOn.reconnectAtMs = 0;
            gOn.code.clear();
            return;
        }
        if (gOn.host) {
            HostLostServer(why);
        } else {
            gOn.hostUpnp.clear(); // the server said no: the host's address won't help either
            JoinerGiveUp(why);
        }
        return;
    }

    if (op == "joiner_version" && gOn.host) {
        const int theirs = std::atoi(Str(msg, "v").c_str());
        SPDLOG_INFO("[Coop] [ONLINE] joiner on protocol {} refused (local {})", theirs, Coop::kProtocolVersion);
        CoopGame::Notify(CoopGame::NoticeKind::Other,
                         theirs > Coop::kProtocolVersion
                             ? "Someone tried to join with a newer SOH-Together. Update yours."
                             : "Someone tried to join with an older SOH-Together. They need to update.");
        return;
    }

    if (op != "peer") {
        return;
    }
    PendingPeer peer;
    if (!Coop::Nat::ParseHexToken(Str(msg, "token"), peer.token)) {
        return;
    }
    if (auto eps = msg.find("eps"); eps != msg.end() && eps->is_array()) {
        for (const auto& entry : *eps) {
            Coop::UdpEndpoint ep;
            if (entry.is_string() && Coop::ParseUdpEndpoint(entry.get<std::string>(), ep) &&
                peer.candidates.size() < 8) {
                peer.candidates.push_back(ep);
            }
        }
    }
    peer.startedMs = NowMs();

    if (gOn.host) {
        SPDLOG_INFO("[Coop] [ONLINE] a joiner is on the way ({} addresses)", peer.candidates.size());
        gOn.peers.push_back(std::move(peer));
        Probe(gOn.peers.back());
        return;
    }
    gOn.target = std::move(peer);
    gOn.hostUpnp = Str(msg, "upnp");
    auto sameNet = msg.find("sameNet");
    gOn.sameNet = sameNet != msg.end() && sameNet->is_boolean() && sameNet->get<bool>();
    SPDLOG_INFO("[Coop] [ONLINE] host addresses={} upnp={} sameNetwork={}", gOn.target.candidates.size(),
                !gOn.hostUpnp.empty(), gOn.sameNet);
    if (gOn.target.candidates.empty() && gOn.hostUpnp.empty()) {
        JoinerGiveUp(kTailscaleHint);
        return;
    }
    SetPhase(Phase::Punching, "Connecting to the host...");
    Probe(gOn.target);
}

void PumpWebSocket() {
    WsEvent event;
    while (gOn.ws.Poll(event)) {
        switch (event.type) {
            case WsEvent::Type::Open:
                SPDLOG_INFO("[Coop] [ONLINE] room server connected");
                break;
            case WsEvent::Type::Text:
                OnServerMessage(event.text);
                break;
            case WsEvent::Type::Closed:
                gOn.ws.Close();
                if (gOn.phase == Phase::Done || gOn.phase == Phase::Failed || gOn.phase == Phase::Idle) {
                    break;
                }
                // the joiner doesn't need the server any more once it's punching
                if (!gOn.host && gOn.phase == Phase::Punching) {
                    break;
                }
                SPDLOG_INFO("[Coop] [ONLINE] room server closed: {}", event.text);
                Fail(event.text);
                return; // Fail() may have closed or replaced the connection
        }
        if (!gOn.ws) {
            return;
        }
    }
}

std::string HostKey() {
    std::string key = CVarGetString(kHostKeyCVar, "");
    if (key.size() >= 16) {
        return key;
    }
    static const char* const kHex = "0123456789abcdef";
    key.clear();
    for (int i = 0; i < 32; ++i) {
        key += kHex[Rng()() & 0xF];
    }
    CVarSetString(kHostKeyCVar, key.c_str());
    SaveCVars();
    return key;
}

std::string HostPath() {
    const std::string key = "key=" + HostKey();
    if (gOn.code.empty()) {
        return "/host?" + key;
    }
    return "/host?code=" + gOn.code + (gOn.named ? "&strict=1" : "") + "&" + key;
}

void OnlineHostBegin(uint16_t udpPort, const std::string& name) {
    OnlineStop();
    gOn.host = true;
    gOn.localPort = udpPort;
    gOn.reconnects = 0;
    gOn.named = !name.empty();
    gOn.code = name;
    OpenWs(HostPath());
}

void OnlineJoinBegin(const std::string& code, uint16_t localUdpPort) {
    OnlineStop();
    gOn.host = false;
    gOn.code = code;
    gOn.localPort = localUdpPort;
    gOn.hostUpnp.clear();
    gOn.punchLogs = 0;
    if (!OpenWs("/join/" + code)) {
        JoinFailed(gOn.status, "");
    }
}

bool OnlineActive() {
    return gOn.phase != Phase::Idle;
}

bool OnlineOnDatagram(const Coop::UdpEndpoint& from, std::span<const uint8_t> data) {
    if (gOn.phase == Phase::Idle && gOn.peers.empty()) {
        return false;
    }
    if (OnPunch(from, data)) {
        return true;
    }
    if (Coop::Nat::LooksLikeStun(data)) {
        return OnStun(data);
    }
    return false;
}

void OnlineUpdate() {
    PumpWebSocket();
    const uint64_t now = NowMs();

    if (gOn.host && gOn.phase == Phase::Failed && gOn.reconnectAtMs != 0 && now >= gOn.reconnectAtMs) {
        gOn.reconnectAtMs = 0;
        if (!OpenWs(HostPath())) {
            HostLostServer("could not reach it");
        }
        return;
    }

    switch (gOn.phase) {
        case Phase::Connecting:
            if (now - gOn.phaseStartMs > kWelcomeTimeoutMs) {
                Fail("the room server did not answer");
            }
            break;
        case Phase::Stun:
            UpdateStun();
            break;
        case Phase::Waiting:
            if (!gOn.host && now - gOn.phaseStartMs > kPeerTimeoutMs) {
                JoinerGiveUp("The host did not answer. Is the game still open on their side?");
            }
            break;
        case Phase::Punching:
            if (now - gOn.phaseStartMs > kJoinPunchMs) {
                JoinerGiveUp(gOn.sameNet ? "You are on the same network as the host. Use Join with their local "
                                           "address instead (or Find games on my wifi)."
                                         : kTailscaleHint);
                break;
            }
            Probe(gOn.target);
            break;
        default:
            break;
    }

    if (gOn.host) {
        // the router may open the port after we said hello: tell the server
        if (gOn.phase == Phase::Waiting && gOn.ws) {
            const std::string upnp = UpnpExternalAddress();
            if (upnp != gOn.sentUpnp) {
                gOn.sentUpnp = upnp;
                SendJson({ { "op", "update" }, { "upnp", upnp } });
            }
        }
        for (PendingPeer& p : gOn.peers) {
            if (now - p.startedMs < kHostPunchMs) {
                Probe(p);
            }
        }
        // (Crests of Courage also stops once the joiner's hello arrives; ENet doesn't tell us
        // which connection was which, so we just punch for the full 15 s)
        std::erase_if(gOn.peers, [&](const PendingPeer& p) { return now - p.startedMs >= kHostPunchMs; });
    }
}

// =====================================================================================
// Same wifi: "Find games on my wifi" broadcasts a probe on the host port, hosts answer.
// Not in Crests of Courage (there, same-wifi players find each other through the room
// server, which we do too: see "lan" in the hello). This one works with no server at all.
// =====================================================================================

struct LanGame {
    Coop::UdpEndpoint at;
    std::string name;
};

struct LanScan {
    bool scanning = false;
    bool scanned = false;
    uint64_t startedMs = 0;
    int probesSent = 0;
    std::vector<LanGame> found;
};
LanScan gLan;

uint16_t HostPortSetting() {
    int port = CVarGetInteger(CVAR_COOP("Port"), Coop::kDefaultPort);
    return port >= 1 && port <= 65535 ? static_cast<uint16_t>(port) : Coop::kDefaultPort;
}

void LanScanStart() {
    gLan = LanScan{};
    if (Coop::Transport::OpenJoinSocket() == 0) {
        gLan.scanned = true;
        return;
    }
    gLan.scanning = true;
    gLan.startedMs = NowMs();
}

void LanUpdate() {
    if (!gLan.scanning) {
        return;
    }
    const uint64_t elapsed = NowMs() - gLan.startedMs;
    // 3 probes, half a second apart (a broadcast can get lost), answers for 2 s
    if (gLan.probesSent < 3 && elapsed >= static_cast<uint64_t>(gLan.probesSent) * 500) {
        Coop::Transport::SendRaw({ "255.255.255.255", HostPortSetting() }, Coop::Nat::LanProbe());
        gLan.probesSent++;
    }
    if (elapsed >= 2000) {
        gLan.scanning = false;
        gLan.scanned = true;
        if (!OnlineActive() && CoopGame::Session().Role() == Coop::SessionRole::Off) {
            Coop::Transport::CloseJoinSocket();
        }
    }
}

bool OnLanPacket(const Coop::UdpEndpoint& from, std::span<const uint8_t> data) {
    if (Coop::Nat::IsLanProbe(data)) {
        Coop::Session& session = CoopGame::Session();
        if (session.Role() == Coop::SessionRole::Hosting) {
            std::string name = CVarGetString(CVAR_COOP("Name"), "");
            Coop::Transport::SendRaw(from, Coop::Nat::LanAnswer(Coop::kProtocolVersion, session.Port(), name));
        }
        return true;
    }
    uint16_t protocol = 0;
    uint16_t port = 0;
    std::string name;
    if (!Coop::Nat::ParseLanAnswer(data, protocol, port, name)) {
        return false;
    }
    if (gLan.scanning && protocol == Coop::kProtocolVersion) {
        LanGame game{ { from.ip, port }, name.empty() ? "Someone" : name };
        auto same =
            std::find_if(gLan.found.begin(), gLan.found.end(), [&](const LanGame& g) { return g.at == game.at; });
        if (same == gLan.found.end() && gLan.found.size() < 16) {
            gLan.found.push_back(game);
        }
    }
    return true;
}

// =====================================================================================
// Glue: raw packets, watching the session, connect on launch
// =====================================================================================

bool OnRawPacket(const Coop::UdpEndpoint& from, std::span<const uint8_t> data) {
    std::lock_guard lock(CoopGame::Mutex());
    if (OnLanPacket(from, data)) {
        return true;
    }
    return OnlineOnDatagram(from, data);
}

std::string gHostNote; // why hosting has no room code, shown under the invite

// start_hosting (mod.cpp): once we host, open the port and get a room code
void HostExtrasBegin(uint16_t port) {
    gHostNote.clear();
    if (CVarGetInteger(kOpenPortCVar, 1)) {
        UpnpBegin(port);
    }
    if (!CVarGetInteger(kRoomCodesCVar, 1) || RoomServer().empty()) {
        return;
    }
    const std::string name = Coop::Nat::NormalizeRoomCode(CVarGetString(kRoomCVar, ""));
    if (!name.empty() && (name.size() < Coop::Nat::kRoomNameMin || name.size() > Coop::Nat::kRoomNameMax)) {
        gHostNote = "Room names are 4 to 24 letters and numbers. Hosting by address only.";
    } else {
        OnlineHostBegin(port, name);
    }
}

void WatchSession() {
    static Coop::SessionRole sLast = Coop::SessionRole::Off;
    Coop::Session& session = CoopGame::Session();
    const Coop::SessionRole role = session.Role();
    if (role == sLast) {
        return;
    }
    const Coop::SessionRole was = sLast;
    sLast = role;

    if (role == Coop::SessionRole::Hosting) {
        // a host answers on its game socket, a leftover join socket would steal SendRaw()
        Coop::Transport::CloseJoinSocket();
        gLan = LanScan{};
        OnlineStop();
        SetMode("host");
        HostExtrasBegin(session.Port());
        return;
    }
    if (was == Coop::SessionRole::Hosting) {
        UpnpRelease();
        OnlineStop();
        gHostNote.clear();
    }
    if (role == Coop::SessionRole::Connecting && was == Coop::SessionRole::Off) {
        SetMode(gJoinFromLobby ? "join_code" : "join");
        gJoinFromLobby = false;
        OnlineStop(); // the punch is done (or we joined by address): the room is no longer needed
    }
}

// Ask for a room by code (start_joining_code in mod.cpp)
void JoinByCode(const std::string& typed) {
    if (CoopGame::Session().Role() != Coop::SessionRole::Off || (OnlineActive() && gOn.phase != Phase::Failed)) {
        return;
    }
    const std::string code = Coop::Nat::NormalizeRoomCode(typed);
    if (code.size() < Coop::Nat::kRoomNameMin || code.size() > Coop::Nat::kRoomNameMax) {
        SetPhase(Phase::Failed, "Enter the room code or name the host sees");
        return;
    }
    const uint16_t localPort = Coop::Transport::OpenJoinSocket();
    if (localPort == 0) {
        SetPhase(Phase::Failed, "Couldn't start networking");
        return;
    }
    SetMode("join_code");
    SPDLOG_INFO("[Coop] joining a room by code (udp {})", localPort);
    OnlineJoinBegin(code, localPort);
}

void CancelJoin() {
    OnlineStop();
    Coop::Transport::CloseJoinSocket();
}

// "Connect on launch": do again what we did last time, 10 s after the game starts
// (Crests of Courage waits 600 ticks, auto_connect_delay_ticks)
void ConnectOnLaunch() {
    static bool sDone = false;
    static uint64_t sFirstMs = 0;
    if (sDone) {
        return;
    }
    if (!CVarGetInteger(kOnLaunchCVar, 0)) {
        sDone = true;
        return;
    }
    if (sFirstMs == 0) {
        sFirstMs = NowMs();
    }
    if (NowMs() - sFirstMs < 10000) {
        return;
    }
    sDone = true;
    if (CoopGame::Session().Role() != Coop::SessionRole::Off ||
        std::string(CVarGetString(CVAR_COOP("Name"), "")).empty()) {
        return;
    }
    const std::string mode = CVarGetString(kModeCVar, "host");
    SPDLOG_INFO("[Coop] connect on launch: {}", mode);
    if (mode == "join") {
        std::string host;
        uint16_t port = 0;
        if (Coop::ParseAddress(CVarGetString(CVAR_COOP("JoinAddress"), ""), host, port)) {
            CoopGame::StartJoin(host, port);
        }
    } else if (mode == "join_code") {
        JoinByCode(CVarGetString(kRoomCVar, ""));
    } else {
        CoopGame::StartHost(HostPortSetting());
    }
}

void LobbyFrame() {
    std::lock_guard lock(CoopGame::Mutex());
    WatchSession();
    Coop::Transport::PollJoinSocket();
    OnlineUpdate();
    LanUpdate();
    ConnectOnLaunch();
}

// =====================================================================================
// Menu (texts from Crests of Courage menu.cpp build_connect / group_advanced / build_online_guide)
// =====================================================================================

void DrawServerInput() {
    UIWidgets::CVarInputString("Room server", kServerCVar,
                               UIWidgets::InputOptions()
                                   .Color(THEME_COLOR)
                                   .PlaceholderText("ws://203.0.113.5:8787")
                                   .Tooltip("Ask whoever runs it. The game itself never goes through it, it "
                                            "only introduces players."));
}

void DrawAdvanced() {
    if (!ImGui::CollapsingHeader("Advanced")) {
        return;
    }
    DrawServerInput();
    int32_t port = HostPortSetting();
    // saved only once it's a usable port (1024-65535 like Crests of Courage), not on every key
    if (UIWidgets::InputInt("Port to listen on", &port, UIWidgets::InputOptions().Color(THEME_COLOR)) &&
        port >= 1024 && port <= 65535) {
        CVarSetInteger(CVAR_COOP("Port"), port);
        SaveCVars();
    }
    UIWidgets::CVarCheckbox("Room codes", kRoomCodesCVar,
                            UIWidgets::CheckboxOptions()
                                .Color(THEME_COLOR)
                                .DefaultValue(true)
                                .Tooltip("Other players can see your IP address while this is on."));
    UIWidgets::CVarCheckbox("Open the port for me", kOpenPortCVar,
                            UIWidgets::CheckboxOptions()
                                .Color(THEME_COLOR)
                                .DefaultValue(true)
                                .Tooltip("Asks your router (UPnP) to open the port while you host."));
    UIWidgets::CVarCheckbox("Connect on launch", kOnLaunchCVar,
                            UIWidgets::CheckboxOptions()
                                .Color(THEME_COLOR)
                                .DefaultValue(false)
                                .Tooltip("Hosts or joins again like last time, 10 seconds after the game starts."));
}

void DrawGuide() {
    if (!ImGui::CollapsingHeader("How to play together")) {
        return;
    }
    ImGui::TextWrapped("One of you presses Host a game and reads the room code to the others. The rest type it "
                       "in Room and press Join room.");
    ImGui::SeparatorText("Same wifi");
    ImGui::TextWrapped("Press Find games on my wifi. If nothing shows up, join with the host's local address "
                       "(the one under \"You're hosting\").");
    ImGui::SeparatorText("If the code won't connect");
    ImGui::TextWrapped("Some networks are too strict. Use Tailscale instead. It's free and works everywhere.");
    ImGui::TextWrapped("1. Everyone installs Tailscale and signs in.");
    ImGui::TextWrapped("2. The host invites the others at login.tailscale.com, under Users.");
    ImGui::TextWrapped("3. The host presses Host a game.");
    ImGui::TextWrapped("4. The others join with the host's Tailscale address. It starts with 100.");
    ImGui::SeparatorText("Firewall");
    ImGui::TextWrapped("If Windows asks to let Ship of Harkinian through, allow it.");
}

// Under the Host / Join buttons
void DrawNotConnected() {
    const bool haveName = !std::string(CVarGetString(CVAR_COOP("Name"), "")).empty();

    ImGui::SeparatorText("Room code");
    if (RoomServer().empty()) {
        ImGui::TextWrapped("Room codes need a room server. Paste its address here:");
        DrawServerInput();
    } else if (!CVarGetInteger(kRoomCodesCVar, 1)) {
        UIWidgets::CVarCheckbox("Room codes", kRoomCodesCVar,
                                UIWidgets::CheckboxOptions()
                                    .Color(THEME_COLOR)
                                    .DefaultValue(true)
                                    .Tooltip("Other players can see your IP address while this is on."));
    } else {
        UIWidgets::CVarInputString("Room", kRoomCVar,
                                   UIWidgets::InputOptions().Color(THEME_COLOR).PlaceholderText("like K7WQ2P"));
        const bool busy = OnlineActive() && gOn.phase != Phase::Failed;
        ImGui::BeginDisabled(!haveName || busy);
        if (UIWidgets::Button("Join room", UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
            JoinByCode(CVarGetString(kRoomCVar, ""));
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled("Hosting? Empty Room gets a random code.");
        ImGui::TextDisabled("Other players can see your IP address while room codes are on.");
    }
    if (OnlineActive() && !gOn.status.empty()) {
        if (gOn.phase == Phase::Failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.4f, 0.4f, 1));
            ImGui::TextWrapped("%s", gOn.status.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextWrapped("%s", gOn.status.c_str());
            if (UIWidgets::Button("Cancel", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
                CancelJoin();
            }
        }
    }

    ImGui::SeparatorText("Same wifi");
    ImGui::BeginDisabled(!haveName || gLan.scanning || (OnlineActive() && gOn.phase != Phase::Failed));
    if (UIWidgets::Button(gLan.scanning ? "Looking..." : "Find games on my wifi",
                          UIWidgets::ButtonOptions().Color(THEME_COLOR))) {
        LanScanStart();
    }
    ImGui::EndDisabled();
    for (const LanGame& game : gLan.found) {
        ImGui::PushID(game.at.Text().c_str());
        ImGui::Text("%s  (%s)", game.name.c_str(), game.at.Text().c_str());
        ImGui::SameLine();
        ImGui::BeginDisabled(!haveName);
        if (ImGui::SmallButton("Join")) {
            // remembered like a typed address (Connect on launch uses it)
            CVarSetString(CVAR_COOP("JoinAddress"), game.at.Text().c_str());
            SaveCVars();
            CoopGame::StartJoin(game.at.ip, game.at.port);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (gLan.scanned && gLan.found.empty()) {
        ImGui::TextDisabled("No game found. Is someone hosting, with the same port as yours?");
    }

    DrawAdvanced();
    DrawGuide();
}

// invite_text (menu.cpp): what the host tells its friends
std::string InviteText() {
    const std::string address = UpnpExternalAddress();
    if (!gOn.code.empty()) {
        std::string out = "Room code: " + gOn.code;
        if (!gOn.status.empty()) {
            out += ". " + gOn.status;
        }
        return out;
    }
    if (!gOn.status.empty()) {
        return gOn.status;
    }
    if (!address.empty()) {
        return "Others can join you at " + address;
    }
    return UpnpStatus();
}

// Hosting / connected: the room code and the port, for the host only
void DrawInSession() {
    if (CoopGame::Session().Role() != Coop::SessionRole::Hosting) {
        return;
    }
    const std::string invite = InviteText();
    if (!invite.empty()) {
        ImGui::TextWrapped("%s", invite.c_str());
        if (!gOn.code.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy code")) {
                ImGui::SetClipboardText(gOn.code.c_str());
            }
        }
    }
    // the router's answer, when the line above is about the room code
    const std::string upnp = UpnpStatus();
    if (!gOn.code.empty() && !upnp.empty()) {
        ImGui::TextDisabled("%s", upnp.c_str());
    }
    if (!gHostNote.empty()) {
        ImGui::TextDisabled("%s", gHostNote.c_str());
    }
}

void Register() {
    COND_HOOK(OnGameFrameUpdate, true, LobbyFrame);
    Coop::Transport::SetRawHandler(OnRawPacket);
    CoopGame::AddMenuSection("Lobby", CoopGame::MenuWhere::NotConnected, DrawNotConnected);
    CoopGame::AddMenuSection("LobbyInvite", CoopGame::MenuWhere::InSession, DrawInSession);
}

} // namespace

static RegisterShipInitFunc sInit(Register);
