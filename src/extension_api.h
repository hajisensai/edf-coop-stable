#pragma once
#include <stdint.h>

// Versioned Windows x64 C ABI. Copy this header into consumers; never link C++ types.
// All callbacks return 1 on success, 0 on failure. No callback throws across the ABI.
// A failed/!ready snapshot must cancel pending work. generation is local, opaque,
// changes on room/roster/host/link changes, and must never be sent as a shared epoch.
#define EDF6COOP_EXTENSION_VERSION 1u
#define EDF6COOP_EXTENSION_MAX_PAYLOAD 1024u
#define EDF6COOP_EXTENSION_MAX_PEERS 1024u
#define EDF6AF_SUPPORT_PROTOCOL_VERSION 2u
#if defined(_WIN32)
#define EDF6COOP_CALL __cdecl
#else
#define EDF6COOP_CALL
#endif

#pragma pack(push, 8)
typedef struct EDF6CoopPeer { char id[65]; } EDF6CoopPeer;
typedef struct EDF6CoopSnapshot {
    uint32_t size; // caller sets sizeof(EDF6CoopSnapshot)
    uint32_t ready;
    uint32_t isHost;
    uint32_t peerCount; // remote members only, excluding local
    uint64_t generation;
    EDF6CoopPeer local;
    EDF6CoopPeer host;
} EDF6CoopSnapshot;

typedef struct EDF6CoopExtensionApi {
    uint32_t size;
    uint32_t version;
    uint32_t (EDF6COOP_CALL *snapshot)(EDF6CoopSnapshot* out);
    uint32_t (EDF6COOP_CALL *peer)(uint64_t expectedGeneration, uint32_t index, EDF6CoopPeer* out);
    uint32_t (EDF6COOP_CALL *send)(uint64_t expectedGeneration, const EDF6CoopPeer* peer,
                                  const void* data, uint32_t bytes);
    // Empty/too-small buffer returns 0 and sets outBytes=0. Sender is authenticated
    // by DirectNet (relay trusts the authenticated room host); never payload-derived.
    uint32_t (EDF6COOP_CALL *poll)(uint64_t expectedGeneration, EDF6CoopPeer* sender,
                                  void* data, uint32_t capacity, uint32_t* outBytes);
} EDF6CoopExtensionApi;
typedef uint32_t (EDF6COOP_CALL *EDF6CoopGetExtensionApiFn)(
    uint32_t version, uint32_t outSize, EDF6CoopExtensionApi* out);

// AF -> Coop, in-process only. The producer snapshots actual current-world player
// PUIDs atomically. Lobby users alone are not participants. ready is 0 while the
// world is loading, after an unknown player enters it without catch-up, or outside
// a mission. worldEpoch identifies a world lifetime, including pointer reuse.
#define EDF6AF_MISSION_PARTICIPANTS_VERSION 1u
#define EDF6AF_MISSION_MAX_PARTICIPANTS 1024u
typedef struct EDF6AFMissionParticipants {
    uint32_t size;
    uint32_t ready;
    uint32_t participantCount;
    uint32_t reserved;
    uint64_t worldEpoch;
    EDF6CoopPeer participants[EDF6AF_MISSION_MAX_PARTICIPANTS];
} EDF6AFMissionParticipants;
typedef uint32_t (EDF6COOP_CALL *EDF6AFGetMissionParticipantsFn)(
    uint32_t version, uint32_t outSize, EDF6AFMissionParticipants* out);
typedef uint32_t (EDF6COOP_CALL *EDF6AFAllowMissionPlayerFn)(int32_t missionIndex);
typedef uint32_t (EDF6COOP_CALL *EDF6CoopResolveMissionPlayerPuidFn)(int32_t missionIndex, EDF6CoopPeer* out);
typedef uint32_t (EDF6COOP_CALL *EDF6CoopMissionAdmissionReadyFn)(void);

// World lifecycle authority, independent of whether extension transport/quorum is ready.
// phase: 0 unknown, 1 verified lobby, 2 loading, 3 sealed, 4 invalid.
#define EDF6AF_MISSION_ADMISSION_VERSION 1u
typedef struct EDF6AFMissionAdmissionState {
    uint32_t size;
    uint32_t phase;
    uint32_t participantCount;
    uint32_t reserved;
    uint64_t worldEpoch;
    EDF6CoopPeer participants[EDF6AF_MISSION_MAX_PARTICIPANTS];
} EDF6AFMissionAdmissionState;
typedef uint32_t (EDF6COOP_CALL *EDF6AFGetMissionAdmissionStateFn)(
    uint32_t version, uint32_t outSize, EDF6AFMissionAdmissionState* out);
typedef void (EDF6COOP_CALL *EDF6AFMissionPlayerCreatedFn)(int32_t missionIndex, const void* object, const void* control);
#pragma pack(pop)
