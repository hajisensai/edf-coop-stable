#pragma once
#include <stdint.h>

// Room member attributes for other plugins, Windows x64 C ABI (docs/member-attribute-api.md). Copy this header into
// consumers; never link C++ types. Resolve EDF6CoopGetMemberAttributeApi from EDF6Coop.dll with GetProcAddress and ask
// for EDF6COOP_MEMBER_ATTRIBUTE_VERSION; an unknown version returns NULL. The table is static: keep the pointer.
//
// Every function may be called from any thread (the game's UI thread is the expected one) and never throws. Strings
// handed to a callback are copies that live until the callback returns; the callback runs outside every lock of
// EDF6Coop, on the calling thread. Values travel on the local member's EOS lobby attributes, published once a second
// on the EOS tick while in an online room; other members' values are read on the same beat.
#define EDF6COOP_MEMBER_ATTRIBUTE_VERSION 1u
#define EDF6COOP_MEMBER_ATTRIBUTE_KEY_PREFIX "EDF6X_"
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_KEY 32u     // whole key, prefix included, without the NUL
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_VALUE 999u  // bytes, printable ASCII 0x20..0x7E, without the NUL
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_KEYS 16u    // distinct extension keys per process (published or read)
#define EDF6COOP_MEMBER_ID_SIZE 65u
#if defined(_WIN32)
#define EDF6COOP_MA_CALL __cdecl
#else
#define EDF6COOP_MA_CALL
#endif

#pragma pack(push, 8)
typedef struct EDF6CoopRoomState {
    uint32_t size;         // caller sets sizeof(EDF6CoopRoomState)
    uint32_t inRoom;       // 1 while EDF6Coop sees us in an online room (EOS lobby)
    uint32_t memberCount;  // members our copy of the lobby lists, us included
    uint32_t isHost;       // we own the lobby
    uint64_t generation;   // changes whenever room, members, host or any extension value changes; never 0
    char self[EDF6COOP_MEMBER_ID_SIZE];  // our EOS ProductUserId text, "" outside a room
    char host[EDF6COOP_MEMBER_ID_SIZE];  // the lobby owner's, "" when unknown
} EDF6CoopRoomState;

typedef struct EDF6CoopMemberValue {
    const char* memberId;  // EOS ProductUserId text, stable for the member's stay in the room
    uint32_t ordinal;      // order of first appearance in this room (0 = earliest), stable while the member stays
    uint32_t isHost;
    uint32_t isSelf;
    uint32_t hasValue;     // 0: the member published nothing for this key, cleared it, or runs no EDF6Coop/extension
    const char* value;     // NUL-terminated, "" when hasValue is 0
    uint32_t valueBytes;
} EDF6CoopMemberValue;

typedef void (EDF6COOP_MA_CALL *EDF6CoopMemberValueFn)(void* context, const EDF6CoopMemberValue* member);

typedef struct EDF6CoopMemberAttributeApi {
    uint32_t size;     // sizeof(EDF6CoopMemberAttributeApi) of the DLL
    uint32_t version;  // EDF6COOP_MEMBER_ATTRIBUTE_VERSION
    // Our value of `key`, published in this room and every room we enter after, until cleared. 1 accepted; 0 for an
    // invalid key or value, the key limit, or when EDF6Coop's lobby glue is not running (nothing would be sent).
    uint32_t (EDF6COOP_MA_CALL *publish)(const char* key, const char* value);
    // Withdraws our value: members read hasValue 0 for us. 1 accepted (also when nothing was published).
    uint32_t (EDF6COOP_MA_CALL *clear)(const char* key);
    // The room now, `out->size` set by the caller. 1 written (also outside a room: inRoom 0); 0 bad arguments.
    uint32_t (EDF6COOP_MA_CALL *room)(EDF6CoopRoomState* out);
    // Calls `each` once per current member (ordinal order) with its latest value of `key`. The first read of a key
    // starts watching it: values show up from the next beat on. `outGeneration` (may be NULL) gets the generation the
    // members belong to. 1 done; 0 invalid key, key limit, or NULL `each`.
    uint32_t (EDF6COOP_MA_CALL *snapshot)(const char* key, EDF6CoopMemberValueFn each, void* context,
                                         uint64_t* outGeneration);
} EDF6CoopMemberAttributeApi;

typedef const EDF6CoopMemberAttributeApi* (EDF6COOP_MA_CALL *EDF6CoopGetMemberAttributeApiFn)(uint32_t version);
#pragma pack(pop)
