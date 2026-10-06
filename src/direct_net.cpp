#include "direct_net.h"

#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include "log.h"
#include "netclass.h"

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

namespace dn {
namespace {

constexpr size_t kMaxDatagram = 2048;
constexpr size_t kMaxInbox = 16384;
constexpr uint64_t kHelloIntervalMs = 1000;
constexpr uint64_t kRosterIntervalMs = 5000;
// Roster datagrams are unreliable; after a change resend quickly so a lost copy does not leave a
// member unreachable (and its traffic falling back to EOS) until the next periodic broadcast.
constexpr uint64_t kRosterBurstMs = 1000;
constexpr uint64_t kRosterBurstIntervalMs = 200;
constexpr uint64_t kResolveIntervalMs = 30000;
// A Reset (unauthenticated) ends a joiner's link only when the host has not been heard for this long:
// a live host pings every second, so a forged Reset cannot end a working link, while a restarted host
// is reconnected within seconds instead of after the link timeout.
constexpr uint64_t kResetQuietMs = 3000;
// A cookie is valid in the time bucket it was made in and the next one (20-40 s): long enough for a
// client to answer, short enough that a captured proven hello soon stops being accepted at all.
constexpr uint64_t kCookieBucketMs = 20000;
constexpr uint16_t kDefaultPort = 27015;
// Rooms hold up to 1024 players (the netcode rewrite's target; EDF6Coop 2.4 rooms hold 32). The margin leaves
// room for links of players who left and have not timed out yet, which are no longer in the member list (see
// rosterLocked); the cap also bounds what a hello flood with made-up ids can allocate. Member lists of more than
// kRosterPage go in pages (wire.h). A joiner's links to other joiners (mesh) are bounded the same way.
constexpr size_t kMaxClients = 1100;
// --- Paths (see direct_net.h) ---
// A link not heard from for this long (or 2.5 ping intervals, whichever is longer) is not healthy.
constexpr uint64_t kPathStaleMinMs = 1500;
// An unhealthy direct link carries state again once it has been healthy this long without a break.
constexpr uint64_t kPathRecoverMs = 1000;
// Losing this share of its pings (smoothed over about ten) makes a link unhealthy.
constexpr double kLossUnhealthy = 0.25;
// A link whose queueing delay (RateController::queueMs) or unacknowledged backlog exceeds these is congested:
// state goes around it through the relay.
constexpr uint32_t kCongestedQueueMs = 150;
constexpr size_t kCongestedBacklog = 256 * 1024;
// Pings: a link carrying game data (within kActiveMs) is pinged every kActivePingMs, but at least kActivePerLinkMs
// apart per active link of ours, so 1000 links cost what 4 do at most... rather, cost a fixed rate; an idle link at
// the configured interval, kIdlePerLinkMs per link of ours, at most kIdlePingMaxMs.
constexpr uint64_t kActiveMs = 2000;
constexpr uint64_t kActivePingMs = 250;
constexpr uint64_t kActivePerLinkMs = 2;
constexpr uint64_t kIdlePerLinkMs = 10;
constexpr uint64_t kIdlePingMaxMs = 10000;
// Introductions (PeerQuery): asked again every kIntroQueryMs while no link comes up; after kIntroFailures in a row
// the pair stays on the relay for kIntroBackoffMs. A dial that got no welcome in kDialTimeoutMs is given up, a
// direct link that carried no game data for kPeerIdleMs is closed (the relay is always there).
constexpr uint64_t kIntroQueryMs = 3000;
constexpr uint32_t kIntroFailures = 3;
constexpr uint64_t kIntroBackoffMs = 60000;
constexpr uint64_t kDialTimeoutMs = 10000;
constexpr uint64_t kPeerIdleMs = 60000;
constexpr uint64_t kIntroducedMs = 1000;  // host: one introduction of a pair per second at most
static_assert(kMaxPayload + 3 * kMaxString + 64 < kMaxDatagram, "a full Data datagram must fit the receive buffer");

// Millisecond resolution: GetTickCount64 moves in ~15.6 ms steps, which made every RTT sample on a
// steady link the same number, the RTT variance zero and the retransmission timeout equal to the RTT.
uint64_t nowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

uint32_t randomNonce() {
    static std::random_device rd;
    uint32_t n = 0;
    while (n == 0) n = rd();
    return n;
}

bool sameAddr(const sockaddr_storage& a, int alen, const sockaddr_storage& b, int blen) {
    return alen == blen && memcmp(&a, &b, static_cast<size_t>(alen)) == 0;
}

// Splits "host:port", "[v6]:port", "v6" or "host" into host and port.
bool splitHostPort(const std::string& s, std::string& host, uint16_t& port) {
    port = kDefaultPort;
    if (s.empty()) return false;
    std::string p;
    if (s[0] == '[') {
        size_t close = s.find(']');
        if (close == std::string::npos) return false;
        host = s.substr(1, close - 1);
        if (close + 1 < s.size() && s[close + 1] == ':') p = s.substr(close + 2);
    } else if (std::count(s.begin(), s.end(), ':') == 1) {
        size_t colon = s.find(':');
        host = s.substr(0, colon);
        p = s.substr(colon + 1);
    } else {
        host = s;  // hostname, IPv4 or bare IPv6 without port
    }
    if (!p.empty()) {
        int v = atoi(p.c_str());
        if (v <= 0 || v > 65535) return false;
        port = static_cast<uint16_t>(v);
    }
    return !host.empty();
}

// Converts an IPv4 address to its IPv4-mapped IPv6 form so a dual-stack socket can use it.
void toDualStack(sockaddr_storage& addr, int& len) {
    if (addr.ss_family != AF_INET) return;
    sockaddr_in v4 = *reinterpret_cast<sockaddr_in*>(&addr);
    sockaddr_in6 v6{};
    v6.sin6_family = AF_INET6;
    v6.sin6_port = v4.sin_port;
    v6.sin6_addr.u.Byte[10] = 0xff;
    v6.sin6_addr.u.Byte[11] = 0xff;
    memcpy(&v6.sin6_addr.u.Byte[12], &v4.sin_addr, 4);
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, &v6, sizeof(v6));
    len = sizeof(v6);
}

// Whether a room host may send its joiners to this address (see DirectOptions::advertisedHost).
// Private LAN ranges are allowed: LAN play is real.
bool dialableV4(const uint8_t* b) {
    return b[0] != 0 && b[0] != 127 && b[0] < 224 && !(b[0] == 169 && b[1] == 254);
}

bool dialable(const sockaddr_storage& addr) {
    if (addr.ss_family == AF_INET)
        return dialableV4(reinterpret_cast<const uint8_t*>(&reinterpret_cast<const sockaddr_in*>(&addr)->sin_addr));
    if (addr.ss_family != AF_INET6) return false;
    const uint8_t* b = reinterpret_cast<const sockaddr_in6*>(&addr)->sin6_addr.u.Byte;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (memcmp(b, mapped, 12) == 0) return dialableV4(b + 12);
    bool zeroHead = std::all_of(b, b + 15, [](uint8_t x) { return x == 0; });
    bool linkLocal = b[0] == 0xfe && (b[1] & 0xc0) == 0x80;
    return !(zeroHead && b[15] <= 1) && b[0] != 0xff && !linkLocal;  // ::, ::1, multicast, fe80::/10
}

// A literal IPv4/IPv6 address as a socket address; false for anything else.
bool parseLiteral(const std::string& host, sockaddr_storage& addr) {
    memset(&addr, 0, sizeof(addr));
    auto* v4 = reinterpret_cast<sockaddr_in*>(&addr);
    auto* v6 = reinterpret_cast<sockaddr_in6*>(&addr);
    if (inet_pton(AF_INET, host.c_str(), &v4->sin_addr) == 1) {
        addr.ss_family = AF_INET;
        return true;
    }
    if (inet_pton(AF_INET6, host.c_str(), &v6->sin6_addr) == 1) {
        addr.ss_family = AF_INET6;
        return true;
    }
    return false;
}

// A DNS name made of letters, digits, '-' and '.', whose last label has a letter: getaddrinfo reads
// all-numeric strings such as "127.1" or "2130706433" as IPv4 addresses.
bool plausibleHostName(const std::string& host) {
    if (host.empty() || host.size() > 253) return false;
    for (char c : host)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '.') return false;
    size_t dot = host.find_last_of('.', host.size() - 2);
    std::string last = host.substr(dot == std::string::npos ? 0 : dot + 1);
    return std::any_of(last.begin(), last.end(), [](char c) { return isalpha(static_cast<unsigned char>(c)) != 0; });
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

// A literal address with its port, as a host's PeerInfo names it ("1.2.3.4:5", "[v6]:5").
bool parseAddress(const std::string& text, sockaddr_storage& addr, int& len) {
    std::string host;
    uint16_t port = 0;
    if (!splitHostPort(text, host, port) || !parseLiteral(host, addr)) return false;
    if (addr.ss_family == AF_INET) {
        reinterpret_cast<sockaddr_in*>(&addr)->sin_port = htons(port);
        len = sizeof(sockaddr_in);
    } else {
        reinterpret_cast<sockaddr_in6*>(&addr)->sin6_port = htons(port);
        len = sizeof(sockaddr_in6);
    }
    return true;
}

constexpr uint8_t kState = static_cast<uint8_t>(TrafficClass::State);
constexpr uint8_t kEvent = static_cast<uint8_t>(TrafficClass::Event);
constexpr uint8_t kControl = static_cast<uint8_t>(TrafficClass::Control);

}  // namespace

const char* pathName(Path p) {
    switch (p) {
        case Path::Direct: return "direct";
        case Path::Relay: return "relay";
        case Path::None: break;
    }
    return "none";
}

std::string shortId(const std::string& puid) { return puid.size() > 8 ? puid.substr(0, 8) : puid; }

std::string addrToString(const sockaddr_storage& addr, int len) {
    char host[INET6_ADDRSTRLEN] = "?";
    char serv[16] = "?";
    getnameinfo(reinterpret_cast<const sockaddr*>(&addr), len, host, sizeof(host), serv, sizeof(serv),
                NI_NUMERICHOST | NI_NUMERICSERV);
    std::string h = host;
    if (h.rfind("::ffff:", 0) == 0) return h.substr(7) + ":" + serv;
    if (addr.ss_family == AF_INET6) return "[" + h + "]:" + serv;
    return h + ":" + serv;
}

DirectNet::~DirectNet() { stop(); }

