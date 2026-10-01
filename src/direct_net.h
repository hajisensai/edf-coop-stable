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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "auth.h"
#include "reliable.h"
#include "wire.h"

namespace dn {

enum class Mode { Off, Host, Join };

struct DirectOptions {
    Mode mode = Mode::Off;
    uint16_t listenPort = 27015;  // host: UDP port to listen on; join: local port (0 = any)
    std::string hostAddress;      // join: "1.2.3.4:27015", "[2408::1]:27015" or "name.ddns.net:27015"
    // join: hostAddress was advertised by a room host (AutoJoin), not typed in by this player. Then it
    // must name a real remote machine: loopback, unspecified, multicast, broadcast and link-local
    // targets are refused, also after DNS, so a host cannot aim its joiners at themselves or others.
    bool advertisedHost = false;
    std::string key;              // optional shared secret; must be identical for everyone
    // Proves our EOS id: a joiner's to the host (see setMemberIdentities), a host's to its joiners (see
    // setRoomOwner). null: processIdentity(), the one this process publishes in the room.
    std::shared_ptr<const Identity> identity;
    // join: the room owner's EOS id and the identity commitment it published; setRoomOwner() replaces
    // them.
    std::string roomOwner;
    std::string roomOwnerIdentity;
    // host: the room members' published identity commitments (EOS id -> Identity::commitment()) to
    // start with; setMemberIdentities() replaces them.
    std::map<std::string, std::string> memberIds;
    uint32_t ifIndexV4 = 0;       // IP_UNICAST_IF: force egress through this adapter (0 = OS routing)
    uint32_t ifIndexV6 = 0;
    // A link closes when nothing authentic arrived from the peer for this long ("silent").
    uint32_t linkTimeoutMs = 60000;
    // ...or when a packet the game sent reliably has been unacknowledged this long while the peer is
    // still heard ("stalled"). Kept short on purpose: while game data is stuck in a stalled link the
    // other players may be waiting for it at a sync point. Unreliable game packets never stall a link:
    // they are given up after ReliableSender::kExpireMs.
    uint32_t stallTimeoutMs = 60000;
    uint32_t pingIntervalMs = 1000;
    // host: the member list sent to joiners names only clients heard from within this long, so every
    // joiner learns within seconds (not after linkTimeoutMs) that a member's link went quiet.
    uint32_t rosterFreshMs = 10000;
    // EDF6 sends all game data UnreliableUnordered, also data whose loss breaks the game. Carrying it
    // with a sequence number repairs loss (delivered at most once, possibly reordered: a pattern the
    // unreliable transport can produce anyway, so the game handles it) until ReliableSender::kExpireMs;
    // a packet the network still has not let through by then is given up, as EOS would have lost it.
    bool upgradeUnreliable = true;
    // Retransmission allowance shared with other links; null: the one of this process.
    std::shared_ptr<RetransmitBudget> retransmitBudget;
    double testDropRate = 0.0;  // tests only: drop this fraction of outgoing datagrams
    // Tests only: an uplink of this many bytes per second (0 = unlimited) with a 100 ms queue; what does
    // not fit is dropped, as a full router queue does.
    uint64_t testUplinkBytesPerSecond = 0;
};

// UDP payload bytes on our socket since the last takeWireTraffic(), retransmits and pings included.
struct WireTraffic {
    uint64_t out = 0;
    uint64_t in = 0;
    uint64_t relayed = 0;  // host: game data forwarded from one client to another
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
    // host: who may connect. A hello claiming EOS id X is accepted only when it is signed by the
    // identity whose commitment X published in the room (EOS id -> commitment): only X itself can set
    // X's lobby member attributes. Members that published none (vanilla players, plugin 0.3.6 and
    // older) can never be claimed; their traffic stays on EOS.
    void setMemberIdentities(std::map<std::string, std::string> commitments);
    // join: which host to accept. A welcome counts only when it comes from the room owner `puid`,
    // signed by the identity whose commitment it published in the room: the address we dial was
    // advertised by the room (or typed in), which proves nothing about who answers on it.
    void setRoomOwner(const std::string& puid, const std::string& commitment);
    // True when packets to `remote` can go over the direct transport right now.
    bool canRoute(const std::string& remote);
    // Sends a game packet over the direct transport. Returns false when `remote` is not routable.
    bool send(const std::string& remote, const std::string& socketName, uint8_t channel, uint8_t reliability,
              const uint8_t* data, size_t size);
    // Pops the next packet for the local player; `channel` filters like EOS RequestedChannel.
    bool pop(const uint8_t* channel, uint32_t maxSize, Delivered& out);

