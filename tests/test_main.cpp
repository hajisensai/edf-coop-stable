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
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include "../src/auth.h"
#include "../src/config.h"
#include "../src/direct_net.h"
#include "../src/fake_lobby.h"
#include "../src/hold.h"
#include "../src/iat.h"
#include "../src/netclass.h"
#include "../src/netif.h"
#include "../src/reliable.h"
#include "../src/room_view.h"
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
    m.counter = 77;
    for (const std::string key : {"", "secret"}) {  // link messages carry the link tag, not the Key's
        auto dg = dn::encode(m, key);
        dn::DecodeError err;
        auto back = dn::decode(dg.data(), dg.size(), key, &err);
        CHECK(back && err == dn::DecodeError::None && back->counter == 77);
        CHECK(back && back->data.payload == m.data.payload && back->data.src == kA && back->data.dst == kB &&
              back->data.socketName == "EDF6" && back->data.channel == 3 && back->data.seq == 42);
    }
    // Handshake messages carry the shared Key's tag.
    dn::Message challenge;
    challenge.type = dn::MsgType::Challenge;
    challenge.challenge.clientNonce = 5;
    auto tagged = dn::encode(challenge, "secret");
    dn::DecodeError err;
    CHECK(dn::decode(tagged.data(), tagged.size(), "secret", &err).has_value());
    CHECK(!dn::decode(tagged.data(), tagged.size(), "other", &err) && err == dn::DecodeError::TagMismatch);
    CHECK(!dn::decode(tagged.data(), tagged.size(), "", &err) && err == dn::DecodeError::TagUnexpected);
    auto plain = dn::encode(challenge, "");
    CHECK(!dn::decode(plain.data(), plain.size(), "secret", &err) && err == dn::DecodeError::TagMissing);
    tagged[10] ^= 1;
    CHECK(!dn::decode(tagged.data(), tagged.size(), "secret", &err) && err == dn::DecodeError::TagMismatch);
    CHECK(!dn::decode(plain.data(), 10, "", &err) && err == dn::DecodeError::Truncated);

    // Link tags: made with one direction's key, checked with it; any changed byte or other key fails.
    dn::LinkKey k1{}, k2{};
    k1.fill(1);
    k2.fill(2);
    dn::LinkMac mac1(k1), mac1b(k1), mac2(k2);
    CHECK(mac1.valid() && mac2.valid());
    auto dg = dn::encode(m, "");
    CHECK(dn::sealLink(dg, 9, mac1));
    CHECK(dn::linkTagValid(dg.data(), dg.size(), mac1b) && !dn::linkTagValid(dg.data(), dg.size(), mac2));
    auto back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->counter == 9);
    bool everyByte = true;
    for (size_t i = 0; i < dg.size(); ++i) {
        auto bent = dg;
        bent[i] ^= 0x40;
        everyByte &= !dn::linkTagValid(bent.data(), bent.size(), mac1b);
    }
    CHECK(everyByte);
    auto resent = dg;  // a retransmission: same datagram, new counter, new tag
    CHECK(dn::sealLink(resent, 10, mac1) && resent != dg && dn::linkTagValid(resent.data(), resent.size(), mac1b));
    CHECK(!dn::linkTagValid(dg.data(), 20, mac1b));

    dn::Message ack;
    ack.type = dn::MsgType::Ack;
    ack.epoch = dn::linkEpoch(7, 9);
    ack.ack.cumulative = 100;
    ack.ack.ranges = {{102, 1}, {400, 3000}};
    auto adg = dn::encode(ack, "");
    auto aback = dn::decode(adg.data(), adg.size(), "", &err);
    CHECK(aback && aback->epoch == ack.epoch && aback->ack.cumulative == 100 && aback->ack.has(100) &&
          !aback->ack.has(101) && aback->ack.has(102) && !aback->ack.has(103) && aback->ack.has(400) &&
          aback->ack.has(3399) && !aback->ack.has(3400));
    ack.ack.ranges.assign(dn::kMaxAckRanges + 5, dn::AckRange{500, 1});  // the encoder keeps what fits
    adg = dn::encode(ack, "");
    aback = dn::decode(adg.data(), adg.size(), "", &err);
    CHECK(aback && aback->ack.ranges.size() == dn::kMaxAckRanges);
    dn::Message fwd;
    fwd.type = dn::MsgType::Forward;
    fwd.forward.floor = 77777;
    auto fdg = dn::encode(fwd, "");
    auto fback = dn::decode(fdg.data(), fdg.size(), "", &err);
    CHECK(fback && fback->type == dn::MsgType::Forward && fback->forward.floor == 77777);
    CHECK(dn::linkEpoch(7, 9) != dn::linkEpoch(9, 7) && dn::linkEpoch(7, 9) != dn::linkEpoch(8, 9));

    dn::Message w;
    w.type = dn::MsgType::Welcome;
    w.welcome.hostNonce = 5;
    w.welcome.clientNonce = 9;
    w.welcome.hostPuid = kHost;
    w.welcome.roster = {kHost, kA, kB};
    w.welcome.ecdh.fill(3);
    w.welcome.publicKey.fill(4);
    w.welcome.signature.fill(5);
    dg = dn::encode(w, "");
    back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->welcome.roster == w.welcome.roster && back->welcome.clientNonce == 9 &&
          back->welcome.ecdh == w.welcome.ecdh && back->welcome.publicKey == w.welcome.publicKey &&
          back->welcome.signature == w.welcome.signature);
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
    m.hello.ecdh.fill(8);
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
    m.welcome.ecdh.fill(9);
    m.welcome.publicKey.fill(10);
    m.welcome.signature.fill(11);
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Roster;
    m.counter = 3;
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
    m.ack.ranges = {{7, 2}, {12, 300}};
    all.push_back(m);
    m = {};
    m.type = dn::MsgType::Forward;
    m.forward.floor = 9;
    all.push_back(m);
    for (auto t : {dn::MsgType::Ping, dn::MsgType::Pong, dn::MsgType::Bye, dn::MsgType::Reset}) {
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

    // Data: header(8) epoch(4) counter(8) seq(4), then src as u8 length + bytes; the link tag last.
    const size_t tag = dn::kLinkTagBytes;
    dn::Message m;
    m.type = dn::MsgType::Data;
    m.data.seq = 1;
    m.data.src = std::string(dn::kMaxString, 'a');
    m.data.dst = kB;
    m.data.payload.assign(dn::kMaxPayload, 7);
    auto dg = dn::encode(m, "");
    CHECK(decodes(dg));  // the limits themselves are fine
    auto longId = dg;
    longId[24] = static_cast<uint8_t>(dn::kMaxString + 1);
    longId.insert(longId.begin() + 25, 'a');
    CHECK(!decodes(longId) && err == dn::DecodeError::Malformed);  // an id the encoder would have cut
    m.data.src = kA;
    m.data.payload.clear();
    dg = dn::encode(m, "");
    auto big = dg;  // payload length is the last field before the payload
    uint16_t over = static_cast<uint16_t>(dn::kMaxPayload + 1);
    memcpy(&big[big.size() - tag - 2], &over, 2);
    big.insert(big.end() - tag, dn::kMaxPayload + 1, 7);
    CHECK(!decodes(big) && err == dn::DecodeError::Malformed);

    dn::Message r;
    r.type = dn::MsgType::Roster;
    r.roster.roster.assign(32, kA);
    dg = dn::encode(r, "");
    CHECK(decodes(dg));
    dg[24] = 33;  // roster count, after epoch, counter and host nonce
    dg.push_back(static_cast<uint8_t>(kA.size()));
    dg.insert(dg.end(), kA.begin(), kA.end());
    CHECK(!decodes(dg) && err == dn::DecodeError::Malformed);

    dg = dn::encode(sampleMessages()[0], "");
    dg[4] = 99;  // a message type this version does not know
    CHECK(!decodes(dg) && err == dn::DecodeError::Malformed);

    // Ack: header(8) epoch(4) counter(8) cumulative(4), then the range count and ranges (first, count).
    dn::Message a;
    a.type = dn::MsgType::Ack;
    a.ack.ranges = {{5, 1}};
    dg = dn::encode(a, "");
    CHECK(decodes(dg));
    auto emptyRange = dg;
    emptyRange[29] = 0;  // the range's count
    emptyRange[30] = 0;
    CHECK(!decodes(emptyRange) && err == dn::DecodeError::Malformed);
    auto tooMany = dg;
    tooMany[24] = static_cast<uint8_t>(dn::kMaxAckRanges + 1);
    tooMany.insert(tooMany.end() - tag, dn::kMaxAckRanges * 6, 1);
    CHECK(!decodes(tooMany) && err == dn::DecodeError::Malformed);

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
        tx.poll(now, [&](const std::vector<uint8_t>& dg) {
            push(dg, false);
            return true;
        });
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
    o.retransmitBudget = std::make_shared<dn::RetransmitBudget>();  // its own machine, not this process's
    return o;
}

dn::DirectOptions joinOptions(const std::string& address, double drop, const std::string& key = "") {
    dn::DirectOptions o;
    o.mode = dn::Mode::Join;
    o.listenPort = 0;
    o.hostAddress = address;
    o.key = key;
    // The room is kHost's, which published this process's identity like every test host proves.
    o.roomOwner = kHost;
    o.roomOwnerIdentity = dn::processIdentity()->commitment();
    o.testDropRate = drop;
    o.linkTimeoutMs = 5000;
    o.retransmitBudget = std::make_shared<dn::RetransmitBudget>();
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

// Sends an already encoded datagram to 127.0.0.1:`port` from `peer`'s socket.
void sendRawTo(RawPeer& peer, uint16_t port, const std::vector<uint8_t>& dg) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(peer.s, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
           reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

// A hand-driven client: says hello as `puid`, proves it with `identity` once the host sent a cookie,
// and keeps the link epoch and keys the host welcomed it with.
struct RawClient {
    RawPeer peer;
    std::shared_ptr<const dn::Identity> identity = dn::processIdentity();
    std::unique_ptr<dn::EcdhKey> ecdh = dn::EcdhKey::generate();  // new for every connect()
    std::string key;
    uint16_t port = 0;
    std::string puid;
    uint32_t nonce = 0;
    uint32_t epoch = 0;
    dn::LinkMac tx;  // client -> host link key
    uint64_t counter = 0;
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
            h.hello.ecdh = ecdh->publicKey();
            h.hello.signature = identity->sign(*dn::helloDigest(h.hello)).value();
        }
        return h;
    }
    bool welcomed(const dn::Message& h, int ms) {
        sendTo(peer, port, h, key);
        auto w = receiveFrom(peer, dn::MsgType::Welcome, ms, key);
        if (!w || w->welcome.clientNonce != h.hello.nonce) return false;
        auto shared = ecdh->agree(w->welcome.ecdh);
        auto keys = shared ? dn::deriveLinkKeys(*shared, key, h.hello, w->welcome) : std::nullopt;
        if (!keys) return false;
        uint32_t welcomedEpoch = dn::linkEpoch(h.hello.nonce, w->welcome.hostNonce);
        if (welcomedEpoch != epoch) counter = 0;  // a new link; a repeated welcome keeps the old one going
        epoch = welcomedEpoch;
        tx = dn::LinkMac(keys->clientToHost);
        return true;
    }
    // Connects and confirms the link keys the way a real client does (any datagram with them), then
    // waits for the member list the host sends once the link is up.
    bool connect(uint16_t hostPort, const std::string& id, uint32_t sessionNonce, int attempts = 5) {
        port = hostPort;
        puid = id;
        nonce = sessionNonce;
        ecdh = dn::EcdhKey::generate();
        uint64_t session = identity->nextSession();
        for (int attempt = 0; attempt < attempts; ++attempt) {
            sendTo(peer, port, hello(session, nullptr), key);
            auto c = receiveFrom(peer, dn::MsgType::Challenge, 500, key);
            if (!c || c->challenge.clientNonce != nonce) continue;
            lastHello = hello(session, &c->challenge.cookie);
            if (!welcomed(lastHello, 500)) continue;
            dn::Message ping;
            ping.type = dn::MsgType::Ping;
            link(ping);
            auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
            while (std::chrono::steady_clock::now() < end) {
                auto r = receiveFrom(peer, dn::MsgType::Roster, 100, key);
                if (r && std::find(r->roster.roster.begin(), r->roster.roster.end(), puid) != r->roster.roster.end())
                    return true;
            }
            return false;
        }
        return false;
    }
    // Encodes and seals a link message with this client's link.
    std::vector<uint8_t> sealed(dn::Message m) {
        m.epoch = epoch;
        auto dg = dn::encode(m, key);
        dn::sealLink(dg, ++counter, tx);
        return dg;
    }
    void link(const dn::Message& m) { sendRawTo(peer, port, sealed(m)); }
    void data(uint32_t seq, const std::string& dst, const std::vector<uint8_t>& payload) {
        dn::Message m;
        m.type = dn::MsgType::Data;
        m.data.seq = seq;
        m.data.src = puid;
        m.data.dst = dst;
        m.data.socketName = "EDF6";
        m.data.channel = 1;
        m.data.reliability = seq ? 1 : 0;
        m.data.payload = payload;
        link(m);
    }
};

