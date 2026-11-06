#include "netplay.h"

#include <android/log.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#define LOG_TAG "EmuNetplay"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace {

using clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr uint8_t kVersion = 1;
constexpr size_t kHeader = 10;
constexpr auto kPingInterval = 500ms;
constexpr auto kPeerTimeout = 8s;
constexpr auto kJoinRetry = 250ms;
constexpr auto kJoinTimeout = 6s;

// The core's send/poll callbacks carry no user data.
Netplay* gActive = nullptr;

void RETRO_CALLCONV sendTrampoline(int flags, const void* buf, size_t len, uint16_t dst) {
    if (gActive) gActive->coreSend(flags, buf, len, dst);
}

void RETRO_CALLCONV pollTrampoline() {
    if (gActive) gActive->corePollReceive();
}

bool sameAddr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

}  // namespace

void Netplay::setCoreCallbacks(const retro_netpacket_callback* callbacks, const std::string& fallbackVersion) {
    core_ = callbacks ? *callbacks : retro_netpacket_callback{};
    coreProtocol_ = (callbacks && callbacks->protocol_version) ? callbacks->protocol_version : fallbackVersion;
    protocol_ = coreProtocol_ + "/" + tag_;
}

void Netplay::setTag(const std::string& tag) {
    tag_ = tag;
    protocol_ = coreProtocol_ + "/" + tag_;
    maxPlayers_ = tag == "GB" ? 2 : tag == "GBA" ? 4 : kMaxPlayers;
}

void Netplay::clearCoreCallbacks() {
    leave();
    core_ = {};
    protocol_.clear();
}

// ---- sockets ---------------------------------------------------------------------------------

bool Netplay::openSocket(uint16_t port) {
    closeSocket();
    socket_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_ < 0) return false;
    int yes = 1;
    setsockopt(socket_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    setsockopt(socket_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(socket_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        LOGW("bind(%u) failed: %s", port, strerror(errno));
        closeSocket();
        return false;
    }
    fcntl(socket_, F_SETFL, fcntl(socket_, F_GETFL) | O_NONBLOCK);
    return true;
}

void Netplay::closeSocket() {
    if (socket_ >= 0) ::close(socket_);
    socket_ = -1;
}

void Netplay::sendTo(const sockaddr_in& to, Type type, uint16_t src, uint16_t dst, const void* payload, size_t len) {
    if (socket_ < 0) return;
    sendBuffer_.resize(kHeader + len);
    uint8_t* p = sendBuffer_.data();
    memcpy(p, "EMNP", 4);
    p[4] = kVersion;
    p[5] = type;
    p[6] = src & 0xFF;
    p[7] = src >> 8;
    p[8] = dst & 0xFF;
    p[9] = dst >> 8;
    if (len) memcpy(p + kHeader, payload, len);
    ::sendto(socket_, p, sendBuffer_.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

// ---- session commands ------------------------------------------------------------------------

std::string Netplay::host() {
    if (!coreSupportsNetplay()) return "이 게임(코어)은 무선 통신을 지원하지 않습니다.";
    leave();
    if (!openSocket(kPort)) return "통신 포트를 열 수 없습니다. 다른 앱이 사용 중일 수 있습니다.";
    state_ = State::Hosting;
    selfId_ = 0;
    peers_.clear();
    lastPing_ = clock::now();
    gActive = this;
    core_.start(0, sendTrampoline, pollTrampoline);
    LOGI("Hosting on port %u (protocol \"%s\")", kPort, protocol_.c_str());
    notify("방을 만들었습니다. 다른 기기에서 참가하세요.");
    return "";
}

std::string Netplay::join(const std::string& address) {
    if (!coreSupportsNetplay()) return "이 게임(코어)은 무선 통신을 지원하지 않습니다.";
    leave();

    // "host" or "host:port" (IPv4 only); tolerate stray spaces and doubled colons.
    std::string text;
    for (char c : address) {
        if (c != ' ' && c != '\t') text += c;
    }
    std::string hostname = text;
    uint16_t port = kPort;
    if (size_t colon = text.find(':'); colon != std::string::npos) {
        hostname = text.substr(0, colon);
        int parsed = atoi(text.c_str() + text.rfind(':') + 1);
        if (parsed > 0 && parsed < 65536) port = static_cast<uint16_t>(parsed);
    }
    if (hostname.empty()) return "주소를 입력하세요.";
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname.c_str(), nullptr, &hints, &result) != 0 || !result) return "주소를 확인할 수 없습니다: " + address;
    hostAddr_ = *reinterpret_cast<sockaddr_in*>(result->ai_addr);
    hostAddr_.sin_port = htons(port);
    freeaddrinfo(result);

    if (!openSocket(0)) return "통신 소켓을 열 수 없습니다.";
    state_ = State::Joining;
    joinStarted_ = clock::now();
    lastJoinSent_ = {};
    notify("호스트에 연결하는 중…");
    frame();  // send the first JOIN right away
    return "";
}

void Netplay::leave(const char* message) {
    if (inCorePoll_) {
        pendingLeave_ = true;
        pendingMessage_ = message ? message : "";
        return;
    }
    pendingLeave_ = false;
    if (state_ == State::Idle && socket_ < 0) return;
    if (state_ == State::Hosting) {
        for (auto& [id, peer] : peers_) sendTo(peer.addr, Bye, 0, id);
    } else if (state_ == State::Client || state_ == State::Joining) {
        sendTo(hostAddr_, Bye, selfId_, 0);
    }
    bool wasActive = active();
    state_ = State::Idle;
    peers_.clear();
    closeSocket();
    if (wasActive && core_.stop) core_.stop();
    gActive = nullptr;
    notify(message ? message : "");
}

// ---- per-frame work --------------------------------------------------------------------------

void Netplay::corePollReceive() {
    inCorePoll_ = true;
    receiveAll(true);
    inCorePoll_ = false;
}

void Netplay::frame() {
    if (pendingLeave_) leave(pendingMessage_.c_str());
    if (socket_ < 0) return;
    receiveAll(true);
    keepalive();
    if (active() && core_.poll) core_.poll();
}

void Netplay::idle() {
    if (pendingLeave_) leave(pendingMessage_.c_str());
    if (socket_ < 0) return;
    receiveAll(false);
    keepalive();
}

void Netplay::waitUntil(std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& stop) {
    if (pendingLeave_) leave(pendingMessage_.c_str());
    while (socket_ >= 0 && !stop) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - clock::now()).count();
        if (remaining <= 0) return;
        pollfd pfd{socket_, POLLIN, 0};
        if (::poll(&pfd, 1, static_cast<int>(remaining)) > 0) receiveAll(true);
    }
}

