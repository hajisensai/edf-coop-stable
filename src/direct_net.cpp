#include "direct_net.h"

#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

#include "log.h"

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
// Rooms hold up to 32 players (31 clients). Twice that leaves room for links of players who left and
// have not timed out yet, which are no longer in the member list (see rosterLocked); the cap also bounds
// what a hello flood with made-up ids can allocate. The member list itself holds at most 32 (wire.cpp).
constexpr size_t kMaxClients = 64;
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

}  // namespace

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
    }
}

bool DirectNet::canRoute(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return false;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        return it != clients_.end() && usable(it->second);
    }
    return hostLink_ && usable(*hostLink_) && (remote == hostLink_->puid || contains(roster_, remote));
}

bool DirectNet::send(const std::string& remote, const std::string& socketName, uint8_t channel, uint8_t reliability,
                     const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_ || size > kMaxPayload) return false;
    Link* link = nullptr;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        if (it != clients_.end() && usable(it->second)) link = &it->second;
    } else if (hostLink_ && usable(*hostLink_) && (remote == hostLink_->puid || contains(roster_, remote))) {
        link = &*hostLink_;
    }
    if (!link) return false;
    DataMsg msg;
    msg.src = localPuid_;
    msg.dst = remote;
    msg.socketName = socketName;
    msg.channel = channel;
    msg.reliability = reliability;
    msg.payload.assign(data, data + size);
    sendData(*link, std::move(msg), nowMs());
    return true;
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
    size_t closed = clients_.size() + (hostLink_ ? 1 : 0);
    clients_.clear();
    hostLink_.reset();
    roster_.clear();
    lastRoster_.clear();
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
    return WireTraffic{wireOut_.exchange(0), wireIn_.exchange(0), relayed_.exchange(0)};
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
    }
    if (uint64_t n = rejected_) s += " rejected=" + std::to_string(n);
    if (uint64_t n = sendFailures_) s += " send-refused=" + std::to_string(n);
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
    // upgradeUnreliable, and gives it up after the deadline like the sender does.
    bool upgraded = msg.reliability == 0 && opt_.upgradeUnreliable;
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
    relayed_ += msg.payload.size();
    sendData(it->second, std::move(msg), nowMs());
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
            break;
        case MsgType::Ack:
            link.tx.onAck(m.ack, now);
            break;
        case MsgType::Data:
        case MsgType::Forward: {
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
    std::vector<std::string> roster = rosterLocked();
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
    onLinkCommon(*link, m, now);
}

void DirectNet::onClientDatagram(const Received& r, const sockaddr_storage& from, int fromLen, uint64_t now) {
    // A host on a wildcard socket answers from the source address its OS picks, which need not be the
    // one we dialled (IPv6 hosts prefer a temporary address). So the host's packets are recognised by
    // their session and link keys, not by their source address, and we keep sending to the address we
    // dialled.
    const Message& m = r.msg;
    switch (m.type) {
        case MsgType::Challenge: {
            // The host keeps nothing for us until we echo this cookie in a hello signed with our identity.
            if (hostLink_ || hostAddrLen_ == 0 || m.challenge.clientNonce != localNonce_) return;
            bool fresh = !cookie_ || *cookie_ != m.challenge.cookie;
            cookie_ = m.challenge.cookie;
            if (fresh && active_ && !localPuid_.empty()) sendHello(now);  // a repeat waits for the next retry
            return;
        }
        case MsgType::Welcome:
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
            return;
        default:
            break;
    }
    // Link messages: only from the host, proven by the link keys. Stray ones are not even logged.
    if (!hostLink_ || !authentic(*hostLink_, r, from, fromLen)) return;
    if (m.type == MsgType::Bye) {
        dropHostLink("the host closed the direct link, reconnecting");
        return;
    }
    if (m.type == MsgType::Roster) {
        // A member list older than the one applied (reordered in flight) would bring back a member
        // the host already dropped.
        if (m.counter < hostLink_->rosterCounter || m.roster.hostNonce != hostLink_->peerNonce) return;
        hostLink_->rosterCounter = m.counter;
        if (roster_ != m.roster.roster) logf("DIRECT roster now has %zu direct members", m.roster.roster.size());
        roster_ = m.roster.roster;
    }
    if (m.type == MsgType::Room) {
        if (m.counter < hostLink_->roomCounter || m.room.hostNonce != hostLink_->peerNonce) return;
        hostLink_->roomCounter = m.counter;
        if (hostRoom_ != m.room.members) {
            hostRoom_ = m.room.members;
            ++hostRoomVersion_;
        }
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
    Message r;
    r.type = MsgType::Roster;
    r.roster.hostNonce = localNonce_;
    r.roster.roster = rosterLocked();
    lastRoster_ = r.roster.roster;
    for (auto& [id, link] : clients_) {
        sendLink(link, r);
        sendRoom(link);
    }
    lastRosterMs_ = nowMs();
}

void DirectNet::sendRoom(Link& link) {
    if (!roomSet_) return;
    Message m;
    m.type = MsgType::Room;
    m.room.hostNonce = localNonce_;
    m.room.members = roomMembers_;
    sendLink(link, m);
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
        if (now - link.lastPingMs >= opt_.pingIntervalMs) {
            Message p;
            p.type = MsgType::Ping;
            p.ping.timeMs = now;
            sendLink(link, p);
            link.lastPingMs = now;
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
        bool burst = now < rosterBurstUntilMs_ && now - lastRosterMs_ >= kRosterBurstIntervalMs;
        if (changed)
            rosterChanged();
        else if (burst || (!clients_.empty() && now - lastRosterMs_ >= kRosterIntervalMs))
            broadcastRoster();
        return;
    }

    if (hostLink_ && closing(*hostLink_, "host link")) dropHostLink("reconnecting to the host");
    if (hostLink_) {
        pollLink(*hostLink_);
        return;
    }
    if (active_ && !localPuid_.empty() && hostAddrLen_ > 0 && now - lastHelloMs_ >= kHelloIntervalMs) sendHello(now);
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