bool DirectNet::openSocket(int family, uint16_t port) {
    SOCKET s = socket(family, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return false;
    if (family == AF_INET6) {
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof(off));
    }
    // Windows reports ICMP port-unreachable as WSAECONNRESET on the next recvfrom; ignore it.
    BOOL noReset = FALSE;
    DWORD bytes = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), nullptr, 0, &bytes, nullptr, nullptr);
    int buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&buf), sizeof(buf));
    // Pin egress to the physical adapter so VPN/TUN catch-all routes do not capture game traffic.
    if (opt_.ifIndexV4) {
        DWORD idx = htonl(opt_.ifIndexV4);
        setsockopt(s, IPPROTO_IP, IP_UNICAST_IF, reinterpret_cast<const char*>(&idx), sizeof(idx));
    }
    if (family == AF_INET6 && opt_.ifIndexV6) {
        DWORD idx = opt_.ifIndexV6;
        setsockopt(s, IPPROTO_IPV6, IPV6_UNICAST_IF, reinterpret_cast<const char*>(&idx), sizeof(idx));
    }

    sockaddr_storage local{};
    int localLen = 0;
    if (family == AF_INET6) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&local);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(port);
        localLen = sizeof(sockaddr_in6);
    } else {
        auto* a = reinterpret_cast<sockaddr_in*>(&local);
        a->sin_family = AF_INET;
        a->sin_port = htons(port);
        localLen = sizeof(sockaddr_in);
    }
    if (bind(s, reinterpret_cast<sockaddr*>(&local), localLen) != 0) {
        logf("DIRECT socket bind failed on UDP %u (family %d): WSA error %d", port, family, WSAGetLastError());
        closesocket(s);
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);

    sockaddr_storage bound{};
    int boundLen = sizeof(bound);
    getsockname(s, reinterpret_cast<sockaddr*>(&bound), &boundLen);
    boundPort_ = ntohs(family == AF_INET6 ? reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port
                                          : reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
    sock_ = s;
    family_ = family;
    return true;
}

bool DirectNet::start(const DirectOptions& options) {
    if (running_ || options.mode == Mode::Off) return false;
    opt_ = options;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    bool opened = openSocket(AF_INET6, opt_.listenPort) || openSocket(AF_INET, opt_.listenPort);
    if (!opened && opt_.mode == Mode::Join && opt_.listenPort != 0) {
        // A joining player's local port is a preference, not something anyone connects to.
        logf("DIRECT UDP port %u is busy; joining from a random port instead", opt_.listenPort);
        opened = openSocket(AF_INET6, 0) || openSocket(AF_INET, 0);
    }
    std::string secret(32, '\0');
    if (opened && !randomBytes(reinterpret_cast<uint8_t*>(secret.data()), secret.size())) {
        logf("DIRECT cannot get random bytes from Windows; direct link disabled");
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
        opened = false;
    }
    if (!opened) {
        WSACleanup();
        return false;
    }
    cookieSecret_ = std::move(secret);
    retransmitBudget_ = opt_.retransmitBudget ? opt_.retransmitBudget : processRetransmitBudget();
    testUplinkTokens_ = 0;
    testUplinkMs_ = 0;
    memberIds_ = opt_.memberIds;
    roomOwner_ = opt_.roomOwner;
    roomOwnerId_ = opt_.roomOwnerIdentity;
    identity_ = opt_.identity ? opt_.identity : processIdentity();
    if (!identity_)
        logf("DIRECT cannot create our direct-link identity (Windows crypto failed): %s, the game stays on EOS",
             opt_.mode == Mode::Join ? "hosts cannot check who we are and will not accept us"
                                     : "joiners cannot check who we are and will not accept us");
    newLocalSession();
    running_ = true;
    thread_ = std::thread([this] { run(); });
    logf("DIRECT started as %s on UDP port %u (%s)%s", opt_.mode == Mode::Host ? "HOST" : "JOIN", boundPort_,
         family_ == AF_INET6 ? "IPv4+IPv6" : "IPv4 only", opt_.key.empty() ? "" : ", key enabled");
    return true;
}

void DirectNet::stop() {
    if (!running_) return;
    // Stop the receive thread first: otherwise a peer that reacts to our BYE with a fresh hello
    // gets welcomed by this dying instance and keeps a link to a socket that is about to close.
    running_ = false;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        sendBye();
        // Another thread may still hold a pointer to this instance (AutoJoin swaps instances):
        // with no links left, send() and canRoute() report false and the game falls back to EOS.
        clients_.clear();
        hostLink_.reset();
        roster_.clear();
        peerLinks_.clear();
        dials_.clear();
        intros_.clear();
        inbox_.clear();
        lastDataMs_.clear();
        closesocket(sock_);
        sock_ = INVALID_SOCKET;
    }
    WSACleanup();
    logf("DIRECT stopped");
}

void DirectNet::setLocalUser(const std::string& puid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (puid == localPuid_) return;
    if (!localPuid_.empty()) {
        // Signed in as a different EOS user: every link was bound to the old identity.
        logf("DIRECT local user changed %s -> %s, resetting links", shortId(localPuid_).c_str(),
             shortId(puid).c_str());
        clients_.clear();
        hostLink_.reset();
        roster_.clear();
        peerLinks_.clear();
        peerSeen_.clear();
        dials_.clear();
        intros_.clear();
        inbox_.clear();
        lastDataMs_.clear();
        newLocalSession();
    }
    localPuid_ = puid;
    logf("DIRECT local EOS user %s", puid.c_str());
}

void DirectNet::setMemberIdentities(std::map<std::string, std::string> commitments) {
    std::lock_guard<std::mutex> lock(mu_);
    memberIds_ = std::move(commitments);
    for (auto it = seen_.begin(); it != seen_.end();)  // bounded by the room, not by what anyone claims
        it = memberIds_.count(it->first) ? std::next(it) : seen_.erase(it);
}

void DirectNet::setRoomOwner(const std::string& puid, const std::string& commitment) {
    std::lock_guard<std::mutex> lock(mu_);
    roomOwner_ = puid;
    roomOwnerId_ = commitment;
}

void DirectNet::newLocalSession() {
    localNonce_ = randomNonce();
    localSession_ = identity_ ? identity_->nextSession() : 0;
    cookie_.reset();
    if (opt_.mode == Mode::Join) localEcdh_ = EcdhKey::generate();  // null (crypto failed): nobody welcomes us
}

// Every link is told three times: one lost datagram must not leave a stale link behind.
void DirectNet::sendBye() {
    Message bye;
    bye.type = MsgType::Bye;
    for (int copy = 0; copy < 3; ++copy) {
        for (auto& [id, link] : clients_) sendLink(link, bye);
        if (hostLink_) sendLink(*hostLink_, bye);
        for (auto& [id, link] : peerLinks_) sendLink(link, bye);
        for (auto& [id, dial] : dials_)
            if (dial.link) sendLink(*dial.link, bye);
    }
}

bool DirectNet::canRoute(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return false;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        return it != clients_.end() && usable(it->second);
    }
    if (hostLink_ && usable(*hostLink_) && (remote == hostLink_->puid || contains(roster_, remote))) return true;
    return peerLink(remote) != nullptr;
}

bool DirectNet::send(const std::string& remote, const std::string& socketName, uint8_t channel, uint8_t reliability,
                     const uint8_t* data, size_t size) {
    return sendClassified(remote, socketName, channel, reliability, data, size, 0).sent;
}

DirectNet::Link* DirectNet::peerLink(const std::string& remote) {
    if (opt_.mode != Mode::Join) return nullptr;
    if (auto it = peerLinks_.find(remote); it != peerLinks_.end() && usable(it->second)) return &it->second;
    if (auto it = dials_.find(remote); it != dials_.end() && it->second.link && usable(*it->second.link))
        return &*it->second.link;
    return nullptr;
}

bool DirectNet::blockedPeer(const std::string& puid) const {
    if (testBlockToMs_) {
        const uint64_t now = nowMs();
        if (now >= testBlockFromMs_ && now < testBlockToMs_) return true;
    }
    auto it = testBlocked_.find(puid);
    return it != testBlocked_.end() && it->second;
}

void DirectNet::setTestBlockPeers(uint64_t afterMs, uint64_t forMs) {
    std::lock_guard<std::mutex> lock(mu_);
    testBlockFromMs_ = nowMs() + afterMs;
    testBlockToMs_ = forMs == UINT64_MAX ? UINT64_MAX : testBlockFromMs_ + forMs;
}

bool DirectNet::congested(const Link& link) const {
    return (link.cc.measured() && link.cc.queueMs() > kCongestedQueueMs) || link.tx.pendingBytes() > kCongestedBacklog;
}

uint64_t DirectNet::pingIntervalFor(const Link& link, uint64_t now) const {
    const uint64_t links = clients_.size() + peerLinks_.size() + dials_.size() + (hostLink_ ? 1 : 0);
    if (link.lastDataMs && now - link.lastDataMs < kActiveMs)
        return std::min<uint64_t>(opt_.pingIntervalMs, std::max<uint64_t>(kActivePingMs, links * kActivePerLinkMs));
    // A joiner's link to its host keeps the configured beat: the host's member list goes by it (rosterFreshMs).
    if (opt_.mode == Mode::Join && !link.peer) return opt_.pingIntervalMs;
    return std::max<uint64_t>(opt_.pingIntervalMs, std::min<uint64_t>(links * kIdlePerLinkMs, kIdlePingMaxMs));
}

bool DirectNet::healthy(const Link& link, uint64_t now) const {
    const uint64_t stale = std::max<uint64_t>(kPathStaleMinMs, pingIntervalFor(link, now) * 5 / 2);
    return usable(link) && now - link.lastRecvMs <= stale && link.loss < kLossUnhealthy && !congested(link);
}

DirectNet::Route DirectNet::routeFor(const std::string& remote, uint8_t cls, uint64_t now) {
    Route route;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        if (it != clients_.end() && usable(it->second)) route = {&it->second, nullptr, Path::Direct};
        return route;
    }
    Link* host = hostLink_ && usable(*hostLink_) ? &*hostLink_ : nullptr;
    if (host && remote == host->puid) return {host, nullptr, Path::Direct};
    Link* direct = opt_.mesh ? peerLink(remote) : nullptr;
    Link* relay = host && contains(roster_, remote) ? host : nullptr;
    if (opt_.mesh && !direct && relay) wantPeer(remote, now);
    if (cls == kEvent || cls == kControl) {
        // Both at once: whichever arrives first is taken, the other is a copy the receiver drops.
        if (direct) return {direct, relay, Path::Direct};
        return {relay, nullptr, relay ? Path::Relay : Path::None};
    }
    const bool good = direct && healthy(*direct, now) && direct->healthySinceMs && now - direct->healthySinceMs >= kPathRecoverMs;
    if (good) {
        // Suspect (two pings' time without a word from it, a round trip included): the copy over the relay makes
        // sure state keeps arriving until the direct link is known healthy or stale, whichever it turns out to be.
        const uint64_t quiet = 2 * pingIntervalFor(*direct, now) + direct->rttMs;
        return {direct, relay && now - direct->lastRecvMs > quiet ? relay : nullptr, Path::Direct};
    }
    if (relay) return {relay, nullptr, Path::Relay};
    if (direct) return {direct, nullptr, Path::Direct};
    return route;
}

