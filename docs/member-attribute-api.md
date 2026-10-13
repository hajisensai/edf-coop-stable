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
and `version`; functions are only ever appended. `lastError` was appended within version 1: check
`EDF6COOP_MA_HAS(api, lastError)` (the table's `size` reaches it) before calling it.

## Keys and values

- Keys start with `EDF6X_`, then 1-26 characters of `A-Z`, `0-9`, `_` (32 characters at most). Pick a short id per
  plugin, e.g. `EDF6X_VCREW`. Coop's own keys (`EDF6MS_*`, `EDF6CO_*`, `EDF6DN_*`, `EDF6NET_*`) are refused.
- Values are printable ASCII (0x20-0x7E), 0-999 bytes. Anything else is refused: EOS rejects a whole member update
  when one attribute is bad. Encode binary data as hex/base64.
- At most 16 distinct keys published per process (`EDF6COOP_MEMBER_ATTRIBUTE_MAX_KEYS`) and, separately, at most 16
  distinct keys read (`EDF6COOP_MEMBER_ATTRIBUTE_MAX_WATCHED_KEYS`): reading never uses a publish slot, publishing
  never uses a read slot. A key cleared before Coop's lobby glue ran gives its publish slot back; later it stays
  taken (its tombstone is on our member).
- All our current values together, every plugin's in this process, are at most 4096 bytes
  (`EDF6COOP_MEMBER_ATTRIBUTE_MAX_TOTAL_VALUE`, the value bytes without the wire marker; a cleared key counts 0). A
  publish that would go over is refused with `EDF6COOP_MA_VALUE_BUDGET`; replacing a value counts only the new one.
- Values received from other members are checked against the same contract: a text that is not `=` + at most 999
  printable ASCII bytes reads as `hasValue == 0` (it is unauthenticated lobby data, possibly from a broken or hostile
  client).

## Functions

- `publish(key, value)`: our value, sent in this room and in every room entered afterwards, until changed or cleared.
  Returns 0 for an invalid key/value, the publish key limit or the value budget. A publish accepted before Coop's
  lobby glue runs (another plugin's `EML6_Load` before Coop's) is kept and sent once the glue attaches; if the glue
  never runs (EOS functions missing, see Coop's log) values are kept but never sent and `room()` never reports a
  room. Publishing the same value again is free.
- `clear(key)`: withdraw our value; other members read `hasValue == 0` for us.
- `room(&state)`: set `state.size` first, to at least `EDF6COOP_ROOM_STATE_V1_SIZE` (160; `sizeof(state)` with this
  header). Coop writes what it knows up to `min(size, its own sizeof)` and sets `state.size` to the bytes written,
  so a caller built against a later, longer struct still works with this build and sees how much was filled.
  Gives `inRoom`, `memberCount` (us included), `isHost`, our and the host's EOS ProductUserId text, and `generation`.
- `snapshot(key, each, context, &generation)`: calls `each(context, member)` once per current member, in ordinal
  order, with `memberId`, `ordinal`, `isHost`, `isSelf`, `hasValue`, `value`/`valueBytes`. The first read of a key
  starts watching it (one read slot), so other members' values show up from the next beat on.
- `lastError()`: why the calling thread's last call of this table returned 0 (thread-local; a call that returns 1
  sets `EDF6COOP_MA_OK`):

  | code | meaning |
  | --- | --- |
  | `EDF6COOP_MA_OK` (0) | success |
  | `EDF6COOP_MA_INVALID_KEY` (1) | NULL, not `EDF6X_` + `[A-Z0-9_]`, or longer than 32 |
  | `EDF6COOP_MA_INVALID_VALUE` (2) | NULL, longer than 999 bytes, or not printable ASCII |
  | `EDF6COOP_MA_PUBLISH_KEY_LIMIT` (3) | a 17th published key |
  | `EDF6COOP_MA_WATCH_KEY_LIMIT` (4) | a 17th read key |
  | `EDF6COOP_MA_VALUE_BUDGET` (5) | our values would exceed 4096 bytes together |
  | `EDF6COOP_MA_BAD_ARGUMENT` (6) | NULL `out`/`each`, or `room()` with `size` below 160 |
  | `EDF6COOP_MA_INTERNAL` (7) | Coop failed internally (out of memory) |

`generation` is never 0 and changes whenever the room, its member list, its host, or any extension value of any
member changes (our own publishes included). Compare it with the last one handled to skip unchanged rooms. It is
process-local: never send it to other machines.

`ordinal` counts members in order of first appearance in the current room (0 = earliest seen); it stays the same
while the member stays and starts again in a new room. `memberId` is the stable identity.

## Semantics and limits

- Members without EDF6Coop, with an older EDF6Coop, or without the extension simply have no value.
- Our own row shows what we published immediately; other members' rows lag their publish by up to a few seconds
  plus EOS relay time. Values arrive eventually, not atomically across members; use your own content versioning (e.g. a
  `v1:` prefix or a digest) to decide whether a value is current.
- Extension values go to EOS in a lobby update of their own, never in the one carrying Coop's own markers
  (`EDF6MS_*`, `EDF6NET_*`, ...), together with Coop's counter `EDF6MS_XSEQ`. That update is sent at most once every
  2 seconds and half a beat apart from Coop's own, carrying every current extension value; changes in between
  coalesce (the last value wins). If EOS refuses, rejects or throttles it, Coop's own markers are unaffected and the
  extension update is tried again on its next turn (the log says so once per room). A change of Coop's own markers
  never resends the extension values; a newcomer to the room gets them sent again.
- The room ends for this API (`inRoom` 0, no members, a new `generation`) when the game leaves or closes it, when EOS
  reports us kicked or the room closed (`EOS_Lobby_AddNotifyLobbyMemberStatusReceived`), when the game enters
  another room without leaving this one, and on any beat that finds no copy of the lobby (EOS keeps none for a room
  we are not in). In the last case the next beat that reads a copy shows the room again, with ordinals from 0.
- Withdrawn values travel as a tombstone (wire text `-`, a value travels as `=` + value): EOS refuses empty texts,
  and Coop's copy of the lobby keeps a text it saw until that member sends another one.
- Only Epic lobby members are listed (at most 64): members beyond Epic's 64 that a large room admits over the
  direct link are not visible here.
- The values are unauthenticated lobby data: any room member can claim anything. Validate before acting on them
  (for example, only treat a "files needed" list as a request to generate known variants, never as paths to open).

## Threading

Every function may be called from any thread (the game's UI thread is the expected caller) and is `noexcept`; it
returns 0 instead of throwing. Room facts are updated on the EOS tick under an internal lock, held through scope
guards (an internal exception never leaves it held). `snapshot` copies only the asked key's values of the members out
under that lock and calls `each` afterwards, on the caller's thread, outside every EDF6Coop lock; the
pointers passed to `each` live until it returns. `each` must not throw. Calling the API again from inside `each` is
allowed.

## Validation

`MemberAttributeTests` (ctest `RoomMemberAttributeApi`, `multislot/tests/memberattr_test.cpp`) runs offline: key and
value validation, remote values outside the contract, publish/clear wire texts, publishes kept until the lobby glue
attaches, the separate publish and read key limits, the value budget, the lock surviving a throwing sink, snapshots
of fake rooms (host, self, withdrawn and foreign texts, leavers, ordinals, host migration, leaving and entering
another room), the generation counter, error codes, the forward-compatible `room()`, and version negotiation through
the exported entry point. `SyncMarkerTests` (ctest `SplitSyncLobbyMarker`) drives the lobby glue against a stand-in
EOS SDK: extension values in their own paced update, a refused extension update leaving Coop's markers alone, and
the room ending on a kick, a closed room, a beat without a lobby copy, or another room entered. Neither exercises
real EOS lobbies or two machines.
