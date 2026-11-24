#pragma once
#include <stdint.h>

// Versioned Windows x64 C ABI. Copy this header into consumers; never link C++ types.
// All callbacks return 1 on success, 0 on failure. No callback throws across the ABI.
// A failed/!ready snapshot must cancel pending work. generation is local, opaque,
// changes on room/roster/host/link changes, and must never be sent as a shared epoch.
// Version 2 (EDF6Coop 2.9.0, direct-link protocol 8) replaced version 1, which is no longer served:
// it adds sendState and reports every delivered message with EDF6CoopExtensionMessage.
#define EDF6COOP_EXTENSION_VERSION 2u
#define EDF6COOP_EXTENSION_MAX_PAYLOAD 1024u
#define EDF6COOP_EXTENSION_MAX_PEERS 1024u
// (sender, key) pairs one receiver tracks per generation; state of a further pair is dropped (until the next
// generation; tracked pairs are never evicted). Keys are 32-bit, so this cap is what bounds the receiver.
// Shared by all senders: keep the keys one extension uses few and fixed.
#define EDF6COOP_EXTENSION_MAX_STATE_KEYS 65536u
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

// One delivered message (poll and EDF6CoopPollExtensionBatch). peer: the index api.peer enumerates for
// the same generation (the authenticated sender). offset/bytes: where its payload is (poll: offset 0).
// flags bit 0 (STATE): a sendState message, and key is its key; otherwise a reliable ordered message
// (send/TrySend) and key is 0.
#define EDF6COOP_EXTENSION_MESSAGE_STATE 1u
typedef struct EDF6CoopExtensionMessage { uint32_t peer, offset, bytes, flags, key; } EDF6CoopExtensionMessage;

// Two kinds of message, both 1..EDF6COOP_EXTENSION_MAX_PAYLOAD bytes, through the authenticated host:
//   send       a transaction: reliable, ordered per sender, retransmitted until acknowledged. Extension
//              messages have an ordered domain of their own on every link: a lost game packet never
//              holds them back, only a lost extension message does.
//   sendState  "the latest replaces the previous" state under a caller-chosen key: sent once, never
//              retransmitted, outside the reliable backlog (pendingBytes, TrySend limits, the overflow
//              drain gate). It carries the sender's sequence; a receiver drops one not newer than the last
//              it took for (sender, key), and an unread queued one is overwritten in place by a newer one.
//              It may be lost, dropped for the link's or the host relay's budget, or arrive while a
//              transaction sent before it is still being repaired: never use it for anything a later
//              state message does not fully replace. Keys are per sender; the same key from two senders
//              is two streams. Returns 1 when taken by the transport, not when delivered: it also returns 1
//              when the sender drops it at once for the link's state budget (shed, counted in stateShed), and
//              the relay or the receiver may drop it later. By design (state may be lost) there is no
//              delivery signal: a caller must never treat 1 as delivered, and keeps sending the latest state
//              (periodically or on change) instead of retrying on 0. 0 means rejected (bad arguments,
//              stale generation, unknown peer, not ready).
typedef struct EDF6CoopExtensionApi {
    uint32_t size;
    uint32_t version;
    uint32_t (EDF6COOP_CALL *snapshot)(EDF6CoopSnapshot* out);
    uint32_t (EDF6COOP_CALL *peer)(uint64_t expectedGeneration, uint32_t index, EDF6CoopPeer* out);
    uint32_t (EDF6COOP_CALL *send)(uint64_t expectedGeneration, const EDF6CoopPeer* peer,
                                  const void* data, uint32_t bytes);
    // Empty/too-small buffer returns 0 and leaves the message queued (*info zeroed). Sender is
    // authenticated by DirectNet (relay trusts the authenticated room host); never payload-derived.
    // info: peer index, bytes, flags and key of the message taken (offset 0).
    uint32_t (EDF6COOP_CALL *poll)(uint64_t expectedGeneration, EDF6CoopPeer* sender,
                                  void* data, uint32_t capacity, EDF6CoopExtensionMessage* info);
    uint32_t (EDF6COOP_CALL *sendState)(uint64_t expectedGeneration, const EDF6CoopPeer* peer,
                                       uint32_t key, const void* data, uint32_t bytes);
} EDF6CoopExtensionApi;
typedef uint32_t (EDF6COOP_CALL *EDF6CoopGetExtensionApiFn)(
    uint32_t version, uint32_t outSize, EDF6CoopExtensionApi* out);

// Optional, read-only local capacity proof.
// pendingBytes/congestedBacklogBytes count the unacknowledged encoded datagrams of the link's
// extension ordered domain (game traffic and state messages have their own). flags bit 0 promises one reliable ordered route;
// bit 1 proves EDF6CoopTrySendExtension atomic, nonfatal bounded admission.
// bit 2: an overflow (legacy send/relay backlog, inbox) renews the generation and readiness returns
// once the links drain, instead of latching the room closed. bit 3 proves EDF6CoopPollExtensionBatch.
#define EDF6COOP_EXTENSION_CAPACITY_VERSION 1u
#define EDF6COOP_EXTENSION_CAPACITY_RELIABLE_ORDERED 1u
#define EDF6COOP_EXTENSION_CAPACITY_BOUNDED_TRY_SEND 2u
#define EDF6COOP_EXTENSION_CAPACITY_RECOVERABLE_FAULT 4u
#define EDF6COOP_EXTENSION_CAPACITY_POLL_BATCH 8u
typedef struct EDF6CoopExtensionCapacity {
    uint32_t size, version, maxPayload, maxPeers, inboxMessages, pendingBytes, congestedBacklogBytes, flags;
} EDF6CoopExtensionCapacity;
typedef uint32_t (EDF6COOP_CALL *EDF6CoopGetExtensionCapacityFn)(
    uint32_t version, uint32_t outSize, EDF6CoopExtensionCapacity* out);

// Optional atomic admission on the local host route. Limits apply to the link's pending
// extension ordered datagrams (not game traffic, not state), and must be nonzero and no larger
// than pendingBytes/8192 packets. WOULD_BLOCK keeps the generation and sequence;
// retain the application work until transport ACKs make room. This does not reserve
// capacity at a second joiner's host relay; host-originated fan-out has one link.
#define EDF6COOP_EXTENSION_SEND_UNAVAILABLE 0u
#define EDF6COOP_EXTENSION_SEND_ACCEPTED 1u
#define EDF6COOP_EXTENSION_SEND_WOULD_BLOCK 2u
typedef uint32_t (EDF6COOP_CALL *EDF6CoopTrySendExtensionFn)(
    uint64_t expectedGeneration, const EDF6CoopPeer* peer, const void* data, uint32_t bytes,
    uint32_t pendingBytesLimit, uint32_t pendingPacketsLimit);

// Optional batch poll (capacity bit 3): one transport lock for up to maxMessages queued messages,
// state and reliable alike, in queue order. Payloads are packed into buffer; messages[i] describes one
// (EDF6CoopExtensionMessage above). A message that does not fit stays queued, so bufferBytes must be at
// least EDF6COOP_EXTENSION_MAX_PAYLOAD: a smaller buffer can leave a full-size message at the head of the
// queue forever. Returns 1 with *outCount (possibly 0), or 0 (*outCount 0) when unavailable or the
// generation is stale.
typedef uint32_t (EDF6COOP_CALL *EDF6CoopPollExtensionBatchFn)(
    uint64_t expectedGeneration, EDF6CoopExtensionMessage* messages, uint32_t maxMessages,
    void* buffer, uint32_t bufferBytes, uint32_t* outCount);

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