bool DirectNet::takeStateBudget(Link& link, size_t bytes, uint64_t now) {
    const double rate = link.cc.rate();
    const double cap = std::max(rate / 4, 2.0 * static_cast<double>(bytes));  // 250 ms, and a datagram always fits
    if (link.stateTokensMs == 0) link.stateTokens = cap;
    link.stateTokens = std::min(cap, link.stateTokens + static_cast<double>(now - link.stateTokensMs) * rate / 1000.0);
    link.stateTokensMs = now;
    if (link.stateTokens < static_cast<double>(bytes)) return false;
    link.stateTokens -= static_cast<double>(bytes);
    return true;
}

SendReport DirectNet::sendClassified(const std::string& remote, const std::string& socketName, uint8_t channel,
                                     uint8_t reliability, const uint8_t* data, size_t size, uint8_t cls) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_ || size > kMaxPayload || cls > 3) return {};
    const uint64_t now = nowMs();
    Route route = routeFor(remote, cls, now);
    if (!route.first) return {};
    // The game's own datagrams only: a packet the plugin sends with a reliability of its own goes as it asks.
    if (reliability != 0) cls = 0;
    if (cls == kState && opt_.shedState && !takeStateBudget(*route.first, size, now)) {
        ++stateShed_;
        return {true, 0};
    }
    DataMsg msg;
    msg.src = localPuid_;
    msg.dst = remote;
    msg.socketName = socketName;
    msg.channel = channel;
    msg.reliability = reliability;
    msg.cls = cls;
    msg.payload.assign(data, data + size);
    auto count = [&](const Link& link) {
        (link.puid == remote ? directOut_ : viaRelayOut_) += size;
    };
    count(*route.first);
    if (route.second) {
        count(*route.second);
        sendData(*route.second, msg, now);
    }
    sendData(*route.first, std::move(msg), now);
    return {true, route.second ? 2 : 1};
}

uint32_t DirectNet::linkBudget(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return 0;
    Route route = routeFor(remote, kState, nowMs());
    if (!route.first) return 0;
    uint32_t rate = route.first->cc.rate();
    // Through the host, what it relays for us also comes out of the host's own budget for that member.
    if (route.path == Path::Relay) rate = std::min<uint32_t>(rate, static_cast<uint32_t>(opt_.relayBytesPerSecond));
    return rate;
}

Path DirectNet::pathTo(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return Path::None;
    return routeFor(remote, kState, nowMs()).path;
}

bool DirectNet::peerLinked(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    return peerLink(remote) != nullptr;
}

void DirectNet::setTestBlockPeer(const std::string& puid, bool blocked) {
    std::lock_guard<std::mutex> lock(mu_);
    testBlocked_[puid] = blocked;
}

bool DirectNet::pop(const uint8_t* channel, uint32_t maxSize, Delivered& out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto it = inbox_.begin(); it != inbox_.end(); ++it) {
        if (channel && it->channel != *channel) continue;
        if (it->data.size() > maxSize) {
            logRateLimited("inbox-too-big", 5000, "DIRECT dropped %zu-byte packet: game buffer is %u bytes",
                           it->data.size(), maxSize);
            inbox_.erase(it);
            return false;
        }
        out = std::move(*it);
        inbox_.erase(it);
        return true;
    }
    return false;
}

bool DirectNet::heardFromRecently(const std::string& remote, uint64_t windowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = lastDataMs_.find(remote);
    return it != lastDataMs_.end() && nowMs() - it->second <= windowMs;
}

bool DirectNet::linkAlive(const std::string& remote, uint64_t windowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote.empty() || remote == localPuid_) return false;
    uint64_t now = nowMs();
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        return it != clients_.end() && it->second.up && now - it->second.lastRecvMs <= windowMs;
    }
    if (Link* peer = peerLink(remote); peer && now - peer->lastRecvMs <= windowMs) return true;
    return hostLink_ && hostLink_->up && now - hostLink_->lastRecvMs <= windowMs &&
           (remote == hostLink_->puid || contains(roster_, remote));
}

uint64_t DirectNet::linkId(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote.empty() || remote == localPuid_) return 0;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        return it != clients_.end() && it->second.up ? it->second.id : 0;
    }
    return hostLink_ && hostLink_->up && (remote == hostLink_->puid || contains(roster_, remote)) ? hostLink_->id : 0;
}

bool DirectNet::anyLinkAlive(uint64_t windowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t now = nowMs();
    if (opt_.mode == Mode::Host) {
        for (const auto& [id, link] : clients_)
            if (link.up && now - link.lastRecvMs <= windowMs) return true;
        return false;
    }
    return hostLink_ && hostLink_->up && now - hostLink_->lastRecvMs <= windowMs;
}

void DirectNet::setActive(bool active) {
    std::lock_guard<std::mutex> lock(mu_);
    if (active == active_) return;
    active_ = active;
    if (active) {
        logf("DIRECT links open again (in a room)");
        return;
    }
    sendBye();
    size_t closed = clients_.size() + (hostLink_ ? 1 : 0) + peerLinks_.size() + dials_.size();
    clients_.clear();
    hostLink_.reset();
    roster_.clear();
    lastRoster_.clear();
    peerLinks_.clear();
    peerSeen_.clear();
    dials_.clear();
    intros_.clear();
    introduced_.clear();
    rosterPages_ = {};
    roomPages_ = {};
    // Unread game packets and their diagnostics belong to the room just left, not the next one.
    inbox_.clear();
    lastDataMs_.clear();
    // The room's member lists belong to the room: the next one starts with nothing said.
    roomMembers_.clear();
    roomSet_ = false;
    if (!hostRoom_.empty()) {
        hostRoom_.clear();
        ++hostRoomVersion_;
    }
    newLocalSession();  // a later room starts fresh sessions
    logf("DIRECT closed %zu direct link(s): not in a room", closed);
}

std::vector<std::string> DirectNet::directMembers() {
    std::lock_guard<std::mutex> lock(mu_);
    return rosterLocked();
}

void DirectNet::setRoomMembers(std::vector<std::string> members) {
    std::sort(members.begin(), members.end());  // a set: another order is the same room
    std::lock_guard<std::mutex> lock(mu_);
    if (roomSet_ && members == roomMembers_) return;
    roomMembers_ = std::move(members);
    roomSet_ = true;
    ++roomVersion_;
    rosterChanged();  // the room list goes out with the roster
}

std::vector<std::string> DirectNet::hostRoom(uint64_t* version) {
    std::lock_guard<std::mutex> lock(mu_);
    if (version) *version = hostRoomVersion_;
    return hostRoom_;
}

std::vector<std::string> DirectNet::rosterLocked() const {
    if (opt_.mode == Mode::Join) return roster_;
    std::vector<std::string> r;
    if (!localPuid_.empty()) r.push_back(localPuid_);
    uint64_t now = nowMs();
    for (const auto& [id, link] : clients_)
        if (link.up && now - link.lastRecvMs <= opt_.rosterFreshMs) r.push_back(id);
    return r;
}

WireTraffic DirectNet::takeWireTraffic() {
    WireTraffic t{wireOut_.exchange(0), wireIn_.exchange(0), relayed_.exchange(0)};
    t.direct = directOut_.exchange(0);
    t.viaRelay = viaRelayOut_.exchange(0);
    t.stateShed = stateShed_.exchange(0);
    t.relayShed = relayShed_.exchange(0);
    return t;
}

std::string DirectNet::statusLine() {
    std::lock_guard<std::mutex> lock(mu_);
    char buf[256];
    std::string s;
    // Counts are totals since the link opened. retx: our resends; dup: the peer's resends of packets we
    // had; gaveup: unreliable game packets we stopped resending; skipped: the peer's that never came;
    // held: time our resends waited for credit (the link delivers less than it loses); credit: resends
    // the link may send now.
    auto describe = [&](const Link& l) {
        snprintf(buf, sizeof(buf),
                 " [%s %s rtt=%ums rto=%ums pending=%zu retx=%llu dup=%llu gaveup=%llu skipped=%llu held=%llums "
                 "credit=%.0f]",
                 shortId(l.puid).c_str(), addrToString(l.addr, l.addrLen).c_str(), l.rttMs, l.tx.rtoMs(),
                 l.tx.pendingCount(), static_cast<unsigned long long>(l.tx.retransmits()),
                 static_cast<unsigned long long>(l.rx.duplicates()), static_cast<unsigned long long>(l.tx.abandoned()),
                 static_cast<unsigned long long>(l.rx.skipped()), static_cast<unsigned long long>(l.tx.limitedMs()),
                 l.tx.credit());
        s += buf;
    };
    if (opt_.mode == Mode::Host) {
        s = "HOST clients=" + std::to_string(clients_.size());
        for (const auto& [id, link] : clients_) describe(link);
    } else {
        s = hostLink_ && hostLink_->up ? "JOIN connected"
            : cookie_                  ? "JOIN host answered, waiting for both identities to check out"
                                       : "JOIN waiting for host";
        if (hostLink_) describe(*hostLink_);
        s += " roster=" + std::to_string(roster_.size());
        size_t up = 0, shown = 0;
        for (const auto& [id, link] : peerLinks_) up += link.up ? 1 : 0;
        for (const auto& [id, dial] : dials_) up += dial.link && dial.link->up ? 1 : 0;
        s += " direct-to-joiners=" + std::to_string(up) + " dialling=" + std::to_string(dials_.size());
        const uint64_t now = nowMs();
        auto peer = [&](const Link& l) {
            if (!l.up || shown++ >= 8) return;
            snprintf(buf, sizeof(buf), " [joiner %s %s rtt=%ums loss=%.0f%% budget=%uKB/s queue=%ums%s]", shortId(l.puid).c_str(),
                     addrToString(l.addr, l.addrLen).c_str(), l.rttMs, l.loss * 100.0, l.cc.rate() / 1024, l.cc.queueMs(),
                     healthy(l, now) ? "" : " UNHEALTHY");
            s += buf;
        };
        for (const auto& [id, link] : peerLinks_) peer(link);
        for (const auto& [id, dial] : dials_)
            if (dial.link) peer(*dial.link);
    }
    if (uint64_t n = rejected_) s += " rejected=" + std::to_string(n);
    if (uint64_t n = sendFailures_) s += " send-refused=" + std::to_string(n);
    if (uint64_t n = stateShed_) s += " state-over-budget=" + std::to_string(n);
    if (uint64_t n = relayShed_) s += " relay-over-budget=" + std::to_string(n);
    if (uint64_t n = retransmitBudget_ ? retransmitBudget_->refusals() : 0) s += " shared-retx-cap-hit=" + std::to_string(n);
    return s;
}

