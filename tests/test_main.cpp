// EDF6DirectNet tests. Build with build.ps1 and run build\edf6_directnet_tests.exe [path-to-EDF.dll].
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "../src/auth.h"
#include "../src/config.h"
#include "../src/direct_net.h"
#include "../src/hold.h"
#include "../src/iat.h"
#include "../src/netif.h"
#include "../src/reliable.h"
#include "../src/traffic.h"
#include "../src/updater.h"
#include "../src/upnp.h"
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
    m.hello.session = 12;
    m.hello.puid = kA;
    m.hello.cookie.fill(3);
    m.hello.publicKey.fill(4);
    m.hello.signature.fill(5);
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Challenge;
    m.challenge.clientNonce = 77;
    m.challenge.cookie.fill(6);
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
    // A and B published this process's identity, which every test client proves by default.
    o.memberIds = {{kA, dn::processIdentity()->commitment()}, {kB, dn::processIdentity()->commitment()}};
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
std::optional<dn::Message> receiveFrom(RawPeer& peer, dn::MsgType type, int ms, const std::string& key = "") {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        uint8_t buf[2048];
        int got = recv(peer.s, reinterpret_cast<char*>(buf), sizeof(buf), 0);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        auto m = dn::decode(buf, static_cast<size_t>(got), key, nullptr);
        if (m && m->type == type) return m;
    }
    return std::nullopt;
}

// A hand-driven client: says hello as `puid`, proves it with `identity` once the host sent a cookie,
// and keeps the link epoch the host welcomed it with.
struct RawClient {
    RawPeer peer;
    std::shared_ptr<const dn::Identity> identity = dn::processIdentity();
    std::string key;
    uint16_t port = 0;
    std::string puid;
    uint32_t nonce = 0;
    uint32_t epoch = 0;
    dn::Message lastHello;  // the last proven hello sent

