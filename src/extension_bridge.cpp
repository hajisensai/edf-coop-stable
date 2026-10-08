#include "direct_net.h"
#include "extension_bridge.h"
#include <cstring>
#include <algorithm>

namespace {
std::atomic<std::shared_ptr<dn::DirectNet>> transport;
std::atomic<bool> stopped{false};
std::atomic<bool> missionGateReady{false};
uint32_t EDF6COOP_CALL snapshot(EDF6CoopSnapshot* out) noexcept {
    if (!out || out->size != sizeof(*out)) return 0;
    if (stopped.load()) { *out = {}; out->size = sizeof(*out); return 0; }
    try {
        if (auto net = transport.load()) net->extensionSnapshot(*out);
        else { *out = {}; out->size = sizeof(*out); }
        return 1;
    } catch (...) { *out = {}; out->size = sizeof(*out); return 0; }
}
uint32_t EDF6COOP_CALL peer(uint64_t generation, uint32_t index, EDF6CoopPeer* out) noexcept {
    if (!out) return 0;
    *out = {};
    if (stopped.load()) return 0;
    try { auto net = transport.load(); return net && net->extensionPeer(generation, index, *out) ? 1u : 0u; }
    catch (...) { return 0; }
}
uint32_t EDF6COOP_CALL send(uint64_t generation, const EDF6CoopPeer* target, const void* data, uint32_t bytes) noexcept {
    if (!target || !memchr(target->id, '\0', sizeof(target->id))) return 0;
    if (stopped.load()) return 0;
    try {
        auto net = transport.load();
        return net && net->extensionSend(generation, target->id, static_cast<const uint8_t*>(data), bytes) ? 1u : 0u;
    } catch (...) { return 0; }
}
uint32_t EDF6COOP_CALL poll(uint64_t generation, EDF6CoopPeer* sender, void* data, uint32_t capacity,
                           uint32_t* outBytes) noexcept {
    if (outBytes) *outBytes = 0;
    if (sender) *sender = {};
    if (!sender || !data || !outBytes) return 0;
    if (stopped.load()) return 0;
    try {
        auto net = transport.load();
        dn::Delivered message;
        if (!net || !net->extensionPoll(generation, capacity, message)) return 0;
        memcpy(sender->id, message.src.data(), message.src.size());
        memcpy(data, message.data.data(), message.data.size());
        *outBytes = static_cast<uint32_t>(message.data.size());
        return 1;
    } catch (...) { return 0; }
}
}
namespace dn {
void bindExtensionTransport(std::shared_ptr<DirectNet> net) {
    auto old = transport.exchange(net);
    if (old && old != net) old->clearExtensionRoom();
}
void invalidateExtensionTransport() {
    if (auto net = transport.exchange(nullptr)) net->clearExtensionRoom();
}
void shutdownExtensionTransport() noexcept { stopped.store(true); }
void invalidateExtensionParticipant(const std::string& peer) {
    if (auto net = transport.load(); net && net->extensionParticipant(peer)) invalidateExtensionTransport();
}
bool readMissionParticipants(uint64_t& epoch, std::vector<std::string>& members) {
    epoch = 0;
    members.clear();
    if (!missionGateReady.load()) return false;
    const auto module = GetModuleHandleW(L"EDF6VehicleCrew.dll");
    if (!module) return false;
    const auto read = reinterpret_cast<EDF6AFGetMissionParticipantsFn>(GetProcAddress(module, "EDF6AF_GetMissionParticipants"));
    if (!read) return false;
    auto snapshot = std::make_unique<EDF6AFMissionParticipants>();
    snapshot->size = sizeof(*snapshot);
    if (read(EDF6AF_MISSION_PARTICIPANTS_VERSION, sizeof(*snapshot), snapshot.get()) != 1 ||
        snapshot->size != sizeof(*snapshot) || snapshot->ready != 1 || snapshot->reserved ||
        !snapshot->worldEpoch || !snapshot->participantCount ||
        snapshot->participantCount > EDF6AF_MISSION_MAX_PARTICIPANTS) return false;
    for (uint32_t i = 0; i < snapshot->participantCount; ++i) {
        const auto& peer = snapshot->participants[i];
        const auto end = static_cast<const char*>(memchr(peer.id, '\0', sizeof(peer.id)));
        if (!end || end == peer.id) { members.clear(); return false; }
        members.emplace_back(peer.id, static_cast<size_t>(end - peer.id));
    }
    std::sort(members.begin(), members.end());
    if (std::adjacent_find(members.begin(), members.end()) != members.end()) { members.clear(); return false; }
    epoch = snapshot->worldEpoch;
    return true;
}
void setExtensionMissionGateReady(bool ready) noexcept { missionGateReady.store(ready); }
}

extern "C" __declspec(dllexport) uint32_t EDF6COOP_CALL EDF6Coop_MissionAdmissionReady() noexcept {
    return !stopped.load() && missionGateReady.load() ? 1u : 0u;
}

extern "C" __declspec(dllexport) uint32_t EDF6COOP_CALL EDF6CoopGetExtensionApi(
    uint32_t version, uint32_t outSize, EDF6CoopExtensionApi* out) noexcept {
    if (!out || version != EDF6COOP_EXTENSION_VERSION || outSize != sizeof(*out)) return 0;
    *out = {sizeof(*out), EDF6COOP_EXTENSION_VERSION, snapshot, peer, send, poll};
    return 1;
}
