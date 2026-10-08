#include "../src/direct_net.h"
#include "../src/extension_bridge.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>

extern "C" uint32_t EDF6COOP_CALL EDF6CoopGetExtensionApi(uint32_t, uint32_t, EDF6CoopExtensionApi*) noexcept;
namespace {
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
const std::string hostId = "0002aaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const std::string aId = "0002bbbbbbbbbbbbbbbbbbbbbbbbbbbb";
const std::string bId = "0002cccccccccccccccccccccccccccc";
bool waitFor(const std::function<bool()>& predicate, unsigned timeout = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < end);
    return false;
}
EDF6CoopSnapshot snap(dn::DirectNet& net) {
    EDF6CoopSnapshot result{}; net.extensionSnapshot(result); return result;
}
}
int main() {
    static_assert(sizeof(EDF6CoopPeer) == 65);
    static_assert(sizeof(EDF6CoopSnapshot) == 160);
    static_assert(sizeof(EDF6CoopExtensionApi) == 40);
    EDF6CoopExtensionApi api{};
    CHECK(!EDF6CoopGetExtensionApi(2, sizeof(api), &api));
    CHECK(!EDF6CoopGetExtensionApi(1, sizeof(api) - 1, &api));
    CHECK(EDF6CoopGetExtensionApi(1, sizeof(api), &api));
    EDF6CoopSnapshot unavailable{};
    CHECK(!api.snapshot(&unavailable));
    unavailable.size = sizeof(unavailable);
    CHECK(api.snapshot(&unavailable) && !unavailable.ready);

    dn::DirectNet host, a, b;
    dn::DirectOptions ho;
    ho.mode = dn::Mode::Host;
    ho.listenPort = 0;
    ho.pingIntervalMs = 100;
    ho.identity = dn::Identity::generate();
    auto ai = dn::Identity::generate(), bi = dn::Identity::generate();
    ho.memberIds = {{aId, ai->commitment()}, {bId, bi->commitment()}};
    CHECK(host.start(ho)); host.setLocalUser(hostId);
    dn::DirectOptions jo;
    jo.mode = dn::Mode::Join; jo.listenPort = 0; jo.pingIntervalMs = 100;
    jo.hostAddress = "127.0.0.1:" + std::to_string(host.boundPort());
    jo.roomOwner = hostId; jo.roomOwnerIdentity = ho.identity->commitment();
    jo.identity = ai;
    CHECK(a.start(jo)); a.setLocalUser(aId);
    jo.identity = bi;
    CHECK(b.start(jo)); b.setLocalUser(bId);
    const std::vector<std::string> members{hostId, aId, bId};
    auto tick = [&] {
        host.setExtensionRoom("room-1", hostId, members);
        a.setExtensionRoom("room-1", hostId, members);
        b.setExtensionRoom("room-1", hostId, members);
    };
    CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));
    auto hs = snap(host), as = snap(a), bs = snap(b);
    CHECK(hs.isHost && hs.peerCount == 2 && !as.isHost && as.peerCount == 2);
    CHECK(hs.generation != as.generation && as.generation != bs.generation);
    const std::vector<std::string> missionMembers{hostId, aId};
    auto missionTick = [&] {
        host.setExtensionRoom("room-1", hostId, missionMembers, 42);
        a.setExtensionRoom("room-1", hostId, missionMembers, 42);
    };
    missionTick();
    auto missionHost = snap(host), missionA = snap(a);
    CHECK(missionHost.ready && missionA.ready && missionHost.peerCount == 1 && missionA.peerCount == 1);
    host.setMemberIdentities({{aId, ai->commitment()}}); // lobby-only member's identity vanishes
    missionTick();
    CHECK(snap(host).generation == missionHost.generation);
    b.setActive(false);
    CHECK(waitFor([&] { missionTick(); return !host.canRoute(bId); }));
    CHECK(snap(host).ready && snap(a).ready && snap(host).generation == missionHost.generation &&
        snap(a).generation == missionA.generation);
    host.setMemberIdentities(ho.memberIds);
    b.setActive(true);
    CHECK(waitFor([&] { missionTick(); return host.canRoute(bId); }));
    CHECK(snap(host).generation == missionHost.generation && snap(a).generation == missionA.generation);
    host.setExtensionRoom("room-1", hostId, missionMembers, 43); // same addresses, new world lifetime
    CHECK(snap(host).generation != missionHost.generation);
    tick(); hs = snap(host); as = snap(a); bs = snap(b);
    EDF6CoopPeer peer{};
    CHECK(host.extensionPeer(hs.generation, 0, peer) && std::string(peer.id) == aId);
    CHECK(!host.extensionPeer(hs.generation, 2, peer));
    uint8_t byte = 9;
    CHECK(!host.extensionSend(hs.generation - 1, aId, &byte, 1));
    CHECK(!host.extensionSend(hs.generation, "unknown", &byte, 1));
    CHECK(!host.extensionSend(hs.generation, aId, &byte, 1025));
    CHECK(!host.extensionSend(hs.generation, aId, nullptr, 1));
    CHECK(a.extensionSend(as.generation, bId, &byte, 1));
    dn::Delivered message;
    CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 1, message); }));
    CHECK(message.src == aId && message.data == std::vector<uint8_t>{9});
    CHECK(!host.pop(nullptr, 2048, message) && !a.pop(nullptr, 2048, message) && !b.pop(nullptr, 2048, message));
    CHECK(host.extensionSend(hs.generation, aId, &byte, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!a.pop(nullptr, 2048, message)); // wildcard game read cannot steal extension data
    CHECK(!a.extensionPoll(as.generation, 0, message)); // short buffers do not consume
    CHECK(a.extensionPoll(as.generation, 1, message) && message.src == hostId);
    CHECK(host.send(aId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return a.pop(nullptr, 2048, message); }));
    CHECK(message.socketName == "EDF6");
    CHECK(host.send(aId, "wrong", dn::DirectNet::kExtensionChannel, 2, &byte, 1));
    CHECK(host.send(aId, dn::DirectNet::kExtensionSocket, 3, 2, &byte, 1));
    CHECK(host.send(aId, dn::DirectNet::kExtensionSocket, dn::DirectNet::kExtensionChannel, 1, &byte, 1));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!a.pop(nullptr, 2048, message));
    CHECK(!a.extensionPoll(as.generation, 2048, message));

    // Ordered delivery survives UDP loss; many packets use the same relay stream.
    // Blackhole briefly so all packets need the production retransmission path.
    a.setTestBlackhole(true);
    for (uint8_t i = 0; i < 32; ++i) CHECK(a.extensionSend(as.generation, bId, &i, 1));
    a.setTestBlackhole(false);
    unsigned received = 0;
    CHECK(waitFor([&] {
        tick();
        while (b.extensionPoll(bs.generation, 1, message)) {
            CHECK(message.src == aId && message.data[0] == received); ++received;
        }
        return received == 32;
    }));

    dn::bindExtensionTransport(std::shared_ptr<dn::DirectNet>(&host, [](dn::DirectNet*){}));
    EDF6CoopSnapshot bridged{}; bridged.size = sizeof(bridged);
    CHECK(api.snapshot(&bridged) && bridged.ready);
    CHECK(api.peer(bridged.generation, 0, &peer));
    CHECK(api.send(bridged.generation, &peer, &byte, 1));
    dn::invalidateExtensionTransport();
    CHECK(!api.send(bridged.generation, &peer, &byte, 1));
    CHECK(api.snapshot(&bridged) && !bridged.ready);
    tick(); hs = snap(host);
    CHECK(hs.generation != bridged.generation);

    // Room change/host migration/key rebinding/leave reject cached generations.
    const auto oldGeneration = snap(a).generation;
    a.setExtensionRoom("room-2", bId, members);
    CHECK(!snap(a).ready && snap(a).generation != oldGeneration);
    CHECK(!a.extensionSend(oldGeneration, hostId, &byte, 1));
    tick(); as = snap(a);
    host.setMemberIdentities({{aId, dn::Identity::generate()->commitment()}, {bId, bi->commitment()}});
    CHECK(!snap(host).ready);
    host.setMemberIdentities(ho.memberIds); tick();
    CHECK(snap(host).ready);
    b.setActive(false);
    CHECK(waitFor([&] { tick(); return !snap(host).ready && !snap(a).ready; }));
    CHECK(!a.extensionSend(as.generation, bId, &byte, 1));
    CHECK(!b.extensionPoll(bs.generation, 2048, message));
    b.setActive(true);
    CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));
    auto freshB = snap(b);
    CHECK(freshB.generation != bs.generation);
    CHECK(!b.extensionPoll(bs.generation, 2048, message));
    hs = snap(host);
    // The dedicated queue is bounded; overflow invalidates authority instead of
    // silently dropping an arbitrary part of a reliable extension transaction.
    for (unsigned i = 0; i < 257; ++i) {
        tick();
        CHECK(host.extensionSend(hs.generation, bId, &byte, 1));
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(waitFor([&] { tick(); return !snap(b).ready; }));
    CHECK(!b.extensionPoll(freshB.generation, 2048, message));
    b.clearExtensionRoom(); tick();
    CHECK(snap(b).ready);
    // EOS room facts cannot remain authoritative while the game's tick stopped.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    CHECK(!snap(a).ready && !snap(host).ready);
    tick(); hs = snap(host);
    host.setTestBlackhole(true);
    std::vector<uint8_t> full(EDF6COOP_EXTENSION_MAX_PAYLOAD, 7);
    unsigned accepted = 0;
    while (accepted < 300 && host.extensionSend(hs.generation, aId, full.data(), static_cast<uint32_t>(full.size())))
        ++accepted;
    CHECK(accepted > 0 && accepted < 300 && !snap(host).ready);
    host.setTestBlackhole(false);
    host.clearExtensionRoom();
    CHECK(!snap(host).ready);
    a.stop(); b.stop(); host.stop();
    dn::shutdownExtensionTransport();
    unavailable.size = sizeof(unavailable);
    CHECK(!api.snapshot(&unavailable) && !unavailable.ready);
    printf("extension transport: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