// False when the socket refused the datagram. A full send buffer (WSAEWOULDBLOCK, WSAENOBUFS) is our own
// uplink congested: callers stop resending (ReliableSender::sendRefused) instead of piling more on.
bool DirectNet::sendRaw(const std::vector<uint8_t>& dg, const sockaddr_storage& to, int toLen) {
    if (testBlackhole_) return true;
    wireOut_ += dg.size();
    if (opt_.testDropRate > 0.0) {
        static thread_local std::mt19937 rng(12345);
        if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < opt_.testDropRate) return true;
    }
    if (uint64_t rate = opt_.testUplinkBytesPerSecond) {
        uint64_t now = nowMs();
        double queue = static_cast<double>(rate) / 10;  // 100 ms
        if (testUplinkMs_ == 0) testUplinkTokens_ = queue;
        testUplinkTokens_ = std::min(queue, testUplinkTokens_ + static_cast<double>((now - testUplinkMs_) * rate) / 1000);
        testUplinkMs_ = now;
        if (testUplinkTokens_ < static_cast<double>(dg.size())) return true;  // lost in the router, unseen by us
        testUplinkTokens_ -= static_cast<double>(dg.size());
    }
    if (sendto(sock_, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
               reinterpret_cast<const sockaddr*>(&to), toLen) != SOCKET_ERROR)
        return true;
    int err = WSAGetLastError();
    uint64_t n = ++sendFailures_;
    bool full = err == WSAEWOULDBLOCK || err == WSAENOBUFS;
    logRateLimited(full ? "send-full" : "send-fail", 10000, "DIRECT cannot send to %s: %s (WSA error %d, %llu so far)",
                   addrToString(to, toLen).c_str(),
                   full ? "the send buffer is full, our uplink is congested; holding back retransmissions"
                        : "the socket refused the datagram",
                   err, static_cast<unsigned long long>(n));
    return false;
}

void DirectNet::sendMsg(const Message& m, const sockaddr_storage& to, int toLen) {
    sendRaw(encode(m, opt_.key), to, toLen);
}

// Stamps a link datagram with the link's next counter and tag, and sends it. Nothing goes out when
// crypto fails: an unsealed datagram would only be dropped by the peer.
// False when the datagram did not leave (see sendRaw).
bool DirectNet::sendSealed(Link& link, std::vector<uint8_t>& dg) {
    if (link.peer && blockedPeer(link.puid)) return true;  // tests: lost on the way
    link.cc.onSent(static_cast<uint32_t>(dg.size()), nowMs());
    if (!sealLink(dg, ++link.txCounter, link.txMac)) {
        logRateLimited("seal", 10000, "DIRECT cannot authenticate a packet to %s (Windows crypto failed)",
                       shortId(link.puid).c_str());
        return false;
    }
    return sendRaw(dg, link.addr, link.addrLen);
}

void DirectNet::sendData(Link& link, DataMsg msg, uint64_t now) {
    Message m;
    m.type = MsgType::Data;
    m.epoch = link.epoch;
    // An unreliable packet keeps its reliability 0 on the wire: a host relaying it applies its own
    // upgradeUnreliable, and gives it up after the deadline like the sender does. A classified one (netclass.h)
    // is not repaired here: state is replaced by the next one, and an event or control datagram is the game's to
    // resend (and goes over two paths).
    link.lastDataMs = now;
    bool upgraded = msg.reliability == 0 && opt_.upgradeUnreliable && msg.cls == 0;
    bool tracked = msg.reliability != 0 || upgraded;
    msg.seq = tracked ? link.tx.nextSeq() : 0;
    uint32_t seq = msg.seq;
    m.data = std::move(msg);
    std::vector<uint8_t> dg = encode(m, opt_.key);
    if (!sendSealed(link, dg)) link.tx.sendRefused();  // still tracked: resent once the link has credit again
    if (tracked) link.tx.track(seq, std::move(dg), now, upgraded);
}

void DirectNet::deliverLocal(DataMsg msg) {
    lastDataMs_[msg.src] = nowMs();
    if (inbox_.size() >= kMaxInbox) {
        logRateLimited("inbox-full", 5000, "DIRECT inbox full (%zu packets): game is not reading, dropping oldest",
                       inbox_.size());
        inbox_.pop_front();
    }
    Delivered d;
    d.src = std::move(msg.src);
    d.socketName = std::move(msg.socketName);
    d.channel = msg.channel;
    d.data = std::move(msg.payload);
    inbox_.push_back(std::move(d));
}

void DirectNet::routeData(DataMsg msg) {
    if (msg.dst == localPuid_) {
        deliverLocal(std::move(msg));
        return;
    }
    if (opt_.mode != Mode::Host) return;
    if (msg.dst == msg.src) {
        // Bounced back over the sender's own link, every packet would sit in our retransmit queue for
        // as long as the sender cares not to acknowledge it.
        logRateLimited("forward-self", 5000, "DIRECT dropped a packet %s addressed to itself",
                       shortId(msg.src).c_str());
        return;
    }
    auto it = clients_.find(msg.dst);
    if (it == clients_.end() || !usable(it->second)) {
        logRateLimited("forward-miss", 5000, "DIRECT cannot forward %s -> %s: destination not connected directly",
                       shortId(msg.src).c_str(), shortId(msg.dst).c_str());
        return;
    }
    const uint64_t now = nowMs();
    if (msg.cls == kState && msg.reliability == 0) {
        // The host's uplink pays for what it relays: state beyond the relay budget is dropped (the next replaces it).
        const double rate = static_cast<double>(opt_.relayBytesPerSecond);
        if (relayTokensMs_ == 0) relayTokens_ = rate / 4;
        relayTokens_ = std::min(rate / 4, relayTokens_ + static_cast<double>(now - relayTokensMs_) * rate / 1000.0);
        relayTokensMs_ = now;
        if (relayTokens_ < static_cast<double>(msg.payload.size())) {
            ++relayShed_;
            return;
        }
        relayTokens_ -= static_cast<double>(msg.payload.size());
    }
    relayed_ += msg.payload.size();
    sendData(it->second, std::move(msg), now);
}

void DirectNet::sendLink(Link& link, Message m) {
    m.epoch = link.epoch;
    std::vector<uint8_t> dg = encode(m, opt_.key);
    sendSealed(link, dg);
}

// Why a datagram for `link` is not authentic, or nullptr when it is: it must carry the link's epoch,
// a tag made with the peer's link key, and a counter not seen before. Only a peer holding the key made
// in this session's handshake can send one, and each datagram counts once.
const char* DirectNet::linkRefusal(Link& link, const Received& r) {
    if (r.msg.epoch != link.epoch) return "it belongs to an earlier session of the link";
    if (!linkTagValid(r.data, r.size, link.rxMac)) return "its authentication tag does not match (forged or altered)";
    if (!link.replay.fresh(r.msg.counter)) return "it was received before (replayed)";
    return nullptr;
}

// Checks a link datagram (see linkRefusal) and takes its counter. Refused ones are dropped, counted and
// logged at a limited rate.
bool DirectNet::authentic(Link& link, const Received& r, const sockaddr_storage& from, int fromLen) {
    if (const char* why = linkRefusal(link, r)) {
        uint64_t n = ++rejected_;
        logRateLimited("link-auth", 10000, "DIRECT dropped a packet on the link of %s from %s: %s (%llu dropped so far)",
                       shortId(link.puid).c_str(), addrToString(from, fromLen).c_str(), why,
                       static_cast<unsigned long long>(n));
        return false;
    }
    link.replay.mark(r.msg.counter);
    return true;
}

void DirectNet::onLinkCommon(Link& link, const Message& m, uint64_t now) {
    link.lastRecvMs = now;
    switch (m.type) {
        case MsgType::Ping: {
            Message pong;
            pong.type = MsgType::Pong;
            pong.ping = m.ping;
            sendLink(link, pong);
            break;
        }
        case MsgType::Pong:
            link.rttMs = static_cast<uint32_t>(now - m.ping.timeMs);
            link.cc.onRtt(link.rttMs, now);
            link.loss *= 0.9;
            if (link.pingOutMs && m.ping.timeMs >= link.pingOutMs) link.pingOutMs = 0;
            break;
        case MsgType::Ack:
            link.tx.onAck(m.ack, now);
            break;
        case MsgType::Data:
        case MsgType::Forward: {
            if (m.type == MsgType::Data) link.lastDataMs = now;
            if (m.type == MsgType::Data && m.data.seq == 0) {
                routeData(m.data);
                break;
            }
            std::vector<DataMsg> ready;
            Message ack;
            ack.type = MsgType::Ack;
            ack.ack = m.type == MsgType::Data ? link.rx.onData(m.data, ready) : link.rx.onForward(m.forward.floor, ready);
            sendLink(link, ack);
            for (auto& d : ready) routeData(std::move(d));
            break;
        }
        default:
            break;
    }
}

DirectNet::Link* DirectNet::hostClientByAddr(const sockaddr_storage& addr, int len) {
    for (auto& [id, link] : clients_)
        if (sameAddr(link.addr, link.addrLen, addr, len)) return &link;
    return nullptr;
}

std::optional<Cookie> DirectNet::cookieFor(const HelloMsg& h, const sockaddr_storage& from, int fromLen,
                                           uint64_t bucket) {
    const uint8_t* addr = reinterpret_cast<const uint8_t*>(&from);
    std::vector<uint8_t> in(addr, addr + fromLen);
    in.insert(in.end(), h.puid.begin(), h.puid.end());
    in.insert(in.end(), reinterpret_cast<const uint8_t*>(&h.nonce), reinterpret_cast<const uint8_t*>(&h.nonce) + 4);
    in.insert(in.end(), reinterpret_cast<const uint8_t*>(&bucket), reinterpret_cast<const uint8_t*>(&bucket) + 8);
    return hmacTag(cookieSecret_, in.data(), in.size());
}

