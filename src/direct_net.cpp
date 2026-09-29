#include "direct_net.h"

#include <mstcpip.h>
#include <windows.h>

#include <algorithm>
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
constexpr uint64_t kMigrateQuietMs = 5000;
constexpr uint16_t kDefaultPort = 27015;

uint64_t nowMs() { return GetTickCount64(); }

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
    if (!openSocket(AF_INET6, opt_.listenPort) && !openSocket(AF_INET, opt_.listenPort)) {
        WSACleanup();
        return false;
    }
    localNonce_ = randomNonce();
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
        Message bye;
        bye.type = MsgType::Bye;
        for (int copy = 0; copy < 3; ++copy) {  // one lost datagram must not leave a stale link behind
            for (auto& [id, link] : clients_) sendMsg(bye, link.addr, link.addrLen);
            if (hostLink_) sendMsg(bye, hostLink_->addr, hostLink_->addrLen);
        }
        // Another thread may still hold a pointer to this instance (AutoJoin swaps instances):
        // with no links left, send() and canRoute() report false and the game falls back to EOS.
        clients_.clear();
        hostLink_.reset();
        roster_.clear();
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
        localNonce_ = randomNonce();
    }
    localPuid_ = puid;
    logf("DIRECT local EOS user %s", puid.c_str());
}

bool DirectNet::canRoute(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return false;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        return it != clients_.end() && it->second.up;
    }
    return hostLink_ && hostLink_->up && (remote == hostLink_->puid || contains(roster_, remote));
}