// Someone on the network path between a joiner and its host: the joiner dials the tap, which forwards
// every datagram both ways, keeps a copy of each and may alter or drop them, or send its own.
struct Tap {
    struct Copy {
        bool toHost;
        std::vector<uint8_t> dg;
    };
    RawPeer sock;
    uint16_t hostPort;
    std::mutex mu;
    sockaddr_storage joiner{};  // guarded by mu
    int joinerLen = 0;
    std::vector<Copy> copies;
    // Called on every forwarded datagram; false drops it.
    std::function<bool(std::vector<uint8_t>&, bool toHost)> alter;
    std::atomic<bool> running{true};
    std::thread thread;

    explicit Tap(uint16_t host) : hostPort(host), thread([this] { run(); }) {}
    ~Tap() {
        running = false;
        thread.join();
    }
    uint16_t port() {
        sockaddr_in bound{};
        int len = sizeof(bound);
        getsockname(sock.s, reinterpret_cast<sockaddr*>(&bound), &len);
        return ntohs(bound.sin_port);
    }
    void run() {
        while (running) {
            uint8_t buf[2048];
            sockaddr_storage from{};
            int fromLen = sizeof(from);
            int got = recvfrom(sock.s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from),
                               &fromLen);
            if (got <= 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            bool toHost = ntohs(reinterpret_cast<sockaddr_in*>(&from)->sin_port) != hostPort;
            std::vector<uint8_t> dg(buf, buf + got);
            std::lock_guard<std::mutex> lock(mu);
            if (toHost) {
                joiner = from;
                joinerLen = fromLen;
            }
            copies.push_back({toHost, dg});
            if (alter && !alter(dg, toHost)) continue;
            send(dg, toHost);
        }
    }
    // Sends `dg` on, as if it had come through the tap. Caller holds mu.
    void send(const std::vector<uint8_t>& dg, bool toHost) {
        if (toHost) {
            sendRawTo(sock, hostPort, dg);
        } else if (joinerLen) {
            sendto(sock.s, reinterpret_cast<const char*>(dg.data()), static_cast<int>(dg.size()), 0,
                   reinterpret_cast<const sockaddr*>(&joiner), joinerLen);
        }
    }
    void inject(const std::vector<uint8_t>& dg, bool toHost) {
        std::lock_guard<std::mutex> lock(mu);
        send(dg, toHost);
    }
    // The datagrams of `type` that went through in one direction and satisfy `match`, oldest first.
    std::vector<std::vector<uint8_t>> captured(dn::MsgType type, bool toHost,
                                               const std::function<bool(const dn::Message&)>& match = nullptr) {
        std::lock_guard<std::mutex> lock(mu);
        std::vector<std::vector<uint8_t>> out;
        for (const auto& c : copies) {
            auto m = dn::decode(c.dg.data(), c.dg.size(), "", nullptr);
            if (c.toHost == toHost && m && m->type == type && (!match || match(*m))) out.push_back(c.dg);
        }
        return out;
    }
    void setAlter(std::function<bool(std::vector<uint8_t>&, bool)> f) {
        std::lock_guard<std::mutex> lock(mu);
        alter = std::move(f);
    }
};

// Pops every packet `net` has for the game right now.
std::vector<dn::Delivered> popAll(dn::DirectNet& net) {
    std::vector<dn::Delivered> out;
    dn::Delivered d;
    while (net.pop(nullptr, 1170, d)) out.push_back(d);
    return out;
}

void testLinkPacketsAuthenticated() {
    printf("direct: without a Key, someone on the path can neither alter, inject nor replay link packets\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    ho.upgradeUnreliable = false;  // unreliable packets show a replay: nothing below would drop a copy
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    Tap tap(host.boundPort());
    dn::DirectOptions ao = joinOptions("127.0.0.1:" + std::to_string(tap.port()), 0);
    ao.upgradeUnreliable = false;
    dn::DirectNet a, b;  // A talks to the host through the tap, B directly
    CHECK(a.start(ao));
    CHECK(b.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    b.setLocalUser(kB);
    CHECK(waitFor([&] { return a.canRoute(kB) && b.canRoute(kA) && host.canRoute(kA) && host.canRoute(kB); }, 5000));
    // The real peers exchange data, also relayed by the host from B to A (re-tagged for A's link).
    CHECK(streamInOrder(a, kHost, host, kA, 200));
    CHECK(streamInOrder(host, kA, a, kHost, 200));
    CHECK(streamInOrder(b, kA, a, kB, 200));
    CHECK(a.rejectedPackets() == 0 && host.rejectedPackets() == 0);

    // One flipped byte in a game packet to A: dropped. Unreliable, it is simply lost; reliable, the
    // resend (a new datagram) delivers the original.
    auto p = payloadFor(501);
    int flips = 0;  // guarded by tap.mu
    tap.setAlter([&](std::vector<uint8_t>& dg, bool toHost) {
        auto m = dn::decode(dg.data(), dg.size(), "", nullptr);
        if (!toHost && m && m->type == dn::MsgType::Data && m->data.payload == p && flips++ == 0)
            dg[dg.size() - dn::kLinkTagBytes - 3] ^= 0x01;  // in the payload: only the first copy
        return true;
    });
    CHECK(host.send(kA, "EDF6", 1, 0, p.data(), p.size()));
    CHECK(!waitFor([&] { return !popAll(a).empty(); }, 500) && a.rejectedPackets() == 1);
    {
        std::lock_guard<std::mutex> lock(tap.mu);
        flips = 0;
        p = payloadFor(502);
    }
    CHECK(host.send(kA, "EDF6", 1, 2, p.data(), p.size()));
    std::vector<dn::Delivered> got;
    CHECK(waitFor([&] {
        auto more = popAll(a);
        got.insert(got.end(), more.begin(), more.end());
        return !got.empty();
    }, 2000));
    CHECK(got.size() == 1 && got[0].data == p && a.rejectedPackets() == 2);
    tap.setAlter(nullptr);

    // A packet from elsewhere with a valid-looking header (A's link epoch, a new counter) and no key:
    // dropped by A, and it does not move A's link at the host either.
    auto seen = tap.captured(dn::MsgType::Data, false);
    CHECK(!seen.empty());
    uint32_t epoch = seen.empty() ? 0 : dn::decode(seen[0].data(), seen[0].size(), "", nullptr)->epoch;
    RawPeer evil;
    dn::Message forged;
    forged.type = dn::MsgType::Data;
    forged.epoch = epoch;
    forged.counter = 1u << 20;
    forged.data.src = kHost;
    forged.data.dst = kA;
    forged.data.socketName = "EDF6";
    forged.data.channel = 1;
    forged.data.payload = payloadFor(503);
    auto forgedDg = dn::encode(forged, "");
    dn::LinkKey guess{};
    dn::LinkMac guessMac(guess);
    dn::sealLink(forgedDg, forged.counter, guessMac);
    uint64_t hostRejected = host.rejectedPackets();
    for (int i = 0; i < 5; ++i) {
        sendRawTo(evil, a.boundPort(), forgedDg);
        forged.data.src = kA;
        forged.data.dst = kHost;
        sendRawTo(evil, host.boundPort(), dn::encode(forged, ""));
    }
    CHECK(!waitFor([&] { return !popAll(a).empty() || !popAll(host).empty(); }, 500));
    CHECK(a.rejectedPackets() == 7 && host.rejectedPackets() == hostRejected + 5);
    CHECK(streamInOrder(host, kA, a, kHost, 50));  // the host still sends to the real A (via the tap)

    // A captured game packet played back: dropped, however often.
    p = payloadFor(504);
    CHECK(host.send(kA, "EDF6", 1, 0, p.data(), p.size()));
    CHECK(waitFor([&] { return popAll(a).size() == 1; }, 2000));
    auto copies = tap.captured(dn::MsgType::Data, false, [&](const dn::Message& m) { return m.data.payload == p; });
    CHECK(copies.size() == 1);
    uint64_t aRejected = a.rejectedPackets();
    for (int i = 0; i < 3 && !copies.empty(); ++i) tap.inject(copies[0], false);
    CHECK(!waitFor([&] { return !popAll(a).empty(); }, 500) && a.rejectedPackets() == aRejected + 3);

    // An unauthenticated Reset "from the host" does not end a working link.
    dn::Message reset;
    reset.type = dn::MsgType::Reset;
    tap.inject(dn::encode(reset, ""), false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(a.canRoute(kHost) && a.canRoute(kB));

    // A captured member list that still names B, played back after B left: A does not route to B again.
    auto withB = tap.captured(dn::MsgType::Roster, false, [&](const dn::Message& m) {
        return std::find(m.roster.roster.begin(), m.roster.roster.end(), kB) != m.roster.roster.end();
    });
    CHECK(!withB.empty());
    b.stop();
    CHECK(waitFor([&] { return !a.canRoute(kB) && !host.canRoute(kB); }, 3000));
    aRejected = a.rejectedPackets();
    tap.inject(withB.back(), false);
    tap.inject(withB.front(), false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!a.canRoute(kB) && a.rejectedPackets() == aRejected + 2);

    // A's goodbye, played back to the host after A reconnected: the new link stays.
    a.setActive(false);
    CHECK(waitFor([&] { return !host.canRoute(kA); }, 2000));
    auto byes = tap.captured(dn::MsgType::Bye, true);
    CHECK(byes.size() >= 1);
    a.setActive(true);
    CHECK(waitFor([&] { return host.canRoute(kA) && a.canRoute(kHost); }, 5000));
    hostRejected = host.rejectedPackets();
    for (const auto& bye : byes) tap.inject(bye, true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(host.canRoute(kA) && host.rejectedPackets() == hostRejected + byes.size());
    CHECK(streamInOrder(a, kHost, host, kA, 50));
}

void testReplayWindow() {
    printf("wire: link counters are taken once, within a 1024-packet reordering window\n");
    dn::ReplayWindow w;
    CHECK(!w.fresh(0) && w.fresh(1) && w.fresh(5000));
    for (uint64_t c = 1; c <= 5; ++c) w.mark(c);
    CHECK(!w.fresh(3) && w.fresh(6) && w.highest() == 5);
    w.mark(8);
    CHECK(w.fresh(7) && w.fresh(6) && !w.fresh(8));  // reordered ones still arrive, once
    w.mark(7);
    CHECK(!w.fresh(7) && w.fresh(6));
    for (uint64_t c = 9; c <= 1024; ++c) w.mark(c);
    w.mark(1030);  // moves the window: slots of 1025-1029 held 1-5, which are now out of it
    CHECK(w.fresh(1027) && !w.fresh(1030) && !w.fresh(3) && !w.fresh(6) && w.fresh(1029));
    CHECK(!w.fresh(1030 - 1024) && !w.fresh(1024) && w.fresh(1025));
    w.mark(100000);  // a jump beyond the window forgets everything older
    CHECK(!w.fresh(1027) && !w.fresh(100000 - 1024) && w.fresh(100000 - 1023) && !w.fresh(100000));
}

void testLinkTagSpeed() {
    printf("wire: tagging and checking 10000 full-size link packets\n");
    dn::LinkKey k{};
    k.fill(7);
    dn::LinkMac tx(k), rx(k);
    dn::Message m;
    m.type = dn::MsgType::Data;
    m.data.seq = 1;
    m.data.src = kA;
    m.data.dst = kB;
    m.data.socketName = "EDF6";
    m.data.payload.assign(1100, 9);
    auto dg = dn::encode(m, "");
    bool ok = true;
    auto t0 = std::chrono::steady_clock::now();
    for (uint64_t i = 1; i <= 10000; ++i) {
        ok &= dn::sealLink(dg, i, tx);
        ok &= dn::linkTagValid(dg.data(), dg.size(), rx);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    printf("  %zu-byte datagrams: %.1f ms for 10000 seal+check\n", dg.size(), ms);
    CHECK(ok && ms < 500.0);
}

void testWelcomeProvesTheRoomOwner() {
    printf("direct: a joiner accepts as host only the room owner, proven by the identity it published\n");
    auto impostor = dn::Identity::generate();
    std::string addr;
    {
        // Someone answering on the advertised address with a key the room owner never published.
        dn::DirectOptions ho = hostOptions(0, 0);
        ho.identity = impostor;
        dn::DirectNet host;
        CHECK(host.start(ho));
        host.setLocalUser(kHost);
        dn::DirectNet a;
        CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
        a.setLocalUser(kA);
        CHECK(!waitFor([&] { return a.canRoute(kHost); }, 2500));
        // The host proved A and answered, but A never confirmed: nothing is sent into that link.
        CHECK(!host.canRoute(kA) && host.directMembers().size() == 1);
        CHECK(a.statusLine().find("host answered") != std::string::npos);
        // Had the room owner published that key, it would be the host: the same instance connects.
        a.setRoomOwner(kHost, impostor->commitment());
        CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 3000));
        CHECK(streamInOrder(host, kA, a, kHost, 50));
    }
    {
        // The owner's identity key is public: a welcome carrying it but not signed with it is refused.
        dn::DirectOptions ho = hostOptions(0, 0);
        ho.identity = impostor;
        dn::DirectNet host;
        CHECK(host.start(ho));
        host.setLocalUser(kHost);
        Tap tap(host.boundPort());
        tap.setAlter([](std::vector<uint8_t>& dg, bool toHost) {
            auto m = dn::decode(dg.data(), dg.size(), "", nullptr);
            if (toHost || !m || m->type != dn::MsgType::Welcome) return true;
            m->welcome.publicKey = dn::processIdentity()->publicKey();
            dg = dn::encode(*m, "");
            return true;
        });
        dn::DirectNet a;
        CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(tap.port()), 0)));
        a.setLocalUser(kA);
        CHECK(!waitFor([&] { return a.canRoute(kHost) || host.canRoute(kA); }, 2500));
        CHECK(!tap.captured(dn::MsgType::Welcome, false).empty());
    }
    {
        // A real plugin player (its own published identity) that is not the room owner.
        dn::DirectNet host;
        CHECK(host.start(hostOptions(0, 0)));
        host.setLocalUser(kB);
        dn::DirectNet a;
        CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
        a.setLocalUser(kA);
        CHECK(!waitFor([&] { return a.canRoute(kB) || host.canRoute(kA); }, 2500));
    }
    {
        // The room owner's identity has not reached the joiner yet: it waits, then connects.
        dn::DirectNet host;
        CHECK(host.start(hostOptions(0, 0)));
        host.setLocalUser(kHost);
        dn::DirectOptions ao = joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0);
        ao.roomOwner.clear();
        ao.roomOwnerIdentity.clear();
        dn::DirectNet a;
        CHECK(a.start(ao));
        a.setLocalUser(kA);
        CHECK(!waitFor([&] { return a.canRoute(kHost); }, 2000));
        a.setRoomOwner(kHost, dn::processIdentity()->commitment());
        CHECK(waitFor([&] { return a.canRoute(kHost) && host.canRoute(kA); }, 3000));
    }
}

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
    for (auto change : {0, 1, 2, 3, 4}) {  // every signed field matters
        dn::HelloMsg t = h;
        if (change == 0) t.nonce ^= 1;
        if (change == 1) ++t.session;
        if (change == 2) t.puid = kB;
        if (change == 3) t.cookie[0] ^= 1;
        if (change == 4) t.ecdh[0] ^= 1;
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

void testOlderProtocolStaysOnEos(uint8_t protocol) {
    printf("direct: peers speaking protocol %u (%s) are ignored both ways, nothing breaks\n", protocol,
           protocol == 2 ? "0.3.6" : protocol == 3 ? "0.4.0" : protocol == 4 ? "0.4.1" : "EDF6Coop 2.4");
    // An old hello: header with the old protocol, nonce, (protocol 3: session,) id.
    std::vector<uint8_t> old = {0x45, 0x44, 0x4E, 0x31, 1, 0, protocol, 0, 7, 0, 0, 0};
    if (protocol == 3) old.insert(old.end(), 8, 1);
    old.push_back(static_cast<uint8_t>(kA.size()));
    old.insert(old.end(), kA.begin(), kA.end());
    if (protocol == 3) old.insert(old.end(), 8 + 64 + 64, 0);  // cookie, public key, signature
    if (protocol >= 4) {  // the handshake of 0.4.1 (and EDF6Coop 2.4's) is ours; only the version differs
        dn::Message hello = sampleMessages()[0];
        old = dn::encode(hello, "");
        old[6] = protocol;
    }
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

    // An old host: it cannot read our hello, whatever it sends back is not an answer we read.
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
        // 0.3.6 answers a hello it understood with a Welcome, 0.4.0 with a Challenge; send one anyway.
        std::vector<uint8_t> answer = {0x45, 0x44, 0x4E, 0x31, 2, 0, protocol, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0, 0};
        if (protocol == 3) answer = {0x45, 0x44, 0x4E, 0x31, 9, 0, 3, 0, 7, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8};
        if (protocol >= 4) {  // a Challenge for this very hello, as 0.4.1 answers it
            dn::Message hello = *dn::decode(buf, static_cast<size_t>(got), "", nullptr);
            dn::Message c;
            c.type = dn::MsgType::Challenge;
            c.challenge.clientNonce = hello.hello.nonce;
            c.challenge.cookie.fill(9);
            answer = dn::encode(c, "");
            answer[6] = protocol;
        }
        sendto(oldHost.s, reinterpret_cast<const char*>(answer.data()), static_cast<int>(answer.size()), 0,
               reinterpret_cast<sockaddr*>(&from), fromLen);
    }
    CHECK(hellos >= 2 && !a.canRoute(kHost) && a.statusLine().find("host answered") == std::string::npos);
}