// Why a hello with a valid cookie does not prove the EOS id it claims, or nullptr when it does.
const char* DirectNet::identityRefusal(const HelloMsg& h) {
    auto member = memberIds_.find(h.puid);
    if (member == memberIds_.end())
        return "that player published no direct-link identity in this room (a game without the plugin, "
               "EDF6DirectNet 0.3.6 or older, or its room info has not reached us yet); it stays on EOS";
    if (identityCommitment(h.publicKey) != member->second)
        return "it is not signed by the identity that player published in the room (someone else claiming "
               "to be that player?)";
    auto digest = helloDigest(h);
    if (!digest || !verifySignature(h.publicKey, *digest, h.signature))
        return "its signature does not verify (someone else claiming to be that player?)";
    return nullptr;
}

void DirectNet::onHostHello(const HelloMsg& h, const sockaddr_storage& from, int fromLen, uint64_t now) {
    if (localPuid_.empty() || !active_) return;  // not signed in yet / not in a room; the client keeps retrying
    const std::string& id = h.puid;
    if (id.empty() || id == localPuid_) {
        logRateLimited("hello-self", 10000, "DIRECT rejected hello with own/empty id from %s",
                       addrToString(from, fromLen).c_str());
        return;
    }
    // Return routability first: until a sender echoes the cookie sent to its address it gets nothing
    // else and nothing is kept for it, so hellos from forged source addresses change nothing and a
    // flood of them has nothing to fill. The reply is smaller than the hello: no amplification.
    uint64_t bucket = now / kCookieBucketMs;
    auto current = cookieFor(h, from, fromLen, bucket);
    auto previous = cookieFor(h, from, fromLen, bucket - 1);
    if (!current) return;  // no crypto: nobody can be let in
    if (h.cookie != *current && (!previous || h.cookie != *previous)) {
        Message c;
        c.type = MsgType::Challenge;
        c.challenge.clientNonce = h.nonce;
        c.challenge.cookie = *current;
        sendMsg(c, from, fromLen);
        return;
    }
    // Then identity: the hello must be signed by the key whose commitment this EOS id published in
    // the room, which nobody but that EOS user can do. A shared Key does not stand in for this.
    if (const char* why = identityRefusal(h)) {
        logRateLimited(why, 10000, "DIRECT refused hello for %s from %s: %s", shortId(id).c_str(),
                       addrToString(from, fromLen).c_str(), why);
        return;
    }
    const std::string commitment = memberIds_[id];
    auto it = clients_.find(id);
    bool sameSession = it != clients_.end() && it->second.session == h.session && it->second.peerNonce == h.nonce &&
                       it->second.peerEcdh == h.ecdh;
    auto seen = seen_.find(id);
    if (!sameSession && seen != seen_.end() && seen->second.commitment == commitment &&
        h.session <= seen->second.session) {
        // Signed, but for a session this member has already replaced: a captured hello played back.
        logRateLimited("hello-replay", 10000, "DIRECT ignored a replayed hello of an earlier session of %s from %s",
                       shortId(id).c_str(), addrToString(from, fromLen).c_str());
        return;
    }
    if (sameSession && !sameAddr(it->second.addr, it->second.addrLen, from, fromLen)) {
        // Proven from the new address (the cookie is bound to it): the client itself moved.
        logf("DIRECT client %s moved %s -> %s", shortId(id).c_str(),
             addrToString(it->second.addr, it->second.addrLen).c_str(), addrToString(from, fromLen).c_str());
        it->second.addr = from;
        it->second.addrLen = fromLen;
    }
    if (!sameSession) {
        Link* other = hostClientByAddr(from, fromLen);
        if (it == clients_.end() && clients_.size() >= kMaxClients && !other) {
            logRateLimited("clients-full", 10000, "DIRECT ignored hello from %s: already %zu direct clients",
                           addrToString(from, fromLen).c_str(), clients_.size());
            return;
        }
        // New client or a new session of a known one: new link keys, reliable streams from scratch.
        Link link;
        link.addr = from;
        link.addrLen = fromLen;
        link.puid = id;
        link.peerNonce = h.nonce;
        link.session = h.session;
        link.epoch = linkEpoch(h.nonce, localNonce_);
        link.lastRecvMs = now;
        if (!openHostLink(link, h)) return;
        bool wasUp = it != clients_.end() && it->second.up;
        if (other && other->puid != id) {
            std::string stale = other->puid;  // copy: erase must not take a key owned by the node
            wasUp |= other->up;
            clients_.erase(stale);
        }
        clients_[id] = std::move(link);
        seen_[id] = Seen{commitment, h.session};
        if (wasUp) rosterChanged();  // the old session's link is gone
    }
    Link& link = clients_[id];
    link.lastRecvMs = now;
    sendWelcome(link, from, fromLen);
}

// Makes the keys of a new link for hello `h`: our ECDH key for it (kept only for this), the welcome
// that carries it, and the key of each direction. False (logged) when crypto fails or the client's
// ECDH key is not a valid point.
bool DirectNet::openHostLink(Link& link, const HelloMsg& h) {
    link.welcome.hostNonce = localNonce_;
    link.welcome.clientNonce = h.nonce;
    link.welcome.hostPuid = localPuid_;
    link.peerEcdh = h.ecdh;
    auto ecdh = EcdhKey::generate();
    auto shared = ecdh ? ecdh->agree(h.ecdh) : std::nullopt;
    if (ecdh) link.welcome.ecdh = ecdh->publicKey();
    auto keys = shared && identity_ ? deriveLinkKeys(*shared, opt_.key, h, link.welcome) : std::nullopt;
    if (keys) {
        link.txMac = LinkMac(keys->hostToClient);
        link.rxMac = LinkMac(keys->clientToHost);
        link.welcome.publicKey = identity_->publicKey();
    }
    if (keys && link.txMac.valid() && link.rxMac.valid()) return true;
    logRateLimited("link-keys", 10000, "DIRECT cannot make link keys for %s: %s", shortId(h.puid).c_str(),
                   ecdh && !shared ? "its key exchange value is invalid" : "Windows crypto failed");
    return false;
}

// Welcomes the client of `link`, signed with our identity. The signature covers the member list, so
// it is made again only when that changed since the last welcome of this link.
void DirectNet::sendWelcome(Link& link, const sockaddr_storage& to, int toLen) {
    std::vector<std::string> roster = link.peer ? std::vector<std::string>() : rosterLocked();
    if (link.welcomeDatagram.empty() || roster != link.welcome.roster) {
        link.welcome.roster = std::move(roster);
        auto digest = welcomeDigest(link.welcome, link.peerEcdh);
        auto signature = digest && identity_ ? identity_->sign(*digest) : std::nullopt;
        if (!signature) {
            logRateLimited("welcome-sign", 10000, "DIRECT cannot sign a welcome (Windows crypto failed)");
            return;
        }
        link.welcome.signature = *signature;
        Message w;
        w.type = MsgType::Welcome;
        w.welcome = link.welcome;
        link.welcomeDatagram = encode(w, opt_.key);
    }
    sendRaw(link.welcomeDatagram, to, toLen);
}

void DirectNet::onHostDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now) {
    const Message& m = r.msg;
    if (m.type == MsgType::Hello) {
        onHostHello(m.hello, from, fromLen, now);
        return;
    }
    if (!isLinkScoped(m.type)) return;  // handshake answers are for joiners
    Link* link = hostClientByAddr(from, fromLen);
    if (!link) {
        // A live session showing up from a new address (NAT rebinding): the link moves there on the
        // first authentic datagram from it. Only the client holds the link key, and a datagram seen
        // before (captured and replayed from elsewhere) is not fresh, so nobody else can move it.
        for (auto& [id, candidate] : clients_) {
            if (candidate.epoch != m.epoch) continue;
            if (!authentic(candidate, r, from, fromLen)) return;
            logf("DIRECT client %s moved %s -> %s", shortId(id).c_str(),
                 addrToString(candidate.addr, candidate.addrLen).c_str(), addrToString(from, fromLen).c_str());
            candidate.addr = from;
            candidate.addrLen = fromLen;
            link = &candidate;
            break;
        }
        if (!link) {
            // Traffic from a client this host has no link with (e.g. the host restarted): a hint to hello again.
            if (m.type != MsgType::Bye) {
                Message reset;
                reset.type = MsgType::Reset;
                sendMsg(reset, from, fromLen);
            }
            return;
        }
    } else if (!authentic(*link, r, from, fromLen)) {
        return;
    }
    if (!link->up && m.type != MsgType::Bye) {
        // The client holds the link keys: it verified our welcome. Game data may flow now.
        link->up = true;
        link->id = ++linkIds_;
        logf("DIRECT client %s connected from %s (%zu direct clients)", shortId(link->puid).c_str(),
             addrToString(from, fromLen).c_str(), clients_.size());
        rosterChanged();
    }
    if (m.type == MsgType::Bye) {
        std::string gone = link->puid;  // copy: erase must not take a key owned by the node
        logf("DIRECT client %s said goodbye", shortId(gone).c_str());
        clients_.erase(gone);
        rosterChanged();
        return;
    }
    if (m.type == MsgType::Data && m.data.src != link->puid) {
        logRateLimited("spoof", 5000, "DIRECT dropped packet claiming src %s from client %s",
                       shortId(m.data.src).c_str(), shortId(link->puid).c_str());
        return;
    }
    if (m.type == MsgType::PeerQuery) {
        link->lastRecvMs = now;
        onPeerQuery(*link, m.peer.puid, now);
        return;
    }
    onLinkCommon(*link, m, now);
}

// A joiner wants a direct link with another one: both learn where the other is, as this host sees them.
void DirectNet::onPeerQuery(Link& from, const std::string& wanted, uint64_t now) {
    auto target = clients_.find(wanted);
    if (wanted == from.puid || target == clients_.end() || !usable(target->second)) return;
    const auto key = from.puid < wanted ? std::make_pair(from.puid, wanted) : std::make_pair(wanted, from.puid);
    uint64_t& last = introduced_[key];
    if (last && now - last < kIntroducedMs) return;
    last = now;
    Message info;
    info.type = MsgType::PeerInfo;
    info.peer = {wanted, addrToString(target->second.addr, target->second.addrLen)};
    sendLink(from, info);
    info.peer = {from.puid, addrToString(from.addr, from.addrLen)};
    sendLink(target->second, info);
}

