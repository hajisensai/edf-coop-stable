#include "../src/direct_net.h"
#include "../src/extension_bridge.h"
#include "../src/netclass.h"
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <thread>
#include "loopback_guard.h"

extern "C" uint32_t EDF6COOP_CALL EDF6CoopGetExtensionApi(uint32_t, uint32_t, EDF6CoopExtensionApi*) noexcept;
extern "C" uint32_t EDF6COOP_CALL EDF6CoopGetExtensionCapacity(uint32_t, uint32_t, EDF6CoopExtensionCapacity*) noexcept;
extern "C" uint32_t EDF6COOP_CALL EDF6CoopTrySendExtension(uint64_t, const EDF6CoopPeer*, const void*, uint32_t, uint32_t, uint32_t) noexcept;
extern "C" uint32_t EDF6COOP_CALL EDF6CoopPollExtensionBatch(uint64_t, EDF6CoopExtensionMessage*, uint32_t, void*, uint32_t, uint32_t*) noexcept;
// Observation of real production queues under the same mutex; no fake send path.
namespace dn {
struct ExtensionTransportTestAccess {
    static void dropTransportAcks(DirectNet& net, bool on) { net.testDropAcks_.store(on); }
    static uint64_t faults(DirectNet& net) {
        std::lock_guard<std::mutex> lock(net.mu_); return net.extensionFaults_;
    }
    static size_t inbox(DirectNet& net) {
        std::lock_guard<std::mutex> lock(net.mu_); return net.extensionInbox_.size();
    }
    static DirectNet::Link& route(DirectNet& net, const std::string& peer) {
        return net.opt_.mode == Mode::Host ? net.clients_.at(peer) : *net.hostLink_;
    }
    // The link's extension ordered domain (what the extension ceiling and TrySend measure).
    static std::pair<size_t, size_t> pending(DirectNet& net, const std::string& peer) {
        std::lock_guard<std::mutex> lock(net.mu_);
        auto& link = route(net, peer);
        return {link.xtx.pendingBytes(), link.xtx.pendingCount()};
    }
    // The link's game reliable domain.
    static std::pair<size_t, size_t> gamePending(DirectNet& net, const std::string& peer) {
        std::lock_guard<std::mutex> lock(net.mu_);
        auto& link = route(net, peer);
        return {link.tx.pendingBytes(), link.tx.pendingCount()};
    }
    static size_t reliableQueued(DirectNet& net) {
        std::lock_guard<std::mutex> lock(net.mu_); return net.extensionReliableQueued_;
    }
    static uint64_t stateSeen(DirectNet& net, const std::string& src, uint32_t key) {
        std::lock_guard<std::mutex> lock(net.mu_);
        const auto it = net.extensionStateSeen_.find({src, key});
        return it == net.extensionStateSeen_.end() ? 0 : it->second;
    }
    static size_t stateKeys(DirectNet& net) {
        std::lock_guard<std::mutex> lock(net.mu_); return net.extensionStateSeen_.size();
    }
    // A state datagram from `src` as the authenticated receive path hands it over (any sequence).
    static void injectState(DirectNet& net, const std::string& src, uint32_t key, uint64_t seq, uint32_t value) {
        std::lock_guard<std::mutex> lock(net.mu_);
        DataMsg msg;
        msg.src = src;
        msg.dst = net.localPuid_;
        msg.socketName = DirectNet::kExtensionSocket;
        msg.channel = DirectNet::kExtensionChannel;
        msg.reliability = 0;
        msg.cls = static_cast<uint8_t>(TrafficClass::State);
        msg.payload.resize(DirectNet::kExtensionStateHeader + sizeof(value));
        memcpy(msg.payload.data(), &key, 4);
        memcpy(msg.payload.data() + 4, &seq, 8);
        memcpy(msg.payload.data() + DirectNet::kExtensionStateHeader, &value, sizeof(value));
        net.extensionPacket(msg);
    }
    // A reliable ordered datagram whose first transmission was lost: its sequence is taken in the game or the
    // extension domain of the link, and nothing goes out (or is tracked) until releaseHeld sends it.
    static std::vector<uint8_t> holdNumbered(DirectNet& net, const std::string& peer, bool extension, uint8_t value) {
        std::lock_guard<std::mutex> lock(net.mu_);
        auto& link = route(net, peer);
        Message m;
        m.type = MsgType::Data;
        m.epoch = link.epoch;
        m.data.src = net.localPuid_;
        m.data.dst = peer;
        m.data.socketName = extension ? DirectNet::kExtensionSocket : "EDF6";
        m.data.channel = extension ? DirectNet::kExtensionChannel : 1;
        m.data.reliability = 2;
        m.data.payload = {value};
        m.data.seq = extension ? link.xtx.nextSeq() : link.tx.nextSeq();
        return encode(m, net.opt_.key);
    }
    static void releaseHeld(DirectNet& net, const std::string& peer, std::vector<uint8_t> dg) {
        std::lock_guard<std::mutex> lock(net.mu_);
        net.sendSealed(route(net, peer), dg);
    }
    static bool routingCongestionDoesNotInvalidate(DirectNet& net, const std::string& peer) {
        std::lock_guard<std::mutex> lock(net.mu_);
        auto& link = net.clients_.at(peer);
        const auto original = link.cc;
        link.cc = {};
        link.cc.onRtt(1, 10000); link.cc.onRtt(201, 10001);
        const auto generation = net.extensionGeneration_;
        const bool result = link.cc.queueMs() > 150 && net.congested(link) &&
            net.refreshExtensionLocked() && net.extensionGeneration_ == generation;
        link.cc = original;
        return result;
    }
};
}
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
    testguard::LoopbackGuard loopbackGuard;  // every UDP socket of this test on loopback (loopback_guard.h)
    static_assert(sizeof(EDF6CoopPeer) == 65);
    static_assert(sizeof(EDF6CoopSnapshot) == 160);
    static_assert(sizeof(EDF6CoopExtensionApi) == 48);
    static_assert(offsetof(EDF6CoopExtensionApi, snapshot) == 8);
    static_assert(offsetof(EDF6CoopExtensionApi, poll) == 32);
    static_assert(offsetof(EDF6CoopExtensionApi, sendState) == 40);
    static_assert(sizeof(EDF6CoopExtensionMessage) == 20);
    static_assert(offsetof(EDF6CoopExtensionMessage, flags) == 12 && offsetof(EDF6CoopExtensionMessage, key) == 16);
    static_assert(EDF6COOP_EXTENSION_VERSION == 2);
    static_assert(sizeof(EDF6CoopExtensionCapacity) == 32);
    static_assert(offsetof(EDF6CoopExtensionCapacity, flags) == 28);
    EDF6CoopExtensionCapacity capacity{};
    memset(&capacity, 0xA5, sizeof(capacity));
    const auto untouched = capacity;
    CHECK(!EDF6CoopGetExtensionCapacity(1, sizeof(capacity), nullptr));
    CHECK(!EDF6CoopGetExtensionCapacity(0, sizeof(capacity), &capacity));
    CHECK(!EDF6CoopGetExtensionCapacity(2, sizeof(capacity), &capacity));
    CHECK(!EDF6CoopGetExtensionCapacity(1, sizeof(capacity) - 1, &capacity));
    CHECK(!EDF6CoopGetExtensionCapacity(1, sizeof(capacity) + 1, &capacity));
    CHECK(memcmp(&capacity, &untouched, sizeof(capacity)) == 0);
    CHECK(EDF6CoopGetExtensionCapacity(1, sizeof(capacity), &capacity));
    CHECK(capacity.size == 32 && capacity.version == 1 && capacity.maxPayload == 1024 &&
        capacity.maxPeers == 1024 && capacity.inboxMessages == 4096 && capacity.pendingBytes == 1048576 &&
        capacity.congestedBacklogBytes == 1048576 && capacity.flags == 15);
    EDF6CoopExtensionApi api{};
    CHECK(!EDF6CoopGetExtensionApi(2, sizeof(api), nullptr));
    CHECK(!EDF6CoopGetExtensionApi(1, 40, &api)); // version 1 is not served at all
    CHECK(!EDF6CoopGetExtensionApi(1, sizeof(api), &api));
    CHECK(!EDF6CoopGetExtensionApi(3, sizeof(api), &api));
    CHECK(!EDF6CoopGetExtensionApi(2, sizeof(api) - 1, &api));
    CHECK(EDF6CoopGetExtensionApi(2, sizeof(api), &api));
    CHECK(api.size == 48 && api.version == 2 && api.snapshot && api.peer && api.send && api.poll && api.sendState);
    EDF6CoopSnapshot unavailable{};
    CHECK(!api.snapshot(&unavailable));
    unavailable.size = sizeof(unavailable);
    CHECK(api.snapshot(&unavailable) && !unavailable.ready);

    dn::DirectNet host, a, b;
    dn::DirectOptions ho;
    ho.mode = dn::Mode::Host;
    ho.listenPort = 0;
    ho.pingIntervalMs = 100;
    ho.testBindLoopback = dn::DirectOptions::Loopback::V4;  // loopback only: no firewall prompt
    ho.identity = dn::Identity::generate();
    auto ai = dn::Identity::generate(), bi = dn::Identity::generate();
    ho.memberIds = {{aId, ai->commitment()}, {bId, bi->commitment()}};
    CHECK(host.start(ho)); host.setLocalUser(hostId);
    dn::DirectOptions jo;
    jo.mode = dn::Mode::Join; jo.listenPort = 0; jo.pingIntervalMs = 100;
    jo.testBindLoopback = dn::DirectOptions::Loopback::V4;
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

    // A burst above the old 256-message inbox survives production retransmission
    // and the host relay, retaining every payload in order before the first poll.
    a.setTestBlackhole(true);
    for (uint32_t i = 0; i < 512; ++i)
        CHECK(a.extensionSend(as.generation, bId, reinterpret_cast<const uint8_t*>(&i), sizeof(i)));
    a.setTestBlackhole(false);
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::inbox(b) == 512; }));
    CHECK(snap(b).ready && snap(b).generation == bs.generation);
    for (uint32_t i = 0; i < 512; ++i) {
        CHECK(b.extensionPoll(bs.generation, sizeof(i), message));
        uint32_t value = UINT32_MAX;
        if (message.data.size() == sizeof(value)) memcpy(&value, message.data.data(), sizeof(value));
        CHECK(message.src == aId && value == i);
    }
    CHECK(dn::ExtensionTransportTestAccess::inbox(b) == 0);
    CHECK(dn::ExtensionTransportTestAccess::routingCongestionDoesNotInvalidate(host, aId));

    dn::bindExtensionTransport(std::shared_ptr<dn::DirectNet>(&host, [](dn::DirectNet*){}));
    EDF6CoopSnapshot bridged{}; bridged.size = sizeof(bridged);
    CHECK(api.snapshot(&bridged) && bridged.ready);
    CHECK(EDF6CoopGetExtensionCapacity(1, sizeof(capacity), &capacity));
    CHECK(snap(host).generation == bridged.generation);
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
    // Exactly the advertised inbox fits without truncation or generation change.
    const auto fillInbox = [&] {
        for (uint32_t i = 0; i < capacity.inboxMessages; ++i) {
            tick();
            CHECK(host.extensionSend(hs.generation, bId, reinterpret_cast<const uint8_t*>(&i), sizeof(i)));
            if ((i + 1) % 32 == 0) {
                CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::inbox(b) == i + 1; }));
            }
        }
    };
    fillInbox();
    CHECK(snap(b).ready && snap(b).generation == freshB.generation);
    CHECK(!b.extensionPoll(freshB.generation, 3, message));
    CHECK(dn::ExtensionTransportTestAccess::inbox(b) == capacity.inboxMessages);
    for (uint32_t i = 0; i < capacity.inboxMessages; ++i) {
        CHECK(b.extensionPoll(freshB.generation, sizeof(i), message));
        uint32_t value = UINT32_MAX;
        if (message.data.size() == sizeof(value)) memcpy(&value, message.data.data(), sizeof(value));
        CHECK(message.src == hostId && value == i);
    }
    fillInbox();
    CHECK(host.extensionSend(hs.generation, bId, &byte, 1));
    CHECK(waitFor([&] { tick(); return snap(b).generation != freshB.generation; }));
    CHECK(dn::ExtensionTransportTestAccess::inbox(b) == 0);
    CHECK(!b.extensionPoll(freshB.generation, 2048, message));
    // The overflow renews the generation, never latches the room: nothing is queued on B's link, so
    // the same room facts make B ready again at once, under a generation no earlier message can use.
    CHECK(waitFor([&] { tick(); return snap(b).ready; }));
    const auto recoveredB = snap(b);
    CHECK(recoveredB.generation != freshB.generation && dn::ExtensionTransportTestAccess::faults(b) == 1);
    CHECK(!b.extensionPoll(freshB.generation, 2048, message));

    // Batch poll: one lock, payloads packed in arrival order, sender as the api.peer index.
    {
        hs = snap(host); as = snap(a);
        const uint32_t values[3] = {11, 22, 33};
        // One at a time: A's goes through the host's relay, so its arrival order is fixed by waiting.
        CHECK(host.extensionSend(hs.generation, bId, reinterpret_cast<const uint8_t*>(&values[0]), 4));
        CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::inbox(b) == 1; }));
        CHECK(a.extensionSend(as.generation, bId, reinterpret_cast<const uint8_t*>(&values[1]), 4));
        CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::inbox(b) == 2; }));
        CHECK(host.extensionSend(hs.generation, bId, reinterpret_cast<const uint8_t*>(&values[2]), 4));
        CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::inbox(b) == 3; }));
        dn::bindExtensionTransport(std::shared_ptr<dn::DirectNet>(&b, [](dn::DirectNet*){}));
        EDF6CoopExtensionMessage items[4]{};
        uint8_t packed[16]{};
        uint32_t count = 99;
        const auto bGen = snap(b).generation;
        CHECK(!EDF6CoopPollExtensionBatch(bGen - 1, items, 4, packed, sizeof(packed), &count) && count == 0);
        CHECK(!EDF6CoopPollExtensionBatch(bGen, nullptr, 4, packed, sizeof(packed), &count));
        // A buffer with room for two payloads takes two; the third stays queued.
        CHECK(EDF6CoopPollExtensionBatch(bGen, items, 4, packed, 8, &count) && count == 2);
        EDF6CoopPeer hostPeer{}, aPeer{};
        CHECK(b.extensionPeer(bGen, items[0].peer, hostPeer) && hostId == hostPeer.id);
        CHECK(items[0].offset == 0 && items[0].bytes == 4 && items[1].offset == 4 && items[1].bytes == 4);
        CHECK(!items[0].flags && !items[0].key && !items[1].flags && !items[1].key);
        CHECK(b.extensionPeer(bGen, items[1].peer, aPeer) && aId == aPeer.id);
        uint32_t got[2]{};
        memcpy(got, packed, sizeof(got));
        CHECK(got[0] == 11 && got[1] == 22 && dn::ExtensionTransportTestAccess::inbox(b) == 1);
        CHECK(EDF6CoopPollExtensionBatch(bGen, items, 4, packed, sizeof(packed), &count) && count == 1);
        memcpy(got, packed, 4);
        CHECK(got[0] == 33 && items[0].offset == 0);
        CHECK(EDF6CoopPollExtensionBatch(bGen, items, 4, packed, sizeof(packed), &count) && count == 0);
        dn::bindExtensionTransport(nullptr);
    }

    // State messages (sendState): unnumbered, only the newest per (sender, key), never retransmitted and
    // outside every backlog; and the two ordered domains of a link never hold each other back.
    {
        using Access = dn::ExtensionTransportTestAccess;
        CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));
        hs = snap(host); as = snap(a); bs = snap(b);
        auto u32 = [](const std::vector<uint8_t>& data) {
            uint32_t value = UINT32_MAX;
            if (data.size() == sizeof(value)) memcpy(&value, data.data(), sizeof(value));
            return value;
        };
        auto bytesOf = [](const uint32_t& value) { return reinterpret_cast<const uint8_t*>(&value); };
        const uint32_t marker = 0xABCD;
        std::vector<uint8_t> tooBig(1025, 1);
        CHECK(!host.extensionSendState(hs.generation - 1, bId, 1, &byte, 1));
        CHECK(!host.extensionSendState(hs.generation, "unknown", 1, &byte, 1));
        CHECK(!host.extensionSendState(hs.generation, bId, 1, &byte, 0));
        CHECK(!host.extensionSendState(hs.generation, bId, 1, nullptr, 1));
        CHECK(!host.extensionSendState(hs.generation, bId, 1, tooBig.data(), 1025));
        CHECK(!b.extensionSendState(bs.generation, bId, 1, &byte, 1)); // not to ourselves

        // Latest only: three states under one key, then a reliable marker; the unread slot holds the newest.
        for (uint32_t v = 1; v <= 3; ++v) CHECK(host.extensionSendState(hs.generation, bId, 1, bytesOf(v), 4));
        CHECK(host.extensionSend(hs.generation, bId, bytesOf(marker), 4));
        CHECK(waitFor([&] { tick(); return Access::reliableQueued(b) == 1; }));
        CHECK(Access::inbox(b) == 2);
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.state && message.key == 1 &&
              message.src == hostId && u32(message.data) == 3);
        CHECK(b.extensionPoll(bs.generation, 2048, message) && !message.state && !message.key && u32(message.data) == marker);
        CHECK(Access::reliableQueued(b) == 0 && Access::inbox(b) == 0);
        CHECK(!host.pop(nullptr, 2048, message) && !b.pop(nullptr, 2048, message)); // never reaches the game

        // Older (or a copy) than the last one taken for (sender, key) is dropped; a newer one is taken.
        const uint64_t taken = Access::stateSeen(b, hostId, 1);
        CHECK(taken != 0);
        Access::injectState(b, hostId, 1, taken - 1, 70);
        Access::injectState(b, hostId, 1, taken, 71);
        CHECK(Access::inbox(b) == 0 && Access::stateSeen(b, hostId, 1) == taken);
        const uint32_t four = 4;
        CHECK(host.extensionSendState(hs.generation, bId, 1, bytesOf(four), 4)); // the sender's sequence only rises
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 1; }));
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.state && u32(message.data) == 4);
        CHECK(Access::stateSeen(b, hostId, 1) > taken);
        // Reordered while unread: an older one never replaces the queued newer one; a newer one does, in place.
        Access::injectState(b, aId, 50, 100, 1);
        Access::injectState(b, aId, 50, 90, 2);
        CHECK(Access::inbox(b) == 1);
        Access::injectState(b, aId, 50, 110, 3);
        Access::injectState(b, aId, 50, 120, 4);
        Access::injectState(b, aId, 51, 5, 5); // another key is another stream
        CHECK(Access::inbox(b) == 2);
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.src == aId && message.key == 50 &&
              u32(message.data) == 4);
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.key == 51 && u32(message.data) == 5);
        // Keys are per sender: the same key from the host is a separate stream with its own sequence.
        Access::injectState(b, hostId, 50, 1, 6);
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.src == hostId && u32(message.data) == 6);

        // Never retransmitted, never in a backlog: all of the host's UDP is lost under a flood of the largest
        // state messages (twice the extension ceiling); the extension and game queues stay empty, readiness and
        // the generation hold, and once the link is back nothing of it ever arrives.
        CHECK(waitFor([&] { tick(); return Access::pending(host, bId).second == 0 && Access::gamePending(host, bId).second == 0; }));
        std::vector<uint8_t> stateFull(capacity.maxPayload, 5);
        host.setTestBlackhole(true);
        for (unsigned i = 0; i < 2048; ++i)
            CHECK(host.extensionSendState(hs.generation, bId, 2, stateFull.data(), static_cast<uint32_t>(stateFull.size())));
        CHECK(Access::pending(host, bId) == std::make_pair(size_t{0}, size_t{0}));
        CHECK(Access::gamePending(host, bId) == std::make_pair(size_t{0}, size_t{0}));
        CHECK(snap(host).ready && snap(host).generation == hs.generation);
        host.setTestBlackhole(false);
        for (unsigned i = 0; i < 12; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(Access::stateSeen(b, hostId, 2) == 0 && Access::inbox(b) == 0);
        CHECK(host.extensionSendState(hs.generation, bId, 2, bytesOf(marker), 4));
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 1; }));
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.key == 2 && u32(message.data) == marker);

        // Joiner to joiner through the host relay, as state (unnumbered, within the host's relay budget).
        const uint32_t relayed = 0x5151;
        CHECK(a.extensionSendState(as.generation, bId, 9, bytesOf(relayed), 4));
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 1; }));
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.src == aId && message.state &&
              message.key == 9 && u32(message.data) == relayed);
        CHECK(Access::pending(host, bId).second == 0 && Access::pending(a, hostId).second == 0);

        // A lost extension transaction is repaired in its own domain; state sent after it is not held back.
        const auto heldExtension = Access::holdNumbered(host, bId, true, 0x51);
        const uint8_t afterHole = 0x52;
        CHECK(host.extensionSend(hs.generation, bId, &afterHole, 1));
        const uint32_t live = 0x7777;
        CHECK(host.extensionSendState(hs.generation, bId, 7, bytesOf(live), 4));
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 1; }));
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.state && message.key == 7 &&
              u32(message.data) == live);
        for (unsigned i = 0; i < 4; ++i) { tick(); std::this_thread::sleep_for(std::chrono::milliseconds(50)); }
        CHECK(Access::inbox(b) == 0 && !b.extensionPoll(bs.generation, 2048, message)); // held behind the hole
        Access::releaseHeld(host, bId, heldExtension);
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 2; }));
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.data == std::vector<uint8_t>{0x51});
        CHECK(b.extensionPoll(bs.generation, 2048, message) && message.data == std::vector<uint8_t>{afterHole});
        CHECK(waitFor([&] { tick(); return Access::pending(host, bId).second == 0; }));

        // A lost game packet holds back the game's later packets, never a reliable extension message.
        const auto heldGame = Access::holdNumbered(host, bId, false, 0x61);
        const uint8_t gameAfter = 0x62, extensionAfter = 0x63;
        CHECK(host.send(bId, "EDF6", 1, 2, &gameAfter, 1));
        CHECK(host.extensionSend(hs.generation, bId, &extensionAfter, 1));
        CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 2048, message); }));
        CHECK(message.src == hostId && !message.state && message.data == std::vector<uint8_t>{extensionAfter});
        CHECK(waitFor([&] { tick(); return Access::pending(host, bId).second == 0; })); // the ExtAck retired it
        CHECK(!b.pop(nullptr, 2048, message)); // the game's later packet waits for the lost one
        Access::releaseHeld(host, bId, heldGame);
        CHECK(waitFor([&] { tick(); return b.pop(nullptr, 2048, message); }));
        CHECK(message.socketName == "EDF6" && message.data == std::vector<uint8_t>{0x61});
        CHECK(waitFor([&] { tick(); return b.pop(nullptr, 2048, message); }));
        CHECK(message.data == std::vector<uint8_t>{gameAfter});
        CHECK(waitFor([&] { tick(); return Access::gamePending(host, bId).second == 0; }));
        CHECK(snap(b).generation == bs.generation && snap(host).generation == hs.generation);

        // The ABI: sendState and poll's message info, and batch poll's flags and key.
        dn::bindExtensionTransport(std::shared_ptr<dn::DirectNet>(&b, [](dn::DirectNet*){}));
        EDF6CoopPeer hostPeer{}; memcpy(hostPeer.id, hostId.data(), hostId.size());
        const uint32_t viaApi = 0x4242;
        CHECK(!api.sendState(bs.generation, nullptr, 5, &viaApi, 4));
        CHECK(!api.sendState(bs.generation - 1, &hostPeer, 5, &viaApi, 4));
        CHECK(api.sendState(bs.generation, &hostPeer, 5, &viaApi, 4));
        CHECK(waitFor([&] { tick(); return host.extensionPoll(hs.generation, 2048, message); }));
        CHECK(message.src == bId && message.state && message.key == 5 && u32(message.data) == viaApi);
        CHECK(host.extensionSendState(hs.generation, bId, 6, bytesOf(viaApi), 4));
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 1; }));
        EDF6CoopPeer sender{};
        EDF6CoopExtensionMessage info{};
        uint8_t got[8]{};
        CHECK(!api.poll(bs.generation, &sender, got, sizeof(got), nullptr));
        CHECK(!api.poll(bs.generation, &sender, got, 3, &info) && !info.bytes && !info.flags);
        CHECK(api.poll(bs.generation, &sender, got, sizeof(got), &info));
        EDF6CoopPeer indexed{};
        CHECK(hostId == sender.id && b.extensionPeer(bs.generation, info.peer, indexed) && hostId == indexed.id);
        CHECK(info.offset == 0 && info.bytes == 4 && info.flags == EDF6COOP_EXTENSION_MESSAGE_STATE && info.key == 6);
        CHECK(host.extensionSend(hs.generation, bId, bytesOf(marker), 4));
        CHECK(host.extensionSendState(hs.generation, bId, 8, bytesOf(viaApi), 4));
        CHECK(waitFor([&] { tick(); return Access::inbox(b) == 2; }));
        CHECK(api.poll(bs.generation, &sender, got, sizeof(got), &info) && info.flags == 0 && info.key == 0 &&
              info.bytes == 4);
        EDF6CoopExtensionMessage items[2]{};
        uint8_t packed[8]{};
        uint32_t count = 0;
        CHECK(EDF6CoopPollExtensionBatch(bs.generation, items, 2, packed, sizeof(packed), &count) && count == 1);
        CHECK(items[0].flags == EDF6COOP_EXTENSION_MESSAGE_STATE && items[0].key == 8 && items[0].bytes == 4);
        dn::bindExtensionTransport(nullptr);

        // At most EDF6COOP_EXTENSION_MAX_STATE_KEYS (sender, key) pairs per generation; state never faults.
        CHECK(waitFor([&] { tick(); return snap(b).ready; }));
        bs = snap(b);
        for (uint32_t key = 1000; key < 1000 + 2 * EDF6COOP_EXTENSION_MAX_STATE_KEYS &&
             Access::stateKeys(b) < EDF6COOP_EXTENSION_MAX_STATE_KEYS; ++key)
            Access::injectState(b, aId, key, 1, key);
        const size_t queuedStates = Access::inbox(b);
        CHECK(Access::stateKeys(b) == EDF6COOP_EXTENSION_MAX_STATE_KEYS && queuedStates > capacity.inboxMessages &&
              snap(b).generation == bs.generation);
        Access::injectState(b, aId, 999, 1, 999); // a further pair is dropped
        CHECK(Access::inbox(b) == queuedStates && Access::stateSeen(b, aId, 999) == 0);
        Access::injectState(b, aId, 1000, 2, 1); // a tracked pair still takes newer state
        CHECK(Access::stateSeen(b, aId, 1000) == 2 && Access::inbox(b) == queuedStates);
        // A new generation forgets the pairs and their sequences.
        b.clearExtensionRoom();
        CHECK(Access::stateKeys(b) == 0 && Access::inbox(b) == 0);
        CHECK(waitFor([&] { tick(); return snap(b).ready; }));
    }

    // Application Applied replies are Data messages, not transport ACKs. Lose only
    // B's transport ACKs while every application reply still reaches the host:
    // application credits may be empty while the real SDK queue remains full.
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }));
    hs = snap(host); bs = snap(b);
    dn::bindExtensionTransport(std::shared_ptr<dn::DirectNet>(&host, [](dn::DirectNet*){}));
    EDF6CoopPeer bPeer{}; memcpy(bPeer.id, bId.data(), bId.size());
    constexpr uint32_t flowBytes = 768 * 1024, flowPackets = 768;
    auto trySend = [&](const uint8_t* payload, uint32_t size, uint32_t byteLimit = 768 * 1024,
                       uint32_t packetLimit = 768) {
        return EDF6CoopTrySendExtension(hs.generation, &bPeer, payload, size, byteLimit, packetLimit);
    };
    CHECK(EDF6CoopTrySendExtension(hs.generation - 1, &bPeer, &byte, 1, flowBytes, flowPackets) == 0);
    CHECK(EDF6CoopTrySendExtension(hs.generation, nullptr, &byte, 1, flowBytes, flowPackets) == 0);
    CHECK(trySend(nullptr, 1) == 0 && trySend(&byte, 0) == 0 && trySend(&byte, 1025) == 0);
    CHECK(trySend(&byte, 1, 0) == 0 && trySend(&byte, 1, capacity.pendingBytes + 1) == 0);
    CHECK(trySend(&byte, 1, flowBytes, 0) == 0 && trySend(&byte, 1, flowBytes, 8193) == 0);
    dn::ExtensionTransportTestAccess::dropTransportAcks(b, true);
    std::vector<uint8_t> flowPayload(capacity.maxPayload, 6);
    unsigned applied = 0;
    for (; applied < 900; ++applied) {
        const auto result = trySend(flowPayload.data(), static_cast<uint32_t>(flowPayload.size()));
        if (result == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK) break;
        CHECK(result == EDF6COOP_EXTENSION_SEND_ACCEPTED);
        if (result != EDF6COOP_EXTENSION_SEND_ACCEPTED) break;
        CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 2048, message); }));
        CHECK(message.src == hostId && message.data == flowPayload);
        const uint32_t ack = applied;
        CHECK(b.extensionTrySend(bs.generation, hostId, reinterpret_cast<const uint8_t*>(&ack), sizeof(ack),
            flowBytes, flowPackets) == EDF6COOP_EXTENSION_SEND_ACCEPTED);
        CHECK(waitFor([&] { tick(); return host.extensionPoll(hs.generation, 2048, message); }));
        uint32_t receivedAck = UINT32_MAX;
        if (message.data.size() == sizeof(receivedAck)) memcpy(&receivedAck, message.data.data(), sizeof(receivedAck));
        CHECK(message.src == bId && receivedAck == ack);
    }
    const auto heldByLostAcks = dn::ExtensionTransportTestAccess::pending(host, bId);
    CHECK(applied > 512 && applied < flowPackets && heldByLostAcks.second == applied && heldByLostAcks.first <= flowBytes);
    CHECK(trySend(flowPayload.data(), static_cast<uint32_t>(flowPayload.size())) == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK);
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == heldByLostAcks);
    CHECK(snap(host).ready && snap(host).generation == hs.generation);
    // The packet limit is checked against the same actual extension queue too.
    CHECK(trySend(&byte, 1, capacity.pendingBytes, applied) == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK);
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == heldByLostAcks);
    // Game traffic has a backlog of its own: game data held by the same lost ACKs takes the game queue above the
    // extension ceiling, and the extension's queue, admission, readiness and generation do not move.
    constexpr unsigned gamePackets = 1100;  // about 1.2 MB: above the 1 MiB extension ceiling
    for (unsigned i = 0; i < gamePackets; ++i)
        CHECK(host.send(bId, "EDF6", 1, 2, flowPayload.data(), flowPayload.size()));
    CHECK(dn::ExtensionTransportTestAccess::gamePending(host, bId).first > capacity.pendingBytes);
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == heldByLostAcks);
    CHECK(snap(host).ready && snap(host).generation == hs.generation);
    CHECK(trySend(&byte, 1, capacity.pendingBytes, applied) == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK);
    dn::ExtensionTransportTestAccess::dropTransportAcks(b, false);
    // Each domain's backlog is retired by its own acknowledgement: one extension message's ExtAck covers
    // the extension backlog, one game packet's Ack the game backlog. No sequence hole, no reset.
    const uint8_t retire = 3;
    CHECK(host.extensionSend(hs.generation, bId, &retire, 1));
    CHECK(host.send(bId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0 &&
        dn::ExtensionTransportTestAccess::gamePending(host, bId).first == 0; }, 15000));
    CHECK(snap(host).ready && snap(host).generation == hs.generation);
    CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 2048, message); }));
    CHECK(message.src == hostId && message.data == std::vector<uint8_t>{retire});
    CHECK(trySend(&byte, 1) == EDF6COOP_EXTENSION_SEND_ACCEPTED);
    CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 2048, message); }));
    CHECK(message.src == hostId && message.data == std::vector<uint8_t>{byte});
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }));
    unsigned background = 0;
    while (b.pop(nullptr, 2048, message)) ++background;
    CHECK(background == gamePackets + 1);

    // Fill the actual host->B wire backlog while all outgoing host UDP is lost.
    // A can still submit one authenticated relay message; projected overflow must
    // latch the host closed without admitting that message or trimming game data.
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }));
    hs = snap(host); as = snap(a);
    host.setTestBlackhole(true);
    std::vector<uint8_t> full(capacity.maxPayload, 7);
    dn::Message encoded;
    encoded.type = dn::MsgType::Data;
    encoded.data.src = hostId; encoded.data.dst = bId;
    encoded.data.socketName = dn::DirectNet::kExtensionSocket;
    encoded.data.channel = dn::DirectNet::kExtensionChannel;
    encoded.data.reliability = 2; encoded.data.payload = full;
    const size_t wireBytes = dn::encode(encoded, ho.key).size();
    unsigned accepted = 0;
    while (accepted < 2000 && dn::ExtensionTransportTestAccess::pending(host, bId).first + wireBytes <= capacity.pendingBytes) {
        const bool sent = host.extensionSend(hs.generation, bId, full.data(), static_cast<uint32_t>(full.size()));
        CHECK(sent);
        if (!sent) break;
        ++accepted;
        if (accepted == 512) CHECK(snap(host).ready && snap(host).generation == hs.generation);
    }
    CHECK(accepted > 512);
    const auto relayBoundary = dn::ExtensionTransportTestAccess::pending(host, bId);
    CHECK(relayBoundary.first == accepted * wireBytes && relayBoundary.first <= capacity.pendingBytes);
    CHECK(a.extensionSend(as.generation, bId, full.data(), static_cast<uint32_t>(full.size())));
    CHECK(waitFor([&] { tick(); return !snap(host).ready; }));
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == relayBoundary);
    CHECK(!host.extensionSend(hs.generation, bId, &byte, 1));
    // Still over the resume mark while the link is dark: the fault holds, no new generation churns.
    const auto faultedGeneration = snap(host).generation;
    for (unsigned i = 0; i < 3; ++i) tick();
    CHECK(!snap(host).ready && snap(host).generation == faultedGeneration);
    host.setTestBlackhole(false);
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }, 15000));
    CHECK(host.send(bId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return b.pop(nullptr, 2048, message); })); // refusal left no sequence hole
    CHECK(message.socketName == "EDF6" && message.data == std::vector<uint8_t>{byte});
    // Drained: the same room is ready again by itself, under a new generation (no room reset needed).
    CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));
    CHECK(snap(host).generation != hs.generation && snap(host).generation != faultedGeneration);
    CHECK(!host.extensionSend(hs.generation, bId, &byte, 1));
    // The refused messages left no hole in the extension domain either: the next one arrives (after any of
    // the admitted backlog B still holds for its generation).
    const uint8_t afterRefusal = 0x33;
    CHECK(host.extensionSend(snap(host).generation, bId, &afterRefusal, 1));
    CHECK(waitFor([&] {
        tick();
        while (b.extensionPoll(snap(b).generation, 2048, message))
            if (message.data == std::vector<uint8_t>{afterRefusal}) return true;
        return false;
    }, 15000));

    // EOS room facts cannot remain authoritative while the game's tick stopped.
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    CHECK(!snap(a).ready && !snap(host).ready);
    tick(); hs = snap(host);
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, aId).first == 0; }));
    host.setTestBlackhole(true);
    accepted = 0;
    while (accepted < 2000 && host.extensionSend(hs.generation, aId, full.data(), static_cast<uint32_t>(full.size())))
        ++accepted;
    const auto directBoundary = dn::ExtensionTransportTestAccess::pending(host, aId);
    CHECK(accepted > 512 && accepted < 2000 && !snap(host).ready);
    CHECK(directBoundary.first == accepted * wireBytes && directBoundary.first <= capacity.pendingBytes &&
        directBoundary.first + wireBytes > capacity.pendingBytes);
    CHECK(!host.extensionSend(hs.generation, aId, &byte, 1));
    CHECK(dn::ExtensionTransportTestAccess::pending(host, aId) == directBoundary);
    host.setTestBlackhole(false);
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, aId).first == 0; }, 15000));
    CHECK(host.send(aId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return a.pop(nullptr, 2048, message); }));
    host.clearExtensionRoom(); a.clearExtensionRoom(); b.clearExtensionRoom(); tick();
    CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));

    // Small datagrams hit the existing reliable packet ceiling before 1 MiB.
    // The rejected packet must not change either real pending counter.
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }));
    hs = snap(host);
    host.setTestBlackhole(true);
    accepted = 0;
    while (accepted < 9000 && host.extensionSend(hs.generation, bId, &byte, 1)) ++accepted;
    const auto packetBoundary = dn::ExtensionTransportTestAccess::pending(host, bId);
    CHECK(accepted == dn::ReliableSender::kMaxPendingPackets && packetBoundary.second == accepted &&
        packetBoundary.first <= capacity.pendingBytes && !snap(host).ready);
    CHECK(!host.extensionSend(hs.generation, bId, &byte, 1));
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == packetBoundary);
    host.setTestBlackhole(false);
    host.clearExtensionRoom();
    CHECK(!snap(host).ready);
    a.stop(); b.stop(); host.stop();
    dn::shutdownExtensionTransport();
    unavailable.size = sizeof(unavailable);
    CHECK(!api.snapshot(&unavailable) && !unavailable.ready);
    CHECK(EDF6CoopGetExtensionCapacity(1, sizeof(capacity), &capacity) && capacity.inboxMessages == 4096);
    printf("extension transport: %d checks, %d failures\n", checks, failures);
    if (!loopbackGuard.report("ExtensionTransportTests")) ++failures;
    return failures ? 1 : 0;
}