void testOlderPluginStaysOnEos() {
    for (uint8_t protocol : {uint8_t{2}, uint8_t{3}, uint8_t{4}, uint8_t{6}}) testOlderProtocolStaysOnEos(protocol);
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
    std::vector<RawPeer> attackers(80);  // one source port each: a single address only replaces itself
    char id[40];
    for (int i = 0; i < 80; ++i) {
        snprintf(id, sizeof(id), "0002ffffffffffffffffffffffff%04d", i);
        attackers[i].hello(host.boundPort(), id, 100 + i);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    size_t members = host.directMembers().size();
    printf("  members after the flood: %zu\n", members);
    CHECK(members <= 1 + 64);
}

// A full room of 32 players: 31 clients on one host. Every client learns every other member from the
// host's member list (exactly as long as the wire allows) and reaches it through the host.
void testFullRoom() {
    printf("direct: a room of 32 players, 31 clients on one host\n");
    dn::DirectOptions ho = hostOptions(0, 0);
    std::vector<std::string> ids;
    char id[40];
    for (int i = 0; i < 31; ++i) {
        snprintf(id, sizeof(id), "0002eeeeeeeeeeeeeeeeeeeeeeee%04d", i);
        ids.push_back(id);
        ho.memberIds[id] = dn::processIdentity()->commitment();
    }
    // The largest handshake and link messages of a full room fit the receive buffer (2048 bytes).
    dn::Message w;
    w.type = dn::MsgType::Welcome;
    w.welcome.hostPuid = kHost;
    w.welcome.roster = ids;
    w.welcome.roster.insert(w.welcome.roster.begin(), kHost);
    size_t welcomeSize = dn::encode(w, "key").size();
    dn::Message r;
    r.type = dn::MsgType::Roster;
    r.roster.roster = w.welcome.roster;
    size_t rosterSize = dn::encode(r, "").size();
    printf("  32 members: Welcome %zu bytes (with a Key), Roster %zu bytes\n", welcomeSize, rosterSize);
    CHECK(welcomeSize < 2048 && rosterSize < 2048);
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    std::string addr = "127.0.0.1:" + std::to_string(host.boundPort());
    std::vector<std::unique_ptr<dn::DirectNet>> clients;
    for (const std::string& client : ids) {
        clients.push_back(std::make_unique<dn::DirectNet>());
        CHECK(clients.back()->start(joinOptions(addr, 0)));
        clients.back()->setLocalUser(client);
    }
    auto everyoneRoutes = [&] {
        if (host.directMembers().size() != 32) return false;
        for (size_t i = 0; i < clients.size(); ++i) {
            if (!clients[i]->canRoute(kHost)) return false;
            for (const std::string& other : ids)
                if (other != ids[i] && !clients[i]->canRoute(other)) return false;
        }
        return true;
    };
    bool full = waitFor(everyoneRoutes, 20000);
    printf("  host members: %zu\n", host.directMembers().size());
    CHECK(full);
    // Each client sends a few packets to the next one, relayed by the host.
    const uint32_t kPackets = 5;
    for (size_t i = 0; i < clients.size(); ++i)
        for (uint32_t k = 0; k < kPackets; ++k) {
            auto p = payloadFor(k);
            CHECK(clients[i]->send(ids[(i + 1) % ids.size()], "EDF6", 1, 2, p.data(), p.size()));
        }
    std::vector<uint32_t> got(clients.size());
    auto allArrived = [&] {
        for (size_t i = 0; i < clients.size(); ++i) {
            dn::Delivered d;
            while (clients[i]->pop(nullptr, 1170, d))
                if (d.src == ids[(i + clients.size() - 1) % clients.size()] && d.data == payloadFor(got[i])) ++got[i];
        }
        for (uint32_t n : got)
            if (n != kPackets) return false;
        return true;
    };
    CHECK(waitFor(allArrived, 10000));
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
        tx.poll(now, [&](const std::vector<uint8_t>& dg) {
            push(dg, false);
            return true;
        });
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
    printf("direct: game packets sent unreliable are repaired under 20%% loss (upgraded, given up after 2 s)\n");
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
    // A packet is sent up to four times before it is given up (ReliableSender::kExpireMs): all four lost
    // happens to 0.2^4 = 0.16% of them, under one of the 500 on average.
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
        dn::ReliableSender::kExpireMs + 2000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));  // catch late duplicates, if any
    dn::Delivered d;
    while (host.pop(nullptr, 1170, d)) ++got;
    bool atMostOnce = std::all_of(seen.begin(), seen.end(), [](int n) { return n <= 1; });
    printf("  received %zu/500, at most once: %s\n", got, atMostOnce ? "yes" : "NO");
    CHECK(got >= 495 && atMostOnce);  // without the repair, 100 of them would be lost
}

