// Direct UDP transport between EDF6 players, used instead of EOS P2P for peers that run the plugin.
//
// Topology is a star: the host listens on a public UDP port; every joining player opens one link
// to the host. Packets between two joining players are forwarded by the host, so only the host
// needs a reachable address (public IPv4 + port forward/UPnP, or public IPv6).
#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "reliable.h"
#include "wire.h"

namespace dn {

enum class Mode { Off, Host, Join };

struct DirectOptions {
    Mode mode = Mode::Off;
    uint16_t listenPort = 27015;  // host: UDP port to listen on; join: local port (0 = any)
    std::string hostAddress;      // join: "1.2.3.4:27015", "[2408::1]:27015" or "name.ddns.net:27015"
    std::string key;              // optional shared secret; must be identical for everyone
    uint32_t ifIndexV4 = 0;       // IP_UNICAST_IF: force egress through this adapter (0 = OS routing)
    uint32_t ifIndexV6 = 0;
    // A stalled link keeps buffering/retransmitting this long. Kept short on purpose: while game data
    // is stuck in a stalled link the other players may be waiting for it at a sync point.
    uint32_t linkTimeoutMs = 60000;
    uint32_t pingIntervalMs = 1000;
    // EDF6 sends all game data UnreliableUnordered. Carrying it reliably-unordered delivers every
    // packet exactly once, possibly reordered: a pattern the unreliable transport can produce anyway,
    // so the game handles it; it just never loses a packet any more.
    bool upgradeUnreliable = true;
    double testDropRate = 0.0;  // tests only: drop this fraction of outgoing datagrams
};

struct Delivered {
    std::string src;
    std::string socketName;
    uint8_t channel = 0;
    std::vector<uint8_t> data;
};

class DirectNet {
public:
    DirectNet() = default;
    ~DirectNet();
    DirectNet(const DirectNet&) = delete;
    DirectNet& operator=(const DirectNet&) = delete;

    bool start(const DirectOptions& options);
    void stop();
    bool running() const { return running_; }
    uint16_t boundPort() const { return boundPort_; }

    // The local EOS ProductUserId as a string. Joining players say hello once it is known.
    void setLocalUser(const std::string& puid);
    // True when packets to `remote` can go over the direct transport right now.
    bool canRoute(const std::string& remote);
    // Sends a game packet over the direct transport. Returns false when `remote` is not routable.
    bool send(const std::string& remote, const std::string& socketName, uint8_t channel, uint8_t reliability,
              const uint8_t* data, size_t size);
    // Pops the next packet for the local player; `channel` filters like EOS RequestedChannel.
    bool pop(const uint8_t* channel, uint32_t maxSize, Delivered& out);

    // True when a game packet from `remote` arrived over the direct link within `windowMs`.
    bool heardFromRecently(const std::string& remote, uint64_t windowMs);

    std::vector<std::string> directMembers();
    std::string statusLine();

    // Tests only: silently drop every outgoing datagram (simulates a total network outage).
    void setTestBlackhole(bool on) { testBlackhole_ = on; }

private:
    struct Link {
        sockaddr_storage addr{};
        int addrLen = 0;
        std::string puid;
        uint32_t peerNonce = 0;
        uint32_t epoch = 0;  // linkEpoch(client nonce, host nonce)
        bool up = false;
        uint64_t lastRecvMs = 0;
        uint64_t lastPingMs = 0;
        uint32_t rttMs = 0;
        ReliableSender tx;
        ReliableReceiver rx;
    };

    void run();
    void processDatagram(const uint8_t* data, size_t size, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onHostDatagram(const Message& m, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onClientDatagram(const Message& m, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onLinkCommon(Link& link, const Message& m, uint64_t now);
    void routeData(DataMsg msg);
    void deliverLocal(DataMsg msg);
    void sendData(Link& link, DataMsg msg, uint64_t now);
    void sendMsg(const Message& m, const sockaddr_storage& to, int toLen);
    void sendLink(Link& link, Message m);
    void sendRaw(const std::vector<uint8_t>& dg, const sockaddr_storage& to, int toLen);
    void tick(uint64_t now);
    void broadcastRoster();
    void rosterChanged();
    std::vector<std::string> rosterLocked() const;
    Link* hostClientByAddr(const sockaddr_storage& addr, int len);
    bool resolveHost();
    bool openSocket(int family, uint16_t port);

    DirectOptions opt_;
    SOCKET sock_ = INVALID_SOCKET;
    int family_ = AF_INET6;
    uint16_t boundPort_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<bool> testBlackhole_{false};
    std::thread thread_;
    std::mutex mu_;

    std::string localPuid_;
    uint32_t localNonce_ = 0;
    std::deque<Delivered> inbox_;
    std::map<std::string, uint64_t> lastDataMs_;  // per source: last game packet received

    // Host mode.
    std::map<std::string, Link> clients_;
    uint64_t lastRosterMs_ = 0;
    uint64_t rosterBurstUntilMs_ = 0;

    // Join mode.
    std::optional<Link> hostLink_;
    std::vector<std::string> roster_;
    sockaddr_storage hostAddr_{};
    int hostAddrLen_ = 0;
    uint64_t lastHelloMs_ = 0;
    uint64_t lastResolveMs_ = 0;
};

std::string shortId(const std::string& puid);
std::string addrToString(const sockaddr_storage& addr, int len);

// Splits a host's advertised address list ("v4:port [v6]:port ...") into the order to try:
// IPv4 and host names first, IPv6 last.
std::vector<std::string> orderHostCandidates(const std::string& advertised);

}  // namespace dn