bool DirectNet::send(const std::string& remote, const std::string& socketName, uint8_t channel, uint8_t reliability,
                     const uint8_t* data, size_t size) {
    std::lock_guard<std::mutex> lock(mu_);
    if (localPuid_.empty() || remote == localPuid_) return false;
    Link* link = nullptr;
    if (opt_.mode == Mode::Host) {
        auto it = clients_.find(remote);
        if (it != clients_.end() && it->second.up) link = &it->second;
    } else if (hostLink_ && hostLink_->up && (remote == hostLink_->puid || contains(roster_, remote))) {
        link = &*hostLink_;
    }
    if (!link) return false;
    DataMsg msg;
    msg.src = localPuid_;
    msg.dst = remote;
    msg.socketName = socketName;
    msg.channel = channel;
    msg.reliability = reliability == 0 && opt_.upgradeUnreliable ? 1 : reliability;
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

std::vector<std::string> DirectNet::directMembers() {
    std::lock_guard<std::mutex> lock(mu_);
    return rosterLocked();
}

std::vector<std::string> DirectNet::rosterLocked() const {
    if (opt_.mode == Mode::Join) return roster_;
    std::vector<std::string> r;
    if (!localPuid_.empty()) r.push_back(localPuid_);
    for (const auto& [id, link] : clients_)
        if (link.up) r.push_back(id);
    return r;
}

std::string DirectNet::statusLine() {
    std::lock_guard<std::mutex> lock(mu_);
    char buf[256];
    std::string s;
    auto describe = [&](const Link& l) {
        snprintf(buf, sizeof(buf), " [%s %s rtt=%ums pending=%zu retx=%llu]", shortId(l.puid).c_str(),
                 addrToString(l.addr, l.addrLen).c_str(), l.rttMs, l.tx.pendingCount(),
                 static_cast<unsigned long long>(l.tx.retransmits()));
        s += buf;
    };
    if (opt_.mode == Mode::Host) {
        s = "HOST clients=" + std::to_string(clients_.size());
        for (const auto& [id, link] : clients_) describe(link);
    } else {
        s = hostLink_ && hostLink_->up ? "JOIN connected" : "JOIN waiting for host";
        if (hostLink_) describe(*hostLink_);
        s += " roster=" + std::to_string(roster_.size());
    }
    return s;
}

void DirectNet::sendRaw(const std::vector<uint8_t>& dg, const sockaddr_storage& to, int toLen) {
    if (testBlackhole_) return;
    if (opt_.testDropRate > 0.0) {
        static thread_local std::mt19937 rng(12345);
        if (std::uniform_real_distribution<double>(0.0, 1.0)(rng) < opt_.testDropRate) return;
    }
    sendto(sock_, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
           reinterpret_cast<const sockaddr*>(&to), toLen);
}

void DirectNet::sendMsg(const Message& m, const sockaddr_storage& to, int toLen) {
    sendRaw(encode(m, opt_.key), to, toLen);
}

void DirectNet::sendData(Link& link, DataMsg msg, uint64_t now) {
    Message m;
    m.type = MsgType::Data;
    m.epoch = link.epoch;
    bool reliable = msg.reliability != 0;
    msg.seq = reliable ? link.tx.nextSeq() : 0;
    uint32_t seq = msg.seq;
    m.data = std::move(msg);
    std::vector<uint8_t> dg = encode(m, opt_.key);
    sendRaw(dg, link.addr, link.addrLen);
    if (reliable) link.tx.track(seq, std::move(dg), now);
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
    auto it = clients_.find(msg.dst);
    if (it == clients_.end() || !it->second.up) {
        logRateLimited("forward-miss", 5000, "DIRECT cannot forward %s -> %s: destination not connected directly",
                       shortId(msg.src).c_str(), shortId(msg.dst).c_str());
        return;
    }
    sendData(it->second, std::move(msg), nowMs());
}

void DirectNet::sendLink(Link& link, Message m) {
    m.epoch = link.epoch;
    sendMsg(m, link.addr, link.addrLen);
}

void DirectNet::onLinkCommon(Link& link, const Message& m, uint64_t now) {
    if (isLinkScoped(m.type) && m.epoch != link.epoch) {
        logRateLimited("stale-epoch", 10000, "DIRECT ignored a packet from an earlier session of link %s",
                       shortId(link.puid).c_str());
        return;
    }
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
        case MsgType::Data: {
            if (m.data.seq == 0) {
                routeData(m.data);
                break;
            }
            std::vector<DataMsg> ready;
            Message ack;
            ack.type = MsgType::Ack;
            ack.ack = link.rx.onData(m.data, ready);
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

void DirectNet::onHostDatagram(const Message& m, const sockaddr_storage& from, int fromLen, uint64_t now) {
    if (m.type == MsgType::Hello) {
        if (localPuid_.empty()) return;  // not signed in yet; the client keeps retrying
        const std::string& id = m.hello.puid;
        if (id.empty() || id == localPuid_) {
            logRateLimited("hello-self", 10000, "DIRECT rejected hello with own/empty id from %s",
                           addrToString(from, fromLen).c_str());
            return;
        }
        auto it = clients_.find(id);
        bool fresh = it == clients_.end() || it->second.peerNonce != m.hello.nonce;
        if (!fresh && !sameAddr(it->second.addr, it->second.addrLen, from, fromLen)) {
            // Same session from a new address. Hellos can be replayed by anyone who saw one, so only
            // follow it when the old address has gone quiet (a live client pings every second);
            // otherwise a replay would hijack the link.
            if (now - it->second.lastRecvMs < kMigrateQuietMs) return;
            logf("DIRECT client %s moved %s -> %s", shortId(id).c_str(),
                 addrToString(it->second.addr, it->second.addrLen).c_str(), addrToString(from, fromLen).c_str());
            it->second.addr = from;
            it->second.addrLen = fromLen;
        }
        if (fresh) {
            // New client or a restarted one: start its reliable streams from scratch.
            if (Link* other = hostClientByAddr(from, fromLen); other && other->puid != id) {
                std::string stale = other->puid;  // copy: erase must not take a key owned by the node
                clients_.erase(stale);
            }
            Link link;
            link.addr = from;
            link.addrLen = fromLen;
            link.puid = id;
            link.peerNonce = m.hello.nonce;
            link.epoch = linkEpoch(m.hello.nonce, localNonce_);
            link.up = true;
            link.lastRecvMs = now;
            clients_[id] = std::move(link);
            logf("DIRECT client %s connected from %s (%zu direct clients)", shortId(id).c_str(),
                 addrToString(from, fromLen).c_str(), clients_.size());
        }
        clients_[id].lastRecvMs = now;
        Message w;
        w.type = MsgType::Welcome;
        w.welcome.hostNonce = localNonce_;
        w.welcome.clientNonce = m.hello.nonce;
        w.welcome.hostPuid = localPuid_;
        w.welcome.roster = rosterLocked();
        sendMsg(w, from, fromLen);
        if (fresh) rosterChanged();
        return;
    }
    Link* link = hostClientByAddr(from, fromLen);
    bool knownSession = false;
    if (!link && isLinkScoped(m.type)) {
        // A live session showing up from a new address (NAT rebinding): migrate instead of resetting.
        // Packets can be replayed by anyone who saw them (a key only stops forgery), so the link moves
        // only on proof of freshness: a reliable packet the host has not received yet, or the old
        // address having gone quiet (a live client pings every second).
        for (auto& [id, candidate] : clients_) {
            if (candidate.epoch != m.epoch) continue;
            knownSession = true;
            bool freshData = m.type == MsgType::Data && m.data.seq != 0 && m.data.src == id &&
                             m.data.seq >= candidate.rx.expected();
            if (!freshData && now - candidate.lastRecvMs < kMigrateQuietMs) break;
            logf("DIRECT client %s moved %s -> %s", shortId(id).c_str(),
                 addrToString(candidate.addr, candidate.addrLen).c_str(), addrToString(from, fromLen).c_str());
            candidate.addr = from;
            candidate.addrLen = fromLen;
            link = &candidate;
            break;
        }
    }
    if (!link) {
        if (knownSession) return;  // belongs to a live link; never reset it on an unproven packet

        // Traffic from a client this host does not know (e.g. the host restarted): make it re-hello now.
        if (m.type != MsgType::Bye) {
            Message bye;
            bye.type = MsgType::Bye;
            sendMsg(bye, from, fromLen);
        }
        return;
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

void DirectNet::onClientDatagram(const Message& m, const sockaddr_storage& from, int fromLen, uint64_t now) {
    if (!sameAddr(from, fromLen, hostAddr_, hostAddrLen_)) return;  // only the configured host talks to us
    switch (m.type) {
        case MsgType::Welcome: {
            if (m.welcome.clientNonce != localNonce_) return;  // answer to an older session
            if (!hostLink_ || hostLink_->peerNonce != m.welcome.hostNonce) {
                hostLink_.emplace();
                hostLink_->addr = from;
                hostLink_->addrLen = fromLen;
                hostLink_->peerNonce = m.welcome.hostNonce;
                hostLink_->epoch = linkEpoch(localNonce_, m.welcome.hostNonce);
                logf("DIRECT connected to host %s at %s", shortId(m.welcome.hostPuid).c_str(),
                     addrToString(from, fromLen).c_str());
            }
            hostLink_->puid = m.welcome.hostPuid;
            hostLink_->up = true;
            hostLink_->lastRecvMs = now;
            roster_ = m.welcome.roster;
            return;
        }
        case MsgType::Roster:
            if (hostLink_ && hostLink_->peerNonce == m.roster.hostNonce) {
                if (roster_ != m.roster.roster)
                    logf("DIRECT roster now has %zu direct members", m.roster.roster.size());
                roster_ = m.roster.roster;
                hostLink_->lastRecvMs = now;
            }
            return;
        case MsgType::Bye:
            if (hostLink_) {
                logf("DIRECT host closed the direct link, reconnecting");
                hostLink_.reset();
                roster_.clear();
                localNonce_ = randomNonce();
                lastHelloMs_ = 0;
            }
            return;
        default:
            if (hostLink_) onLinkCommon(*hostLink_, m, now);
            return;
    }
}

void DirectNet::processDatagram(const uint8_t* data, size_t size, const sockaddr_storage& from, int fromLen,
                                uint64_t now) {
    DecodeError err;
    auto msg = decode(data, size, opt_.key, &err);
    if (!msg) {
        if (err != DecodeError::BadMagic)
            logRateLimited("decode", 10000, "DIRECT rejected datagram from %s: %s",
                           addrToString(from, fromLen).c_str(), decodeErrorName(err));
        return;
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (opt_.mode == Mode::Host)
        onHostDatagram(*msg, from, fromLen, now);
    else
        onClientDatagram(*msg, from, fromLen, now);
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
    for (auto& [id, link] : clients_) sendMsg(r, link.addr, link.addrLen);
    lastRosterMs_ = nowMs();
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
    int len = static_cast<int>(res->ai_addrlen);
    memcpy(&addr, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (family_ == AF_INET6) toDualStack(addr, len);

    std::lock_guard<std::mutex> lock(mu_);
    if (!sameAddr(addr, len, hostAddr_, hostAddrLen_)) {
        logf("DIRECT host address %s -> %s", opt_.hostAddress.c_str(), addrToString(addr, len).c_str());
        hostAddr_ = addr;
        hostAddrLen_ = len;
        hostLink_.reset();
        roster_.clear();
    }
    return true;
}

void DirectNet::tick(uint64_t now) {
    auto pollLink = [&](Link& link) {
        link.tx.poll(now, [&](const std::vector<uint8_t>& dg) { sendRaw(dg, link.addr, link.addrLen); });
        if (now - link.lastPingMs >= opt_.pingIntervalMs) {
            Message p;
            p.type = MsgType::Ping;
            p.ping.timeMs = now;
            sendLink(link, p);
            link.lastPingMs = now;
        }
    };
    auto timedOut = [&](const Link& link) {
        return now - link.lastRecvMs > opt_.linkTimeoutMs || link.tx.oldestPendingAgeMs(now) > opt_.linkTimeoutMs;
    };

    if (opt_.mode == Mode::Host) {
        bool changed = false;
        for (auto it = clients_.begin(); it != clients_.end();) {
            if (timedOut(it->second)) {
                logf("DIRECT client %s timed out (no reply for %u ms, %zu packets unacknowledged)",
                     shortId(it->first).c_str(), opt_.linkTimeoutMs, it->second.tx.pendingCount());
                it = clients_.erase(it);
                changed = true;
                continue;
            }
            pollLink(it->second);
            ++it;
        }
        bool burst = now < rosterBurstUntilMs_ && now - lastRosterMs_ >= kRosterBurstIntervalMs;
        if (changed)
            rosterChanged();
        else if (burst || (!clients_.empty() && now - lastRosterMs_ >= kRosterIntervalMs))
            broadcastRoster();
        return;
    }

    if (hostLink_ && timedOut(*hostLink_)) {
        logf("DIRECT host link timed out (no reply for %u ms), reconnecting", opt_.linkTimeoutMs);
        hostLink_.reset();
        roster_.clear();
        localNonce_ = randomNonce();
    }
    if (hostLink_) {
        pollLink(*hostLink_);
        return;
    }
    if (!localPuid_.empty() && hostAddrLen_ > 0 && now - lastHelloMs_ >= kHelloIntervalMs) {
        Message h;
        h.type = MsgType::Hello;
        h.hello.nonce = localNonce_;
        h.hello.puid = localPuid_;
        sendMsg(h, hostAddr_, hostAddrLen_);
        lastHelloMs_ = now;
    }
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
    while (pos < advertised.size()) {
        size_t end = advertised.find(' ', pos);
        if (end == std::string::npos) end = advertised.size();
        std::string a = advertised.substr(pos, end - pos);
        if (!a.empty()) (a[0] == '[' ? v6 : first).push_back(a);
        pos = end + 1;
    }
    first.insert(first.end(), v6.begin(), v6.end());
    return first;
}

}  // namespace dn
