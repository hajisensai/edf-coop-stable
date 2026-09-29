// EDF6DirectNet tests. Build with build.ps1 and run build\edf6_directnet_tests.exe [path-to-EDF.dll].
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/config.h"
#include "../src/direct_net.h"
#include "../src/hold.h"
#include "../src/iat.h"
#include "../src/netif.h"
#include "../src/reliable.h"
#include "../src/traffic.h"
#include "../src/updater.h"
#include "../src/wire.h"

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        ++g_checks;                                                       \
        if (!(cond)) {                                                    \
            ++g_failures;                                                 \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
        }                                                                 \
    } while (0)

const std::string kHost = "0002aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const std::string kA = "0002bbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const std::string kB = "0002cccccccccccccccccccccccccccc";

bool waitFor(const std::function<bool()>& cond, int timeoutMs) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return cond();
}

std::vector<uint8_t> payloadFor(uint32_t i) {
    std::vector<uint8_t> p(8 + i % 1100);
    for (size_t k = 0; k < p.size(); ++k) p[k] = static_cast<uint8_t>(i * 31 + k);
    memcpy(p.data(), &i, 4);
    return p;
}

void testWire() {
    printf("wire\n");
    dn::Message m;
    m.type = dn::MsgType::Data;
    m.data.seq = 42;
    m.data.src = kA;
    m.data.dst = kB;
    m.data.socketName = "EDF6";
    m.data.channel = 3;
    m.data.reliability = 2;
    m.data.payload = payloadFor(7);
    for (const std::string key : {"", "secret"}) {
        auto dg = dn::encode(m, key);
        dn::DecodeError err;
        auto back = dn::decode(dg.data(), dg.size(), key, &err);
        CHECK(back && err == dn::DecodeError::None);
        CHECK(back && back->data.payload == m.data.payload && back->data.src == kA && back->data.dst == kB &&
              back->data.socketName == "EDF6" && back->data.channel == 3 && back->data.seq == 42);
    }
    auto tagged = dn::encode(m, "secret");
    dn::DecodeError err;
    CHECK(!dn::decode(tagged.data(), tagged.size(), "other", &err) && err == dn::DecodeError::TagMismatch);
    CHECK(!dn::decode(tagged.data(), tagged.size(), "", &err) && err == dn::DecodeError::TagUnexpected);
    auto plain = dn::encode(m, "");
    CHECK(!dn::decode(plain.data(), plain.size(), "secret", &err) && err == dn::DecodeError::TagMissing);
    tagged[20] ^= 1;
    CHECK(!dn::decode(tagged.data(), tagged.size(), "secret", &err) && err == dn::DecodeError::TagMismatch);
    CHECK(!dn::decode(plain.data(), 10, "", &err) && err == dn::DecodeError::Truncated);

    dn::Message ack;
    ack.type = dn::MsgType::Ack;
    ack.epoch = dn::linkEpoch(7, 9);
    ack.ack.cumulative = 100;
    ack.ack.set(0);
    ack.ack.set(255);
    auto adg = dn::encode(ack, "");
    auto aback = dn::decode(adg.data(), adg.size(), "", &err);
    CHECK(aback && aback->epoch == ack.epoch && aback->ack.cumulative == 100 && aback->ack.has(0) &&
          aback->ack.has(255) && !aback->ack.has(1));
    CHECK(dn::linkEpoch(7, 9) != dn::linkEpoch(9, 7) && dn::linkEpoch(7, 9) != dn::linkEpoch(8, 9));

    dn::Message w;
    w.type = dn::MsgType::Welcome;
    w.welcome.hostNonce = 5;
    w.welcome.clientNonce = 9;
    w.welcome.hostPuid = kHost;
    w.welcome.roster = {kHost, kA, kB};
    auto dg = dn::encode(w, "");
    auto back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->welcome.roster == w.welcome.roster && back->welcome.clientNonce == 9);
}

// One of every message type, with fields filled in.
std::vector<dn::Message> sampleMessages() {
    std::vector<dn::Message> all;
    dn::Message m;
    m.type = dn::MsgType::Hello;
    m.hello.nonce = 77;
    m.hello.puid = kA;
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Welcome;
    m.welcome.hostNonce = 5;
    m.welcome.clientNonce = 9;
    m.welcome.hostPuid = kHost;
    m.welcome.roster = {kHost, kA, kB};
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Roster;
    m.roster.hostNonce = 5;
    m.roster.roster = {kHost, kB};
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Data;
    m.epoch = 3;
    m.data.seq = 9;
    m.data.src = kA;
    m.data.dst = kB;
    m.data.socketName = "EDF6";
    m.data.reliability = 1;
    m.data.payload = payloadFor(3);
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Ack;
    m.ack.cumulative = 4;
    m.ack.set(7);
    all.push_back(m);
    for (auto t : {dn::MsgType::Ping, dn::MsgType::Pong, dn::MsgType::Bye}) {
        m = {};
        m.type = t;
        m.ping.timeMs = 1234;
        all.push_back(m);
    }
    return all;
}

