#include "../src/direct_net.h"
#include "../src/extension_bridge.h"
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
// Observation of real production queues under the same mutex; no fake send path.
namespace dn {
struct ExtensionTransportTestAccess {
    static void dropTransportAcks(DirectNet& net, bool on) { net.testDropAcks_.store(on); }
    static size_t inbox(DirectNet& net) {
        std::lock_guard<std::mutex> lock(net.mu_); return net.extensionInbox_.size();
    }
    static std::pair<size_t, size_t> pending(DirectNet& net, const std::string& peer) {
        std::lock_guard<std::mutex> lock(net.mu_);
        auto& link = net.opt_.mode == Mode::Host ? net.clients_.at(peer) : *net.hostLink_;
        return {link.tx.pendingBytes(), link.tx.pendingCount()};
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
    static_assert(sizeof(EDF6CoopExtensionApi) == 40);
    static_assert(offsetof(EDF6CoopExtensionApi, snapshot) == 8);
    static_assert(offsetof(EDF6CoopExtensionApi, poll) == 32);
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
        capacity.congestedBacklogBytes == 1048576 && capacity.flags == 3);
    EDF6CoopExtensionApi api{};
    CHECK(!EDF6CoopGetExtensionApi(1, sizeof(api), nullptr));
    CHECK(!EDF6CoopGetExtensionApi(2, sizeof(api), &api));
    CHECK(!EDF6CoopGetExtensionApi(1, sizeof(api) - 1, &api));
    CHECK(EDF6CoopGetExtensionApi(1, sizeof(api), &api));
    CHECK(api.size == 40 && api.version == 1 && api.snapshot && api.peer && api.send && api.poll);
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
    CHECK(waitFor([&] { tick(); return !snap(b).ready; }));
    CHECK(snap(b).generation != freshB.generation && dn::ExtensionTransportTestAccess::inbox(b) == 0);
    CHECK(!b.extensionPoll(freshB.generation, 2048, message));
    for (unsigned i = 0; i < 3; ++i) tick();
    CHECK(!snap(b).ready); // repeating identical room facts cannot reopen an overflow
    b.clearExtensionRoom(); tick();
    CHECK(snap(b).ready);

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
    // The packet limit is checked against the same actual shared queue too.
    CHECK(trySend(&byte, 1, capacity.pendingBytes, applied) == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK);
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId) == heldByLostAcks);
    // Ordinary game data may take the shared backlog above the extension ceiling;
    // that is still flow control, never a new mission/identity generation.
    for (unsigned i = 0; i < 300; ++i)
        CHECK(host.send(bId, "EDF6", 1, 2, flowPayload.data(), flowPayload.size()));
    CHECK(dn::ExtensionTransportTestAccess::pending(host, bId).first > capacity.pendingBytes);
    CHECK(snap(host).ready && snap(host).generation == hs.generation);
    CHECK(trySend(&byte, 1) == EDF6COOP_EXTENSION_SEND_WOULD_BLOCK);
    dn::ExtensionTransportTestAccess::dropTransportAcks(b, false);
    // One newly acknowledged game packet cumulatively retires the delayed ACK
    // backlog. The next extension must arrive without a sequence hole or reset.
    CHECK(host.send(bId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }, 15000));
    CHECK(snap(host).ready && snap(host).generation == hs.generation);
    CHECK(trySend(&byte, 1) == EDF6COOP_EXTENSION_SEND_ACCEPTED);
    CHECK(waitFor([&] { tick(); return b.extensionPoll(bs.generation, 2048, message); }));
    CHECK(message.src == hostId && message.data == std::vector<uint8_t>{byte});
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }));
    unsigned background = 0;
    while (b.pop(nullptr, 2048, message)) ++background;
    CHECK(background == 301);

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
    host.setTestBlackhole(false);
    CHECK(waitFor([&] { tick(); return dn::ExtensionTransportTestAccess::pending(host, bId).first == 0; }, 15000));
    CHECK(host.send(bId, "EDF6", 1, 2, &byte, 1));
    CHECK(waitFor([&] { tick(); return b.pop(nullptr, 2048, message); })); // refusal left no sequence hole
    CHECK(message.socketName == "EDF6" && message.data == std::vector<uint8_t>{byte});
    host.clearExtensionRoom(); a.clearExtensionRoom(); b.clearExtensionRoom(); tick();
    CHECK(waitFor([&] { tick(); return snap(host).ready && snap(a).ready && snap(b).ready; }));

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