void Netplay::keepalive() {
    auto now = clock::now();
    switch (state_) {
        case State::Joining:
            if (now - joinStarted_ > kJoinTimeout) {
                leave("호스트를 찾을 수 없습니다. 같은 Wi-Fi에 있는지, 호스트가 방을 만들었는지 확인하세요.");
                return;
            }
            if (now - lastJoinSent_ >= kJoinRetry) {
                sendTo(hostAddr_, Join, 0, 0, protocol_.data(), protocol_.size());
                lastJoinSent_ = now;
            }
            break;
        case State::Client:
            if (now - hostLastSeen_ > kPeerTimeout) {
                leave("호스트와 연결이 끊어졌습니다.");
                return;
            }
            if (now - lastPing_ >= kPingInterval) {
                sendTo(hostAddr_, Ping, selfId_, 0);
                lastPing_ = now;
            }
            break;
        case State::Hosting: {
            std::vector<uint16_t> lost;
            for (auto& [id, peer] : peers_) {
                if (now - peer.lastSeen > kPeerTimeout) lost.push_back(id);
            }
            for (uint16_t id : lost) dropPeer(id, "플레이어 연결이 끊어졌습니다.");
            if (now - lastPing_ >= kPingInterval) {
                for (auto& [id, peer] : peers_) sendTo(peer.addr, Ping, 0, id);
                lastPing_ = now;
            }
            break;
        }
        case State::Idle:
            break;
    }
}

// ---- receiving -------------------------------------------------------------------------------