    // True when a game packet from `remote` arrived over the direct link within `windowMs`.
    bool heardFromRecently(const std::string& remote, uint64_t windowMs);
    // True when the direct link that carries `remote`'s traffic answered within `windowMs` (links
    // ping every second, in menus too). A joiner reaches other members through the host, which drops
    // them from its roster once their own link to it times out.
    bool linkAlive(const std::string& remote, uint64_t windowMs);
    // True when any of our direct links answered within `windowMs`.
    bool anyLinkAlive(uint64_t windowMs);
    // Direct links belong to the room we are in. Inactive (not in a room): every link is closed with a
    // BYE, a host welcomes nobody and a joiner dials nobody, so a player back at the title screen whose
    // plugin still runs cannot look like someone still playing. Active by default.
    void setActive(bool active);

    std::vector<std::string> directMembers();
    std::string statusLine();
    WireTraffic takeWireTraffic();
    // Link datagrams dropped since start because they failed authentication: a bad tag (forged,
    // modified, or of an earlier session), or a counter already seen (replayed).
    uint64_t rejectedPackets() const { return rejected_; }
    // Datagrams the socket refused to send since start (its buffer full, or no route).
    uint64_t sendFailures() const { return sendFailures_; }

    // Tests only: silently drop every outgoing datagram (simulates a total network outage).
    void setTestBlackhole(bool on) { testBlackhole_ = on; }

private:
    struct Link {
        sockaddr_storage addr{};
        int addrLen = 0;
        std::string puid;
        uint32_t peerNonce = 0;
        uint64_t session = 0;  // host: the client's HelloMsg::session
        uint32_t epoch = 0;  // linkEpoch(client nonce, host nonce)
        // A joiner's link is up once the host's welcome verified. A host's link is up once the client
        // sent a datagram with the link keys (proof it got the welcome): until then data sent to it
        // would be dropped unread.
        bool up = false;
        uint64_t lastRecvMs = 0;
        uint64_t lastPingMs = 0;
        uint32_t rttMs = 0;
        ReliableSender tx;
        ReliableReceiver rx;
        LinkMac txMac, rxMac;  // our direction's key, the peer's
        uint64_t txCounter = 0;
        ReplayWindow replay;
        uint64_t rosterCounter = 0;  // joiner: counter of the newest member list applied
        PublicKey peerEcdh{};  // host: the client's ECDH key of this session
        WelcomeMsg welcome;  // host: what we answer this session's hellos with (signed on demand)
        std::vector<uint8_t> welcomeDatagram;  // ...and its last encoding, reused while the roster stays
    };

    // A link that carries game packets now: up, and not being closed for an unacknowledged backlog.
    static bool usable(const Link& link) { return link.up && !link.tx.overloaded(); }
    void run();
    // A received datagram: its bytes (a link message's tag is checked against them) and decoding.
    struct Received {
        const uint8_t* data;
        size_t size;
        const Message& msg;
    };