void DirectNet::onClientDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now) {
    // A host on a wildcard socket answers from the source address its OS picks, which need not be the
    // one we dialled (IPv6 hosts prefer a temporary address). So the host's packets are recognised by
    // their session and link keys, not by their source address, and we keep sending to the address we
    // dialled.
    const Message& m = r.msg;
    switch (m.type) {
        case MsgType::Challenge: {
            if (m.challenge.clientNonce != localNonce_) {
                // Another joiner we dial (mesh) asks us to prove our address first, as a host does.
                for (auto& [puid, dial] : dials_) {
                    if (dial.link || dial.nonce != m.challenge.clientNonce) continue;
                    const bool fresh = !dial.cookie || *dial.cookie != m.challenge.cookie;
                    dial.cookie = m.challenge.cookie;
                    if (fresh) sendPeerHello(puid, dial, now);
                    return;
                }
                return;
            }
            // The host keeps nothing for us until we echo this cookie in a hello signed with our identity.
            if (hostLink_ || hostAddrLen_ == 0) return;
            bool fresh = !cookie_ || *cookie_ != m.challenge.cookie;
            cookie_ = m.challenge.cookie;
            if (fresh && active_ && !localPuid_.empty()) sendHello(now);  // a repeat waits for the next retry
            return;
        }
        case MsgType::Welcome:
            if (m.welcome.clientNonce != localNonce_) {
                for (auto& [puid, dial] : dials_)
                    if (!dial.link && dial.nonce == m.welcome.clientNonce) {
                        const std::string who = puid;  // copy: onPeerWelcome may log it after changes
                        onPeerWelcome(who, dial, m.welcome, now);
                        return;
                    }
                return;
            }
            onClientWelcome(m.welcome, from, fromLen, now);
            return;
        case MsgType::Reset: {
            // Anyone can send this; it only speeds up what the link timeout would do anyway.
            bool fromHost = sameAddr(from, fromLen, hostAddr_, hostAddrLen_) ||
                            (hostReplyAddrLen_ > 0 && sameAddr(from, fromLen, hostReplyAddr_, hostReplyAddrLen_));
            if (hostLink_ && fromHost && now - hostLink_->lastRecvMs >= kResetQuietMs)
                dropHostLink("the host no longer knows our link (restarted?), reconnecting");
            return;
        }
        case MsgType::Hello:
            onPeerHello(m.hello, from, fromLen, now);
            return;
        case MsgType::Punch:
            return;  // it only opened the sender's NAT towards us
        default:
            break;
    }
    // A link to another joiner (by its epoch), or else the host's: proven by the link keys. Stray ones are not
    // even logged.
    if (onPeerDatagram(r, from, fromLen, now)) return;
    if (!hostLink_ || !authentic(*hostLink_, r, from, fromLen)) return;
    if (m.type == MsgType::Bye) {
        dropHostLink("the host closed the direct link, reconnecting");
        return;
    }
    if (m.type == MsgType::Roster) {
        // A member list older than the one applied (reordered in flight) would bring back a member
        // the host already dropped. A list of several pages applies once all of them are in.
        if (m.roster.hostNonce != hostLink_->peerNonce) return;
        std::vector<std::string> list;
        const bool single = m.roster.offset == 0 && m.roster.total == m.roster.roster.size();
        if (single ? m.counter >= hostLink_->rosterCounter && m.roster.version >= rosterPages_.applied
                   : applyPage(rosterPages_, m.roster.version, m.roster.total, m.roster.offset, m.roster.roster, list)) {
            if (single) {
                hostLink_->rosterCounter = m.counter;
                rosterPages_.applied = m.roster.version;
                list = m.roster.roster;
            }
            if (roster_ != list) logf("DIRECT roster now has %zu direct members", list.size());
            roster_ = std::move(list);
        }
    }
    if (m.type == MsgType::Room) {
        if (m.room.hostNonce != hostLink_->peerNonce) return;
        std::vector<std::string> list;
        const bool single = m.room.offset == 0 && m.room.total == m.room.members.size();
        if (single ? m.counter >= hostLink_->roomCounter && m.room.version >= roomPages_.applied
                   : applyPage(roomPages_, m.room.version, m.room.total, m.room.offset, m.room.members, list)) {
            if (single) {
                hostLink_->roomCounter = m.counter;
                roomPages_.applied = m.room.version;
                list = m.room.members;
            }
            if (hostRoom_ != list) {
                hostRoom_ = std::move(list);
                ++hostRoomVersion_;
            }
        }
    }
    if (m.type == MsgType::PeerInfo) {
        hostLink_->lastRecvMs = now;
        onPeerInfo(m.peer, now);
        return;
    }
    onLinkCommon(*hostLink_, m, now);
}

// Why the welcome `w` (an answer to our current hello) is not from the host we may accept, or nullptr.
const char* DirectNet::welcomeRefusal(const WelcomeMsg& w) {
    if (roomOwner_.empty() || roomOwnerId_.empty())
        return "the room owner's direct-link identity has not reached us yet (or it runs no plugin, or "
               "EDF6DirectNet 0.4.0 or older)";
    if (w.hostPuid != roomOwner_) return "it is not the room owner";
    if (identityCommitment(w.publicKey) != roomOwnerId_)
        return "it is not signed by the identity the room owner published (someone else answering on the "
               "host's address?)";
    auto digest = localEcdh_ ? welcomeDigest(w, localEcdh_->publicKey()) : std::nullopt;
    if (!digest || !verifySignature(w.publicKey, *digest, w.signature))
        return "its signature does not verify (someone else answering on the host's address?)";
    return nullptr;
}

void DirectNet::onClientWelcome(const WelcomeMsg& w, const sockaddr_storage& from, int fromLen, uint64_t now) {
    if (hostAddrLen_ == 0 || w.clientNonce != localNonce_) return;  // not an answer to our hello
    if (hostLink_ && hostLink_->peerNonce == w.hostNonce) return;  // a repeat: the link is up already
    if (const char* why = welcomeRefusal(w)) {
        logRateLimited(why, 10000, "DIRECT refused the welcome of %s from %s: %s; the game stays on EOS",
                       shortId(w.hostPuid).c_str(), addrToString(from, fromLen).c_str(), why);
        return;
    }
    HelloMsg hello;  // what the host derived the keys from (see sendHello)
    hello.nonce = localNonce_;
    hello.session = localSession_;
    hello.puid = localPuid_;
    hello.ecdh = localEcdh_->publicKey();
    auto shared = localEcdh_->agree(w.ecdh);
    auto keys = shared ? deriveLinkKeys(*shared, opt_.key, hello, w) : std::nullopt;
    Link link;
    if (keys) {
        link.txMac = LinkMac(keys->clientToHost);
        link.rxMac = LinkMac(keys->hostToClient);
    }
    if (!link.txMac.valid() || !link.rxMac.valid()) {
        logRateLimited("link-keys", 10000, "DIRECT cannot make link keys with host %s: %s", shortId(w.hostPuid).c_str(),
                       shared ? "Windows crypto failed" : "its key exchange value is invalid");
        return;
    }
    link.addr = hostAddr_;
    link.addrLen = hostAddrLen_;
    link.puid = w.hostPuid;
    link.peerNonce = w.hostNonce;
    link.epoch = linkEpoch(localNonce_, w.hostNonce);
    link.up = true;
    link.id = ++linkIds_;
    link.lastRecvMs = now;
    hostLink_ = std::move(link);
    hostReplyAddr_ = from;
    hostReplyAddrLen_ = fromLen;
    roster_ = w.roster;
    bool other = !sameAddr(from, fromLen, hostAddr_, hostAddrLen_);
    logf("DIRECT connected to host %s at %s%s%s", shortId(w.hostPuid).c_str(), addrToString(hostAddr_, hostAddrLen_).c_str(),
         other ? ", it answers from " : "", other ? addrToString(from, fromLen).c_str() : "");
}

void DirectNet::dropHostLink(const char* why) {
    logf("DIRECT %s", why);
    hostLink_.reset();
    roster_.clear();
    rosterPages_ = {};
    roomPages_ = {};
    newLocalSession();
    lastHelloMs_ = 0;
}

void DirectNet::processDatagram(const uint8_t* data, size_t size, const sockaddr_storage& from, int fromLen,
                                uint64_t now) {
    DecodeError err;
    auto msg = decode(data, size, opt_.key, &err);
    if (!msg && err == DecodeError::BadProtocol) {
        uint16_t theirs = 0;
        memcpy(&theirs, data + 6, 2);
        logRateLimited("protocol", 30000,
                       "DIRECT %s speaks direct-link protocol %u, we speak %u: it runs another EDF6DirectNet version "
                       "(0.3.6 and older speak 2, 0.4.0 speaks 3, 0.4.1 speaks 4). No direct link with it; the game talks to it over "
                       "EOS as usual",
                       addrToString(from, fromLen).c_str(), theirs, kProtocol);
        return;
    }
    if (!msg) {
        if (err != DecodeError::BadMagic)
            logRateLimited("decode", 10000, "DIRECT rejected datagram from %s: %s",
                           addrToString(from, fromLen).c_str(), decodeErrorName(err));
        return;
    }
    Received r{data, size, *msg};
    std::lock_guard<std::mutex> lock(mu_);
    if (opt_.mode == Mode::Host)
        onHostDatagram(r, from, fromLen, now);
    else
        onClientDatagram(r, from, fromLen, now);
}

void DirectNet::rosterChanged() {
    rosterBurstUntilMs_ = nowMs() + kRosterBurstMs;
    broadcastRoster();
}

void DirectNet::broadcastRoster() {
    std::vector<std::string> roster = rosterLocked();
    if (roster != lastRoster_ || rosterVersion_ == 0) ++rosterVersion_;
    lastRoster_ = roster;
    for (auto& [id, link] : clients_) {
        sendListPages(link, MsgType::Roster, roster, rosterVersion_);
        sendRoom(link);
    }
    lastRosterMs_ = nowMs();
}

void DirectNet::sendRoom(Link& link) {
    if (!roomSet_) return;
    sendListPages(link, MsgType::Room, roomMembers_, roomVersion_);
}

// A member list in pages of kRosterPage (one page, total = its size, for a list that fits).
void DirectNet::sendListPages(Link& link, MsgType type, const std::vector<std::string>& list, uint32_t version) {
    size_t offset = 0;
    do {
        const size_t n = std::min(kRosterPage, list.size() - offset);
        Message m;
        m.type = type;
        RosterMsg page;
        page.hostNonce = localNonce_;
        page.roster.assign(list.begin() + static_cast<std::ptrdiff_t>(offset),
                           list.begin() + static_cast<std::ptrdiff_t>(offset + n));
        page.version = version;
        page.total = static_cast<uint16_t>(list.size());
        page.offset = static_cast<uint16_t>(offset);
        if (type == MsgType::Roster) {
            m.roster = std::move(page);
        } else {
            m.room.hostNonce = page.hostNonce;
            m.room.members = std::move(page.roster);
            m.room.version = page.version;
            m.room.total = page.total;
            m.room.offset = page.offset;
        }
        sendLink(link, m);
        offset += n;
    } while (offset < list.size());
}