void testRetransmitsFollowDelivery() {
    printf("reliable: resends follow what the link delivers, and all links share one cap\n");
    dn::ReliableSender tx;
    for (uint32_t i = 0; i < 3000; ++i) tx.track(tx.nextSeq(), std::vector<uint8_t>(1000), 1);
    size_t sent = 0;
    auto count = [&](std::vector<uint8_t>&) {
        ++sent;
        return true;
    };
    tx.poll(60000, count);  // every timer expired during a 60 s stall
    CHECK(sent <= 64);
    size_t burst = sent;
    // Nothing acknowledged: a link the network drops everything of only probes, ~10 a second.
    for (uint64_t t = 60005; t <= 61000; t += 5) tx.poll(t, count);
    printf("  burst=%zu, then %zu in a second without acknowledgements\n", burst, sent - burst);
    CHECK(sent - burst <= 12 && tx.limitedMs() >= 990);
    // Acknowledgements earn resends again (two each, up to the burst).
    dn::AckMsg ack;
    ack.cumulative = 100;
    CHECK(tx.onAck(ack, 61000) == 100);
    size_t before = sent;
    tx.poll(61005, count);
    CHECK(sent - before == 64);

    // The shared cap: two links that may each resend 64 packets of 1000 bytes get 64 KB together.
    dn::RetransmitBudget shared;
    dn::ReliableSender x, y;
    for (uint32_t i = 0; i < 200; ++i) {
        x.track(x.nextSeq(), std::vector<uint8_t>(1000), 1);
        y.track(y.nextSeq(), std::vector<uint8_t>(1000), 1);
    }
    size_t bytes = 0;
    auto countBytes = [&](std::vector<uint8_t>& dg) {
        bytes += dg.size();
        return true;
    };
    x.poll(5000, countBytes, &shared);
    y.poll(5000, countBytes, &shared);
    printf("  two links resent %zu bytes under the shared cap\n", bytes);
    CHECK(bytes <= 64 * 1024 && bytes >= 60000 && shared.refusals() > 0);

    // A socket that refuses a datagram (send buffer full) is congestion: the round stops there.
    dn::ReliableSender z;
    for (uint32_t i = 0; i < 10; ++i) z.track(z.nextSeq(), std::vector<uint8_t>(100), 1);
    int tries = 0;
    z.poll(5000, [&](std::vector<uint8_t>&) {
        ++tries;
        return false;
    });
    CHECK(tries == 1 && z.credit() < 1.0 && z.retransmits() == 0 && z.pendingCount() == 10);
}

// Feeds `count` packets (reliability `rel`) through a receiver, dropping those `lost` picks, and every
// ACK back to the sender. Returns the sender's packets still unacknowledged.
size_t ackAllExcept(dn::ReliableSender& tx, dn::ReliableReceiver& rx, uint32_t count, uint8_t rel,
                    const std::function<bool(uint32_t)>& lost, std::vector<dn::DataMsg>& ready) {
    for (uint32_t i = 0; i < count; ++i) {
        dn::Message m;
        m.type = dn::MsgType::Data;
        m.data.reliability = rel;
        m.data.seq = tx.nextSeq();
        m.data.payload = payloadFor(i);
        tx.track(m.data.seq, dn::encode(m, ""), 1);
        if (lost(m.data.seq)) continue;
        dn::Message ack;
        ack.type = dn::MsgType::Ack;
        ack.ack = rx.onData(m.data, ready);
        auto dg = dn::encode(ack, "");  // through the wire format
        tx.onAck(dn::decode(dg.data(), dg.size(), "", nullptr)->ack, 2);
    }
    return tx.pendingCount();
}

void testAckCoversLongGap() {
    printf("reliable: every packet received behind a gap is acknowledged, however far and however many gaps\n");
    // The first packet lost: 2000 behind it held. The 256-bit bitmap left all but 256 unacknowledged,
    // resent every second for as long as the gap stayed open.
    dn::ReliableSender tx;
    dn::ReliableReceiver rx;
    std::vector<dn::DataMsg> ready;
    size_t pending = ackAllExcept(tx, rx, 2001, 2, [](uint32_t seq) { return seq == 1; }, ready);
    CHECK(pending == 1 && ready.empty() && rx.buffered() == 2000);
    // Every tenth lost: 200 gaps, more than one ACK names. Each packet is named by the ACK it brings.
    dn::ReliableSender tx2;
    dn::ReliableReceiver rx2;
    std::vector<dn::DataMsg> ready2;
    pending = ackAllExcept(tx2, rx2, 2000, 1, [](uint32_t seq) { return seq % 10 == 0; }, ready2);
    printf("  one gap: %zu unacknowledged; 200 gaps: %zu unacknowledged\n", tx.pendingCount(), pending);
    CHECK(pending == 200 && ready2.size() == 1800);
    // Resending the lost ones closes every gap: one burst of credit, refilled by what it acknowledges.
    std::vector<std::vector<uint8_t>> wire;
    auto resend = [&](std::vector<uint8_t>& dg) {
        wire.push_back(dg);
        return true;
    };
    auto deliver = [&](uint64_t now) {
        for (auto& dg : wire) tx2.onAck(rx2.onData(dn::decode(dg.data(), dg.size(), "", nullptr)->data, ready2), now);
        size_t n = wire.size();
        wire.clear();
        return n;
    };
    tx2.poll(10000, resend);
    CHECK(deliver(10001) == 64 && tx2.pendingCount() == 136);
    for (uint64_t t = 10005; tx2.pendingCount() > 0 && t < 20000; t += 5) {
        tx2.poll(t, resend);
        deliver(t + 1);
    }
    CHECK(tx2.pendingCount() == 0 && rx2.expected() == 2001 && rx2.buffered() == 0 && ready2.size() == 2000);
}

void testAbandonAndSkip() {
    printf("reliable: an unreliable game packet is given up after its deadline and the receiver moves past it\n");
    dn::ReliableSender tx;
    dn::ReliableReceiver rx;
    auto data = [&](uint8_t rel, uint64_t now) {
        dn::Message m;
        m.type = dn::MsgType::Data;
        m.data.reliability = rel;
        m.data.seq = tx.nextSeq();
        m.data.payload = payloadFor(m.data.seq);
        tx.track(m.data.seq, dn::encode(m, ""), now, rel == 0);
        return m.data;
    };
    std::vector<dn::DataMsg> ready;
    dn::DataMsg lost = data(0, 0);     // seq 1, never arrives
    dn::DataMsg ordered = data(2, 0);  // seq 2, waits for seq 1
    dn::DataMsg unordered = data(0, 0);  // seq 3, handed out on arrival
    tx.onAck(rx.onData(ordered, ready), 10);
    tx.onAck(rx.onData(unordered, ready), 10);
    CHECK(ready.size() == 1 && ready[0].seq == 3 && tx.pendingCount() == 1);
    CHECK(!tx.forwardDue(20));  // nothing given up yet
    // Retried (lost again) until the deadline, then given up.
    size_t resends = 0;
    auto lose = [&](std::vector<uint8_t>&) {
        ++resends;
        return true;
    };
    for (uint64_t t = 20; t < dn::ReliableSender::kExpireMs; t += 5) tx.poll(t, lose);
    CHECK(tx.pendingCount() == 1 && tx.abandoned() == 0 && resends >= 3 && resends <= 4);
    tx.poll(dn::ReliableSender::kExpireMs, lose);
    CHECK(tx.pendingCount() == 0 && tx.abandoned() == 1);
    // The receiver learns from a Forward, repeated every RTO until an ACK shows it arrived.
    auto floor = tx.forwardDue(dn::ReliableSender::kExpireMs);
    CHECK(floor && *floor == 4);
    CHECK(!tx.forwardDue(dn::ReliableSender::kExpireMs + 1));  // lost, say
    floor = tx.forwardDue(dn::ReliableSender::kExpireMs + tx.rtoMs());
    CHECK(floor && *floor == 4);
    ready.clear();
    tx.onAck(rx.onForward(*floor, ready), dn::ReliableSender::kExpireMs + 500);
    CHECK(ready.size() == 1 && ready[0].seq == 2);  // the ordered packet that only waited for the lost one
    CHECK(rx.expected() == 4 && rx.skipped() == 1 && rx.buffered() == 0);
    CHECK(!tx.forwardDue(dn::ReliableSender::kExpireMs + 5000));
    // The lost packet turning up late is not delivered any more; the next ones flow normally.
    ready.clear();
    rx.onData(lost, ready);
    tx.onAck(rx.onData(data(1, 3000), ready), 3010);
    CHECK(ready.size() == 1 && ready[0].seq == 4 && tx.pendingCount() == 0);

    // Packets the game sent reliably never expire, and only they count towards a stalled link.
    dn::ReliableSender mixed;
    mixed.track(mixed.nextSeq(), std::vector<uint8_t>(100), 0, true);
    CHECK(mixed.oldestReliableAgeMs(1000) == 0);
    mixed.track(mixed.nextSeq(), std::vector<uint8_t>(100), 500);
    CHECK(mixed.oldestReliableAgeMs(1000) == 500);
    for (uint64_t t = 0; t <= 100000; t += 50) mixed.poll(t, [](std::vector<uint8_t>&) { return true; });
    CHECK(mixed.pendingCount() == 1 && mixed.abandoned() == 1 && mixed.oldestReliableAgeMs(100000) == 99500);
    floor = mixed.forwardDue(100000);
    CHECK(floor && *floor == 2);  // past the given-up packet, not past the reliable one
    // A reliable packet older than the given-up one holds the floor: no Forward until it is through.
    dn::ReliableSender older;
    older.track(older.nextSeq(), std::vector<uint8_t>(100), 0);
    older.track(older.nextSeq(), std::vector<uint8_t>(100), 0, true);
    for (uint64_t t = 0; t <= 3000; t += 50) older.poll(t, [](std::vector<uint8_t>&) { return true; });
    CHECK(older.abandoned() == 1 && !older.forwardDue(3000));
    dn::AckMsg first;
    first.cumulative = 1;
    older.onAck(first, 3000);
    floor = older.forwardDue(3000);
    CHECK(floor && *floor == 3);
}

