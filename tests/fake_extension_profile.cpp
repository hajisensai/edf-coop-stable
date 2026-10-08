// Test-only AF presence/readiness exports, isolated from the real game/plugin.
#include "../src/extension_api.h"
#include <cstring>
static bool ready = true;
static unsigned version = EDF6AF_SUPPORT_PROTOCOL_VERSION;
static unsigned missionMode = 0;
extern "C" __declspec(dllexport) void EDF6AF_DecodeRoomType() {}
extern "C" __declspec(dllexport) bool EDF6AF_RoomIsolationReady() { return ready; }
extern "C" __declspec(dllexport) void FakeAF_SetReady(bool value) { ready = value; }
extern "C" __declspec(dllexport) unsigned EDF6AF_SupportProtocolVersion() { return version; }
extern "C" __declspec(dllexport) void FakeAF_SetVersion(unsigned value) { version = value; }
extern "C" __declspec(dllexport) void FakeAF_SetMission(unsigned value) { missionMode = value; }
extern "C" __declspec(dllexport) uint32_t EDF6COOP_CALL EDF6AF_AllowMissionPlayer(int32_t index) {
    return index == 1 ? 1u : 0u;
}
extern "C" __declspec(dllexport) uint32_t EDF6COOP_CALL EDF6AF_GetMissionParticipants(
    uint32_t apiVersion, uint32_t outSize, EDF6AFMissionParticipants* out) {
    if (!out || apiVersion != 1 || outSize != sizeof(*out)) return 0;
    *out = {};
    out->size = sizeof(*out);
    out->ready = missionMode == 1 ? 0u : 1u;
    out->worldEpoch = missionMode == 5 ? 0u : 7u;
    out->participantCount = missionMode == 2 ? 1025u : 2u;
    out->reserved = missionMode == 6 ? 1u : 0u;
    memcpy(out->participants[0].id, "self", 5);
    memcpy(out->participants[1].id, missionMode == 3 ? "self" : "peer", 5);
    if (missionMode == 4) memset(out->participants[1].id, 'x', 65);
    return 1;
}
