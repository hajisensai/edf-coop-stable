// EDF6DirectNet tests. Build with build.ps1 and run build\edf6_directnet_tests.exe [path-to-EDF.dll].
#include <winsock2.h>
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
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
        if (i % 50 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
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
    a.setTestBlackhole(false);
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

void testDisconnectHold() {
    printf("hold: transient disconnects are hidden from the game and recovered\n");
    dn::DisconnectHold hold({true, 30000, 2000});
    int forwarded = 0, reaccepts = 0;
    auto fwd = [&] { ++forwarded; };
    auto re = [&] { ++reaccepts; };

    CHECK(!hold.offer(kA, 3, 0, fwd, re));  // never connected: a failed first handshake must reach the game
    hold.onEstablished(kA, 0);
    CHECK(!hold.offer(kA, 2, 0, fwd, re));    // ClosedByPeer: the player really left
    CHECK(!hold.offer(kA, 1, 0, fwd, re));    // ClosedByLocalUser
    CHECK(hold.offer(kA, 3, 1000, fwd, re));  // TimedOut: held
    CHECK(hold.isHeld(kA) && reaccepts == 0);  // no EOS call from inside the EOS callback
    hold.poll(1001);
    CHECK(reaccepts == 1);
    hold.poll(2000);
    CHECK(reaccepts == 1);
    hold.poll(3002);
    CHECK(reaccepts == 2);
    CHECK(hold.onEstablished(kA, 5000) == 1 && !hold.isHeld(kA) && forwarded == 0);  // game saw nothing

    CHECK(hold.offer(kA, 7, 10000, fwd, re));  // ConnectionFailed
    CHECK(hold.poll(39999).empty() && forwarded == 0);
    auto expired = hold.poll(40000);
    CHECK(expired.size() == 1 && forwarded == 1 && !hold.isHeld(kA));
    CHECK(!hold.offer(kA, 3, 50000, fwd, re));  // after expiry the peer is gone until re-established

    hold.onEstablished(kB, 0);
    CHECK(hold.offer(kB, 3, 0, fwd, re));
    CHECK(hold.onGameClosed(kB) == 1 && !hold.isHeld(kB));
    hold.poll(100000);
    CHECK(forwarded == 1);  // the game closed it itself: nothing forwarded

    dn::DisconnectHold off({false, 30000, 2000});
    off.onEstablished(kA, 0);
    CHECK(!off.offer(kA, 3, 0, fwd, re));
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
    CHECK(dn::findImportSlot(edf, dll, "EOS_P2P_NoSuchFunction") == nullptr);
    CHECK(dn::findImportSlot(edf, "nosuch.dll", "EOS_P2P_SendPacket") == nullptr);
    FreeLibrary(edf);
}

void testNetif() {
    printf("netif: physical adapter\n");
    auto pi = dn::findPhysicalInterface();
    CHECK(pi.has_value());
    if (pi) {
        printf("  %s | %s | ipv4=%s v4idx=%u v6idx=%u ipv6=%zu\n", pi->name.c_str(), pi->description.c_str(),
               pi->ipv4.c_str(), pi->ifIndexV4, pi->ifIndexV6, pi->globalIpv6.size());
        CHECK(pi->description.find("Clash") == std::string::npos);
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

    FILE* f = _wfopen(path.c_str(), L"wb");
    fputs("[DirectNet]\r\nMode= Join \r\nHostAddress=[2408:8207::5]:30000\r\nKey=abc\r\n"
          "[EOS]\r\nFixedPort=27100\r\nRelay=NoRelay\r\n", f);
    fclose(f);
    dn::Config c = dn::loadConfig(path);
    CHECK(c.direct.mode == dn::Mode::Join);
    CHECK(c.direct.hostAddress == "[2408:8207::5]:30000");
    CHECK(c.direct.listenPort == 0);  // join without ListenPort binds any port
    CHECK(c.direct.key == "abc" && c.eosFixedPort == 27100 && c.eosRelay == 0);
    DeleteFileW(path.c_str());
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    const wchar_t* edf = argc > 1 ? argv[1] : L"D:\\steam\\steamapps\\common\\EARTH DEFENSE FORCE 6\\EDF.dll";
    testWire();
    testReliableUnderLoss();
    testConfig();
    testNetif();
    testIat(edf);
    testKeyMismatch();
    testDisconnectHold();
    testHostRestart();
    testStalledLinkSurvives();
    testThreeNodesOverLoopback();
    printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
