# Room member attributes for other plugins, ABI v1

`multislot/src/member_attribute_api.h` is a Windows x64 C ABI that lets another plugin (for example EDF6VehicleCrew)
publish a short text on its own member of the online room (EOS lobby) and read every other member's text, before
the mission starts. Copy the header; do not link C++ types.

```c
HMODULE coop = GetModuleHandleW(L"EDF6Coop.dll");
EDF6CoopGetMemberAttributeApiFn get = coop ? (EDF6CoopGetMemberAttributeApiFn)GetProcAddress(coop,
    "EDF6CoopGetMemberAttributeApi") : NULL;
const EDF6CoopMemberAttributeApi* api = get ? get(EDF6COOP_MEMBER_ATTRIBUTE_VERSION) : NULL;
if (api) api->publish("EDF6X_VCREW", "v1:abc123,def456");
```

`EDF6CoopGetMemberAttributeApi(version)` returns a static function table, or `NULL` for a version this build does not
know (an older EDF6Coop has no such export at all: treat both as "not available"). The table starts with its `size`
and `version`; later versions only append.

## Keys and values

- Keys start with `EDF6X_`, then 1-26 characters of `A-Z`, `0-9`, `_` (32 characters at most). Pick a short id per
  plugin, e.g. `EDF6X_VCREW`. Coop's own keys (`EDF6MS_*`, `EDF6CO_*`, `EDF6DN_*`, `EDF6NET_*`) are refused.
- Values are printable ASCII (0x20-0x7E), 0-999 bytes. Anything else is refused: EOS rejects a whole member update
  when one attribute is bad, which would take Coop's own markers down with it. Encode binary data as hex/base64.
- At most 16 distinct extension keys per process (published or read).

## Functions

- `publish(key, value)`: our value, sent on the next EOS beat (once a second) in this room and in every room entered
  afterwards, until changed or cleared. Returns 0 for an invalid key/value, the key limit, or when Coop's lobby glue
  is not running (nothing could be sent). Publishing the same value again is free.
- `clear(key)`: withdraw our value; other members read `hasValue == 0` for us.
- `room(&state)` (`state.size = sizeof(state)` first): `inRoom`, `memberCount` (us included), `isHost`, our and the
  host's EOS ProductUserId text, and `generation`.
- `snapshot(key, each, context, &generation)`: calls `each(context, member)` once per current member, in ordinal
  order, with `memberId`, `ordinal`, `isHost`, `isSelf`, `hasValue`, `value`/`valueBytes`. The first read of a key
  starts watching it, so other members' values show up from the next beat on.

`generation` is never 0 and changes whenever the room, its member list, its host, or any extension value of any
member changes (our own publishes included). Compare it with the last one handled to skip unchanged rooms. It is
process-local: never send it to other machines.

`ordinal` counts members in order of first appearance in the current room (0 = earliest seen); it stays the same
while the member stays and starts again in a new room. `memberId` is the stable identity.

## Semantics and limits

- Members without EDF6Coop, with an older EDF6Coop, or without the extension simply have no value.
- Our own row shows what we published immediately; other members' rows lag their publish by one or two beats plus
  EOS relay time. Values arrive eventually, not atomically across members; use your own content versioning (e.g. a
  `v1:` prefix or a digest) to decide whether a value is current.
- Withdrawn values travel as a tombstone (wire text `-`, a value travels as `=` + value): EOS refuses empty texts,
  and Coop's copy of the lobby keeps a text it saw until that member sends another one.
- Only Epic lobby members are listed (at most 64): members beyond Epic's 64 that a large room admits over the
  direct link are not visible here.
- The values are unauthenticated lobby data: any room member can claim anything. Validate before acting on them
  (for example, only treat a "files needed" list as a request to generate known variants, never as paths to open).

## Threading

Every function may be called from any thread (the game's UI thread is the expected caller) and is `noexcept`; it
returns 0 instead of throwing. Room facts are updated on the EOS tick under an internal lock. `snapshot` copies the
members out under that lock and calls `each` afterwards, on the caller's thread, outside every EDF6Coop lock; the
pointers passed to `each` live until it returns. `each` must not throw. Calling the API again from inside `each` is
allowed.

## Validation

`MemberAttributeTests` (ctest `RoomMemberAttributeApi`, `multislot/tests/memberattr_test.cpp`) runs offline: key and
value validation, publish/clear wire texts, the key limit, snapshots of fake rooms (host, self, withdrawn and foreign
texts, leavers, ordinals, host migration, leaving and entering another room), the generation counter, and version
negotiation through the exported entry point. It does not exercise real EOS lobbies or two machines.