void testWireRejectsMalformed() {
    printf("wire: over-long fields, unknown types, truncation and random bytes are rejected\n");
    dn::DecodeError err;
    auto decodes = [&](const std::vector<uint8_t>& dg) { return dn::decode(dg.data(), dg.size(), "", &err).has_value(); };

    // Data: header(8) epoch(4) seq(4), then src as u8 length + bytes.
    dn::Message m;
    m.type = dn::MsgType::Data;
    m.data.seq = 1;
    m.data.src = std::string(dn::kMaxString, 'a');
    m.data.dst = kB;
    m.data.payload.assign(dn::kMaxPayload, 7);
    auto dg = dn::encode(m, "");
    CHECK(decodes(dg));  // the limits themselves are fine
    auto longId = dg;
    longId[16] = static_cast<uint8_t>(dn::kMaxString + 1);
    longId.insert(longId.begin() + 17, 'a');
    CHECK(!decodes(longId) && err == dn::DecodeError::Malformed);  // an id the encoder would have cut
    m.data.src = kA;
    m.data.payload.clear();
    dg = dn::encode(m, "");
    auto big = dg;  // payload length is the last field before the payload
    uint16_t over = static_cast<uint16_t>(dn::kMaxPayload + 1);
    memcpy(&big[big.size() - 2], &over, 2);
    big.insert(big.end(), dn::kMaxPayload + 1, 7);
    CHECK(!decodes(big) && err == dn::DecodeError::Malformed);

    dn::Message r;
    r.type = dn::MsgType::Roster;
    r.roster.roster.assign(32, kA);
    dg = dn::encode(r, "");
    CHECK(decodes(dg));
    dg[12] = 33;  // roster count, after the host nonce
    dg.push_back(static_cast<uint8_t>(kA.size()));
    dg.insert(dg.end(), kA.begin(), kA.end());
    CHECK(!decodes(dg) && err == dn::DecodeError::Malformed);

    dg = dn::encode(sampleMessages()[0], "");
    dg[4] = 99;  // a message type this version does not know
    CHECK(!decodes(dg) && err == dn::DecodeError::Malformed);

    // Every strict prefix of every valid message is rejected, tagged or not.
    bool prefixesRejected = true;
    for (const auto& msg : sampleMessages()) {
        for (const std::string key : {"", "k"}) {
            auto full = dn::encode(msg, key);
            if (!dn::decode(full.data(), full.size(), key, &err)) prefixesRejected = false;
            for (size_t n = 0; n < full.size(); ++n)
                if (dn::decode(full.data(), n, key, &err) || err == dn::DecodeError::None) prefixesRejected = false;
        }
    }
    CHECK(prefixesRejected);

    // Random datagrams and random corruptions of valid ones: never a crash, and anything accepted
    // is a well-formed message that encodes back within the limits.
    std::mt19937 rng(20260930);
    auto valid = sampleMessages();
    size_t accepted = 0;
    bool consistent = true;
    for (int i = 0; i < 200000; ++i) {
        std::vector<uint8_t> buf;
        if (i % 2) {
            buf = dn::encode(valid[rng() % valid.size()], "");
            for (int flips = 1 + rng() % 4; flips > 0; --flips) buf[rng() % buf.size()] = static_cast<uint8_t>(rng());
            if (rng() % 4 == 0) buf.resize(rng() % (buf.size() + 1));
        } else {
            buf.resize(rng() % 300);
            for (auto& b : buf) b = static_cast<uint8_t>(rng());
            if (buf.size() >= 8 && rng() % 2) {  // a valid header, so the body parser gets exercised
                uint32_t magic = dn::kMagic;
                uint16_t protocol = dn::kProtocol;
                memcpy(buf.data(), &magic, 4);
                buf[4] = static_cast<uint8_t>(rng() % 12);
                buf[5] = 0;
                memcpy(buf.data() + 6, &protocol, 2);
            }
        }
        auto got = dn::decode(buf.data(), buf.size(), "", &err);
        if (!got) {
            consistent &= err != dn::DecodeError::None;
            continue;
        }
        ++accepted;
        consistent &= err == dn::DecodeError::None && got->data.payload.size() <= dn::kMaxPayload &&
                      got->data.src.size() <= dn::kMaxString && got->welcome.roster.size() <= 32;
        auto again = dn::encode(*got, "");
        consistent &= dn::decode(again.data(), again.size(), "", &err).has_value();
    }
    printf("  fuzz: %zu of 200000 random datagrams decoded as valid messages\n", accepted);
    CHECK(consistent);
}

void testReliableUnderLoss() {
    printf("reliable: 40%% loss + reordering, 5000 packets\n");
    dn::ReliableSender tx;
    dn::ReliableReceiver rx;
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> coin(0, 1);
    struct InFlight {
        uint64_t at;
        std::vector<uint8_t> dg;
        bool isAck;
    };
    std::vector<InFlight> wire;
    std::vector<uint32_t> delivered;
    uint64_t now = 0;
    uint32_t sent = 0;
    auto push = [&](std::vector<uint8_t> dg, bool isAck) {
        if (coin(rng) < 0.4) return;
        wire.push_back({now + 20 + static_cast<uint64_t>(coin(rng) * 60), std::move(dg), isAck});
    };
    while (delivered.size() < 5000 && now < 600000) {
        for (int k = 0; k < 5 && sent < 5000; ++k, ++sent) {
            dn::Message m;
            m.type = dn::MsgType::Data;
            m.data.reliability = 2;
            m.data.seq = tx.nextSeq();
            m.data.payload = payloadFor(sent);
            auto dg = dn::encode(m, "");
            tx.track(m.data.seq, dg, now);
            push(dg, false);
        }
        tx.poll(now, [&](const std::vector<uint8_t>& dg) { push(dg, false); });
        std::vector<InFlight> due;
        std::stable_partition(wire.begin(), wire.end(), [&](const InFlight& f) { return f.at > now; });
        while (!wire.empty() && wire.back().at <= now) {
            due.push_back(std::move(wire.back()));
            wire.pop_back();
        }
        std::shuffle(due.begin(), due.end(), rng);
        for (auto& f : due) {
            auto m = dn::decode(f.dg.data(), f.dg.size(), "", nullptr);
            if (f.isAck) {
                tx.onAck(m->ack, now);
                continue;
            }
            std::vector<dn::DataMsg> ready;
            dn::Message ack;
            ack.type = dn::MsgType::Ack;
            ack.ack = rx.onData(m->data, ready);
            push(dn::encode(ack, ""), true);
            for (auto& d : ready) {
                uint32_t id = 0;
                memcpy(&id, d.payload.data(), 4);
                delivered.push_back(id);
                if (d.payload != payloadFor(id)) ++g_failures;
            }
        }
        now += 5;
    }
    CHECK(delivered.size() == 5000);
    bool inOrder = true;
    for (uint32_t i = 0; i < delivered.size(); ++i) inOrder &= delivered[i] == i;
    CHECK(inOrder);
    printf("  delivered %zu in %.1fs simulated, retransmits=%llu\n", delivered.size(), now / 1000.0,
           static_cast<unsigned long long>(tx.retransmits()));
}

dn::DirectOptions hostOptions(uint16_t port, double drop, const std::string& key = "") {
    dn::DirectOptions o;
    o.mode = dn::Mode::Host;
    o.listenPort = port;
    o.key = key;
    o.testDropRate = drop;
    o.linkTimeoutMs = 5000;
    return o;
}

dn::DirectOptions joinOptions(const std::string& address, double drop, const std::string& key = "") {
    dn::DirectOptions o;
    o.mode = dn::Mode::Join;
    o.listenPort = 0;
    o.hostAddress = address;
    o.key = key;
    o.testDropRate = drop;
    o.linkTimeoutMs = 5000;
    return o;
}

// Sends `count` reliable packets from `from` to `to` and checks they all arrive in order.
bool streamInOrder(dn::DirectNet& from, const std::string& toId, dn::DirectNet& to, const std::string& fromId,
                   uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        auto p = payloadFor(i);
        if (!from.send(toId, "EDF6", 1, 2, p.data(), p.size())) return false;
        // ~1000 packets/s: still an order of magnitude above EDF6's per-peer traffic.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    uint32_t next = 0;
    bool ok = true;
    waitFor(
        [&] {
            dn::Delivered d;
            uint8_t ch = 1;
            while (to.pop(&ch, 1170, d)) {
                uint32_t id = 0;
                memcpy(&id, d.data.data(), 4);
                ok &= id == next && d.src == fromId && d.socketName == "EDF6" && d.data == payloadFor(id);
                ++next;
            }
            return next >= count;
        },
        30000);
    printf("  %s -> %s: %u/%u in order=%s\n", dn::shortId(fromId).c_str(), dn::shortId(toId).c_str(), next, count,
           ok ? "yes" : "NO");
    return ok && next == count;
}