// A host and three clients each sending to everyone at EDF6's rate (25 packets/s per player, ~600 B)
// for 6 s, 80% unreliable and 20% ordered reliable, with 2% loss everywhere and the host's uplink capped
// just above what it needs, cut off entirely for 2 s in the middle. Lost packets are resent into an
// uplink with little room left: the resends must follow what gets through, not bury it. The transport of
// 0.4.1 fails this (wire 2.3 x payload, two of the three links dropped, two thirds of the data delivered).
void testBottleneckDoesNotCollapse() {
    printf("direct: a capped host uplink with loss and an outage carries the game without resends burying it\n");
    dn::DirectOptions ho = hostOptions(0, 0.02);
    // Host sends ~54 KB/s of its own, relays ~108 KB/s and acknowledges ~11 KB/s: ~175 KB/s.
    ho.testUplinkBytesPerSecond = 200 * 1024;
    dn::DirectNet host;
    CHECK(host.start(ho));
    host.setLocalUser(kHost);
    const std::string kC = "0002dddddddddddddddddddddddddddd";
    ho.memberIds[kC] = dn::processIdentity()->commitment();
    host.setMemberIdentities(ho.memberIds);
    std::string addr = "127.0.0.1:" + std::to_string(host.boundPort());
    dn::DirectNet a, b, c;
    std::vector<std::pair<dn::DirectNet*, std::string>> nodes = {{&host, kHost}, {&a, kA}, {&b, kB}, {&c, kC}};
    for (size_t i = 1; i < nodes.size(); ++i) {
        CHECK(nodes[i].first->start(joinOptions(addr, 0.02)));
        nodes[i].first->setLocalUser(nodes[i].second);
    }
    auto allRoute = [&] {
        for (auto& [from, fromId] : nodes)
            for (auto& [to, toId] : nodes)
                if (from != to && !from->canRoute(toId)) return false;
        return true;
    };
    CHECK(waitFor(allRoute, 10000));
    for (auto& [net, id] : nodes) net->takeWireTraffic();

    // Payload: u32 sender index, u32 receiver index, u32 packet number, u32 reliable number (or ~0).
    const size_t n = nodes.size();
    std::vector<std::vector<uint32_t>> sentAll(n, std::vector<uint32_t>(n)), sentReliable = sentAll;
    std::vector<std::vector<uint32_t>> gotUnreliable = sentAll, nextReliable = sentAll;
    bool inOrder = true;
    uint64_t payloadBytes = 0;
    auto drain = [&] {
        for (size_t to = 0; to < n; ++to) {
            dn::Delivered d;
            while (nodes[to].first->pop(nullptr, 1170, d)) {
                uint32_t f[4] = {};
                memcpy(f, d.data.data(), sizeof(f));
                if (f[0] >= n || f[1] != to || d.src != nodes[f[0]].second) {
                    inOrder = false;
                    continue;
                }
                if (f[3] == ~0u)
                    ++gotUnreliable[f[0]][to];
                else
                    inOrder &= f[3] == nextReliable[f[0]][to]++;
            }
        }
    };
    auto start = std::chrono::steady_clock::now();
    bool linksUp = true;
    for (uint32_t tick = 0; tick < 150; ++tick) {  // 6 s of 40 ms ticks
        host.setTestBlackhole(tick >= 50 && tick < 100);
        for (size_t from = 0; from < n; ++from) {
            for (size_t to = 0; to < n; ++to) {
                if (from == to) continue;
                uint32_t number = sentAll[from][to]++;
                bool reliable = number % 5 == 0;
                std::vector<uint8_t> p(300 + (number * 37 + from * 101 + to * 13) % 600, static_cast<uint8_t>(number));
                uint32_t f[4] = {static_cast<uint32_t>(from), static_cast<uint32_t>(to), number,
                                 reliable ? sentReliable[from][to]++ : ~0u};
                memcpy(p.data(), f, sizeof(f));
                linksUp &= nodes[from].first->send(nodes[to].second, "EDF6", 1, reliable ? 2 : 0, p.data(), p.size());
                payloadBytes += p.size();
            }
        }
        drain();
        std::this_thread::sleep_until(start + std::chrono::milliseconds(40 * (tick + 1)));
    }
    host.setTestBlackhole(false);
    auto reliableDone = [&] {
        drain();
        for (size_t from = 0; from < n; ++from)
            for (size_t to = 0; to < n; ++to)
                if (nextReliable[from][to] != sentReliable[from][to]) return false;
        return true;
    };
    bool delivered = waitFor(reliableDone, 15000);
    std::this_thread::sleep_for(std::chrono::milliseconds(dn::ReliableSender::kExpireMs));  // the rest given up
    drain();
    linksUp &= allRoute();
    uint64_t wire = 0, relayed = 0;
    for (auto& [net, id] : nodes) {
        dn::WireTraffic w = net->takeWireTraffic();
        wire += w.out;
        relayed += w.relayed;
    }
    uint64_t unreliableSent = 0, unreliableGot = 0;
    for (size_t from = 0; from < n; ++from)
        for (size_t to = 0; to < n; ++to) {
            unreliableSent += sentAll[from][to] - sentReliable[from][to];
            unreliableGot += gotUnreliable[from][to];
        }
    // Payload bytes crossing a link: what the players sent plus what the host forwarded between clients.
    double ratio = static_cast<double>(wire) / static_cast<double>(payloadBytes + relayed);
    double share = 100.0 * static_cast<double>(unreliableGot) / static_cast<double>(unreliableSent);
    printf("  wire %.2f x payload, reliable all delivered in order: %s, unreliable delivered %.1f%%, links up: %s\n",
           ratio, delivered && inOrder ? "yes" : "NO", share, linksUp ? "yes" : "NO");
    printf("  host: %s\n", host.statusLine().c_str());
    // Every datagram carries ~120 bytes of header and tag on ~600 of payload and brings back an ACK.
    CHECK(ratio < 2.0);
    CHECK(delivered && inOrder && linksUp);
    // Everything crosses the host, so the outage held a third of all traffic; what it held for less than
    // kExpireMs is repaired (89-93% arrives in all; 0.4.1 delivered 64-68%).
    CHECK(share > 80.0);
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

void testLobbyStatusHoldKeys() {
    printf("hold: statuses held by key, checked against another member's link, without grace\n");
    const std::string host = kA, member = kB;
    int delivered = 0, newer = 0;
    auto deliver = [&] { ++delivered; };
    std::unordered_set<std::string> up{host, member};
    auto reachable = [&](const std::string& r) { return up.count(r) != 0; };
    dn::LobbyStatusHold h(30000);

    // Room closed by Epic: held while the host's link is up, delivered on the first poll it is down.
    CHECK(!h.offer("#room", host, false, 0, false, 0, deliver) && !h.isHeld("#room"));
    CHECK(h.offer("#room", host, true, 0, false, 0, deliver) && h.isHeld("#room"));
    CHECK(h.poll(60000, reachable).empty() && delivered == 0);  // never expires while the link lives
    up.erase(host);
    auto gone = h.poll(60001, reachable);
    CHECK(gone.size() == 1 && gone[0] == "#room" && delivered == 1);
    up.insert(host);

    // A member that left Epic's lobby: no grace, and a join swallows the pair.
    CHECK(h.offer(member, member, true, 0, false, 0, deliver));
    CHECK(h.poll(10, reachable).empty() && h.isHeld(member));  // up on the very poll after: still held
    CHECK(h.onStatus(member, 0) && !h.isHeld(member) && delivered == 1);

    // A newer owner replaces the held one; the older is never delivered.
    CHECK(h.offer("#owner", host, true, 0, true, 0, deliver));
    CHECK(h.offer("#owner", host, false, 0, true, 0, [&] { ++newer; }) && h.heldCount() == 1);
    up.erase(host);
    CHECK(h.poll(1, reachable).size() == 1 && newer == 1 && delivered == 1);
    up.insert(host);

    // Discarded: Epic gave the room back, nothing to tell.
    CHECK(h.offer("#owner", host, true, 0, true, 0, deliver));
    CHECK(h.discard("#owner") && !h.discard("#owner") && h.heldCount() == 0);
    up.erase(host);
    CHECK(h.poll(2, reachable).empty() && delivered == 1);

    // Delivered together: members first, then the owner, the room last (closing it leaves it).
    up = {host, member};
    std::string order;
    CHECK(h.offer("#room", host, true, 0, false, 0, [&] { order += "R"; }));
    CHECK(h.offer("#owner", host, true, 0, true, 0, [&] { order += "O"; }));
    CHECK(h.offer(member, host, true, 0, false, 0, [&] { order += "M"; }));
    up.clear();
    CHECK(h.poll(3, reachable).size() == 3 && order == "MOR");

    // A member's LEFT replaces its hidden disconnect: no grace, and the game is told the LEFT.
    int left = 0;
    up = {member};
    CHECK(h.offer(member, true, 0, deliver));                                       // disconnected, 30 s grace
    CHECK(h.offer(member, member, true, 0, true, 0, [&] { ++left; }) && h.heldCount() == 1);
    up.clear();
    CHECK(h.poll(4, reachable).size() == 1 && left == 1 && delivered == 1);
}

void testLobbyOwnerPin() {
    printf("owner pin: Epic's owner changes do not move the room while the pinned owner's link is up\n");
    const std::string host = kA, me = kB, other = "0002dddddddddddddddddddddddddddd";
    dn::LobbyOwnerPin pin;

    pin.entered(host);
    CHECK(pin.pinned() == host && pin.usurper().empty() && pin.kickAuthorized());

    // Epic makes us owner while the host still plays: hidden, and handed back.
    auto p = pin.onPromoted(me, me, true);
    CHECK(p.hide && p.promoteBack && pin.pinned() == host && pin.usurper() == me && !pin.kickAuthorized());
    CHECK(pin.onPinnedJoined(me));  // the host back in the lobby: hand it back (again) now
    p = pin.onPromoted(host, me, true);  // handed back
    CHECK(p.hide && !p.promoteBack && pin.usurper().empty() && pin.kickAuthorized() && !pin.onPinnedJoined(me));

    // Someone else made owner: hidden, not ours to hand back.
    p = pin.onPromoted(other, me, true);
    CHECK(p.hide && !p.promoteBack && pin.usurper() == other && !pin.onPinnedJoined(me));
    pin.follow(other);  // the host's link died: the hidden promotion reached the game
    CHECK(pin.pinned() == other && pin.usurper().empty() && pin.kickAuthorized());

    // The pinned owner unreachable at the promotion: the room follows Epic at once.
    pin.entered(host);
    p = pin.onPromoted(me, me, false);
    CHECK(!p.hide && !p.promoteBack && pin.pinned() == me && pin.kickAuthorized());

    // Owner unknown on entering: the first promotion pins, and reaches the game.
    pin.entered("");
    CHECK(pin.kickAuthorized() && pin.usurper().empty());
    p = pin.onPromoted(other, me, true);
    CHECK(!p.hide && pin.pinned() == other);

    pin.left();
    CHECK(pin.pinned().empty() && pin.usurper().empty() && pin.kickAuthorized() && !pin.onPinnedJoined(me));
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
    std::wstring installed = dir + L"EDF6Coop.dll";
    DeleteFileW((installed + L".old").c_str());
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    CHECK(CopyFileW(self, installed.c_str(), FALSE));  // any image does: it is mapped, not run
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
    CHECK(CopyFileW(self, stale.c_str(), FALSE) && CopyFileW(self, other.c_str(), FALSE));
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

void testReceiveStateFollowsSession() {
    printf("direct: queued packets and recent traffic belong to the local room/user session\n");
    for (int transition = 0; transition < 3; ++transition) {
        dn::DirectNet host, joiner;
        CHECK(host.start(hostOptions(0, 0)));
        host.setLocalUser(kHost);
        CHECK(joiner.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
        joiner.setLocalUser(kA);
        CHECK(waitFor([&] { return host.canRoute(kA) && joiner.canRoute(kHost); }, 5000));
        const auto payload = payloadFor(42);
        CHECK(host.send(kA, "EDF6", 1, 1, payload.data(), payload.size()));
        CHECK(joiner.send(kHost, "EDF6", 1, 1, payload.data(), payload.size()));
        CHECK(waitFor([&] {
            return host.heardFromRecently(kA, 10000) && joiner.heardFromRecently(kHost, 10000);
        }, 5000));  // wait for delivery into both inboxes without draining either

        // Repeated observations of the same room/user must preserve unread packets.
        host.setActive(true);
        joiner.setLocalUser(kA);
        dn::Delivered out;
        CHECK(host.pop(nullptr, 1170, out) && out.data == payload);
        CHECK(joiner.pop(nullptr, 1170, out) && out.data == payload);

        // A second channel lets us wait for a later packet without consuming the stale one.
        CHECK(host.send(kA, "EDF6", 1, 2, payload.data(), payload.size()));
        CHECK(host.send(kA, "EDF6", 2, 2, payload.data(), payload.size()));
        CHECK(joiner.send(kHost, "EDF6", 1, 2, payload.data(), payload.size()));
        CHECK(joiner.send(kHost, "EDF6", 2, 2, payload.data(), payload.size()));
        const uint8_t channel = 2;
        CHECK(waitFor([&] { return host.pop(&channel, 1170, out); }, 5000));
        CHECK(waitFor([&] { return joiner.pop(&channel, 1170, out); }, 5000));

        if (transition == 0) {
            host.setActive(false);
            joiner.setActive(false);
        } else if (transition == 1) {
            host.setLocalUser(kB);
            joiner.setLocalUser(kB);
        } else {
            host.stop();
            joiner.stop();
        }
        CHECK(!host.pop(nullptr, 1170, out));
        CHECK(!joiner.pop(nullptr, 1170, out));
        CHECK(!host.heardFromRecently(kA, 10000));
        CHECK(!joiner.heardFromRecently(kHost, 10000));

        if (transition == 0) {
            host.setActive(true);
            joiner.setActive(true);
            CHECK(waitFor([&] { return host.canRoute(kA) && joiner.canRoute(kHost); }, 5000));
            CHECK(!host.heardFromRecently(kA, 10000));
            CHECK(!joiner.heardFromRecently(kHost, 10000));
            CHECK(streamInOrder(host, kA, joiner, kHost, 3));
            CHECK(streamInOrder(joiner, kHost, host, kA, 3));
        }
    }
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
    // The Key goes into the link keys too: with it everything works as without.
    CHECK(waitFor([&] { return host.canRoute(kB); }, 2000));
    CHECK(streamInOrder(good, kHost, host, kB, 200));
    CHECK(streamInOrder(host, kB, good, kHost, 200));
    CHECK(good.rejectedPackets() == 0 && host.rejectedPackets() == 0);
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
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0x4f, 0xf1, 0x0e, 0xd8, 0xb7, 0x69, 0xae, 0x45, 0xae, 0xf1,
    0xce, 0xd9, 0x23, 0x68, 0x57, 0xd0, 0xb1, 0x78, 0xce, 0x42, 0x4f, 0xcb, 0x7a, 0x3e, 0x9f, 0x44, 0x2b, 0xb3,
    0x3c, 0xcf, 0x1e, 0xec, 0x53, 0xd2, 0xa2, 0x20, 0x57, 0xe3, 0x5b, 0x17, 0x4e, 0x73, 0x5d, 0x66, 0x0a, 0x42,
    0x2c, 0x13, 0x1a, 0x24, 0x57, 0x08, 0x93, 0xbf, 0xb7, 0xe3, 0xf7, 0xa6, 0xe0, 0x6e, 0xeb, 0xaf, 0x7d, 0x04};
const char* const kTestSig037 =
    "EDF6Coop 0.3.7\n"
    "3825d0abfb42d32b077e869bb6fca3636e0746cbc634622c05feae149540571f\n"
    "cb6135c39730aa7bea937087ce81d95abe435986d581e2fdf9722d91c2e56711"
    "96b86370db3cebf6febfe41a53a5a723f7999ef00f66a76e0cf40ace2424f961\n";
const char* const kTestSig035 =
    "EDF6Coop 0.3.5\n"
    "2a18ec0e2ad765cf5b05991eb268f83da5acf9c59f05b3155154d28ef8058ba5\n"
    "68da353b7a3f04008d2ff8e49b4a80e4041a519d9e7b894b8ddf979f52715b5a"
    "0151c37a637a8d4d6bfb0f2e700ad81944404f26313fe98039f66ecb0f546265\n";

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
    CHECK(dn::sha256Hex(dll) == "3825d0abfb42d32b077e869bb6fca3636e0746cbc634622c05feae149540571f");
    dn::SignedManifest m;
    CHECK(dn::readSignedManifest(kTestSig037, kTestKey, &m, &why) && m.version == v037);
    CHECK(dn::verifyRelease(dll, kTestSig037, v037, v036, kTestKey, &why));
    // Production trusts only the embedded release key: the test key's signature does not count.
    CHECK(dn::releaseSigningKey().size() == 72 && dn::releaseSigningKey() != kTestKey);
    CHECK(!dn::verifyRelease(dll, kTestSig037, v037, v036, dn::releaseSigningKey(), &why));
    CHECK(why.find("signature is not valid") != std::string::npos);
    // The signature binds version and digest.
    std::string other = kTestSig037;
    other[13] = '8';  // "EDF6Coop 0.3.8"
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
          std::string("EDF6Coop 00.3.7") + good.substr(14), std::string("EDF6Coop  0.3.7") + good.substr(14),
          std::string("EDF6Coop v0.3.7") + good.substr(14), std::string("edf6coop 0.3.7") + good.substr(14),
          std::string("EDF6DirectNet 0.3.7") + good.substr(14)}) {
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

// What the updater last told the menu (dn::statusToSink).
std::string& menuStatus() {
    static std::string text;
    return text;
}
void recordMenuStatus(const char* text) { menuStatus() = text; }

void testMenuStatus() {
    using dn::UpdateStage;
    const auto text = [](UpdateStage stage, const char* from = "", const char* downloaded = "") {
        return dn::menuStatusText({"2.3.2", from, downloaded, stage});
    };
    CHECK(text(UpdateStage::Checking) == "EDF6Coop 2.3.2 (checking for updates)");
    CHECK(text(UpdateStage::Latest) == "EDF6Coop 2.3.2 (latest)");
    CHECK(text(UpdateStage::Off) == "EDF6Coop 2.3.2 (auto update off)");
    CHECK(text(UpdateStage::CheckFailed) == "EDF6Coop 2.3.2 (update check failed)");
    CHECK(text(UpdateStage::Failed) == "EDF6Coop 2.3.2 (update failed, see EDF6Coop.log)");
    CHECK(text(UpdateStage::Installed, "", "2.3.3") == "EDF6Coop 2.3.2 -> 2.3.3 downloaded, restart the game");
    // A finished update outranks this start's check, but not a newer download or a failure the player must act on.
    CHECK(text(UpdateStage::Latest, "2.3.1") == "EDF6Coop 2.3.2 (updated from 2.3.1)");
    CHECK(text(UpdateStage::CheckFailed, "2.3.1") == "EDF6Coop 2.3.2 (updated from 2.3.1)");
    CHECK(text(UpdateStage::Installed, "2.3.1", "2.3.3") == "EDF6Coop 2.3.2 -> 2.3.3 downloaded, restart the game");
    CHECK(text(UpdateStage::Failed, "2.3.1") == "EDF6Coop 2.3.2 (update failed, see EDF6Coop.log)");
    // Every one fits the menu's field (multislot updatecheck.cpp: 64 characters) with long version numbers.
    for (UpdateStage stage : {UpdateStage::Off, UpdateStage::Checking, UpdateStage::Latest, UpdateStage::Installed,
                              UpdateStage::CheckFailed, UpdateStage::Failed})
        CHECK(dn::menuStatusText({"12.34.56", "12.34.55", "12.34.57", stage}).size() < 64);

    // AutoUpdate=0 says so at once, without a thread or a request.
    dn::startAutoUpdate(L"", "2.3.2", false);
    CHECK(menuStatus() == "EDF6Coop 2.3.2 (auto update off)");
}

void testUpdateRollback() {
    printf("update: rollback state machine (trial, healthy, rolled back, bad version)\n");
    std::wstring dir = freshDir(L"edf6dn_rollback_test");
    std::wstring dll = dir + L"EDF6DirectNet.dll", old = dll + L".old", trial = dll + L".trial", bad = dll + L".bad";
    // NUL-terminated like the marker in a real build, so fileVersion reads it back.
    auto install = [&](const char* version) { putFile(dll, "MZ EDF6COOP_VERSION=" + std::string(version) + '\0'); };
    std::string why;

    // The version a DLL says it is, past the marker prefix its own updater code also contains.
    const char marked[] = "MZ EDF6COOP_VERSION=\0%s\0 EDF6COOP_VERSION=1.2\0 EDF6COOP_VERSION=0.3.6\0";
    putFile(dll, std::string(marked, sizeof(marked) - 1));
    CHECK(dn::fileVersion(dll) == "0.3.6");
    const char unmarked[] = "MZ EDF6COOP_VERSION=\0 no marker\0";
    putFile(dll, std::string(unmarked, sizeof(unmarked) - 1));
    CHECK(dn::fileVersion(dll) == "?");
    // A room-size build of 2.2.x, kept for rollback after the update to the one build (product.h).
    const char legacy[] = "MZ EDF6COOP_VERSION=\0 EDF6COOP_12P_VERSION=\0 EDF6COOP_12P_VERSION=2.2.0\0";
    putFile(dll, std::string(legacy, sizeof(legacy) - 1));
    CHECK(dn::fileVersion(dll) == "2.2.0");

    // Installed by hand: nothing to prove, a stray trial is dropped.
    install("0.3.7");
    putFile(trial, "0.3.7 0\n");
    CHECK(dn::beginRun(dll, "0.3.7") == dn::RunState::Normal && !fileExists(trial));
    CHECK(menuStatus().rfind("EDF6Coop 0.3.7 (", 0) == 0 && menuStatus().find("updated") == std::string::npos);  // no update behind this start

    // 0.3.7 updates itself to 0.3.8: 0.3.7 is kept as .old.
    std::vector<uint8_t> v038 = testDll("0.3.8");
    CHECK(dn::installOver(dll, v038, &why));
    CHECK(fileText(old).find("EDF6COOP_VERSION=0.3.7") != std::string::npos && fileText(dll).size() == v038.size());
    // The first start of 0.3.8 is a trial; healthy after a while: the trial and .old go.
    CHECK(dn::beginRun(dll, "0.3.8") == dn::RunState::Trial);
    CHECK(menuStatus() == "EDF6Coop 0.3.8 (updated from 0.3.7)");  // the menu confirms the update
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
    CHECK(fileText(dll).find("EDF6COOP_VERSION=0.3.8") != std::string::npos);  // 0.3.8 runs from the next start
    CHECK(fileText(dll + L".rolledback").find("EDF6COOP_VERSION=0.3.9") != std::string::npos);
    CHECK(!fileExists(old) && !fileExists(trial));
    CHECK(dn::badVersion(dll) == dn::parseVersion("0.3.9"));
    // The next start is 0.3.8 again, normal; the moved-aside DLL is cleaned up.
    CHECK(dn::beginRun(dll, "0.3.8") == dn::RunState::Normal && !fileExists(dll + L".rolledback"));

    // An older updater installs 0.3.9 again anyway: rolled back at once, without another trial.
    CHECK(dn::installOver(dll, v039, &why));
    CHECK(dn::beginRun(dll, "0.3.9") == dn::RunState::RolledBack);
    CHECK(fileText(dll).find("EDF6COOP_VERSION=0.3.8") != std::string::npos);

    // Another game running the same trial right now (this process) is not a failed run.
    CHECK(dn::installOver(dll, testDll("0.4.0"), &why));
    putFile(trial, "0.4.0 " + std::to_string(GetCurrentProcessId()) + "\n");
    CHECK(dn::beginRun(dll, "0.4.0") == dn::RunState::Trial && fileExists(old));

    // A version on trial that installs the next one gives up its own trial: its health check must not
    // delete the new version's rollback target, and that target stays the proven 0.3.8, not unproven 0.4.0.
    CHECK(dn::installOver(dll, testDll("0.4.1"), &why));
    CHECK(!fileExists(trial) && fileText(old).find("EDF6COOP_VERSION=0.3.8") != std::string::npos);
    CHECK(fileText(dll + L".rolledback").find("EDF6COOP_VERSION=0.4.0") != std::string::npos);
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
    const std::wstring built = self;  // any image will do: this test program is one the loader can map
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
    putFile(dll + L".old", "MZ EDF6COOP_VERSION=0.3.9");
    putFile(dll + L".trial", "0.9.9 4\n");  // its game (pid 4 is System, never a game) died on trial
    bool rolledBack = dn::beginRun(dll, "0.9.9") == dn::RunState::RolledBack;
    done = true;
    watcher.join();
    CHECK(allOk && rolledBack);
    CHECK(missing == 0 && looks > 0);
    CHECK(fileText(dll) == "MZ EDF6COOP_VERSION=0.3.9");
    if (loaded) FreeLibrary(loaded);
    DeleteFileW((dll + L".rolledback").c_str());
    DeleteFileW((dll + L".bad").c_str());
    // A download that could not be put in place leaves the installed file as it was.
    CHECK(CopyFileW(built.c_str(), (dll + L".old").c_str(), FALSE));
    HANDLE lock = CreateFileW((dll + L".old").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(!dn::installOver(dll, testDll("0.4.0"), &why));  // the old backup cannot be removed
    CHECK(fileText(dll) == "MZ EDF6COOP_VERSION=0.3.9");
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

std::vector<std::string> sorted(std::vector<std::string> v) {
    std::sort(v.begin(), v.end());
    return v;
}

void testRoomView() {
    printf("room view: two sources of the same status reach the game once\n");
    const std::string kC = "0002dddddddddddddddddddddddddddd";
    dn::RoomView v;
    CHECK(v.admit(kA, dn::kJoined));  // no room: nothing is filtered
    v.reset(kA, {kHost, kA});
    CHECK(v.active() && v.has(kA) && v.has(kHost));
    CHECK(v.admit(kB, dn::kJoined) && !v.admit(kB, dn::kJoined));  // Epic and the host both say B joined
    CHECK(v.admit(kB, dn::kLeft) && !v.admit(kB, dn::kDisconnected));
    CHECK(v.admit(kA, dn::kKicked) && v.admit(kHost, dn::kPromoted) && v.admit(kHost, dn::kClosed));

    printf("room view: Epic speaks first; the plugin's say waits, and starts over when it changes\n");
    v.reset(kHost, {kHost});
    CHECK((v.settle({{kA, dn::kJoined}}, 1000, 5000).empty()));
    CHECK((v.settle({{kA, dn::kJoined}}, 5999, 5000).empty()));
    CHECK((v.settle({{kA, dn::kJoined}}, 6000, 5000) == std::vector<dn::StatusChange>{{kA, dn::kJoined}}));
    CHECK((v.settle({{kB, dn::kJoined}}, 7000, 5000).empty()));
    CHECK((v.settle({}, 8000, 5000).empty()));  // Epic reported it meanwhile: no longer wanted...
    CHECK((v.settle({{kB, dn::kJoined}}, 9000, 5000).empty()));  // ...and wanted again waits all over
    CHECK((v.settle({{kB, dn::kLeft}}, 14500, 5000).empty()));  // another status starts over too
    CHECK((v.settle({{kC, dn::kJoined}}, 1, 0).size() == 1));  // no delay: at once

    printf("room view: a member follows the host's list, keeping members only Epic told it of\n");
    v.reset(kA, {kHost, kA, kC});  // Epic listed C before the host's game had it
    CHECK(v.followHost().empty());  // nothing heard from the host yet
    v.heardHost({kHost, kA, kB});
    auto first = v.followHost();
    CHECK((first == std::vector<dn::StatusChange>{{kB, dn::kJoined}}));
    for (const auto& c : first) v.admit(c.target, c.status);
    v.heardHost({kHost, kA, kC});
    auto second = v.followHost();  // C is not joined again: our game has it already
    CHECK((second == std::vector<dn::StatusChange>{{kB, dn::kLeft}}));
    for (const auto& c : second) v.admit(c.target, c.status);
    CHECK(!v.has(kB) && v.has(kC));
    v.heardHost({kHost, kC});
    CHECK((v.followHost() == std::vector<dn::StatusChange>{{kA, dn::kKicked}}));

    printf("room view: the host lets linked players in while there is room, and Epic's are Epic's\n");
    v.reset(kHost, {kHost, kA});
    auto joins = v.hostJoins({{kA, 1}, {kB, 2}, {kC, 3, true}, {kHost, 4}, {"", 5}}, 8);
    CHECK((joins == std::vector<dn::StatusChange>{{kB, dn::kJoined}}));
    CHECK((v.hostJoins({{kB, 2}, {kC, 3}}, 3).size() == 1));  // one place left
    CHECK((v.hostJoins({{kB, 2}}, 2).empty()));  // full
    CHECK((v.hostJoins({{kB, 2}}, 0).empty()));  // size never read: nobody gets in this way

    printf("room view: one that left is not brought back by the link it left with, only by a new one\n");
    v.admit(kB, dn::kJoined);
    v.hostJoins({{kA, 1}, {kB, 2}}, 8);  // B's link is noted while it is in the room
    v.admit(kB, dn::kLeft);
    CHECK((v.hostJoins({{kB, 2}}, 8).empty()));
    CHECK((v.hostJoins({{kB, 6}}, 8) == std::vector<dn::StatusChange>{{kB, dn::kJoined}}));

    printf("room view: a member in by its link only leaves by its link; kicked ones stay out\n");
    v.admit(kB, dn::kJoined);
    v.markDirect(kB);
    v.markDirect(kC);  // not in the room: nothing to mark
    CHECK(v.direct(kB) && !v.direct(kA) && !v.direct(kC));
    auto inEpic = [&](const std::string& m) { return !v.direct(m); };  // no copy of Epic's lobby
    auto down = [](const std::string&) { return false; };
    CHECK((v.hostLeaves(inEpic, down) == std::vector<dn::StatusChange>{{kB, dn::kLeft}}));
    CHECK((v.hostLeaves(inEpic, [](const std::string&) { return true; }).empty()));
    CHECK(v.kick(kB) && v.banned(kB));
    CHECK(!v.kick(kB));  // the game's repeats of the same kick
    CHECK(!v.kick(kHost) && !v.kick(kC));
    v.admit(kB, dn::kKicked);
    CHECK(!v.direct(kB));
    CHECK((v.hostJoins({{kB, 7}}, 8).empty()));  // a new link does not let a kicked player back in
    CHECK(v.admit(kB, dn::kJoined) && !v.banned(kB));  // back in through Epic's lobby: the ban is lifted
    v.reset(kHost, {kHost});
    CHECK(!v.banned(kB));  // a ban is for one room
    v.clear();
    CHECK((!v.active() && v.followHost().empty() && v.hostJoins({{kA, 1}}, 8).empty()));

    printf("room view: members in the order the game added them, joins in the host's order\n");
    v.reset(kB, {kHost, kB});
    CHECK((v.members() == std::vector<std::string>{kHost, kB}));
    v.heardHost({kHost, kB, kC, kA});  // the host's order, not the ids' (kA sorts before kC)
    auto ordered = v.followHost();
    CHECK((ordered == std::vector<dn::StatusChange>{{kC, dn::kJoined}, {kA, dn::kJoined}}));
    for (const auto& c : ordered) v.admit(c.target, c.status);
    CHECK((v.members() == std::vector<std::string>{kHost, kB, kC, kA}));
    v.admit(kC, dn::kLeft);
    CHECK((v.members() == std::vector<std::string>{kHost, kB, kA}));
    v.reset(kC, {kHost, kB, kC, kA});  // entering with the host's list keeps its order
    CHECK((v.members() == std::vector<std::string>{kHost, kB, kC, kA}));

    printf("room view: the host's slots - a member joins our game once the host's game has it, in its slot\n");
    v.reset(kB, {kHost, kB, kA});  // Epic listed A, the host's game does not have it yet
    CHECK(!v.slotted());
    v.heardHost({kHost, "", kB, kC});  // slot 1 empty (someone left), C beyond Epic's lobby
    CHECK(v.slotted() && v.hostSlot(kHost) == 0 && v.hostSlot(kB) == 2 && v.hostSlot(kC) == 3 && v.hostSlot(kA) == -1);
    CHECK((v.hostMembers() == std::vector<std::string>{kHost, kB, kC}));
    v.adoptHost();  // the game enters with the host's members: A is not among them
    CHECK((v.members() == std::vector<std::string>{kHost, kB, kC}) && !v.has(kA));
    CHECK(v.followHost().empty());
    v.heardHost({kHost, kA, kB, kC});  // the host's game took A in the empty slot
    CHECK((v.followHost() == std::vector<dn::StatusChange>{{kA, dn::kJoined}}) && v.hostSlot(kA) == 1);
    v.heardHost({kHost, kA});  // a host list without us
    CHECK(!v.slotted());

    printf("room order: Epic's members in Epic's order, then the others in the game's\n");
    CHECK((dn::roomOrder({kHost, kB}, {kHost, kC, kB, kA}) == std::vector<std::string>{kHost, kB, kC, kA}));
    CHECK((dn::roomOrder({kHost, kB, kA}, {kHost, kB}) == std::vector<std::string>{kHost, kB}));  // A not in yet
    CHECK((dn::roomOrder({}, {kB, kHost}) == std::vector<std::string>{kB, kHost}));
}

void testRoomWire() {
    printf("wire: Room carries the host's member list\n");
    dn::Message m;
    m.type = dn::MsgType::Room;
    m.room.hostNonce = 41;
    m.room.members = {kHost, kA, kB};
    auto dg = dn::encode(m, "");
    dn::DecodeError err;
    auto back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->type == dn::MsgType::Room && back->room.hostNonce == 41 && back->room.members == m.room.members);
    m.room.members.clear();  // a host whose game has left its room
    dg = dn::encode(m, "");
    back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->type == dn::MsgType::Room && back->room.members.empty());
}

void testFakeLobbies() {
    printf("fake lobby: our details handles and copies are told from EOS's and freed exactly once\n");
    dn::FakeLobbies fakes;
    dn::FakeDetails d;
    d.roomId = "room1";
    d.owner = kHost;
    d.members = {kHost, kA};
    d.maxMembers = 8;
    d.attributes = {{"NAME", 3, 0, 0.0, "my room", 1}, {"LEVEL", 1, 12, 0.0, "", 1}, {"OPEN", 0, 1, 0.0, "", 1}};
    EOS_HLobbyDetails h = fakes.make(d);
    dn::FakeDetails got;
    CHECK(fakes.owns(h) && fakes.lookup(h, &got) && got.roomId == "room1" && got.members == d.members);
    int notOurs = 0;
    CHECK(!fakes.owns(reinterpret_cast<EOS_HLobbyDetails>(&notOurs)));
    CHECK(!fakes.release(reinterpret_cast<EOS_HLobbyDetails>(&notOurs)));

    EOS_Lobby_Attribute* name = fakes.copyAttribute(d.attributes[0]);
    EOS_Lobby_Attribute* level = fakes.copyAttribute(d.attributes[1]);
    EOS_Lobby_Attribute* open = fakes.copyAttribute(d.attributes[2]);
    CHECK(name->Data && std::string(name->Data->Key) == "NAME" && std::string(name->Data->Value.AsUtf8) == "my room");
    CHECK(level->Data->ValueType == 1 && level->Data->Value.AsInt64 == 12 && open->Data->Value.AsBool == 1);
    int owner = 0;
    EOS_LobbyDetails_Info* info = fakes.copyInfo(d, reinterpret_cast<EOS_ProductUserId>(&owner));
    CHECK(std::string(info->LobbyId) == "room1" && info->MaxMembers == 8 && info->AvailableSlots == 6 &&
          info->LobbyOwnerUserId == reinterpret_cast<EOS_ProductUserId>(&owner));
    d.members.assign(9, kA);  // more than fit: no slot left, never a wrapped count
    EOS_LobbyDetails_Info* full = fakes.copyInfo(d, nullptr);
    CHECK(full->AvailableSlots == 0);
    CHECK(fakes.liveCopies() == 5);
    CHECK(fakes.releaseAttribute(name) && !fakes.releaseAttribute(name) && fakes.releaseAttribute(level) &&
          fakes.releaseAttribute(open));
    CHECK(fakes.releaseInfo(info) && !fakes.releaseInfo(info) && fakes.releaseInfo(full));
    CHECK(fakes.release(h) && !fakes.release(h) && !fakes.owns(h));
    CHECK(fakes.liveHandles() == 0 && fakes.liveCopies() == 0);
}

void testRoomFollowsHost() {
    printf("direct: the host's room list reaches its joiners, and changes reach them too\n");
    dn::DirectNet host;
    CHECK(host.start(hostOptions(0, 0)));
    host.setLocalUser(kHost);
    host.setRoomMembers({kHost, kA});
    dn::DirectNet a;
    CHECK(a.start(joinOptions("127.0.0.1:" + std::to_string(host.boundPort()), 0)));
    a.setLocalUser(kA);
    uint64_t version = 0;
    CHECK(waitFor([&] { return sorted(a.hostRoom(&version)) == sorted({kHost, kA}); }, 5000));
    const uint64_t before = version;
    host.setRoomMembers({kHost, kA, kB});
    CHECK(waitFor([&] { return sorted(a.hostRoom(&version)) == sorted({kHost, kA, kB}); }, 5000));
    CHECK(version > before);
    CHECK((a.hostRoom(&version) == std::vector<std::string>{kHost, kA, kB}));  // in the host's order
    const uint64_t same = version;
    host.setRoomMembers({kHost, kA, kB});  // the same list again: not a change
    host.setRoomMembers({kHost, kB});
    CHECK(waitFor([&] { return a.hostRoom(&version) == std::vector<std::string>{kHost, kB}; }, 5000));
    CHECK(version == same + 1);
    // Another order is another room to the games (they number the members by it, room_view.h roomOrder).
    host.setRoomMembers({kB, kHost});
    CHECK(waitFor([&] { return a.hostRoom(&version) == std::vector<std::string>{kB, kHost}; }, 5000));
    CHECK(version == same + 2);
    CHECK(host.linkId(kA) != 0 && host.linkId(kB) == 0 && a.linkId(kHost) != 0);
    a.setActive(false);  // our game left the room: what the host said no longer applies
    CHECK(a.hostRoom(&version).empty());
}


// --- Netcode rewrite W1: the joiners' direct links (mesh) and the paths between them ---

struct Mesh {
    dn::DirectNet host, a, b;
    std::string port;
    bool start(double drop = 0.0) {
        dn::DirectOptions ho = hostOptions(0, drop);
        if (!host.start(ho)) return false;
        host.setLocalUser(kHost);
        port = std::to_string(host.boundPort());
        dn::DirectOptions ao = joinOptions("127.0.0.1:" + port, drop), bo = joinOptions("127.0.0.1:" + port, drop);
        const std::string id = dn::processIdentity()->commitment();
        ao.memberIds = bo.memberIds = {{kHost, id}, {kA, id}, {kB, id}};
        if (!a.start(ao) || !b.start(bo)) return false;
        a.setLocalUser(kA);
        b.setLocalUser(kB);
        return waitFor([&] { return a.canRoute(kB) && b.canRoute(kA); }, 10000);
    }
};

// Sends `count` datagrams of `cls` from `from` to `toId` every `everyMs`, and returns what `to` got, in order of
// arrival, and the longest gap between two arrivals.
struct Arrivals {
    std::vector<uint32_t> ids;
    uint64_t longestGapMs = 0;
};
Arrivals stream(dn::DirectNet& from, const std::string& toId, dn::DirectNet& to, uint8_t cls, uint32_t count,
                uint32_t everyMs, const std::function<void(uint32_t)>& each = nullptr) {
    Arrivals got;
    uint64_t last = 0;
    auto drain = [&] {
        dn::Delivered d;
        uint8_t ch = 1;
        while (to.pop(&ch, 1170, d)) {
            uint32_t id = 0;
            memcpy(&id, d.data.data(), 4);
            got.ids.push_back(id);
            const uint64_t now = GetTickCount64();
            if (last) got.longestGapMs = std::max(got.longestGapMs, now - last);
            last = now;
        }
    };
    for (uint32_t i = 0; i < count; ++i) {
        if (each) each(i);
        std::vector<uint8_t> p(100, static_cast<uint8_t>(i));
        memcpy(p.data(), &i, 4);
        from.sendClassified(toId, "EDF6", 1, 0, p.data(), p.size(), cls);
        std::this_thread::sleep_for(std::chrono::milliseconds(everyMs));
        drain();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    drain();
    return got;
}

void testMeshLinksJoiners() {
    printf("mesh: two joiners link directly; state goes over the direct link, not through the host\n");
    Mesh m;
    CHECK(m.start());
    CHECK(m.a.pathTo(kB) == dn::Path::Relay);  // until the direct link is up and proven
    auto p = payloadFor(1);
    m.a.sendClassified(kB, "EDF6", 1, 0, p.data(), p.size(), 1);  // the first datagram asks for the link
    CHECK(waitFor([&] { return m.a.peerLinked(kB) && m.b.peerLinked(kA); }, 10000));
    CHECK(waitFor([&] { return m.a.pathTo(kB) == dn::Path::Direct; }, 5000));
    dn::Delivered first;
    while (m.b.pop(nullptr, 1170, first)) {}  // the datagram that asked for the link
    m.host.takeWireTraffic();
    const Arrivals got = stream(m.a, kB, m.b, 1, 100, 10);
    printf("  %zu of 100 state datagrams arrived\n", got.ids.size());
    CHECK(got.ids.size() == 100);
    const dn::WireTraffic hostSaw = m.host.takeWireTraffic();
    CHECK(hostSaw.relayed == 0);  // the host relayed none of it
    CHECK(m.a.takeWireTraffic().direct > 0);
    CHECK(m.a.linkBudget(kB) >= dn::RateController::kMinRate);
    CHECK(m.a.statusLine().find("direct-to-joiners=1") != std::string::npos);
}

void testMeshBlockedFromTheStart() {
    printf("mesh: joiners that cannot reach each other stay on the relay, and lose nothing\n");
    Mesh m;
    m.a.setTestBlockPeers(0, UINT64_MAX);
    m.b.setTestBlockPeers(0, UINT64_MAX);
    CHECK(m.start());
    const Arrivals got = stream(m.a, kB, m.b, 1, 200, 10);
    printf("  %zu of 200 state datagrams arrived through the host\n", got.ids.size());
    CHECK(got.ids.size() == 200);
    CHECK(!m.a.peerLinked(kB) && m.a.pathTo(kB) == dn::Path::Relay);
    CHECK(m.host.takeWireTraffic().relayed > 0);
}

void testMeshFailsOverAndBack() {
    printf("mesh: the direct link breaks and comes back - state keeps flowing, events arrive once each\n");
    Mesh m;
    CHECK(m.start());
    auto p = payloadFor(1);
    m.a.sendClassified(kB, "EDF6", 1, 0, p.data(), p.size(), 1);
    CHECK(waitFor([&] { return m.a.pathTo(kB) == dn::Path::Direct; }, 10000));
    std::vector<dn::Path> paths;
    // 400 datagrams 10 ms apart (4 s); from the 50th on (0.5 s) the direct link loses everything for 1.5 s.
    const Arrivals got = stream(m.a, kB, m.b, 1, 400, 10, [&](uint32_t i) {
        if (i == 50) {
            m.a.setTestBlockPeers(0, 1500);
            m.b.setTestBlockPeers(0, 1500);
        }
        if (i % 20 == 0) paths.push_back(m.a.pathTo(kB));
    });
    const bool wentRelay = std::find(paths.begin(), paths.end(), dn::Path::Relay) != paths.end();
    printf("  %zu of 400 arrived, longest gap %llu ms, went through the host %s, back on the direct link %s\n",
           got.ids.size(), static_cast<unsigned long long>(got.longestGapMs), wentRelay ? "yes" : "NO",
           m.a.pathTo(kB) == dn::Path::Direct ? "yes" : "NO");
    CHECK(wentRelay);
    CHECK(waitFor([&] { return m.a.pathTo(kB) == dn::Path::Direct; }, 5000));
    // Lost only until the direct link turned suspect (two pings' time) and the relay took copies: a gap of well
    // under a second, against 1.5 s of outage.
    CHECK(got.ids.size() >= 400 - 80);
    CHECK(got.longestGapMs < 900);
    // Events: both paths, the receiver gets two copies of each while both work (the filter drops the second).
    dn::DuplicateFilter filter;
    uint32_t firsts = 0, copies = 0;
    for (uint32_t i = 0; i < 50; ++i) {
        std::vector<uint8_t> e(60, 0xE0);
        memcpy(e.data(), &i, 4);
        const dn::SendReport r = m.a.sendClassified(kB, "EDF6", 1, 0, e.data(), e.size(), 2);
        CHECK(r.sent && r.paths == 2);
    }
    waitFor(
        [&] {
            dn::Delivered d;
            uint8_t ch = 1;
            while (m.b.pop(&ch, 1170, d)) (filter.first(d.src, d.data.data(), d.data.size(), GetTickCount64()) ? firsts : copies)++;
            return firsts + copies >= 100;
        },
        5000);
    printf("  events: %u delivered once, %u copies dropped\n", firsts, copies);
    CHECK(firsts == 50 && copies == 50);
}

void testMeshAndShedSwitches() {
    printf("mesh: [Netcode] ShedState and Mesh act after start\n");
    Mesh m;
    CHECK(m.start());
    std::vector<uint8_t> big(1000, 0x33);
    auto burst = [&] {
        m.host.takeWireTraffic();
        for (int i = 0; i < 300; ++i) m.host.sendClassified(kA, "EDF6", 1, 0, big.data(), big.size(), 1);  // 300 KB at once
        return m.host.takeWireTraffic().stateShed;
    };
    const uint64_t shedOn = burst();
    printf("  a 300 KB burst of state over a fresh path: %llu dropped for its budget\n", static_cast<unsigned long long>(shedOn));
    CHECK(shedOn > 0);
    m.host.setShedState(false);
    CHECK(burst() == 0);
    // Mesh: linked, then switched off - the direct link closes, the relay carries.
    auto p = payloadFor(1);
    m.a.sendClassified(kB, "EDF6", 1, 0, p.data(), p.size(), 1);
    CHECK(waitFor([&] { return m.a.peerLinked(kB); }, 10000));
    m.a.setMesh(false);
    m.b.setMesh(false);
    CHECK(!m.a.peerLinked(kB) && m.a.pathTo(kB) == dn::Path::Relay);
    m.a.sendClassified(kB, "EDF6", 1, 0, p.data(), p.size(), 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    CHECK(!m.a.peerLinked(kB));  // no new link while off
}

void testRosterPages() {
    printf("wire: member lists of large rooms go in pages\n");
    dn::Message m;
    m.type = dn::MsgType::Roster;
    m.roster.hostNonce = 3;
    for (int i = 0; i < 32; ++i) m.roster.roster.push_back("member" + std::to_string(i));
    m.roster.version = 9;
    m.roster.total = 1000;
    m.roster.offset = 960;
    auto dg = dn::encode(m, "");
    dn::DecodeError err;
    auto back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->roster.total == 1000 && back->roster.offset == 960 && back->roster.version == 9 &&
          back->roster.roster.size() == 32);
    m.roster.offset = 980;  // runs past the list: no valid sender writes it
    dg = dn::encode(m, "");
    CHECK(!dn::decode(dg.data(), dg.size(), "", &err) && err == dn::DecodeError::Malformed);
    dn::Message q;
    q.type = dn::MsgType::PeerInfo;
    q.peer = {kB, "[2001:db8::1]:27015"};
    dg = dn::encode(q, "");
    back = dn::decode(dg.data(), dg.size(), "", &err);
    CHECK(back && back->peer.puid == kB && back->peer.address == q.peer.address);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--mesh") {
        testRosterPages();
        testMeshLinksJoiners();
        testMeshBlockedFromTheStart();
        testMeshFailsOverAndBack();
        testMeshAndShedSwitches();
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--receive-lifecycle") {
        testReceiveStateFollowsSession();
        printf("\n%d checks, %d failures\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
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
    testRetransmitsFollowDelivery();
    testAckCoversLongGap();
    testAbandonAndSkip();
    testDirectUpgradesUnreliable();
    testBottleneckDoesNotCollapse();
    testDisconnectHold();
    testLobbyStatusHold();
    testLobbyStatusHoldKeys();
    testLobbyOwnerPin();
    testTrafficMeter();
    testUpdater();
    testUpdateSigning();
    dn::statusToSink(&recordMenuStatus);
    testMenuStatus();
    testUpdateRollback();
    testSwapKeepsDllLoadable();
    testUpnpAddresses();
    testConfigParsing();
    testRosterDropsQuietMember();
    testLinksFollowTheRoom();
    testReceiveStateFollowsSession();
    testHostRestart();
    testStalledLinkSurvives();
    testReplyFromOtherAddress();
    testHelloCannotHijackLiveLink();
    testHelloFloodIsBounded();
    testThreeNodesOverLoopback();
    testFullRoom();
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
    testReplayWindow();
    testLinkTagSpeed();
    testWelcomeProvesTheRoomOwner();
    testLinkPacketsAuthenticated();
    testRoomView();
    testRoomWire();
    testFakeLobbies();
    testRoomFollowsHost();
    testRosterPages();
    testMeshLinksJoiners();
    testMeshBlockedFromTheStart();
    testMeshFailsOverAndBack();
    testMeshAndShedSwitches();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
