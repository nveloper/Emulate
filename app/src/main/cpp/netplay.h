#pragma once

#include "libretro.h"

#include <netinet/in.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// Frontend side of libretro's netpacket interface (RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE),
// used by melonDS DS to emulate the DS's local wireless between devices on the same LAN.
//
// Star topology over UDP: one device hosts (client id 0), others join and get ids 1..15. The host
// relays broadcasts and client-to-client packets. Everything here runs on the emulation thread,
// because the core may only be called back from within retro_run / netpacket callbacks.
//
// Wire format (little endian): "EMNP" u8 version, u8 type, u16 src, u16 dst, payload.
class Netplay {
public:
    enum class State { Idle = 0, Hosting = 1, Joining = 2, Client = 3 };
    // state, connected players (including us), user-facing message or ""
    using StatusFn = std::function<void(State, int, const std::string&)>;

    static constexpr uint16_t kPort = 47000;
    static constexpr int kMaxPlayers = 16;
    int maxPlayers_ = kMaxPlayers;

    void setCoreCallbacks(const retro_netpacket_callback* callbacks, const std::string& fallbackVersion);
    void clearCoreCallbacks();
    bool coreSupportsNetplay() const { return core_.start != nullptr; }
    void setStatusFn(StatusFn fn) { status_ = std::move(fn); }
    void setTitle(std::string title) { title_ = std::move(title); }
    // Which consoles can link with each other ("GB" covers GB and GBC, "GBA", "NDS"); rooms with a
    // different tag are not compatible. Also decides the room size.
    void setTag(const std::string& tag);

    // Commands. Return "" on success or a user-facing error.
    std::string host();
    std::string join(const std::string& address);
    void leave(const char* message = nullptr);

    // Once per emulated frame, before retro_run: delivers packets, keepalives, timeouts.
    void frame();
    // While the game is paused: keep the session alive but drop game traffic.
    void idle();
    // Sleeps until the deadline, delivering packets the moment they arrive (a link partner may
    // be blocked waiting for our answer). Returns early if stopWaiting() is called.
    void waitUntil(std::chrono::steady_clock::time_point deadline, const std::atomic<bool>& stop);

    // Session active and the core may be exchanging packets.
    bool active() const { return state_ == State::Hosting || state_ == State::Client; }
    State state() const { return state_; }

    // Called by the core (via static trampolines).
    void coreSend(int flags, const void* buf, size_t len, uint16_t dst);
    void corePollReceive();

private:
    enum Type : uint8_t { Discover = 1, Announce = 2, Join = 3, Welcome = 4, Reject = 5, Data = 6, Ping = 7, Bye = 8 };

    struct Peer {
        sockaddr_in addr;
        std::chrono::steady_clock::time_point lastSeen;
    };

    bool openSocket(uint16_t port);
    void closeSocket();
    void receiveAll(bool deliverData);
    void handle(const uint8_t* data, size_t len, const sockaddr_in& from, bool deliverData);
    void sendTo(const sockaddr_in& to, Type type, uint16_t src, uint16_t dst, const void* payload = nullptr, size_t len = 0);
    void deliver(const uint8_t* payload, size_t len, uint16_t src);
    void keepalive();
    void dropPeer(uint16_t id, const char* message);
    void notify(const std::string& message = "");
    int players() const;
    const Peer* peerByAddr(const sockaddr_in& addr, uint16_t* id) const;

    retro_netpacket_callback core_{};
    std::string protocol_;
    std::string coreProtocol_;
    std::string tag_;
    std::string title_;
    StatusFn status_;

    State state_ = State::Idle;
    int socket_ = -1;
    uint16_t selfId_ = 0;
    sockaddr_in hostAddr_{};
    std::map<uint16_t, Peer> peers_;  // host: connected clients
    std::chrono::steady_clock::time_point hostLastSeen_{}, joinStarted_{}, lastJoinSent_{}, lastPing_{};
    // The core must not be stopped from inside its own poll loop (it keeps calling the send
    // function it was given), so a leave requested there is deferred to the next frame.
    bool inCorePoll_ = false;
    bool pendingLeave_ = false;
    std::string pendingMessage_;
    std::vector<uint8_t> sendBuffer_;
    std::vector<uint8_t> recvBuffer_ = std::vector<uint8_t>(65536);
};