bool DirectNet::applyPage(Pages& pages, uint32_t version, uint16_t total, uint16_t offset,
                          const std::vector<std::string>& entries, std::vector<std::string>& out) {
    if (version <= pages.applied) return false;  // an older list than the one we have
    if (version != pages.version) {
        if (version < pages.version) return false;
        pages.version = version;
        pages.total = total;
        pages.pages.clear();
    }
    if (total != pages.total) return false;
    pages.pages[offset] = entries;
    size_t have = 0;
    for (const auto& [at, page] : pages.pages) have += page.size();
    if (have < total) return false;
    out.clear();
    for (const auto& [at, page] : pages.pages) out.insert(out.end(), page.begin(), page.end());
    out.resize(total);
    pages.applied = version;
    pages.pages.clear();
    return true;
}

bool DirectNet::resolveHost() {
    std::string host;
    uint16_t port = 0;
    if (!splitHostPort(opt_.hostAddress, host, port)) {
        logRateLimited("resolve-parse", 60000, "DIRECT HostAddress '%s' is not a valid address",
                       opt_.hostAddress.c_str());
        return false;
    }
    addrinfo hints{};
    hints.ai_family = family_ == AF_INET6 ? AF_UNSPEC : AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
        logRateLimited("resolve-fail", 60000, "DIRECT cannot resolve host '%s'", host.c_str());
        return false;
    }
    sockaddr_storage addr{};
    int len = 0;
    for (const addrinfo* r = res; r && len == 0; r = r->ai_next) {
        if (r->ai_addrlen > sizeof(addr)) continue;
        memcpy(&addr, r->ai_addr, r->ai_addrlen);
        len = static_cast<int>(r->ai_addrlen);
        if (opt_.advertisedHost && !dialable(addr)) len = 0;
    }
    freeaddrinfo(res);
    if (len == 0) {
        logRateLimited("resolve-refused", 60000,
                       "DIRECT refused host '%s': the room host advertises an address that is not a remote "
                       "machine (loopback, multicast, broadcast or link-local)",
                       host.c_str());
        return false;
    }
    if (family_ == AF_INET6) toDualStack(addr, len);

    std::lock_guard<std::mutex> lock(mu_);
    if (!sameAddr(addr, len, hostAddr_, hostAddrLen_)) {
        logf("DIRECT host address %s -> %s", opt_.hostAddress.c_str(), addrToString(addr, len).c_str());
        hostAddr_ = addr;
        hostAddrLen_ = len;
        hostReplyAddrLen_ = 0;
        hostLink_.reset();
        roster_.clear();
    }
    return true;
}

void DirectNet::tick(uint64_t now) {
    auto pollLink = [&](Link& link) {
        // A resent packet goes out with a new counter: the peer takes every counter once, and must be
        // able to acknowledge again a packet whose acknowledgement was lost.
        link.tx.poll(now, [&](std::vector<uint8_t>& dg) { return sendSealed(link, dg); }, retransmitBudget_.get());
        if (auto floor = link.tx.forwardDue(now)) {
            Message f;
            f.type = MsgType::Forward;
            f.forward.floor = *floor;
            sendLink(link, f);
        }
        measure(link, now);
        if (now - link.lastPingMs >= pingIntervalFor(link, now)) {
            Message p;
            p.type = MsgType::Ping;
            p.ping.timeMs = now;
            sendLink(link, p);
            link.lastPingMs = now;
            if (!link.pingOutMs) link.pingOutMs = now;
        }
    };
    // True (and logged) when `link` has to close: silent too long, stuck on a packet the game sent
    // reliably, or holding more unacknowledged data than a working peer ever lets pile up.
    auto closing = [&](const Link& link, const char* who) {
        if (link.tx.overloaded()) {
            logf("DIRECT %s dropped: %zu packets (%zu KB) sent but never acknowledged", who, link.tx.pendingCount(),
                 link.tx.pendingBytes() / 1024);
            return true;
        }
        if (now - link.lastRecvMs > opt_.linkTimeoutMs) {
            logf("DIRECT %s timed out: nothing received for %llu ms (limit %u ms), %zu packets unacknowledged", who,
                 static_cast<unsigned long long>(now - link.lastRecvMs), opt_.linkTimeoutMs, link.tx.pendingCount());
            return true;
        }
        if (uint64_t age = link.tx.oldestReliableAgeMs(now); age > opt_.stallTimeoutMs) {
            logf("DIRECT %s stalled: a packet the game sent reliably is unacknowledged after %llu ms (limit %u ms) "
                 "although the peer answers (last heard %llu ms ago); %zu packets unacknowledged, %llu resends",
                 who, static_cast<unsigned long long>(age), opt_.stallTimeoutMs,
                 static_cast<unsigned long long>(now - link.lastRecvMs), link.tx.pendingCount(),
                 static_cast<unsigned long long>(link.tx.retransmits()));
            return true;
        }
        return false;
    };

    if (opt_.mode == Mode::Host) {
        bool changed = false;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (closing(it->second, ("client " + shortId(it->first)).c_str())) {
                it = clients_.erase(it);
                changed = true;
                continue;
            }
            pollLink(it->second);
            ++it;
        }
        if (!changed && rosterLocked() != lastRoster_) changed = true;  // a client went quiet or came back
        for (auto it = introduced_.begin(); it != introduced_.end();)
            it = now - it->second > 60000 ? introduced_.erase(it) : std::next(it);
        bool burst = now < rosterBurstUntilMs_ && now - lastRosterMs_ >= kRosterBurstIntervalMs;
        if (changed)
            rosterChanged();
        else if (burst || (!clients_.empty() && now - lastRosterMs_ >= kRosterIntervalMs))
            broadcastRoster();
        return;
    }

    if (hostLink_ && closing(*hostLink_, "host link")) dropHostLink("reconnecting to the host");
    // Links to other joiners (mesh): dials retried and given up, links measured, idle or dead ones closed.
    for (auto it = dials_.begin(); it != dials_.end();) {
        Dial& d = it->second;
        if (d.link) {
            const char* why = closing(*d.link, ("direct link to joiner " + shortId(it->first)).c_str()) ? "it failed"
                              : now - std::max(d.link->lastDataMs, d.startMs) > kPeerIdleMs ? "it carried nothing for a minute"
                                                                                               : nullptr;
            if (why) {
                logf("DIRECT closing the direct link to joiner %s (%s); the host relays", shortId(it->first).c_str(), why);
                Message bye;
                bye.type = MsgType::Bye;
                sendLink(*d.link, bye);
                it = dials_.erase(it);
                continue;
            }
            pollLink(*d.link);
        } else if (now - d.startMs > kDialTimeoutMs) {
            logRateLimited(("dial-" + it->first).c_str(), 30000,
                           "DIRECT joiner %s did not answer at its address within %llu s; the host relays",
                           shortId(it->first).c_str(), static_cast<unsigned long long>(kDialTimeoutMs / 1000));
            it = dials_.erase(it);
            continue;
        } else if (now - d.lastHelloMs >= kHelloIntervalMs) {
            sendPeerHello(it->first, d, now);
        }
        ++it;
    }
    for (auto it = peerLinks_.begin(); it != peerLinks_.end();) {
        Link& l = it->second;
        const bool dead = l.up ? closing(l, ("direct link from joiner " + shortId(it->first)).c_str())
                               : now - l.lastRecvMs > kDialTimeoutMs;
        if (dead || (l.up && l.lastDataMs && now - l.lastDataMs > kPeerIdleMs)) {
            it = peerLinks_.erase(it);
            continue;
        }
        if (l.up) pollLink(l);
        ++it;
    }
    if (hostLink_) {
        pollLink(*hostLink_);
        return;
    }
    if (active_ && !localPuid_.empty() && hostAddrLen_ > 0 && now - lastHelloMs_ >= kHelloIntervalMs) sendHello(now);
}

// --- Paths and the mesh ---

void DirectNet::measure(Link& link, uint64_t now) {
    // A ping unanswered for three round trips (a second at least) is lost: the path loses packets, or queues them
    // longer than anything the game can use.
    if (link.pingOutMs && now - link.pingOutMs > std::max<uint64_t>(1000, 3ull * link.rttMs)) {
        link.loss = 0.9 * link.loss + 0.1;
        link.cc.onLoss(now);
        link.pingOutMs = 0;
    }
    const bool h = healthy(link, now);
    if (h && !link.healthySinceMs) link.healthySinceMs = now;
    if (!h) link.healthySinceMs = 0;
}

void DirectNet::wantPeer(const std::string& remote, uint64_t now) {
    if (dials_.count(remote) || peerLinks_.count(remote) || !hostLink_) return;
    Intro& in = intros_[remote];
    if (now < in.retryAfterMs || (in.lastQueryMs && now - in.lastQueryMs < kIntroQueryMs)) return;
    if (in.lastQueryMs && ++in.failures >= kIntroFailures) {
        logf("DIRECT no direct link to joiner %s after %u tries; the host relays its packets, trying again in %llu s",
             shortId(remote).c_str(), kIntroFailures, static_cast<unsigned long long>(kIntroBackoffMs / 1000));
        in = {};
        in.retryAfterMs = now + kIntroBackoffMs;
        return;
    }
    in.lastQueryMs = now;
    Message q;
    q.type = MsgType::PeerQuery;
    q.peer.puid = remote;
    sendLink(*hostLink_, q);
}

// The host told us where another joiner is. The lower EOS id dials, the other opens its NAT towards it.
void DirectNet::onPeerInfo(const PeerMsg& info, uint64_t now) {
    if (!opt_.mesh || !active_ || info.puid.empty() || info.puid == localPuid_ || !contains(roster_, info.puid)) return;
    sockaddr_storage addr{};
    int len = 0;
    if (!parseAddress(info.address, addr, len)) return;
    if (family_ == AF_INET6) toDualStack(addr, len);
    if (localPuid_ < info.puid) {
        if (peerLinks_.count(info.puid)) return;
        Dial& d = dials_[info.puid];
        if (d.link) return;
        if (d.nonce == 0 || !sameAddr(d.addr, d.addrLen, addr, len)) {
            d.addr = addr;
            d.addrLen = len;
            d.nonce = randomNonce();
            d.session = identity_ ? identity_->nextSession() : 0;
            d.ecdh = EcdhKey::generate();
            d.cookie.reset();
            d.startMs = now;
        }
        sendPeerHello(info.puid, d, now);
        return;
    }
    if (blockedPeer(info.puid)) return;
    Message punch;
    punch.type = MsgType::Punch;
    sendMsg(punch, addr, len);
}