// A plain UDP socket that speaks the wire format, for playing an attacker against a host.
struct RawPeer {
    SOCKET s = INVALID_SOCKET;
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;
    RawPeer() {
        s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in any{};
        any.sin_family = AF_INET;
        bind(s, reinterpret_cast<sockaddr*>(&any), sizeof(any));
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
    }
    ~RawPeer() { closesocket(s); }
    void hello(uint16_t port, const std::string& puid, uint32_t nonce) {
        dn::Message m;
        m.type = dn::MsgType::Hello;
        m.hello.nonce = nonce;
        m.hello.puid = puid;
        auto dg = dn::encode(m, "");
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sendto(s, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
    }
    // Counts datagrams of `type` received until `ms` elapse.
    int count(dn::MsgType type, int ms) {
        int n = 0;
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        while (std::chrono::steady_clock::now() < end) {
            uint8_t buf[2048];
            int got = recv(s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (got <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            auto m = dn::decode(buf, static_cast<size_t>(got), "", nullptr);
            if (m && m->type == type) ++n;
        }
        return n;
    }
};

// Sends `m` to 127.0.0.1:`port` from `peer`'s socket.
void sendTo(RawPeer& peer, uint16_t port, const dn::Message& m, const std::string& key = "") {
    auto dg = dn::encode(m, key);
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(peer.s, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

// The next datagram of `type` arriving at `peer` within `ms`.
std::optional<dn::Message> receiveFrom(RawPeer& peer, dn::MsgType type, int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        uint8_t buf[2048];
        int got = recv(peer.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        auto m = dn::decode(buf, static_cast<size_t>(got), "", nullptr);
        if (m && m->type == type) return m;
    }
    return std::nullopt;
}

// A hand-driven client: says hello as `puid` and keeps the link epoch the host welcomed it with.
struct RawClient {
    RawPeer peer;
    uint16_t port = 0;
    std::string puid;
    uint32_t nonce = 0;
    uint32_t epoch = 0;

    bool connect(uint16_t hostPort, const std::string& id, uint32_t sessionNonce) {
        port = hostPort;
        puid = id;
        nonce = sessionNonce;
        dn::Message h;
        h.type = dn::MsgType::Hello;
        h.hello.nonce = nonce;
        h.hello.puid = puid;
        for (int attempt = 0; attempt < 5; ++attempt) {
            sendTo(peer, port, h);
            auto w = receiveFrom(peer, dn::MsgType::Welcome, 500);
            if (w && w->welcome.clientNonce == nonce) {
                epoch = dn::linkEpoch(nonce, w->welcome.hostNonce);
                return true;
            }
        }
        return false;
    }
    void data(uint32_t seq, const std::string& dst, const std::vector<uint8_t>& payload) {
        dn::Message m;
        m.type = dn::MsgType::Data;
        m.epoch = epoch;
        m.data.seq = seq;
        m.data.src = puid;
        m.data.dst = dst;
        m.data.socketName = "EDF6";
        m.data.channel = 1;
        m.data.reliability = seq ? 1 : 0;
        m.data.payload = payload;
        sendTo(peer, port, m);
    }
};

void testNoReflectionToSender() {
    printf("direct: the host never sends a client's packets back to that client\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    RawClient c;
    CHECK(c.connect(host.boundPort(), kA, 0x51));
    for (uint32_t seq = 1; seq <= 50; ++seq) c.data(seq, kA, payloadFor(seq));  // addressed to itself
    c.data(0, kA, payloadFor(0));
    CHECK(c.peer.count(dn::MsgType::Data, 500) == 0);
    c.data(51, kHost, payloadFor(51));  // the link still works for real traffic
    dn::Delivered d;
    CHECK(waitFor([&] { return host.pop(nullptr, 1170, d); }, 2000) && d.src == kA && d.data == payloadFor(51));
}

void testReplyFromOtherAddress() {
    // A host bound to the wildcard address answers from whatever source address the OS picks for the
    // client. With IPv6 privacy addresses that is usually not the (stable) address the client sent to;
    // on loopback the same happens with 127.0.0.2 -> reply from 127.0.0.1.
    printf("direct: host answering from a different address than the client dialled still connects\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.2:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    CHECK(streamInOrder(a, kHost, host, kA, 200));
    CHECK(streamInOrder(host, kA, a, kHost, 200));
}

void testHelloCannotHijackLiveLink() {
    printf("direct: a forged hello for a connected player does not take over its link\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    RawPeer attacker;
    attacker.hello(host.boundPort(), kA, 0x1234567);  // A's id, a session nonce A never used
    CHECK(attacker.count(dn::MsgType::Welcome, 300) == 0);
    auto p = payloadFor(1);
    CHECK(host.send(kA, "EDF6", 1, 2, p.data(), p.size()));
    CHECK(attacker.count(dn::MsgType::Data, 300) == 0);  // the host still talks to the real A...
    dn::Delivered d;
    uint8_t ch = 1;
    CHECK(waitFor([&] { return a.pop(&ch, 1170, d); }, 2000) && d.data == p);  // ...which gets the packet
    CHECK(streamInOrder(host, kA, a, kHost, 100));
}

void testHelloFloodIsBounded() {
    printf("direct: a hello flood with made-up ids cannot grow the host's member list without bound\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    std::vector<RawPeer> attackers(40);  // one source port each: a single address only replaces itself
    char id[40];
    for (int i = 0; i < 40; ++i) {
        snprintf(id, sizeof(id), "0002ffffffffffffffffffffffff%04d", i);
        attackers[i].hello(host.boundPort(), id, 100 + i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    size_t members = host.directMembers().size();
    printf("  members after the flood: %zu\n", members);
    CHECK(members <= 1 + 16);
}

void testThreeNodesOverLoopback() {
    printf("direct: host + 2 clients on loopback (IPv4 and IPv6), 20%% loss everywhere\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0.2)));
    host.setLocalUser(kHost);
    std::string port = std::to_string(host.boundPort());

    dn::DirectNet a, b;
    CHECK(a.start(joinOptions("127.0.0.1:" + port, 0.2)));
    CHECK(b.start(joinOptions("[::1]:" + port, 0.2)));
    a.setLocalUser(kA);
    b.setLocalUser(kB);

    CHECK(waitFor([&] { return a.canRoute(kB) && b.canRoute(kA) && host.canRoute(kA) && host.canRoute(kB); }, 10000));
    CHECK(a.canRoute(kHost) && !a.canRoute("0002dddddddddddddddddddddddddddd"));
    CHECK(host.directMembers().size() == 3);
    // Link liveness: the host sees its clients, a client sees the host and, through it, the others.
    CHECK(host.linkAlive(kA, 5000) && host.linkAlive(kB, 5000) && !host.linkAlive(kHost, 5000));
    CHECK(a.linkAlive(kHost, 5000) && a.linkAlive(kB, 5000) && !a.linkAlive("0002dddddddddddddddddddddddddddd", 5000));
    CHECK(host.anyLinkAlive(5000) && a.anyLinkAlive(5000));

    CHECK(streamInOrder(a, kB, b, kA, 3000));      // forwarded by the host
    CHECK(streamInOrder(host, kA, a, kHost, 2000));
    CHECK(streamInOrder(b, kHost, host, kB, 2000));

    // Channel filtering: a channel-2 packet must not be returned for a channel-1 request.
    uint8_t one = 1, two = 2;
    auto p = payloadFor(1);
    CHECK(a.send(kB, "EDF6", 2, 2, p.data(), p.size()));
    dn::Delivered d;
    CHECK(waitFor([&] { return !b.pop(&one, 1170, d) && b.pop(&two, 1170, d); }, 5000));

    printf("direct: client restart with the same id resets its streams\n");
    a.stop();
    // Stopped: nothing more from it (its BYE may be lost to the simulated loss).
    CHECK(waitFor([&] { return !host.linkAlive(kA, 1500) && host.linkAlive(kB, 1500); }, 4000));
    dn::DirectNet a2;
    CHECK(a2.start(joinOptions("127.0.0.1:" + port, 0.2)));
    a2.setLocalUser(kA);
    CHECK(waitFor([&] { return a2.canRoute(kB) && b.canRoute(kA); }, 10000));
    CHECK(streamInOrder(b, kA, a2, kB, 500));
}

void testHostRestart() {
    printf("direct: host restart on the same port, client reconnects without waiting for timeout\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    uint16_t port = host.boundPort();
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(port), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost); }, 5000));
    host.stop();  // sends BYE
    auto p = payloadFor(0);
    CHECK(!host.canRoute(kA) && !host.send(kA, "EDF6", 1, 2, p.data(), p.size()));  // stopped: routes nothing
    CHECK(waitFor([&] { return !a.canRoute(kHost); }, 2000));
    dn::DirectNet host2;
    CHECK(host2.start(hostOptions(port, 0)));
    host2.setLocalUser(kHost);
    auto t0 = std::chrono::steady_clock::now();
    CHECK(waitFor([&] { return a.canRoute(kHost) && host2.canRoute(kA); }, 5000));
    printf("  reconnected after %lld ms\n",
           static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - t0)
                                      .count()));
    CHECK(streamInOrder(a, kHost, host2, kA, 500));
}

void testStalledLinkSurvives() {
    printf("direct: a 3 s total outage loses nothing (packets buffered, delivered in order)\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    a.setTestBlackhole(true);  // every datagram the client sends vanishes
    for (uint32_t i = 0; i < 300; ++i) {
        auto p = payloadFor(i);
        CHECK(a.send(kHost, "EDF6", 1, 2, p.data(), p.size()));
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
    CHECK(a.canRoute(kHost) && host.canRoute(kA));  // still connected during the outage
    // ...but the host hears nothing from the client any more, while the client still hears the host.
    CHECK(!host.linkAlive(kA, 2000) && host.linkAlive(kA, 5000) && a.linkAlive(kHost, 2000));
    CHECK(!host.anyLinkAlive(2000));
    a.setTestBlackhole(false);
    CHECK(waitFor([&] { return host.linkAlive(kA, 2000); }, 3000));
    uint32_t next = 0;
    bool ok = true;
    waitFor(
        [&] {
            dn::Delivered d;
            while (host.pop(nullptr, 1170, d)) {
                uint32_t id = 0;
                memcpy(&id, d.data.data(), 4);
                ok &= id == next++;
            }
            return next >= 300;
        },
        10000);
    printf("  delivered %u/300 after the outage, in order=%s\n", next, ok ? "yes" : "NO");
    CHECK(ok && next == 300);
}

void testReliableUnordered() {
    printf("reliable: unordered mode delivers every packet exactly once under 40%% loss\n");
    dn::ReliableSender tx;
    dn::ReliableReceiver rx;
    std::mt19937 rng(11);
    std::uniform_real_distribution<double> coin(0, 1);
    std::vector<std::pair<std::vector<uint8_t>, bool>> wire;  // datagram, isAck
    std::vector<int> seen(3000, 0);
    size_t delivered = 0;
    uint64_t now = 0;
    uint32_t sent = 0;
    auto push = [&](std::vector<uint8_t> dg, bool isAck) {
        if (coin(rng) >= 0.4) wire.emplace_back(std::move(dg), isAck);
    };
    while (delivered < 3000 && now < 600000) {
        for (int k = 0; k < 3 && sent < 3000; ++k, ++sent) {
            dn::Message m;
            m.type = dn::MsgType::Data;
            m.data.reliability = 1;
            m.data.seq = tx.nextSeq();
            m.data.payload = payloadFor(sent);
            auto dg = dn::encode(m, "");
            tx.track(m.data.seq, dg, now);
            push(dg, false);
        }
        tx.poll(now, [&](const std::vector<uint8_t>& dg) { push(dg, false); });
        auto due = std::move(wire);
        wire.clear();
        std::shuffle(due.begin(), due.end(), rng);
        for (auto& [dg, isAck] : due) {
            auto m = dn::decode(dg.data(), dg.size(), "", nullptr);
            if (isAck) {
                tx.onAck(m->ack, now);
                continue;
            }
            std::vector<dn::DataMsg> ready;
            dn::Message ack;
            ack.type = dn::MsgType::Ack;
            ack.ack = rx.onData(m->data, ready);
            push(dn::encode(ack, ""), true);
            for (auto& d : ready) {
                uint32_t id = 0;
                memcpy(&id, d.payload.data(), 4);
                ++seen[id];
                ++delivered;
            }
        }
        now += 10;
    }
    bool exactlyOnce = std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; });
    printf("  delivered %zu (exactly once: %s) in %.1fs simulated\n", delivered, exactlyOnce ? "yes" : "NO",
           now / 1000.0);
    CHECK(delivered == 3000 && exactlyOnce);
}

void testDirectUpgradesUnreliable() {
    printf("direct: game packets sent unreliable arrive completely under 20%% loss (upgraded)\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0.2)));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0.2)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 10000));
    for (uint32_t i = 0; i < 500; ++i) {
        auto p = payloadFor(i);
        CHECK(a.send(kHost, "EDF6", 0, 0, p.data(), p.size()));  // reliability 0, as EDF6 sends it
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<int> seen(500, 0);
    size_t got = 0;
    waitFor(
        [&] {
            dn::Delivered d;
            while (host.pop(nullptr, 1170, d)) {
                uint32_t id = 0;
                memcpy(&id, d.data.data(), 4);
                if (id < 500) ++seen[id];
                ++got;
            }
            return got >= 500;
        },
        15000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // catch late duplicates, if any
    dn::Delivered d;
    while (host.pop(nullptr, 1170, d)) ++got;
    bool exactlyOnce = std::all_of(seen.begin(), seen.end(), [](int n) { return n == 1; });
    printf("  received %zu/500, exactly once: %s\n", got, exactlyOnce ? "yes" : "NO");
    CHECK(got == 500 && exactlyOnce);
}

void testRetransmitBudget() {
    printf("reliable: a large stalled backlog is retransmitted at a bounded rate\n");
    dn::ReliableSender tx;
    for (uint32_t i = 0; i < 3000; ++i) tx.track(tx.nextSeq(), std::vector<uint8_t>(1000), 1);
    size_t sent = 0;
    auto count = [&](const std::vector<uint8_t>&) { ++sent; };
    tx.poll(60000, count);  // every timer expired during a 60 s stall
    CHECK(sent <= 64);
    size_t burst = sent;
    for (uint64_t t = 60005; t <= 61000; t += 5) tx.poll(t, count);
    printf("  burst=%zu, first second=%zu\n", burst, sent);
    CHECK(sent >= 1900 && sent <= 2100);
}

void testLobbyStatusHold() {
    printf("hold: a member Epic's lobby service drops is hidden while its direct link is up\n");
    int delivered = 0;
    auto deliver = [&] { ++delivered; };
    bool up = true;
    auto reachable = [&](const std::string&) { return up; };
    dn::LobbyStatusHold h(30000);

    CHECK(!h.offer(kA, false, 0, deliver) && !h.isHeld(kA));  // no direct link: the game is told
    CHECK(h.offer(kA, true, 0, deliver) && h.isHeld(kA));
    CHECK(h.offer(kA, false, 0, deliver) && h.heldCount() == 1);  // a repeat keeps the first one
    CHECK(h.onStatus(kA, 0) && !h.isHeld(kA) && delivered == 0);  // it joined again: both swallowed
    CHECK(!h.onStatus(kA, 0));                                    // a normal join reaches the game

    CHECK(h.offer(kA, true, 0, deliver));
    CHECK(!h.onStatus(kA, 1) && delivered == 1 && !h.isHeld(kA));  // left for real: disconnect first
    CHECK(h.offer(kA, true, 0, deliver));
    CHECK(!h.onStatus(kA, 3) && delivered == 2);  // kicked
    CHECK(h.offer(kA, true, 0, deliver));
    CHECK(!h.onStatus(kA, 4) && delivered == 2 && !h.isHeld(kA));  // promoted: it is there after all
    CHECK(!h.onStatus(kB, 1) && delivered == 2);                   // someone else's status

    // Expiry: only after the direct link has been silent for the whole grace period.
    CHECK(h.offer(kA, true, 1000, deliver));
    CHECK(h.poll(100000, reachable).empty() && h.isHeld(kA));  // link still up: never expires
    up = false;
    CHECK(h.poll(129999, reachable).empty() && h.isHeld(kA));
    auto gone = h.poll(130000, reachable);
    CHECK(gone.size() == 1 && gone[0] == kA && delivered == 3 && !h.isHeld(kA));

    CHECK(h.offer(kA, true, 0, deliver) && h.offer(kB, true, 0, deliver));
    CHECK(h.releaseAll() == 2 && delivered == 5 && h.heldCount() == 0);  // room closed
    CHECK(h.offer(kA, true, 0, deliver));
    CHECK(h.clear() == 1 && delivered == 5 && h.heldCount() == 0);  // we left: nothing to tell

    // A delivery may call straight back into the hold (leaving the room clears it) without deadlock.
    auto leave = [&] {
        ++delivered;
        h.clear();
    };
    CHECK(h.offer(kA, true, 0, leave) && h.offer(kB, true, 0, deliver));
    CHECK(!h.onStatus(kA, 1) && delivered == 6 && h.heldCount() == 0);
    CHECK(h.offer(kA, true, 0, leave));
    CHECK(h.poll(40000, reachable).size() == 1 && delivered == 7);

    // The game kicking a hidden member: delivered on the next poll even though the link is up.
    up = true;
    CHECK(!h.abandon(kA));
    CHECK(h.offer(kA, true, 0, deliver) && h.abandon(kA) && h.isHeld(kA) && delivered == 7);
    CHECK(h.poll(1, reachable).size() == 1 && delivered == 8 && !h.isHeld(kA));
}

void testTrafficMeter() {
    printf("traffic: the game's own send rate, busiest second, peers and copies\n");
    dn::TrafficMeter m;
    uint8_t x[100] = {1}, y[100] = {2};
    uint64_t hx = dn::TrafficMeter::hash(x, sizeof(x)), hy = dn::TrafficMeter::hash(y, sizeof(y));
    CHECK(hx != hy && dn::TrafficMeter::hash(x, sizeof(x)) == hx);
    m.record(kA, 100, hx, 1000);
    m.record(kB, 100, hx, 1001);  // the same data to another player: a copy
    m.record(kB, 100, hx, 1002);  // the same data to the same player again: not a copy
    m.record(kA, 100, hy, 1500);
    m.record(kA, 400, hy, 2100);  // next second
    dn::TrafficSummary s = m.take();
    CHECK(s.bytes == 800 && s.packets == 5 && s.peers == 2 && s.copyBytes == 100);
    CHECK(s.busiestSecondBytes == 400 && s.largestPacket == 400);
    s = m.take();
    CHECK(s.bytes == 0 && s.packets == 0 && s.peers == 0 && s.busiestSecondBytes == 0);
}

void testUpdater() {
    printf("update: versions, release JSON, digest checks, replacing a loaded DLL\n");
    using dn::parseVersion;
    CHECK(parseVersion("v0.3.6").valid() && parseVersion("v0.3.6").patch == 6);
    CHECK(parseVersion("EDF6DirectNet 1.12.0 x").minor == 12 && !parseVersion("v1.2").valid());
    CHECK(parseVersion("v0.3.10").newerThan(parseVersion("0.3.9")) && !parseVersion("0.3.5").newerThan(parseVersion("0.3.5")));
    CHECK(parseVersion("1.0.0").newerThan(parseVersion("0.9.9")) && !parseVersion("0.3.4").newerThan(parseVersion("0.4.0")));
    CHECK(!parseVersion("1. 2. 3").valid() && !parseVersion("v99999999999.0.0").valid() && !parseVersion("1.2.-3").valid());
    CHECK(parseVersion("v1.2.3.4").patch == 3 && parseVersion("x 12.0.999999").patch == 999999);

    std::string json = R"({"tag_name": "v0.3.6", "assets": [
        {"name":"EDF6DirectNet-v0.3.6.zip","browser_download_url":"https://github.com/o/r/releases/download/v0.3.6/EDF6DirectNet-v0.3.6.zip"},
        {"name": "EDF6DirectNet.dll", "browser_download_url": "https://github.com/o/r/releases/download/v0.3.6/EDF6DirectNet.dll"},
        {"name":"EDF6DirectNet.dll.sha256","browser_download_url":"https://evil.example/EDF6DirectNet.dll.sha256"}]})";
    CHECK(dn::jsonString(json, "tag_name") == "v0.3.6");
    CHECK(dn::assetUrl(json, "EDF6DirectNet.dll") == "https://github.com/o/r/releases/download/v0.3.6/EDF6DirectNet.dll");
    CHECK(dn::assetUrl(json, "EDF6DirectNet.dll.sha256").empty());  // not GitHub: refused
    CHECK(dn::assetUrl(json, "missing.dll").empty());
    // The shape GitHub actually returns: release name/author before the assets, label and uploader inside
    // each asset after its name, a body with quotes after them.
    std::string real = R"({"url":"https://api.github.com/repos/o/r/releases/1","assets_url":"https://api.github.com/x",
        "tag_name":"v0.3.7","name":"EDF6DirectNet.dll","author":{"login":"o","name":"EDF6DirectNet.dll"},"assets":[
        {"url":"https://api.github.com/a/1","id":1,"name":"EDF6DirectNet.dll.sha256","label":null,
         "uploader":{"login":"github-actions[bot]","id":2},"content_type":"text/plain","size":84,
         "digest":"sha256:00","browser_download_url":"https://github.com/o/r/releases/download/v0.3.7/EDF6DirectNet.dll.sha256"},
        {"url":"https://api.github.com/a/2","id":3,"name":"EDF6DirectNet.dll","label":"",
         "uploader":{"login":"github-actions[bot]","id":2},"content_type":"application/x-msdownload","size":400000,
         "digest":"sha256:11","browser_download_url":"https://github.com/o/r/releases/download/v0.3.7/EDF6DirectNet.dll"}],
        "body":"see \"name\": \"EDF6DirectNet.dll\" https://evil.example/x"})";
    CHECK(dn::parseVersion(dn::jsonString(real, "tag_name")).patch == 7);
    CHECK(dn::assetUrl(real, "EDF6DirectNet.dll") == "https://github.com/o/r/releases/download/v0.3.7/EDF6DirectNet.dll");
    CHECK(dn::assetUrl(real, "EDF6DirectNet.dll.sha256") ==
          "https://github.com/o/r/releases/download/v0.3.7/EDF6DirectNet.dll.sha256");

    std::vector<uint8_t> abc = {'a', 'b', 'c'};
    CHECK(dn::sha256Hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    dn::Version v = parseVersion("0.3.6");
    std::vector<uint8_t> dll(4096, 0);
    dll[0] = 'M';
    dll[1] = 'Z';
    std::string marker = dn::versionMarker(v);
    std::copy(marker.begin(), marker.end(), dll.begin() + 1000);
    std::string sha = dn::sha256Hex(dll) + "  EDF6DirectNet.dll\n";
    std::string why;
    CHECK(dn::verifyUpdate(dll, sha, v, &why));
    std::string upper = sha;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](char c) { return static_cast<char>(toupper(c)); });
    CHECK(dn::verifyUpdate(dll, upper, v, &why));
    CHECK(!dn::verifyUpdate(dll, sha, parseVersion("0.3.7"), &why));  // another version inside
    CHECK(!dn::verifyUpdate(dll, "not a digest", v, &why));
    std::vector<uint8_t> damaged = dll;
    damaged[2000] ^= 1;
    CHECK(!dn::verifyUpdate(damaged, sha, v, &why));
    std::vector<uint8_t> notDll = dll;
    notDll[0] = 'X';
    CHECK(!dn::verifyUpdate(notDll, dn::sha256Hex(notDll), v, &why));

    // Replacing a DLL that is loaded, the way the game has the plugin loaded while it updates.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"edf6dn_update_test\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring installed = dir + L"EDF6DirectNet.dll";
    DeleteFileW((installed + L".old").c_str());
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring built = std::wstring(self).substr(0, std::wstring(self).find_last_of(L'\\') + 1) + L"EDF6DirectNet.dll";
    CHECK(CopyFileW(built.c_str(), installed.c_str(), FALSE));
    HMODULE loaded = LoadLibraryExW(installed.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(loaded != nullptr);
    CHECK(dn::installOver(installed, dll, &why));
    HANDLE f = CreateFileW(installed.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD size = f != INVALID_HANDLE_VALUE ? GetFileSize(f, nullptr) : 0;
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    CHECK(size == dll.size());                                                               // the new file is in place
    CHECK(GetFileAttributesW((installed + L".old").c_str()) != INVALID_FILE_ATTRIBUTES);  // the loaded one moved aside
    dn::removeOldUpdate(installed);  // still mapped: cannot go yet
    CHECK(GetFileAttributesW((installed + L".old").c_str()) != INVALID_FILE_ATTRIBUTES);
    if (loaded) FreeLibrary(loaded);
    dn::removeOldUpdate(installed);  // the next game start: gone
    CHECK(GetFileAttributesW((installed + L".old").c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW((installed + L".new" + std::to_wstring(GetCurrentProcessId())).c_str()) ==
          INVALID_FILE_ATTRIBUTES);
    // A game that quit between writing its download and renaming it leaves installed.new<pid>.
    std::wstring stale = installed + L".new4242", other = installed + L".newer";
    CHECK(CopyFileW(built.c_str(), stale.c_str(), FALSE) && CopyFileW(built.c_str(), other.c_str(), FALSE));
    dn::removeOldUpdate(installed);
    CHECK(GetFileAttributesW(stale.c_str()) == INVALID_FILE_ATTRIBUTES);
    CHECK(GetFileAttributesW(other.c_str()) != INVALID_FILE_ATTRIBUTES);  // not ours: left alone
    DeleteFileW(other.c_str());
    DeleteFileW(installed.c_str());
    RemoveDirectoryW(dir.c_str());
}

void testRosterDropsQuietMember() {
    printf("direct: other joiners learn within seconds that a member's link went quiet\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    ho.rosterFreshMs = 1500;
    ho.linkTimeoutMs = 60000;  // long: the roster, not the timeout, must tell them
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    std::string port = std::to_string(host.boundPort());
    dn::DirectNet a, b;
    CHECK(a.start(joinOptions("127.0.0.1:" + port, 0)));
    CHECK(b.start(joinOptions("127.0.0.1:" + port, 0)));
    a.setLocalUser(kA);
    b.setLocalUser(kB);
    CHECK(waitFor([&] { return b.linkAlive(kA, 5000) && a.linkAlive(kB, 5000); }, 10000));
    a.setTestBlackhole(true);
    CHECK(waitFor([&] { return !b.linkAlive(kA, 5000) && b.linkAlive(kHost, 5000); }, 6000));
    CHECK(host.canRoute(kA));  // the host still holds the link (it only times out after 60 s)
    a.setTestBlackhole(false);
    CHECK(waitFor([&] { return b.linkAlive(kA, 5000); }, 6000));
}

void testLinksFollowTheRoom() {
    printf("direct: not in a room, a host welcomes nobody and a joiner dials nobody\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    host.setActive(false);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(!waitFor([&] { return a.canRoute(kHost); }, 2000));  // host not in a room
    host.setActive(true);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    host.setActive(false);  // the host left its room: its clients are told at once
    CHECK(waitFor([&] { return !a.canRoute(kHost) && !host.canRoute(kA); }, 2000));
    CHECK(!waitFor([&] { return a.canRoute(kHost); }, 2000));
    host.setActive(true);
    CHECK(waitFor([&] { return a.canRoute(kHost); }, 5000));
    a.setActive(false);  // the joiner left: it closes the link and does not dial again
    CHECK(waitFor([&] { return !host.canRoute(kA) && !a.anyLinkAlive(5000); }, 2000));
    CHECK(!waitFor([&] { return host.canRoute(kA); }, 2000));
    a.setActive(true);
    CHECK(waitFor([&] { return host.canRoute(kA) && a.canRoute(kHost); }, 5000));
}

void testDisconnectHold() {
    printf("hold: transient disconnects are hidden from the game and recovered\n");
    int forwarded = 0, reaccepts = 0;
    auto fwd = [&] { ++forwarded; };
    auto re = [&] { ++reaccepts; };
    auto noDirect = [](const std::string&) { return false; };

    // mode "all": any previously connected peer
    dn::DisconnectHold all({30000, 2000, true});
    CHECK(!all.offer(kA, 3, false, false, 0, fwd, re));  // never connected: a failed first handshake must reach the game
    all.onEstablished(kA);
    CHECK(!all.offer(kA, 2, false, false, 0, fwd, re));    // ClosedByPeer: the player really left
    CHECK(!all.offer(kA, 1, false, false, 0, fwd, re));    // ClosedByLocalUser
    all.onEstablished(kA);
    CHECK(all.offer(kA, 3, false, false, 1000, fwd, re));  // TimedOut: held
    CHECK(all.isHeld(kA) && reaccepts == 0);        // no EOS call from inside the EOS callback
    all.poll(1001, noDirect);
    CHECK(reaccepts == 1);
    all.poll(2000, noDirect);
    CHECK(reaccepts == 1);
    all.poll(3002, noDirect);
    CHECK(reaccepts == 2);
    CHECK(all.onEstablished(kA) == 1 && !all.isHeld(kA) && forwarded == 0);  // recovered, game saw nothing

    CHECK(all.offer(kA, 7, false, false, 10000, fwd, re));  // ConnectionFailed
    CHECK(all.poll(39999, noDirect).empty() && forwarded == 0);
    auto expired = all.poll(40000, noDirect);
    CHECK(expired.size() == 1 && forwarded == 1 && !all.isHeld(kA));
    CHECK(!all.offer(kA, 3, false, false, 50000, fwd, re));  // after expiry the peer is gone until re-established

    all.onEstablished(kB);
    CHECK(all.offer(kB, 3, false, false, 0, fwd, re));
    CHECK(all.onGameClosed(kB) == 1 && !all.isHeld(kB));
    all.poll(100000, noDirect);
    CHECK(forwarded == 1);  // the game closed it itself: nothing forwarded

    // a deliberate close supersedes a held transient one (the game gets exactly one event)
    all.onEstablished(kB);
    CHECK(all.offer(kB, 3, false, false, 0, fwd, re));
    CHECK(!all.offer(kB, 2, false, false, 10, fwd, re) && !all.isHeld(kB));
    all.poll(100000, noDirect);
    CHECK(forwarded == 1);

    // the lobby says the player left: held events reach the game at once, even with a live direct link
    all.onEstablished(kA);
    CHECK(all.offer(kA, 3, true, false, 0, fwd, re));
    CHECK(all.release(kA) == 1 && forwarded == 2 && !all.isHeld(kA));
    CHECK(all.release(kA) == 0);
    all.onEstablished(kA);
    all.onEstablished(kB);
    CHECK(all.offer(kA, 3, false, false, 0, fwd, re) && all.offer(kB, 8, false, false, 0, fwd, re));
    CHECK(all.releaseAll() == 2 && forwarded == 4 && all.heldCount() == 0);
    forwarded = 1;

    // mode "auto": plain EOS peers are never held (they may not run the plugin)
    dn::DisconnectHold autoHold({30000, 2000, false});
    autoHold.onEstablished(kA);
    CHECK(!autoHold.offer(kA, 3, false, false, 0, fwd, re));
    // players carrying the plugin's lobby marker are held for the grace period like mode "all"
    CHECK(autoHold.offer(kA, 3, false, true, 0, fwd, re) && autoHold.isHeld(kA));
    CHECK(autoHold.poll(29999, noDirect).empty() && autoHold.isHeld(kA));
    CHECK(autoHold.poll(30000, noDirect).size() == 1 && !autoHold.isHeld(kA) && forwarded == 2);
    forwarded = 1;
    // ...but, like mode "all", only after a working connection: a failed first handshake reaches the game
    dn::DisconnectHold fresh({30000, 2000, false});
    CHECK(!fresh.offer(kA, 3, false, true, 0, fwd, re));
    // direct-link members are held, even without a prior EOS connection, and never expire while
    // the direct link is alive
    CHECK(autoHold.offer(kB, 3, true, false, 0, fwd, re));
    bool directUp = true;
    auto direct = [&](const std::string& r) { return directUp && r == kB; };
    CHECK(autoHold.poll(100000, direct).empty() && autoHold.isHeld(kB));
    CHECK(autoHold.poll(200000, direct).empty() && autoHold.isHeld(kB));
    directUp = false;  // direct link lost too: the normal grace applies from the last time it was alive
    CHECK(autoHold.poll(229999, direct).empty());
    CHECK(autoHold.poll(230000, direct).size() == 1 && forwarded == 2);
}

void testKeyMismatch() {
    printf("direct: wrong key never connects, right key does\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0, "abc123")));
    host.setLocalUser(kHost);
    std::string addr = "127.0.0.1:" + std::to_string(host.boundPort());
    dn::DirectNet bad, good;
    CHECK(bad.start(joinOptions(addr, 0, "wrong")));
    CHECK(good.start(joinOptions(addr, 0, "abc123")));
    bad.setLocalUser(kA);
    good.setLocalUser(kB);
    CHECK(waitFor([&] { return good.canRoute(kHost); }, 5000));
    CHECK(!waitFor([&] { return bad.canRoute(kHost); }, 2500));
    CHECK(!host.canRoute(kA));
}

void testIat(const wchar_t* edfPath) {
    printf("iat: EDF.dll import slots\n");
    HMODULE edf = LoadLibraryExW(edfPath, nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!edf) {
        printf("  SKIP: cannot load %ls (error %lu)\n", edfPath, GetLastError());
        return;
    }
    const char* dll = "EOSSDK-Win64-Shipping.dll";
    CHECK(dn::findImportSlot(edf, dll, "EOS_Platform_GetP2PInterface") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_SendPacket") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_ReceivePacket") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_AddNotifyPeerConnectionClosed") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_CloseConnection") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_CloseConnections") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_Lobby_AddNotifyLobbyMemberStatusReceived") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_Platform_Tick") != nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_RemoveNotifyPeerConnectionClosed") != nullptr);
    // Lobby hooks: plugin detection, the host's address, room-level disconnect hiding, diagnostics.
    for (const char* name : {"EOS_Lobby_CreateLobby", "EOS_Lobby_JoinLobby", "EOS_Lobby_LeaveLobby",
                             "EOS_Lobby_DestroyLobby", "EOS_Lobby_AddNotifyLobbyMemberUpdateReceived",
                             "EOS_Lobby_KickMember", "EOS_Lobby_UpdateLobby"})
        CHECK(dn::findImportSlot(edf, dll, name) != nullptr);
    // The receive hook relies on the game reading packets only through ReceivePacket.
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_GetNextReceivedPacketSize") == nullptr);
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_NoSuchFunction") == nullptr);
    CHECK(dn::findImportSlot(edf, "nosuch.dll", "EOS_P2P_SendPacket") == nullptr);
    FreeLibrary(edf);
}

void testNetif() {
    printf("netif: physical adapter\n");
    auto pi = dn::findPhysicalInterface();
    if (!pi && getenv("GITHUB_ACTIONS")) {  // CI runners are Hyper-V VMs: no physical adapter to find
        printf("  skipped: no physical adapter on this CI machine\n");
        return;
    }
    CHECK(pi.has_value());
    if (pi) {
        printf("  %s | %s | ipv4=%s v4idx=%u v6idx=%u ipv6=%zu\n", pi->name.c_str(), pi->description.c_str(),
               pi->ipv4.c_str(), pi->ifIndexV4, pi->ifIndexV6, pi->globalIpv6.size());
        CHECK(pi->description.find("Clash") == std::string::npos);
    }
}

void testReliableBacklogLimit() {
    printf("reliable: a peer that never acknowledges makes the link overloaded, by count and by bytes\n");
    dn::ReliableSender count;
    for (uint32_t i = 0; i < 8192; ++i) count.track(count.nextSeq(), std::vector<uint8_t>(20), 1);
    CHECK(!count.overloaded());
    count.track(count.nextSeq(), std::vector<uint8_t>(20), 1);
    CHECK(count.overloaded() && count.pendingCount() == 8193);  // nothing dropped: the caller closes the link
    dn::AckMsg ack;
    ack.cumulative = 100;
    count.onAck(ack, 2);
    CHECK(!count.overloaded() && count.pendingBytes() == 8093 * 20);

    dn::ReliableSender bytes;
    size_t tracked = 0;
    while (!bytes.overloaded()) {
        bytes.track(bytes.nextSeq(), std::vector<uint8_t>(1200), 1);
        ++tracked;
    }
    CHECK(tracked < 8192 && bytes.pendingBytes() > (8u << 20) && bytes.pendingBytes() <= (8u << 20) + 1200);
}

void testUnacknowledgedLinkIsDropped() {
    printf("direct: a client that takes data but never acknowledges it is disconnected, not buffered forever\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    ho.linkTimeoutMs = 60000;  // the backlog, not the timeout, must end it
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    a.setTestBlackhole(true);  // receives everything, acknowledges nothing
    std::vector<uint8_t> p(1100, 1);
    size_t sent = 0;
    while (sent < 20000 && host.send(kA, "EDF6", 1, 1, p.data(), p.size())) ++sent;
    printf("  host stopped taking packets for it after %zu\n", sent);
    CHECK(sent < 8193);
    CHECK(waitFor([&] { return !host.canRoute(kA) && host.directMembers().size() == 1; }, 2000));
}

void testHostCandidates() {
    printf("auto-connect: IPv4 addresses of the room host are tried before IPv6\n");
    using V = std::vector<std::string>;
    CHECK(dn::orderHostCandidates("[2408::5]:27015 1.2.3.4:27015") == V({"1.2.3.4:27015", "[2408::5]:27015"}));
    CHECK(dn::orderHostCandidates("1.2.3.4:27015 [2408::5]:27015") == V({"1.2.3.4:27015", "[2408::5]:27015"}));
    CHECK(dn::orderHostCandidates("my.ddns.net:40000") == V({"my.ddns.net:40000"}));
    CHECK(dn::orderHostCandidates("  a:1   b:2 ") == V({"a:1", "b:2"}));
    CHECK(dn::orderHostCandidates("").empty());
}

void testConfig() {
    printf("config\n");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"edf6directnet_test.ini";
    DeleteFileW(path.c_str());
    dn::Config def = dn::loadConfig(path);
    CHECK(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES);  // default file written
    CHECK(def.enabled && def.direct.mode == dn::Mode::Off && def.direct.listenPort == 27015 && def.eosRelay == -1);
    CHECK(def.hold == dn::Config::Hold::Auto && def.graceMs == 30000 && def.direct.linkTimeoutMs == 60000);
    CHECK(def.reliableGameTraffic && def.direct.upgradeUnreliable);
    CHECK(def.autoJoin && def.publicAddress.empty() && def.autoUpdate);

    // Every language writes the same settings; only the comments differ.
    const unsigned short langs[] = {MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_SIMPLIFIED),
                                    MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL),
                                    MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT),
                                    MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US),
                                    MAKELANGID(LANG_GERMAN, SUBLANG_DEFAULT)};
    auto settingsOnly = [](const std::string& text) {
        std::string out;
        size_t pos = 3;  // BOM
        while (pos < text.size()) {
            size_t end = text.find("\r\n", pos);
            std::string line = text.substr(pos, end - pos);
            if (!line.empty() && line[0] != ';') out += line + "\n";
            pos = end == std::string::npos ? text.size() : end + 2;
        }
        return out;
    };
    std::string zh = dn::defaultIni(langs[0]), ja = dn::defaultIni(langs[2]), en = dn::defaultIni(langs[3]);
    CHECK(zh.compare(0, 3, "\xEF\xBB\xBF") == 0 && zh != ja && ja != en && zh != en);
    CHECK(dn::defaultIni(langs[1]) == zh && dn::defaultIni(langs[4]) == en);
    CHECK(en.find("settings") != std::string::npos);
    CHECK(settingsOnly(zh) == settingsOnly(ja) && settingsOnly(ja) == settingsOnly(en));
    CHECK(settingsOnly(en).find("GraceSeconds=30\n") != std::string::npos);
    for (unsigned short lang : langs) {
        FILE* w = _wfopen(path.c_str(), L"wb");
        std::string text = dn::defaultIni(lang);
        fwrite(text.data(), 1, text.size(), w);
        fclose(w);
        dn::Config d = dn::loadConfig(path);
        CHECK(d.enabled && d.direct.mode == dn::Mode::Off && d.direct.listenPort == 27015 && d.eosRelay == -1);
        CHECK(d.hold == dn::Config::Hold::Auto && d.graceMs == 30000 && d.autoJoin && d.publicAddress.empty());
    }

    FILE* f = _wfopen(path.c_str(), L"wb");
    fputs("[DirectNet]\r\nMode= Join \r\nHostAddress=[2408:8207::5]:30000\r\nKey=abc\r\n"
          "PublicAddress= 1.2.3.4:40000 \r\nAutoJoin=0\r\n[Update]\r\nAutoUpdate=0\r\n"
          "[EOS]\r\nFixedPort=27100\r\nRelay=NoRelay\r\n[Resilience]\r\nHoldDisconnects=ALL\r\nGraceSeconds=45\r\n", f);
    fclose(f);
    dn::Config c = dn::loadConfig(path);
    CHECK(c.direct.mode == dn::Mode::Join);
    CHECK(c.direct.hostAddress == "[2408:8207::5]:30000");
    CHECK(c.direct.listenPort == 0);  // join without ListenPort binds any port
    CHECK(c.direct.key == "abc" && c.eosFixedPort == 27100 && c.eosRelay == 0);
    CHECK(c.hold == dn::Config::Hold::All && c.graceMs == 45000);
    CHECK(c.publicAddress == "1.2.3.4:40000" && !c.autoJoin && !c.autoUpdate);
    DeleteFileW(path.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    // Manual end-to-end check against the real GitHub release: --live-update <dll path> <pretend version>
    if (argc == 4 && std::wstring(argv[1]) == L"--live-update") {
        std::string version;
        for (const wchar_t* c = argv[3]; *c; ++c) version += static_cast<char>(*c);  // digits and dots
        printf("%s\n", dn::updateOnce(argv[2], version).c_str());
        return 0;
    }
    const wchar_t* edf = argc > 1 ? argv[1] : L"D:\\steam\\steamapps\\common\\EARTH DEFENSE FORCE 6\\EDF.dll";
    testWire();
    testWireRejectsMalformed();
    testReliableUnderLoss();
    testConfig();
    testHostCandidates();
    testNetif();
    testIat(edf);
    testKeyMismatch();
    testReliableUnordered();
    testRetransmitBudget();
    testDirectUpgradesUnreliable();
    testDisconnectHold();
    testLobbyStatusHold();
    testTrafficMeter();
    testUpdater();
    testRosterDropsQuietMember();
    testLinksFollowTheRoom();
    testHostRestart();
    testStalledLinkSurvives();
    testReplyFromOtherAddress();
    testHelloCannotHijackLiveLink();
    testHelloFloodIsBounded();
    testThreeNodesOverLoopback();
    testReliableBacklogLimit();
    testUnacknowledgedLinkIsDropped();
    testNoReflectionToSender();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