    void processDatagram(const uint8_t* data, size_t size, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onHostDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onHostHello(const HelloMsg& h, const sockaddr_storage& from, int fromLen, uint64_t now);
    bool openHostLink(Link& link, const HelloMsg& h);
    void sendWelcome(Link& link, const sockaddr_storage& to, int toLen);
    std::optional<Cookie> cookieFor(const HelloMsg& h, const sockaddr_storage& from, int fromLen, uint64_t bucket);
    const char* identityRefusal(const HelloMsg& h);
    const char* welcomeRefusal(const WelcomeMsg& w);
    void sendHello(uint64_t now);
    void newLocalSession();
    void onClientDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now);
    void onClientWelcome(const WelcomeMsg& w, const sockaddr_storage& from, int fromLen, uint64_t now);
    void dropHostLink(const char* why);
    const char* linkRefusal(Link& link, const Received& r);
    bool authentic(Link& link, const Received& r, const sockaddr_storage& from, int fromLen);
    void onLinkCommon(Link& link, const Message& m, uint64_t now);
    void routeData(DataMsg msg);
    void deliverLocal(DataMsg msg);
    void sendData(Link& link, DataMsg msg, uint64_t now);
    void sendMsg(const Message& m, const sockaddr_storage& to, int toLen);
    void sendLink(Link& link, Message m);
    bool sendSealed(Link& link, std::vector<uint8_t>& dg);
    void sendBye();
    bool sendRaw(const std::vector<uint8_t>& dg, const sockaddr_storage& to, int toLen);
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
    std::atomic<uint64_t> wireOut_{0}, wireIn_{0}, relayed_{0}, rejected_{0}, sendFailures_{0};
    std::shared_ptr<RetransmitBudget> retransmitBudget_;
    double testUplinkTokens_ = 0;  // bytes, see DirectOptions::testUplinkBytesPerSecond
    uint64_t testUplinkMs_ = 0;
    std::thread thread_;
    std::mutex mu_;

    std::string localPuid_;
    uint32_t localNonce_ = 0;
    std::shared_ptr<const Identity> identity_;
    std::deque<Delivered> inbox_;
    std::map<std::string, uint64_t> lastDataMs_;  // per source: last game packet received

    // Host mode.
    std::map<std::string, Link> clients_;
    std::string cookieSecret_;  // random per instance: cookies need no state and cannot be forged
    std::map<std::string, std::string> memberIds_;  // see setMemberIdentities
    struct Seen {
        std::string commitment;
        uint64_t session = 0;
    };
    std::map<std::string, Seen> seen_;  // newest session accepted per member: older hellos are replays
    uint64_t lastRosterMs_ = 0;
    std::vector<std::string> lastRoster_;  // what the clients were last told
    bool active_ = true;
    uint64_t rosterBurstUntilMs_ = 0;

    // Join mode.
    std::optional<Link> hostLink_;
    uint64_t localSession_ = 0;  // HelloMsg::session of our current session
    std::unique_ptr<EcdhKey> localEcdh_;  // our ECDH key of the current session
    std::optional<Cookie> cookie_;  // the host's cookie for our current session
    std::string roomOwner_, roomOwnerId_;  // see setRoomOwner
    std::vector<std::string> roster_;
    sockaddr_storage hostAddr_{};  // where we send: the address we dialled
    int hostAddrLen_ = 0;
    sockaddr_storage hostReplyAddr_{};  // where the host's Welcome came from (may differ, see onClientDatagram)
    int hostReplyAddrLen_ = 0;
    uint64_t lastHelloMs_ = 0;
    uint64_t lastResolveMs_ = 0;
};

std::string shortId(const std::string& puid);
std::string addrToString(const sockaddr_storage& addr, int len);

// Splits a host's advertised address list ("v4:port [v6]:port ...") into the order to try:
// IPv4 and host names first, IPv6 last. Anything a joiner must not dial is left out (see
// DirectOptions::advertisedHost; numeric-looking names like "127.1" count as addresses, not names),
// and at most kMaxHostCandidates entries are kept, which also bounds the DNS lookups a host can cause.
constexpr size_t kMaxHostCandidates = 4;
std::vector<std::string> orderHostCandidates(const std::string& advertised);

}  // namespace dn