void DirectNet::sendPeerHello(const std::string& puid, Dial& d, uint64_t now) {
    d.lastHelloMs = now;
    if (blockedPeer(puid)) return;  // tests: lost on the way
    Message h;
    h.type = MsgType::Hello;
    h.hello.nonce = d.nonce;
    h.hello.session = d.session;
    h.hello.puid = localPuid_;
    if (d.cookie && identity_ && d.ecdh) {
        h.hello.cookie = *d.cookie;
        h.hello.publicKey = identity_->publicKey();
        h.hello.ecdh = d.ecdh->publicKey();
        auto digest = helloDigest(h.hello);
        auto signature = digest ? identity_->sign(*digest) : std::nullopt;
        if (signature) h.hello.signature = *signature;
    }
    sendMsg(h, d.addr, d.addrLen);
}

// Another joiner dials us (it has the lower EOS id): we answer as a host does - a cookie for its address first, then
// its hello must be signed by the identity it published in the room.
void DirectNet::onPeerHello(const HelloMsg& h, const sockaddr_storage& from, int fromLen, uint64_t now) {
    if (!opt_.mesh || localPuid_.empty() || !active_ || h.puid.empty() || h.puid >= localPuid_ || blockedPeer(h.puid))
        return;
    const uint64_t bucket = now / kCookieBucketMs;
    auto current = cookieFor(h, from, fromLen, bucket);
    auto previous = cookieFor(h, from, fromLen, bucket - 1);
    if (!current) return;
    if (h.cookie != *current && (!previous || h.cookie != *previous)) {
        Message c;
        c.type = MsgType::Challenge;
        c.challenge.clientNonce = h.nonce;
        c.challenge.cookie = *current;
        sendMsg(c, from, fromLen);
        return;
    }
    if (const char* why = identityRefusal(h)) {
        logRateLimited(why, 10000, "DIRECT refused the direct link of joiner %s from %s: %s", shortId(h.puid).c_str(),
                       addrToString(from, fromLen).c_str(), why);
        return;
    }
    const std::string commitment = memberIds_[h.puid];
    auto it = peerLinks_.find(h.puid);
    const bool sameSession = it != peerLinks_.end() && it->second.session == h.session &&
                             it->second.peerNonce == h.nonce && it->second.peerEcdh == h.ecdh;
    auto seen = peerSeen_.find(h.puid);
    if (!sameSession && seen != peerSeen_.end() && seen->second.commitment == commitment && h.session <= seen->second.session)
        return;  // a replayed hello of an earlier session
    if (!sameSession) {
        if (it == peerLinks_.end() && peerLinks_.size() >= kMaxClients) return;
        Link link;
        link.addr = from;
        link.addrLen = fromLen;
        link.puid = h.puid;
        link.peer = true;
        link.peerNonce = h.nonce;
        link.session = h.session;
        link.epoch = linkEpoch(h.nonce, localNonce_);
        link.lastRecvMs = now;
        if (!openHostLink(link, h)) return;
        peerLinks_[h.puid] = std::move(link);
        peerSeen_[h.puid] = Seen{commitment, h.session};
    }
    Link& link = peerLinks_[h.puid];
    link.lastRecvMs = now;
    sendWelcome(link, from, fromLen);
}

// The joiner we dialled answered: its welcome must be signed by the identity it published in the room.
void DirectNet::onPeerWelcome(const std::string& puid, Dial& d, const WelcomeMsg& w, uint64_t now) {
    auto member = memberIds_.find(puid);
    const char* why = nullptr;
    if (w.hostPuid != puid) why = "another member answered at its address";
    else if (member == memberIds_.end() || identityCommitment(w.publicKey) != member->second)
        why = "it is not signed by the identity that member published in the room";
    auto digest = d.ecdh ? welcomeDigest(w, d.ecdh->publicKey()) : std::nullopt;
    if (!why && (!digest || !verifySignature(w.publicKey, *digest, w.signature))) why = "its signature does not verify";
    if (why) {
        logRateLimited(why, 10000, "DIRECT refused the direct link answer of joiner %s: %s", shortId(puid).c_str(), why);
        return;
    }
    HelloMsg hello;
    hello.nonce = d.nonce;
    hello.session = d.session;
    hello.puid = localPuid_;
    hello.ecdh = d.ecdh->publicKey();
    auto shared = d.ecdh->agree(w.ecdh);
    auto keys = shared ? deriveLinkKeys(*shared, opt_.key, hello, w) : std::nullopt;
    Link link;
    if (keys) {
        link.txMac = LinkMac(keys->clientToHost);
        link.rxMac = LinkMac(keys->hostToClient);
    }
    if (!link.txMac.valid() || !link.rxMac.valid()) return;
    link.addr = d.addr;
    link.addrLen = d.addrLen;
    link.puid = puid;
    link.peer = true;
    link.peerNonce = w.hostNonce;
    link.epoch = linkEpoch(d.nonce, w.hostNonce);
    link.up = true;
    link.id = ++linkIds_;
    link.lastRecvMs = now;
    link.lastDataMs = now;
    d.link = std::move(link);
    intros_.erase(puid);
    logf("DIRECT linked directly to joiner %s at %s", shortId(puid).c_str(), addrToString(d.addr, d.addrLen).c_str());
    // The first datagram with the link keys brings our side of the link up on its end.
    Message p;
    p.type = MsgType::Ping;
    p.ping.timeMs = now;
    sendLink(*d.link, p);
}

bool DirectNet::onPeerDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now) {
    const Message& m = r.msg;
    Link* link = nullptr;
    bool responder = false;
    for (auto& [puid, l] : peerLinks_)
        if (l.epoch == m.epoch) {
            link = &l;
            responder = true;
            break;
        }
    if (!link)
        for (auto& [puid, d] : dials_)
            if (d.link && d.link->epoch == m.epoch) {
                link = &*d.link;
                break;
            }
    if (!link) return false;
    if (blockedPeer(link->puid)) return true;  // tests: lost on the way
    if (!authentic(*link, r, from, fromLen)) return true;
    const std::string puid = link->puid;  // copy: the link may go below
    if (responder && !sameAddr(link->addr, link->addrLen, from, fromLen)) {
        link->addr = from;  // the joiner's NAT moved it; only it holds the key
        link->addrLen = fromLen;
    }
    if (responder && !link->up && m.type != MsgType::Bye) {
        link->up = true;
        link->id = ++linkIds_;
        link->lastDataMs = now;
        intros_.erase(puid);
        logf("DIRECT joiner %s linked directly from %s", shortId(puid).c_str(), addrToString(from, fromLen).c_str());
    }
    if (m.type == MsgType::Bye) {
        dropPeer(puid, "it closed the direct link");
        return true;
    }
    if (m.type == MsgType::Data && (m.data.src != puid || m.data.dst != localPuid_)) return true;
    if (!isLinkScoped(m.type) || m.type == MsgType::Roster || m.type == MsgType::Room || m.type == MsgType::PeerQuery ||
        m.type == MsgType::PeerInfo)
        return true;  // the host's business, not a joiner's
    onLinkCommon(*link, m, now);
    return true;
}

void DirectNet::dropPeer(const std::string& puid, const char* why) {
    if (peerLinks_.erase(puid) + dials_.erase(puid)) logf("DIRECT direct link to joiner %s closed (%s); the host relays", shortId(puid).c_str(), why);
}


// Our hello: plain until the host sent a cookie, then with our ECDH key, signed with our identity.
void DirectNet::sendHello(uint64_t now) {
    Message h;
    h.type = MsgType::Hello;
    h.hello.nonce = localNonce_;
    h.hello.session = localSession_;
    h.hello.puid = localPuid_;
    if (cookie_ && identity_ && localEcdh_) {
        h.hello.cookie = *cookie_;
        h.hello.publicKey = identity_->publicKey();
        h.hello.ecdh = localEcdh_->publicKey();
        auto digest = helloDigest(h.hello);
        auto signature = digest ? identity_->sign(*digest) : std::nullopt;
        if (signature) h.hello.signature = *signature;
    }
    sendMsg(h, hostAddr_, hostAddrLen_);
    lastHelloMs_ = now;
}

void DirectNet::run() {
    std::vector<uint8_t> buf(kMaxDatagram);
    while (running_) {
        if (opt_.mode == Mode::Join) {
            // DNS may block, so resolve outside the lock; retry while the link is down (DDNS may move).
            bool needResolve;
            {
                std::lock_guard<std::mutex> lock(mu_);
                needResolve = hostAddrLen_ == 0 || (!hostLink_ && nowMs() - lastResolveMs_ >= kResolveIntervalMs);
            }
            if (needResolve) {
                resolveHost();
                std::lock_guard<std::mutex> lock(mu_);
                lastResolveMs_ = nowMs();
            }
        }

        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(sock_, &rd);
        timeval tv{0, 5000};
        select(0, &rd, nullptr, nullptr, &tv);
        for (;;) {
            sockaddr_storage from{};
            int fromLen = sizeof(from);
            int n = recvfrom(sock_, reinterpret_cast<char*>(buf.data()), static_cast<int>(buf.size()), 0,
                             reinterpret_cast<sockaddr*>(&from), &fromLen);
            if (n <= 0) break;
            wireIn_ += static_cast<uint64_t>(n);
            processDatagram(buf.data(), static_cast<size_t>(n), from, fromLen, nowMs());
        }
        std::lock_guard<std::mutex> lock(mu_);
        tick(nowMs());
    }
}

// IPv4 (and host names) first, IPv6 after.
std::vector<std::string> orderHostCandidates(const std::string& advertised) {
    std::vector<std::string> first, v6;
    size_t pos = 0;
    while (pos < advertised.size() && first.size() + v6.size() < kMaxHostCandidates) {
        size_t end = advertised.find(' ', pos);
        if (end == std::string::npos) end = advertised.size();
        std::string a = advertised.substr(pos, end - pos);
        pos = end + 1;
        std::string host;
        uint16_t port = 0;
        if (a.empty() || !splitHostPort(a, host, port)) continue;
        sockaddr_storage literal;
        bool ok = parseLiteral(host, literal) ? dialable(literal) : plausibleHostName(host);
        if (ok) (literal.ss_family == AF_INET6 ? v6 : first).push_back(a);
    }
    first.insert(first.end(), v6.begin(), v6.end());
    return first;
}

}  // namespace dn