void Netplay::receiveAll(bool deliverData) {
    while (socket_ >= 0 && !pendingLeave_) {
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n = ::recvfrom(socket_, recvBuffer_.data(), recvBuffer_.size(), 0,
                               reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (n < 0) break;  // EAGAIN: drained
        if (static_cast<size_t>(n) < kHeader || memcmp(recvBuffer_.data(), "EMNP", 4) != 0) continue;
        if (recvBuffer_[4] != kVersion) continue;
        handle(recvBuffer_.data(), static_cast<size_t>(n), from, deliverData);
    }
}

void Netplay::handle(const uint8_t* data, size_t len, const sockaddr_in& from, bool deliverData) {
    const auto type = static_cast<Type>(data[5]);
    const uint16_t dst = rd16(data + 8);
    const uint8_t* payload = data + kHeader;
    const size_t payloadLen = len - kHeader;
    const auto now = clock::now();

    if (state_ == State::Hosting) {
        if (type == Discover) {
            std::vector<uint8_t> info;
            info.push_back(static_cast<uint8_t>(players()));
            info.push_back(static_cast<uint8_t>(maxPlayers_));
            info.insert(info.end(), title_.begin(), title_.end());
            info.push_back(0);
            info.insert(info.end(), tag_.begin(), tag_.end());
            sendTo(from, Announce, 0, 0, info.data(), info.size());
            return;
        }
        uint16_t id = 0;
        const Peer* peer = peerByAddr(from, &id);
        if (type == Join) {
            std::string version(reinterpret_cast<const char*>(payload), payloadLen);
            if (peer) {  // our WELCOME was lost; repeat it
                sendTo(from, Welcome, 0, id);
                return;
            }
            if (version != protocol_) {
                std::string theirTag = version.substr(version.rfind('/') + 1);
                std::string msg = theirTag != tag_ ? "서로 연결할 수 없는 기종의 게임입니다." : "에뮬레이터 버전이 서로 다릅니다.";
                sendTo(from, Reject, 0, 0, msg.data(), msg.size());
                return;
            }
            uint16_t newId = 1;
            while (peers_.count(newId)) newId++;
            if (newId >= maxPlayers_) {
                static const char msg[] = "방이 가득 찼습니다.";
                sendTo(from, Reject, 0, 0, msg, sizeof(msg) - 1);
                return;
            }
            if (core_.connected && !core_.connected(newId)) {
                static const char msg[] = "호스트가 참가를 거부했습니다.";
                sendTo(from, Reject, 0, 0, msg, sizeof(msg) - 1);
                return;
            }
            peers_[newId] = Peer{from, now};
            sendTo(from, Welcome, 0, newId);
            char addr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &from.sin_addr, addr, sizeof(addr));
            LOGI("Player %u joined from %s:%u", newId, addr, ntohs(from.sin_port));
            notify("플레이어가 참가했습니다.");
            return;
        }
        if (!peer) return;  // not part of the session
        peers_[id].lastSeen = now;
        switch (type) {
            case Data:
                // The sender id comes from the address, never from the (spoofable) header.
                if (dst == 0 || dst == RETRO_NETPACKET_BROADCAST) {
                    if (deliverData) deliver(payload, payloadLen, id);
                }
                if (dst == RETRO_NETPACKET_BROADCAST) {
                    for (auto& [otherId, other] : peers_) {
                        if (otherId != id) sendTo(other.addr, Data, id, dst, payload, payloadLen);
                    }
                } else if (dst != 0) {
                    auto target = peers_.find(dst);
                    if (target != peers_.end()) sendTo(target->second.addr, Data, id, dst, payload, payloadLen);
                }
                break;
            case Bye:
                dropPeer(id, "플레이어가 나갔습니다.");
                break;
            default:
                break;  // Ping: lastSeen already updated
        }
        return;
    }

    // Joining / client: only listen to the host.
    if (!sameAddr(from, hostAddr_)) return;
    hostLastSeen_ = now;
    switch (type) {
        case Welcome:
            if (state_ == State::Joining) {
                selfId_ = dst;
                state_ = State::Client;
                lastPing_ = now;
                gActive = this;
                core_.start(selfId_, sendTrampoline, pollTrampoline);
                LOGI("Joined as player %u", selfId_);
                notify("호스트에 연결되었습니다.");
            }
            break;
        case Reject:
            if (state_ == State::Joining) {
                leave(std::string(reinterpret_cast<const char*>(payload), payloadLen).c_str());
            }
            break;
        case Data:
            if (state_ == State::Client && deliverData) deliver(payload, payloadLen, rd16(data + 6));
            break;
        case Bye:
            leave("호스트가 통신을 종료했습니다.");
            break;
        default:
            break;
    }
}

void Netplay::deliver(const uint8_t* payload, size_t len, uint16_t src) {
    if (core_.receive) core_.receive(payload, len, src);
}

// ---- sending (from the core) -----------------------------------------------------------------

void Netplay::coreSend(int flags, const void* buf, size_t len, uint16_t dst) {
    // Every packet goes out immediately as one UDP datagram, so FLUSH_HINT needs no extra work.
    // Reliable delivery is not implemented; melonDS DS only sends unreliable packets.
    if (!buf || len == 0 || !active()) return;
    if (state_ == State::Hosting) {
        if (dst == RETRO_NETPACKET_BROADCAST) {
            for (auto& [id, peer] : peers_) sendTo(peer.addr, Data, 0, dst, buf, len);
        } else {
            auto peer = peers_.find(dst);
            if (peer != peers_.end()) sendTo(peer->second.addr, Data, 0, dst, buf, len);
        }
    } else {
        sendTo(hostAddr_, Data, selfId_, dst, buf, len);
    }
}

// ---- helpers ---------------------------------------------------------------------------------

void Netplay::dropPeer(uint16_t id, const char* message) {
    if (!peers_.erase(id)) return;
    LOGI("Player %u left", id);
    if (core_.disconnected) core_.disconnected(id);
    notify(message);
}

const Netplay::Peer* Netplay::peerByAddr(const sockaddr_in& addr, uint16_t* id) const {
    for (auto& [peerId, peer] : peers_) {
        if (sameAddr(peer.addr, addr)) {
            *id = peerId;
            return &peer;
        }
    }
    return nullptr;
}

int Netplay::players() const {
    switch (state_) {
        case State::Hosting: return 1 + static_cast<int>(peers_.size());
        case State::Client: return 2;  // at least us and the host; clients don't track others
        default: return 0;
    }
}

void Netplay::notify(const std::string& message) {
    if (status_) status_(state_, players(), message);
}