    // A hello of session `session`; signed when `cookie` is given.
    dn::Message hello(uint64_t session, const dn::Cookie* cookie) const {
        dn::Message h;
        h.type = dn::MsgType::Hello;
        h.hello.nonce = nonce;
        h.hello.session = session;
        h.hello.puid = puid;
        if (cookie) {
            h.hello.cookie = *cookie;
            h.hello.publicKey = identity->publicKey();
            h.hello.signature = identity->sign(*dn::helloDigest(h.hello)).value();
        }
        return h;
    }
    bool welcomed(const dn::Message& h, int ms) {
        sendTo(peer, port, h, key);
        auto w = receiveFrom(peer, dn::MsgType::Welcome, ms, key);
        if (!w || w->welcome.clientNonce != h.hello.nonce) return false;
        epoch = dn::linkEpoch(h.hello.nonce, w->welcome.hostNonce);
        return true;
    }
    bool connect(uint16_t hostPort, const std::string& id, uint32_t sessionNonce, int attempts = 5) {
        port = hostPort;
        puid = id;
        nonce = sessionNonce;
        uint64_t session = identity->nextSession();
        for (int attempt = 0; attempt < attempts; ++attempt) {
            sendTo(peer, port, hello(session, nullptr), key);
            auto c = receiveFrom(peer, dn::MsgType::Challenge, 500, key);
            if (!c || c->challenge.clientNonce != nonce) continue;
            lastHello = hello(session, &c->challenge.cookie);
            if (welcomed(lastHello, 500)) return true;
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
        sendTo(peer, port, m, key);
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

void testIdentityCrypto() {
    printf("identity: commitments, signatures and session numbers\n");
    auto a = dn::Identity::generate(), b = dn::Identity::generate();
    CHECK(a && b && dn::processIdentity() && dn::processIdentity() == dn::processIdentity());
    if (!a || !b) return;
    CHECK(a->commitment().size() == 32 && a->commitment() != b->commitment());
    CHECK(a->commitment() == dn::identityCommitment(a->publicKey()));
    dn::HelloMsg h;
    h.nonce = 5;
    h.session = 1;
    h.puid = kA;
    h.cookie.fill(9);
    auto digest = dn::helloDigest(h);
    CHECK(digest.has_value());
    auto sig = a->sign(*digest);
    CHECK(sig && dn::verifySignature(a->publicKey(), *digest, *sig));
    CHECK(!dn::verifySignature(b->publicKey(), *digest, *sig));  // someone else's key
    for (auto change : {0, 1, 2, 3}) {  // every signed field matters
        dn::HelloMsg t = h;
        if (change == 0) t.nonce ^= 1;
        if (change == 1) ++t.session;
        if (change == 2) t.puid = kB;
        if (change == 3) t.cookie[0] ^= 1;
        CHECK(!dn::verifySignature(a->publicKey(), *dn::helloDigest(t), *sig));
    }
    dn::PublicKey offCurve{};
    offCurve.fill(1);
    CHECK(!dn::verifySignature(offCurve, *digest, *sig));
    uint64_t s1 = a->nextSession(), s2 = a->nextSession();
    CHECK(s2 > s1);
}

void testHelloNeedsCookieAndIdentity() {
    printf("direct: a hello proves its EOS id: a cookie for its address, signed by the id's published identity\n");
    for (const std::string key : {"", "roomkey"}) {  // a shared Key lets nobody claim someone else
        auto idA = dn::Identity::generate(), idB = dn::Identity::generate();
        dn::DirectOptions ho = hostOptions(0, 0, key);
        ho.memberIds = {{kA, idA->commitment()}, {kB, idB->commitment()}};  // kC: a player without the plugin
        dn::DirectNet host;
        CHECK(host.start(ho));
        host.setLocalUser(kHost);
        const std::string kC = "0002dddddddddddddddddddddddddddd";

        RawClient plain;  // no cookie: answered with a challenge only
        plain.key = key;
        plain.port = host.boundPort();
        plain.puid = kA;
        plain.nonce = 0x10;
        sendTo(plain.peer, plain.port, plain.hello(1, nullptr), key);
        auto c = receiveFrom(plain.peer, dn::MsgType::Challenge, 500, key);
        CHECK(c && c->challenge.clientNonce == 0x10);
        CHECK(receiveFrom(plain.peer, dn::MsgType::Welcome, 200, key) == std::nullopt);
        dn::Cookie forged{};
        forged.fill(0x42);
        CHECK(!plain.welcomed(plain.hello(1, &forged), 300));  // a made-up cookie: another challenge

        RawClient memberB;  // B, a room member, claims to be A: right cookie, wrong identity
        memberB.identity = idB;
        memberB.key = key;
        CHECK(!memberB.connect(host.boundPort(), kA, 0x20, 1));
        RawClient vanilla;  // someone claims kC, who published nothing
        vanilla.identity = idB;
        vanilla.key = key;
        CHECK(!vanilla.connect(host.boundPort(), kC, 0x30, 1));
        CHECK(!host.canRoute(kA) && !host.canRoute(kC) && host.directMembers().size() == 1);

        RawClient realA;
        realA.identity = idA;
        realA.key = key;
        CHECK(realA.connect(host.boundPort(), kA, 0x40));
        CHECK(host.canRoute(kA));
        auto p = payloadFor(1);
        CHECK(host.send(kA, "EDF6", 1, 0, p.data(), p.size()));
        auto d = receiveFrom(realA.peer, dn::MsgType::Data, 1000, key);
        CHECK(d && d->epoch == realA.epoch && d->data.payload == p);
        // B still cannot take A's place, now that A is connected.
        CHECK(!memberB.connect(host.boundPort(), kA, 0x21, 1));
        CHECK(memberB.peer.count(dn::MsgType::Data, 300) == 0);
    }
}

void testIdentityPublishedLate() {
    printf("direct: a client connects soon after its identity reaches the host's copy of the room\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    ho.memberIds.clear();  // the room info has not arrived yet
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(!waitFor([&] { return a.canRoute(kHost); }, 2500));
    host.setMemberIdentities({{kA, dn::processIdentity()->commitment()}});
    auto t0 = std::chrono::steady_clock::now();
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 3000));
    printf("  connected %lld ms after the identity arrived\n",
           static_cast<long long>(
               std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    CHECK(streamInOrder(a, kHost, host, kA, 100));
}

void testJunkHellosDoNotBlockPlayers() {
    printf("direct: floods of hellos from many addresses keep nothing and do not keep a player out\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    for (int i = 0; i < 40; ++i) {  // even ids that are room members: without their key nothing sticks
        char id[40];
        snprintf(id, sizeof(id), "0002eeeeeeeeeeeeeeeeeeeeeeee%04d", i);
        ho.memberIds[id] = dn::Identity::generate()->commitment();
    }
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    std::vector<RawClient> junk(40);
    std::atomic<bool> flooding{true};
    std::thread flood([&] {
        dn::Cookie made{};
        for (uint32_t round = 0; flooding; ++round) {
            for (int i = 0; i < 40; ++i) {
                char id[40];
                snprintf(id, sizeof(id), "0002eeeeeeeeeeeeeeeeeeeeeeee%04d", i);
                junk[i].port = host.boundPort();
                junk[i].puid = id;
                junk[i].nonce = round * 64 + i + 1;
                made.fill(static_cast<uint8_t>(round));
                sendTo(junk[i].peer, junk[i].port, junk[i].hello(round, (round & 1) ? &made : nullptr));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    });
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    flooding = false;
    flood.join();
    CHECK(host.directMembers() == std::vector<std::string>({kHost, kA}));
    CHECK(host.statusLine().rfind("HOST clients=1 ", 0) == 0);
}

void testSpoofedHelloFromVictimAddress() {
    printf("direct: a hello for a new id sent from a connected player's address does not end its link\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 5000));
    // Same source address and port as A: what an attacker forging A's address sends.
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_port = htons(a.boundPort());
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
        printf("  SKIP: cannot share A's port (error %d)\n", WSAGetLastError());
        closesocket(s);
        return;
    }
    {
        RawClient spoof;  // while it exists it also takes the datagrams sent to A
        closesocket(spoof.peer.s);
        spoof.peer.s = s;
        spoof.port = host.boundPort();
        spoof.puid = kB;  // a fresh id: the old host dropped A's link for it
        spoof.nonce = 0x77;
        dn::Cookie made{};
        for (int i = 0; i < 5; ++i) {
            sendTo(spoof.peer, spoof.port, spoof.hello(1, nullptr));
            sendTo(spoof.peer, spoof.port, spoof.hello(1, &made));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    CHECK(host.canRoute(kA) && !host.canRoute(kB));
    CHECK(streamInOrder(host, kA, a, kHost, 100));
}

void testReplayedHelloIgnored() {
    printf("direct: a captured hello of an earlier session does not replace the live one\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    RawClient c;
    CHECK(c.connect(host.boundPort(), kA, 0x100));
    dn::Message first = c.lastHello;
    CHECK(c.connect(host.boundPort(), kA, 0x200));  // a newer session replaces the first
    uint32_t live = c.epoch;
    CHECK(!c.welcomed(first, 500));  // same address, cookie still valid: still refused
    RawClient elsewhere;             // from another address the old cookie is worthless anyway
    elsewhere.port = host.boundPort();
    CHECK(!elsewhere.welcomed(first, 300));
    CHECK(c.welcomed(c.lastHello, 500) && c.epoch == live);  // the live session's own hello: just welcomed again
    auto p = payloadFor(2);
    CHECK(host.send(kA, "EDF6", 1, 0, p.data(), p.size()));
    auto d = receiveFrom(c.peer, dn::MsgType::Data, 1000);
    CHECK(d && d->epoch == live);
}

void testOlderPluginStaysOnEos() {
    printf("direct: peers speaking the 0.3.6 protocol are ignored both ways, nothing breaks\n");
    // A 0.3.6 hello: header with protocol 2, nonce, id.
    std::vector<uint8_t> old = {0x45, 0x44, 0x4E, 0x31, 1, 0, 2, 0, 7, 0, 0, 0, static_cast<uint8_t>(kA.size())};
    old.insert(old.end(), kA.begin(), kA.end());
    dn::DecodeError err;
    CHECK(!dn::decode(old.data(), old.size(), "", &err) && err == dn::DecodeError::BadProtocol);
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    RawPeer oldClient;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(host.boundPort());
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 3; ++i)
        sendto(oldClient.s, reinterpret_cast<const char*>(old.data()), static_cast<int>(old.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
    int replies = 0;
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (std::chrono::steady_clock::now() < end) {
        uint8_t buf[2048];
        if (recv(oldClient.s, reinterpret_cast<char*>(buf), sizeof(buf), 0) > 0) ++replies;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(replies == 0 && !host.canRoute(kA));

    // A 0.3.6 host: it cannot read our hello, whatever it sends back is not a protocol-3 answer.
    RawPeer oldHost;
    sockaddr_in bound{};
    int len = sizeof(bound);
    getsockname(oldHost.s, reinterpret_cast<sockaddr*>(&bound), &len);
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(ntohs(bound.sin_port)), 0)));
    a.setLocalUser(kA);
    int hellos = 0;
    end = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
    while (std::chrono::steady_clock::now() < end) {
        uint8_t buf[2048];
        sockaddr_storage from{};
        int fromLen = sizeof(from);
        int got = recvfrom(oldHost.s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                           &fromLen);
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        ++hellos;
        // 0.3.6 answers a hello it understood with a protocol-2 Welcome; send one anyway.
        std::vector<uint8_t> welcome = {0x45, 0x44, 0x4E, 0x31, 2, 0, 2, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0, 0};
        sendto(oldHost.s, reinterpret_cast<const char*>(welcome.data()), static_cast<int>(welcome.size()), 0,
               reinterpret_cast<sockaddr*>(&from), fromLen);
    }
    CHECK(hellos >= 2 && !a.canRoute(kHost));
}

void testRetiredInstanceIsFreed() {
    printf("direct: an instance swapped out while another thread uses it is stopped, then freed by its last user\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    std::string addr = "127.0.0.1:" + std::to_string(host.boundPort());
    std::atomic<std::shared_ptr<dn::DirectNet>> current;  // what the EOS hooks read (AutoJoin's pattern)
    std::weak_ptr<dn::DirectNet> first;
    std::atomic<bool> running{true};
    std::atomic<uint64_t> calls{0};
    std::thread game([&] {  // the game thread sending through whatever instance is current
        auto p = payloadFor(1);
        while (running) {
            if (std::shared_ptr<dn::DirectNet> net = current.load()) {
                net->send(kHost, "EDF6", 1, 0, p.data(), p.size());
                net->canRoute(kHost);
                ++calls;
            }
            std::this_thread::yield();
        }
    });
    for (int attempt = 0; attempt < 5; ++attempt) {
        auto net = std::make_shared<dn::DirectNet>();
        CHECK(net->start(joinOptions(addr, 0)));
        net->setLocalUser(kA);
        if (attempt == 0) first = net;
        std::shared_ptr<dn::DirectNet> old = current.exchange(net);
        if (old) std::thread([old = std::move(old)] { old->stop(); }).detach();  // retire()
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    CHECK(waitFor([&] { return first.expired(); }, 3000));  // no leak: the first instance is gone
    CHECK(waitFor([&] { return current.load()->canRoute(kHost); }, 5000));
    running = false;
    game.join();
    printf("  %llu calls through swapped instances\n", static_cast<unsigned long long>(calls.load()));
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
    m.record(kB, 100, hx, 1002);  // the same data to the same player again: not a copy, a repeat
    m.record(kA, 100, hy, 1500);
    m.record(kA, 400, hy, 2100);  // next second; another size is other data
    dn::TrafficSummary s = m.take();
    CHECK(s.bytes == 800 && s.packets == 5 && s.peers == 2 && s.copyBytes == 100);
    CHECK(s.busiestSecondBytes == 400 && s.largestPacket == 400 && s.repeatPackets == 1);
    s = m.take();
    CHECK(s.bytes == 0 && s.packets == 0 && s.peers == 0 && s.busiestSecondBytes == 0 && s.repeatPackets == 0);

    // Repeats count within the window only.
    m.record(kA, 100, hx, 10000);
    m.record(kA, 100, hx, 10000 + dn::TrafficMeter::kRepeatWindowMs);      // just inside: a repeat
    m.record(kA, 100, hy, 20000);
    m.record(kA, 100, hy, 20001 + dn::TrafficMeter::kRepeatWindowMs);      // just outside: not
    CHECK(m.take().repeatPackets == 1);
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
        {"name":"EDF6DirectNet-v0.3.6.zip","browser_download_url":"https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.6/EDF6DirectNet-v0.3.6.zip"},
        {"name": "EDF6DirectNet.dll", "browser_download_url": "https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.6/EDF6DirectNet.dll"},
        {"name":"EDF6DirectNet.dll.sha256","browser_download_url":"https://evil.example/EDF6DirectNet.dll.sha256"}]})";
    CHECK(dn::jsonString(json, "tag_name") == "v0.3.6");
    CHECK(dn::assetUrl(json, "EDF6DirectNet.dll") == "https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.6/EDF6DirectNet.dll");
    CHECK(dn::assetUrl(json, "EDF6DirectNet.dll.sha256").empty());  // not GitHub: refused
    CHECK(dn::assetUrl(json, "missing.dll").empty());
    // The shape GitHub actually returns: release name/author before the assets, label and uploader inside
    // each asset after its name, a body with quotes after them.
    std::string real = R"({"url":"https://api.github.com/repos/o/r/releases/1","assets_url":"https://api.github.com/x",
        "tag_name":"v0.3.7","name":"EDF6DirectNet.dll","author":{"login":"o","name":"EDF6DirectNet.dll"},"assets":[
        {"url":"https://api.github.com/a/1","id":1,"name":"EDF6DirectNet.dll.sha256","label":null,
         "uploader":{"login":"github-actions[bot]","id":2},"content_type":"text/plain","size":84,
         "digest":"sha256:00","browser_download_url":"https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll.sha256"},
        {"url":"https://api.github.com/a/2","id":3,"name":"EDF6DirectNet.dll","label":"",
         "uploader":{"login":"github-actions[bot]","id":2},"content_type":"application/x-msdownload","size":400000,
         "digest":"sha256:11","browser_download_url":"https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll"}],
        "body":"see \"name\": \"EDF6DirectNet.dll\" https://evil.example/x"})";
    CHECK(dn::parseVersion(dn::jsonString(real, "tag_name")).patch == 7);
    CHECK(dn::assetUrl(real, "EDF6DirectNet.dll") == "https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll");
    CHECK(dn::assetUrl(real, "EDF6DirectNet.dll.sha256") ==
          "https://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll.sha256");

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
    dn::removeUpdateLeftovers(installed);  // kept for rollback until the new version is healthy
    CHECK(GetFileAttributesW((installed + L".old").c_str()) != INVALID_FILE_ATTRIBUTES);
    if (loaded) FreeLibrary(loaded);
    dn::removeUpdateLeftovers(installed);
    CHECK(GetFileAttributesW((installed + L".old").c_str()) != INVALID_FILE_ATTRIBUTES);
    DeleteFileW((installed + L".old").c_str());
    CHECK(GetFileAttributesW((installed + L".new" + std::to_wstring(GetCurrentProcessId())).c_str()) ==
          INVALID_FILE_ATTRIBUTES);
    // A game that quit between writing its download and renaming it leaves installed.new<pid>.
    std::wstring stale = installed + L".new4242", other = installed + L".newer";
    CHECK(CopyFileW(built.c_str(), stale.c_str(), FALSE) && CopyFileW(built.c_str(), other.c_str(), FALSE));
    dn::removeUpdateLeftovers(installed);
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

void testRetransmitTimeout() {
    printf("reliable: the retransmission timeout stays above a steady RTT; duplicates are counted\n");
    // A steady 62 ms link (what real games showed): every sample the same, so RTTVAR goes to zero. The
    // timeout must not then equal the RTT, which resent nearly every packet.
    dn::ReliableSender tx;
    uint64_t now = 1000;
    for (int i = 0; i < 64; ++i) {
        uint32_t seq = tx.nextSeq();
        tx.track(seq, std::vector<uint8_t>(20), now);
        dn::AckMsg ack;
        ack.cumulative = seq;
        now += 62;
        tx.onAck(ack, now);
    }
    CHECK(tx.srttMs() == 62 && tx.rtoMs() >= 150);
    // A slow link keeps SRTT plus a margin above the floor.
    dn::ReliableSender slow;
    now = 1000;
    for (int i = 0; i < 64; ++i) {
        uint32_t seq = slow.nextSeq();
        slow.track(seq, std::vector<uint8_t>(20), now);
        dn::AckMsg ack;
        ack.cumulative = seq;
        now += 300;
        slow.onAck(ack, now);
    }
    CHECK(slow.rtoMs() >= 305);

    dn::ReliableReceiver rx;
    std::vector<dn::DataMsg> ready;
    dn::DataMsg m;
    m.reliability = 1;
    m.seq = 1;
    rx.onData(m, ready);
    rx.onData(m, ready);  // resent after it had arrived
    m.seq = 3;
    rx.onData(m, ready);
    rx.onData(m, ready);  // resent while buffered out of order
    CHECK(ready.size() == 2 && rx.duplicates() == 2);
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

void testHostCandidateFilter() {
    printf("auto-connect: a room host cannot aim joiners at loopback, broadcast, multicast or link-local\n");
    using V = std::vector<std::string>;
    CHECK(dn::orderHostCandidates("127.0.0.1:1 0.0.0.0:1 224.0.0.1:1 255.255.255.255:1 169.254.1.1:1 [::1]:1 [::]:1 "
                                  "[fe80::1]:1 [ff02::1]:1 [::ffff:127.0.0.1]:1 [fe80::1%3]:1 127.1:1 2130706433:1 "
                                  "bad_name:1 1.2.3.4:0 1.2.3.4:99999 [2408::1")
              .empty());
    CHECK(dn::orderHostCandidates("[fd00::1]:5 192.168.1.5:27015 10.0.0.2:1 my.ddns.net:40000") ==
          V({"192.168.1.5:27015", "10.0.0.2:1", "my.ddns.net:40000", "[fd00::1]:5"}));  // LAN play is fine
    CHECK(dn::orderHostCandidates("1.1.1.1:1 1.1.1.2:1 127.0.0.1:1 1.1.1.3:1 [2408::5]:1 1.1.1.4:1 1.1.1.5:1") ==
          V({"1.1.1.1:1", "1.1.1.2:1", "1.1.1.3:1", "[2408::5]:1"}));  // at most 4, refused ones not counted
    CHECK(dn::orderHostCandidates("2408::5").size() == 1);

    // Names are checked again after DNS: "localhost" is a fine-looking name for a loopback address.
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    for (std::string target : {"127.0.0.1", "localhost"}) {
        dn::DirectOptions o = joinOptions(target + ":" + std::to_string(host.boundPort()), 0);
        o.advertisedHost = true;
        dn::DirectNet a;
        CHECK(a.start(o));
        a.setLocalUser(kA);
        CHECK(!waitFor([&] { return a.canRoute(kHost) || host.canRoute(kA); }, 1500));
    }
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

// A throwaway P-256 key pair made for these tests (only its public half is here) and .NET ECDsaCng
// signatures over manifests of testDll("0.3.7") and testDll("0.3.5"), made the way the release workflow
// signs.
const std::vector<uint8_t> kTestKey = {
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0x03, 0x95, 0xbe, 0x95, 0x54, 0x5d, 0xe1, 0xbc, 0xc8, 0x6c,
    0x11, 0xf9, 0x92, 0x0e, 0x76, 0xec, 0x91, 0x4a, 0x50, 0x28, 0xc3, 0x95, 0xad, 0xa6, 0x8e, 0xb7, 0x19, 0x75,
    0x44, 0x4e, 0x07, 0x0e, 0xba, 0x6b, 0xd2, 0x28, 0x98, 0x11, 0x69, 0xf0, 0x56, 0x4a, 0x8e, 0x9b, 0x42, 0xf9,
    0x47, 0xf7, 0x03, 0xd7, 0xda, 0xb3, 0x08, 0xc9, 0x0b, 0x01, 0xf9, 0x2a, 0x67, 0x17, 0x82, 0x4f, 0x6e, 0x90};
const char* const kTestSig037 =
    "EDF6DirectNet 0.3.7\n"
    "de617cd97f26103fdf974aaa4b78e49de03a76513ce472414a7e872ec9f51d0d\n"
    "0eeed29a0bd1595119798327f0a5f3bb8dbdb09a101734747355139df0456cba"
    "58f065c04b40800adac57da10a8ff836caa19f1e187b91ed94f083eedd073b10\n";
const char* const kTestSig035 =
    "EDF6DirectNet 0.3.5\n"
    "661665d34625dd6cf62577435eae547b2b30d73a85a74f01c7f2ef94b28e43f2\n"
    "82082f5551989fc3792d8f4fed80f3f44448c6abfa12d2532666e866c71acfab"
    "09dde9021d56ba265e40e3427747ba140d3012cec9620469a66fc700d2db8c4a\n";

// 4 KiB: "MZ", zeros, the version marker at 1000.
std::vector<uint8_t> testDll(const char* version) {
    std::vector<uint8_t> dll(4096, 0);
    dll[0] = 'M';
    dll[1] = 'Z';
    std::string marker = dn::versionMarker(dn::parseVersion(version));
    std::copy(marker.begin(), marker.end(), dll.begin() + 1000);
    return dll;
}

void testUpdateSigning() {
    printf("update: signed manifest, version binding, no downgrade\n");
    using dn::parseVersion;
    std::string why;
    std::vector<uint8_t> dll = testDll("0.3.7");
    dn::Version v037 = parseVersion("0.3.7"), v036 = parseVersion("0.3.6");
    CHECK(dn::sha256Hex(dll) == "de617cd97f26103fdf974aaa4b78e49de03a76513ce472414a7e872ec9f51d0d");
    dn::SignedManifest m;
    CHECK(dn::readSignedManifest(kTestSig037, kTestKey, &m, &why) && m.version == v037);
    CHECK(dn::verifyRelease(dll, kTestSig037, v037, v036, kTestKey, &why));
    // Production trusts only the embedded release key: the test key's signature does not count.
    CHECK(dn::releaseSigningKey().size() == 72 && dn::releaseSigningKey() != kTestKey);
    CHECK(!dn::verifyRelease(dll, kTestSig037, v037, v036, dn::releaseSigningKey(), &why));
    CHECK(why.find("signature is not valid") != std::string::npos);
    // The signature binds version and digest.
    std::string other = kTestSig037;
    other[16] = '8';  // "EDF6DirectNet 0.3.8"
    CHECK(!dn::readSignedManifest(other, kTestKey, &m, &why));
    other = kTestSig037;
    other[20] = other[20] == 'd' ? 'e' : 'd';  // first digest digit
    CHECK(!dn::readSignedManifest(other, kTestKey, &m, &why));
    other = kTestSig037;
    other[other.size() - 2] = other[other.size() - 2] == '0' ? '1' : '0';  // last signature digit
    CHECK(!dn::readSignedManifest(other, kTestKey, &m, &why));
    // A validly signed release is taken only as the release it says it is, and only forward.
    CHECK(!dn::verifyRelease(dll, kTestSig037, parseVersion("0.3.8"), v036, kTestKey, &why));  // tag 0.3.8, signed 0.3.7
    CHECK(!dn::verifyRelease(dll, kTestSig037, v037, v037, kTestKey, &why));                   // not newer
    CHECK(!dn::verifyRelease(testDll("0.3.5"), kTestSig035, parseVersion("0.3.5"), v036, kTestKey, &why));  // downgrade
    CHECK(why.find("not newer") != std::string::npos);
    CHECK(dn::verifyRelease(testDll("0.3.5"), kTestSig035, parseVersion("0.3.5"), parseVersion("0.3.4"), kTestKey, &why));
    // The DLL must be the signed one.
    std::vector<uint8_t> damaged = dll;
    damaged[3000] ^= 1;
    CHECK(!dn::verifyRelease(damaged, kTestSig037, v037, v036, kTestKey, &why));
    CHECK(!dn::verifyRelease(testDll("0.3.5"), kTestSig037, v037, v036, kTestKey, &why));
    // Strict format: nothing missing, added or respelled.
    std::string good = kTestSig037;
    for (const std::string& bad :
         {std::string(), good.substr(0, good.size() - 1), good + "\n", good + "x", "\n" + good,
          std::string("EDF6DirectNet 00.3.7") + good.substr(19), std::string("EDF6DirectNet  0.3.7") + good.substr(19),
          std::string("EDF6DirectNet v0.3.7") + good.substr(19), std::string("edf6directnet 0.3.7") + good.substr(19)}) {
        CHECK(!dn::readSignedManifest(bad, kTestKey, &m, &why));
    }
    std::string crlf;
    for (char c : good) crlf += c == '\n' ? std::string("\r\n") : std::string(1, c);
    CHECK(!dn::readSignedManifest(crlf, kTestKey, &m, &why));
    std::string upper = good;
    std::transform(upper.begin() + 20, upper.end(), upper.begin() + 20, [](char c) { return static_cast<char>(toupper(c)); });
    CHECK(!dn::readSignedManifest(upper, kTestKey, &m, &why));
    CHECK(!dn::readSignedManifest(good, std::vector<uint8_t>(72, 0), &m, &why));  // not a key
    CHECK(!dn::readSignedManifest(good, {}, &m, &why));

    // Asset URLs: this repository, this release's tag, this name; nothing else.
    auto release = [](const std::string& tag, const std::string& url) {
        return R"({"tag_name":")" + tag + R"(","assets":[{"name":"EDF6DirectNet.dll.sig","browser_download_url":")" +
               url + R"("}]})";
    };
    const std::string base = "https://github.com/hajisensai/edf-coop-stable/releases/download/";
    CHECK(dn::assetUrl(release("v0.3.7", base + "v0.3.7/EDF6DirectNet.dll.sig"), "EDF6DirectNet.dll.sig") ==
          base + "v0.3.7/EDF6DirectNet.dll.sig");
    for (const std::string& url :
         {std::string("https://github.com/other/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll.sig"),
          std::string("https://github.com/hajisensai/edf-coop-stable-fork/releases/download/v0.3.7/EDF6DirectNet.dll.sig"),
          base + "v0.3.6/EDF6DirectNet.dll.sig", base + "v0.3.7/../../../../x/y/releases/download/v0.3.7/EDF6DirectNet.dll.sig",
          base + "v0.3.7/EDF6DirectNet.dll.sig?x=1", std::string("http://github.com/hajisensai/edf-coop-stable/releases/download/v0.3.7/EDF6DirectNet.dll.sig"),
          base + "v0.3.7/EDF6DirectNet.dll"}) {
        CHECK(dn::assetUrl(release("v0.3.7", url), "EDF6DirectNet.dll.sig").empty());
    }
    CHECK(dn::assetUrl(release("v0.3.7/../x", base + "v0.3.7/../x/EDF6DirectNet.dll.sig"), "EDF6DirectNet.dll.sig").empty());
}

bool fileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::string fileText(const std::wstring& path) {
    std::string text;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return text;
    char buf[4096];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    fclose(f);
    return text;
}

void putFile(const std::wstring& path, const std::string& text) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

std::wstring freshDir(const wchar_t* name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + name + L"\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((dir + L"*").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + found.cFileName).c_str());
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    return dir;
}

void testUpdateRollback() {
    printf("update: rollback state machine (trial, healthy, rolled back, bad version)\n");
    std::wstring dir = freshDir(L"edf6dn_rollback_test");
    std::wstring dll = dir + L"EDF6DirectNet.dll", old = dll + L".old", trial = dll + L".trial", bad = dll + L".bad";
    auto install = [&](const char* version) { putFile(dll, "MZ EDF6DN_VERSION=" + std::string(version)); };
    std::string why;

    // The version a DLL says it is, past the marker prefix its own updater code also contains.
    const char marked[] = "MZ EDF6DN_VERSION=\0%s\0 EDF6DN_VERSION=1.2\0 EDF6DN_VERSION=0.3.6\0";
    putFile(dll, std::string(marked, sizeof(marked) - 1));
    CHECK(dn::fileVersion(dll) == "0.3.6");
    const char unmarked[] = "MZ EDF6DN_VERSION=\0 no marker\0";
    putFile(dll, std::string(unmarked, sizeof(unmarked) - 1));
    CHECK(dn::fileVersion(dll) == "?");

    // Installed by hand: nothing to prove, a stray trial is dropped.
    install("0.3.7");
    putFile(trial, "0.3.7 0\n");
    CHECK(dn::beginRun(dll, "0.3.7") == dn::RunState::Normal && !fileExists(trial));

    // 0.3.7 updates itself to 0.3.8: 0.3.7 is kept as .old.
    std::vector<uint8_t> v038 = testDll("0.3.8");
    CHECK(dn::installOver(dll, v038, &why));
    CHECK(fileText(old).find("EDF6DN_VERSION=0.3.7") != std::string::npos && fileText(dll).size() == v038.size());
    // The first start of 0.3.8 is a trial; healthy after a while: the trial and .old go.
    CHECK(dn::beginRun(dll, "0.3.8") == dn::RunState::Trial);
    CHECK(fileText(trial) == "0.3.8 " + std::to_string(GetCurrentProcessId()) + "\n");
    dn::confirmHealthy(dll, "0.3.7");  // someone else's trial: nothing happens
    CHECK(fileExists(trial) && fileExists(old));
    dn::confirmHealthy(dll, "0.3.8");
    CHECK(!fileExists(trial) && !fileExists(old));
    CHECK(dn::beginRun(dll, "0.3.8") == dn::RunState::Normal);

    // 0.3.8 updates to 0.3.9. A game that ends normally before 0.3.9 is proven is no failure: the trial
    // goes on at the next start.
    std::vector<uint8_t> v039 = testDll("0.3.9");
    CHECK(dn::installOver(dll, v039, &why));
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::Trial);
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::Trial);  // the same game asking again: still its trial
    CHECK(!dn::noteCleanExit(dll, "0.3.8"));                    // not the version on trial
    CHECK(dn::noteCleanExit(dll, "0.3.9") && fileText(trial) == "0.3.9 0\n");
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::Trial && fileExists(old));
    CHECK(fileText(trial) == "0.3.9 " + std::to_string(GetCurrentProcessId()) + "\n");
    // Then a game running it dies before it is healthy (no clean exit): pid 4 is System, never a game.
    putFile(trial, "0.3.9 4\n");
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::RolledBack);
    CHECK(fileText(dll).find("EDF6DN_VERSION=0.3.8") != std::string::npos);  // 0.3.8 runs from the next start
    CHECK(fileText(dll + L".rolledback").find("EDF6DN_VERSION=0.3.9") != std::string::npos);
    CHECK(!fileExists(old) && !fileExists(trial));
    CHECK(dn::badVersion(dll) == dn::parseVersion("0.3.9"));
    // The next start is 0.3.8 again, normal; the moved-aside DLL is cleaned up.
    CHECK(dn::beginRun(dll, "0.3.8") == dn::RunState::Normal && !fileExists(dll + L".rolledback"));

    // An older updater installs 0.3.9 again anyway: rolled back at once, without another trial.
    CHECK(dn::installOver(dll, v039, &why));
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::RolledBack);
    CHECK(fileText(dll).find("EDF6DN_VERSION=0.3.8") != std::string::npos);

    // Another game running the same trial right now (this process) is not a failed run.
    CHECK(dn::installOver(dll, testDll("0.4.0"), &why));
    putFile(trial, "0.4.0 " + std::to_string(GetCurrentProcessId()) + "\n");
    CHECK(dn::beginRun(dll, "0.4.0") == dn::RunState::Trial && fileExists(old));

    // A version on trial that installs the next one gives up its own trial: its health check must not
    // delete the new version's rollback target, and that target stays the proven 0.3.8, not unproven 0.4.0.
    CHECK(dn::installOver(dll, testDll("0.4.1"), &why));
    CHECK(!fileExists(trial) && fileText(old).find("EDF6DN_VERSION=0.3.8") != std::string::npos);
    CHECK(fileText(dll + L".rolledback").find("EDF6DN_VERSION=0.4.0") != std::string::npos);
    dn::confirmHealthy(dll, "0.4.0");
    CHECK(fileExists(old));

    // Garbage in the state files is harmless.
    putFile(trial, "garbage");
    putFile(bad, "\xff\xfe");
    CHECK(dn::beginRun(dll, "0.4.1") == dn::RunState::Trial && dn::badVersion(dll).valid() == false);

    for (const wchar_t* f : {L"", L".old", L".trial", L".bad", L".rolledback"}) DeleteFileW((dll + f).c_str());
    RemoveDirectoryW(dir.c_str());
}

