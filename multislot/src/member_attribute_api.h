#pragma once
#include <stddef.h>
#include <stdint.h>

// Room member attributes for other plugins, Windows x64 C ABI (docs/member-attribute-api.md). Copy this header into
// consumers; never link C++ types. Resolve EDF6CoopGetMemberAttributeApi from EDF6Coop.dll with GetProcAddress and ask
// for EDF6COOP_MEMBER_ATTRIBUTE_VERSION; an unknown version returns NULL. The table is static: keep the pointer.
//
// Every function may be called from any thread (the game's UI thread is the expected one) and never throws. Strings
// handed to a callback are copies that live until the callback returns; the callback runs outside every lock of
// EDF6Coop, on the calling thread. Values travel on the local member's EOS lobby attributes, in an update of their own
// (never together with EDF6Coop's own markers) at most once every 2 seconds while in an online room; other members'
// values are read on EDF6Coop's beat, once a second.
#define EDF6COOP_MEMBER_ATTRIBUTE_VERSION 1u
#define EDF6COOP_MEMBER_ATTRIBUTE_KEY_PREFIX "EDF6X_"
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_KEY 32u     // whole key, prefix included, without the NUL
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_VALUE 999u  // bytes, printable ASCII 0x20..0x7E, without the NUL
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_KEYS 16u    // distinct keys this process publishes (reads do not count)
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_WATCHED_KEYS 16u  // distinct keys this process reads (publishes do not count)
#define EDF6COOP_MEMBER_ATTRIBUTE_MAX_TOTAL_VALUE 4096u  // bytes of all our current values together, every plugin's
#define EDF6COOP_MEMBER_ID_SIZE 65u
#define EDF6COOP_ROOM_STATE_V1_SIZE 160u          // sizeof(EDF6CoopRoomState) of ABI v1; later versions only append

// lastError() codes: why the calling thread's last call returned 0 (EDF6COOP_MA_OK after a call that returned 1).
#define EDF6COOP_MA_OK 0u
#define EDF6COOP_MA_INVALID_KEY 1u        // NULL, not EDF6X_ + [A-Z0-9_], or longer than MAX_KEY
#define EDF6COOP_MA_INVALID_VALUE 2u      // NULL, longer than MAX_VALUE, or not printable ASCII
#define EDF6COOP_MA_PUBLISH_KEY_LIMIT 3u  // a new key past MAX_KEYS published keys
#define EDF6COOP_MA_WATCH_KEY_LIMIT 4u    // a new key past MAX_WATCHED_KEYS read keys
#define EDF6COOP_MA_VALUE_BUDGET 5u       // the value would take our values past MAX_TOTAL_VALUE bytes
#define EDF6COOP_MA_BAD_ARGUMENT 6u       // NULL out/callback, or out->size below EDF6COOP_ROOM_STATE_V1_SIZE
#define EDF6COOP_MA_INTERNAL 7u           // EDF6Coop failed internally (out of memory)

#if defined(_WIN32)
#define EDF6COOP_MA_CALL __cdecl
#else
#define EDF6COOP_MA_CALL
#endif

#pragma pack(push, 8)
typedef struct EDF6CoopRoomState {
    uint32_t size;         // caller sets the size it has (at least EDF6COOP_ROOM_STATE_V1_SIZE); set to bytes written
    uint32_t inRoom;       // 1 while EDF6Coop reads us in an online room (EOS lobby)
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
    uint32_t hasValue;     // 0: nothing published for this key, cleared, not a valid value, or no EDF6Coop/extension
    const char* value;     // NUL-terminated printable ASCII, at most MAX_VALUE bytes; "" when hasValue is 0
    uint32_t valueBytes;
} EDF6CoopMemberValue;

typedef void (EDF6COOP_MA_CALL *EDF6CoopMemberValueFn)(void* context, const EDF6CoopMemberValue* member);

typedef struct EDF6CoopMemberAttributeApi {
    uint32_t size;     // sizeof(EDF6CoopMemberAttributeApi) of the DLL: check it before using an appended function
    uint32_t version;  // EDF6COOP_MEMBER_ATTRIBUTE_VERSION
    // Our value of `key`, published in this room and every room we enter after, until cleared. A value accepted
    // before EDF6Coop's lobby glue runs is kept and published once it does. 1 accepted; 0 for an invalid key or
    // value, the key limit or the value budget (lastError tells which).
    uint32_t (EDF6COOP_MA_CALL *publish)(const char* key, const char* value);
    // Withdraws our value: members read hasValue 0 for us. 1 accepted (also when nothing was published).
    uint32_t (EDF6COOP_MA_CALL *clear)(const char* key);
    // The room now. `out->size` set by the caller (at least EDF6COOP_ROOM_STATE_V1_SIZE); EDF6Coop writes the state
    // it knows up to that size and sets `out->size` to the bytes written. 1 written (also outside a room: inRoom 0);
    // 0 bad arguments.
    uint32_t (EDF6COOP_MA_CALL *room)(EDF6CoopRoomState* out);
    // Calls `each` once per current member (ordinal order) with its latest value of `key`. The first read of a key
    // starts watching it: values show up from the next beat on. `outGeneration` (may be NULL) gets the generation the
    // members belong to. 1 done; 0 invalid key, watched-key limit, or NULL `each`.
    uint32_t (EDF6COOP_MA_CALL *snapshot)(const char* key, EDF6CoopMemberValueFn each, void* context,
                                         uint64_t* outGeneration);
    // Appended within version 1 (check EDF6COOP_MA_HAS(api, lastError) first: an older EDF6Coop's table ends before
    // it): why the calling thread's last call of this table returned 0, one of EDF6COOP_MA_* above.
    uint32_t (EDF6COOP_MA_CALL *lastError)(void);
} EDF6CoopMemberAttributeApi;

// The table holds `field` (an older EDF6Coop's table may end before it).
#define EDF6COOP_MA_HAS(api, field) \
    ((api)->size >= offsetof(EDF6CoopMemberAttributeApi, field) + sizeof((api)->field))

typedef const EDF6CoopMemberAttributeApi* (EDF6COOP_MA_CALL *EDF6CoopGetMemberAttributeApiFn)(uint32_t version);
#pragma pack(pop)