void testSwapKeepsDllLoadable() {
    printf("update: the plugin file never goes missing while it is replaced (loaded image)\n");
    std::wstring dir = freshDir(L"edf6dn_swap_test");
    std::wstring dll = dir + L"EDF6DirectNet.dll";
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring built = std::wstring(self).substr(0, std::wstring(self).find_last_of(L'\\') + 1) + L"EDF6DirectNet.dll";
    CHECK(CopyFileW(built.c_str(), dll.c_str(), FALSE));
    HMODULE loaded = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(loaded != nullptr);
    std::atomic<bool> done{false};
    std::atomic<int> missing{0}, looks{0};
    std::thread watcher([&] {
        while (!done) {
            if (!fileExists(dll)) ++missing;
            ++looks;
        }
    });
    std::string why;
    // The running plugin is replaced: its image stays mapped under the second name .old.
    bool allOk = dn::installOver(dll, testDll("0.3.8"), &why);
    CHECK(allOk && fileExists(dll + L".old"));
    CHECK(!DeleteFileW((dll + L".old").c_str()));  // still the loaded image
    if (loaded) FreeLibrary(loaded);
    // A watcher opening the file at the wrong moment must not make the replacement fail either.
    for (int i = 0; i < 40; ++i) allOk = dn::installOver(dll, testDll(i % 2 ? "0.3.9" : "0.3.8"), &why) && allOk;
    // A rollback also replaces the running (loaded) version.
    CHECK(CopyFileW(built.c_str(), dll.c_str(), FALSE));
    loaded = LoadLibraryExW(dll.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    CHECK(loaded != nullptr);
    putFile(dll + L".old", "MZ EDF6DN_VERSION=0.3.9");
    putFile(dll + L".trial", "0.9.9 4\n");  // its game (pid 4 is System, never a game) died on trial
    bool rolledBack = dn::beginRun(dll, "0.9.9") == dn::RunState::RolledBack;
    done = true;
    watcher.join();
    CHECK(allOk && rolledBack);
    CHECK(missing == 0 && looks > 0);
    CHECK(fileText(dll) == "MZ EDF6DN_VERSION=0.3.9");
    if (loaded) FreeLibrary(loaded);
    DeleteFileW((dll + L".rolledback").c_str());
    DeleteFileW((dll + L".bad").c_str());
    // A download that could not be put in place leaves the installed file as it was.
    CHECK(CopyFileW(built.c_str(), (dll + L".old").c_str(), FALSE));
    HANDLE lock = CreateFileW((dll + L".old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(!dn::installOver(dll, testDll("0.4.0"), &why));  // the old backup cannot be removed
    CHECK(fileText(dll) == "MZ EDF6DN_VERSION=0.3.9");
    CHECK(!fileExists(dll + L".new" + std::to_wstring(GetCurrentProcessId())));
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    for (const wchar_t* f : {L"", L".old", L".trial"}) DeleteFileW((dll + f).c_str());
    RemoveDirectoryW(dir.c_str());
}

void testUpnpAddresses() {
    printf("upnp: which WAN addresses are advertised, which mappings are ours\n");
    for (const char* ok : {"1.2.3.4", "8.8.8.8", "100.63.255.255", "100.128.0.1", "172.15.0.1", "172.32.0.1",
                           "169.253.1.1", "223.255.255.254", "198.17.0.1", "198.20.0.1"})
        CHECK(dn::isPublicIpv4(ok));
    for (const char* no : {"0.0.0.0", "0.1.2.3", "127.0.0.1", "127.255.255.254", "169.254.1.1", "10.0.0.1", "172.16.0.1",
                           "172.31.255.255", "192.168.1.1", "100.64.0.1", "100.127.255.255", "224.0.0.1", "239.1.1.1",
                           "240.0.0.1", "255.255.255.255", "192.0.0.8", "198.18.0.1", "198.19.255.1", "", "1.2.3",
                           "1.2.3.4.5", "1.2.3.256", "01.2.3.4", " 1.2.3.4", "1.2.3.4 ", "1.2.3.4x", "a.b.c.d", "::1"})
        CHECK(!dn::isPublicIpv4(no));
    CHECK(dn::isBehindNatIpv4("100.64.0.1") && dn::isBehindNatIpv4("192.168.0.1") && !dn::isBehindNatIpv4("0.0.0.0"));
    CHECK(!dn::isBehindNatIpv4("8.8.8.8") && !dn::isBehindNatIpv4("127.0.0.1"));

    std::vector<std::string> me = {"192.168.1.10", "10.0.0.5"};
    CHECK(dn::upnpMayReplace("192.168.1.10", me) && dn::upnpMayReplace("10.0.0.5", me));
    CHECK(!dn::upnpMayReplace("192.168.1.11", me) && !dn::upnpMayReplace("", me));
    // Another PC's mapping is never touched, even under our description.
    CHECK(!dn::upnpMayRemove("192.168.1.11", "EDF6DirectNet", me));
    CHECK(dn::upnpMayRemove("192.168.1.10", "EDF6DirectNet", me));
    CHECK(!dn::upnpMayRemove("192.168.1.10", "Some game server", me));
}

void testConfigParsing() {
    printf("config: UTF-8 with BOM, inline comments, strict numbers, AutoUpdate of old files\n");
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"edf6directnet_parse_test.ini";
    // What Notepad saves: UTF-8 with BOM, here straight before the first section (no comment line).
    putFile(path,
            "\xEF\xBB\xBF[DirectNet]\r\nMode=host ; \xE6\x88\xBF\xE4\xB8\xBB\r\nHostAddress=\xE4\xBE\x8B\xE3\x81\x88.jp:1\r\n"
            "Key=abc#def\r\nPublicAddress=\"1.2.3.4:40000\"\r\nListenPort=27020\t# mine\r\nAutoJoin=0;x\r\n"
            "[update]\r\nautoupdate = 1 ; on\r\n");
    dn::Config c = dn::loadConfig(path);
    CHECK(c.direct.mode == dn::Mode::Host && c.direct.listenPort == 27020);
    CHECK(c.direct.hostAddress == "\xE4\xBE\x8B\xE3\x81\x88.jp:1");
    CHECK(c.direct.key == "abc#def" && c.publicAddress == "1.2.3.4:40000");
    CHECK(c.autoJoin);  // "0;x" is not a number (no space before ';'): the default, warned
    CHECK(c.autoUpdate && c.warnings.size() == 1 && c.warnings[0].find("AutoJoin") != std::string::npos);

    // Numbers are numbers: anything else keeps the default and says so.
    putFile(path, "[DirectNet]\r\nListenPort=abc\r\nEnabled=yes\r\nLinkTimeoutMs=0x10\r\nUPnP=2\r\n"
                  "[EOS]\r\nFixedPort=70000\r\n[Resilience]\r\nGraceSeconds=-5\r\n[Update]\r\nAutoUpdate=0\r\n");
    c = dn::loadConfig(path);
    CHECK(c.direct.listenPort == 27015 && c.enabled && c.direct.linkTimeoutMs == 60000 && c.upnp);
    CHECK(c.eosFixedPort == 0 && c.graceMs == 30000 && !c.autoUpdate);
    CHECK(c.warnings.size() == 6);
    bool mentionsPort = false;
    for (const std::string& w : c.warnings) mentionsPort = mentionsPort || w.find("ListenPort=abc") != std::string::npos;
    CHECK(mentionsPort);

    // A settings file from before auto-update (no AutoUpdate line) keeps it on, as 0.3.6 did, and says how
    // to turn it off.
    putFile(path, "; old\r\n[DirectNet]\r\nMode=off\r\n");
    c = dn::loadConfig(path);
    CHECK(c.autoUpdate && c.warnings.size() == 1 && c.warnings[0].find("AutoUpdate=0") != std::string::npos);
    // A new default file has it on, with nothing to warn about.
    DeleteFileW(path.c_str());
    c = dn::loadConfig(path);
    CHECK(c.autoUpdate && c.warnings.empty());

    // UTF-16 (Notepad's "Unicode") and ANSI files work too.
    std::wstring wide = L"\xFEFF[DirectNet]\r\nMode=join\r\nHostAddress=\x4F8B.jp:2\r\n";
    putFile(path, std::string(reinterpret_cast<const char*>(wide.data()), wide.size() * sizeof(wchar_t)));
    c = dn::loadConfig(path);
    CHECK(c.direct.mode == dn::Mode::Join && c.direct.hostAddress == "\xE4\xBE\x8B.jp:2" && c.direct.listenPort == 0);
    putFile(path, "[DirectNet]\r\nMode=join\r\nKey=\xC4\xE3\r\n");  // not UTF-8: the ANSI code page
    c = dn::loadConfig(path);
    wchar_t ansi[4] = {};
    int n = MultiByteToWideChar(CP_ACP, 0, "\xC4\xE3", 2, ansi, 4);
    char expected[16] = {};
    WideCharToMultiByte(CP_UTF8, 0, ansi, n, expected, sizeof(expected), nullptr, nullptr);
    CHECK(c.direct.mode == dn::Mode::Join && c.direct.key == expected);
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
    testHostCandidateFilter();
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
    testUpdateSigning();
    testUpdateRollback();
    testSwapKeepsDllLoadable();
    testUpnpAddresses();
    testConfigParsing();
    testRosterDropsQuietMember();
    testLinksFollowTheRoom();
    testHostRestart();
    testStalledLinkSurvives();
    testReplyFromOtherAddress();
    testHelloCannotHijackLiveLink();
    testHelloFloodIsBounded();
    testThreeNodesOverLoopback();
    testReliableBacklogLimit();
    testRetransmitTimeout();
    testUnacknowledgedLinkIsDropped();
    testNoReflectionToSender();
    testIdentityCrypto();
    testHelloNeedsCookieAndIdentity();
    testIdentityPublishedLate();
    testJunkHellosDoNotBlockPlayers();
    testSpoofedHelloFromVictimAddress();
    testReplayedHelloIgnored();
    testOlderPluginStaysOnEos();
    testRetiredInstanceIsFreed();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
