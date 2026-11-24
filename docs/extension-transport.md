# Authenticated extension transport, ABI v2

`src/extension_api.h` is the Windows x64 C ABI shared with All Forces. Resolve
`EDF6CoopGetExtensionApi` dynamically from `EDF6Coop.dll`, request version 2 and
the exact 48-byte API structure size. Callers initialize `Snapshot.size` before querying.
All callbacks return `uint32_t` 0/1 and catch C++ exceptions at the boundary.

ABI v2 (EDF6Coop 2.8.0, direct-link protocol 8, netcode protocol 4) replaced v1, which
is no longer served: a request for version 1 returns 0. Changes from v1:

- the API structure gained a last member, `sendState(expectedGeneration, peer, key,
  data, bytes)` (40 → 48 bytes);
- `poll`'s last parameter is now `EDF6CoopExtensionMessage* info` (was `uint32_t*
  outBytes`);
- `EDF6CoopExtensionMessage` is 20 bytes: `peer`, `offset`, `bytes`, `flags`, `key`
  (was `peer`, `offset`, `bytes`, `reserved`). Flags bit 0
  (`EDF6COOP_EXTENSION_MESSAGE_STATE`) marks a state message, whose key is in `key`;
- reliable extension messages have an ordered domain of their own on every link (below).

Because the wire format changed, every member of a room must run the same build:
Coop's direct links refuse protocol 7 (`BadProtocol`) and the host removes members
publishing another netcode protocol (`EDF6NET_PROTO`).

## Two kinds of message

`send` (and `EDF6CoopTrySendExtension`) is a transaction: reliable, ordered per
sender, retransmitted until acknowledged. On every link the extension namespace has
its own ordered domain: a separate sequence space, sender (`Link::xtx`), receiver
(`Link::xrx`) and acknowledgement (`ExtAck`, message type 16). A lost game packet
never holds an extension message back, and a lost extension message never holds the
game back. Nothing in the extension domain expires, so it needs no `Forward`.
Extension packets are never upgraded by `upgradeUnreliable`.

`sendState(generation, peer, key, data, bytes)` is "the latest replaces the
previous" state under a caller-chosen 32-bit key. It travels unnumbered (reliability
0, traffic class State) over the same stable route, and is never tracked,
retransmitted or upgraded; it is outside the extension backlog, the TrySend limits
and the overflow drain gate. Its payload carries the key and a 64-bit sender sequence
ahead of the data (`[u32 key][u64 seq][data]`, little-endian, 12-byte header). The
sequence follows the sender's steady clock in microseconds (or one above the last if
the clock has not moved), so it only rises, also across a restart of the sender's
process. A receiver keeps, per (sender, key), the last sequence it took and drops
anything not newer; an unread queued state message is overwritten in place by a
newer one, so the inbox holds at most one per (sender, key). At most
`EDF6COOP_EXTENSION_MAX_STATE_KEYS` (65536) (sender, key) pairs are tracked per
generation; state of a further pair is dropped (logged). A new generation forgets
the pairs and their sequences. The sender applies the link's state budget (the
same one as game state), and the host relays joiner-to-joiner state unnumbered
within its relay budget (`relayBytesPerSecond`); either may drop a state message.
`sendState` returns 1 once the transport took it, even if it is then dropped. Keys
are per sender. Never use state for anything a later state message does not fully
replace: a state message may arrive while a transaction sent before it is still
being repaired.

Both kinds arrive through `poll` and `EDF6CoopPollExtensionBatch`, in queue order.
`poll` fills `info` with the sender's peer index, the size, the flags and the key
(offset 0); state messages never count toward the 4096-message inbox limit.

High-volume consumers additionally resolve the optional read-only export
`EDF6CoopGetExtensionCapacity(version, outSize, out)`. Version 1 requires the exact
32-byte `EDF6CoopExtensionCapacity`: eight `uint32_t` fields in order `size`,
`version`, `maxPayload`, `maxPeers`, `inboxMessages`, `pendingBytes`,
`congestedBacklogBytes`, `flags`. It reports 1024-byte payloads, 1024 reserved remote
peer slots, 4096 inbox messages and a 1 MiB per-link encoded-datagram backlog.
Flag bit 0 means reliable ordered delivery on one stable route. Bit 1 proves the
optional `EDF6CoopTrySendExtension` flow-control export described below. Bit 2 means
overflows are recoverable (below). Bit 3 proves the optional
`EDF6CoopPollExtensionBatch` export; other bits are zero.
Bad version, size or null output returns 0 without writing. No room or ready
transport is required, and the query neither changes transport state nor grants
mission authority. The capacity structure stays at its version 1; its backlog figures
describe the extension ordered domain only (not game traffic, not state messages).
Consumers needing recoverable queue pressure also require flag bit 1 and resolve
`EDF6CoopTrySendExtension(expectedGeneration, peer, data, bytes, pendingBytesLimit,
pendingPacketsLimit)`. The fixed x64 ABI returns 0 for unavailable/invalid, 1 for
accepted and 2 for would-block. Limits must be nonzero and at most 1 MiB/8192
packets. Admission holds the transport mutex and checks actual encoded bytes and
packet count of the outgoing link's extension ordered domain; game traffic and
state messages are not in it. A would-block result allocates no sequence, changes no
generation and leaves all pending datagrams intact; the consumer retains its work
until transport ACKs make room. Enemy control uses 768 KiB/768 packets. An application Applied/Restored ACK is a separate Data
message and never proves the SDK transport ACK has arrived. Consequently consumer
application credits alone cannot bound this real queue. The atomic limit covers
only this local outgoing link: host fan-out and joiner-to-host have one link;
joiner-to-joiner still requires the host's separately bounded relay, which retains
its fail-closed overflow policy.

The snapshot exposes local and host EOS ProductUserIds, remote peer count, and a
process-local opaque generation. Enumerate peers with that generation, then pass
the same generation to send/poll. A generation mismatch fails. A snapshot that
fails or has `ready == 0` requires cancelling pending extension work. Generations
are unique across replacement DirectNet instances but are **not shared mission
epochs**. Consumers must negotiate a fresh host epoch and each participant's
mission challenge and validate application messages against those values.

The quorum is the current world's actual participants, read atomically from AF's
`EDF6AF_GetMissionParticipants(1, sizeof(EDF6AFMissionParticipants), out)` export.
Its nonzero world-lifetime epoch distinguishes successive worlds even when native
addresses are reused. Lobby-only members are not added to this quorum; their joins,
departures, capability changes and DirectNet roster updates do not change an
existing mission's transport generation. Each participant must still be present
in the current game room and EOS lobby and have an authenticated live route.
Each participant must currently publish `EDF6DN_EXT = af-support/2`; no
cached capability is accepted. This marker is published only with All Forces
present, its room isolation initialized, and its
`EDF6AF_SupportProtocolVersion()` export returning 2. An absent extension publishes the
nonempty value `disabled` because EOS rejects empty string attributes, including
the rest of the same update. Virtual rooms, offline rejoin, and overflow members
outside EOS's 64-member lobby are unavailable in this ABI. The ABI reserves room for
1024 remote peers without promising those paths are currently supported.

The profile also requires `EDF6AF_MissionPlayerCreated`. After the safe native
creation call succeeds, Coop notifies AF with the exact object and its weak-self
control block (`+0x30`), only while the object is not deleted (`+0x18 & 4 == 0`)
and the control has positive strong uses (`+8`). Failed creation, bad reads and
expired identities never notify; admission attempts alone cannot seal a roster.

Messages have 1–1024 bytes and use socket `EDF6CoopExt1`, channel `0xAF`.
The sender always uses the host link (or, as host, the recipient's link) and exactly
one path; transactions are reliable ordered in the link's extension domain, state
messages unnumbered. Joiner-to-joiner messages are relayed by
the authenticated host. A peer's identity comes from DirectNet's signed handshake
and authenticated link; relayed sender identities trust the authenticated host,
not claims in an extension payload. Mesh packets in this namespace are refused.
The transport performs no game operations and creates no additional threads.

Mission admission is a separate native guard. Coop redirects only the null-safe
`EDF+1DC525 -> 591130` call, consults
`uint32_t EDF6AF_AllowMissionPlayer(int32_t missionIndex)` and returns null before
allocation when AF refuses. The original caller's `1DC544..1DC550` branch writes
an empty weak pointer, and its outer loop skips that player at `1D9B17`. The other
direct caller of 591130 is not null-safe and is **not** globally detoured; AF must
guard its upper call at `22B626 -> 22AB90` separately. AF's participant exporter
must remain unready until its own guard is installed.

`EDF6Coop_MissionAdmissionReady()` returns 1 only after Coop's complete native patch
transaction succeeds with mission support enabled. A missing/failed guard disables
participant queries. `EDF6Coop_ResolveMissionPlayerPuid(index, EDF6CoopPeer* out)`
is a game-thread-only lookup used before player creation: it matches `User+0x48`
in the current native `eos::Users` vector, reads the PUID at `+0x18`, and verifies
that identity against the same slot recorded by Users::Add/Remove. Duplicate
indices, stale members and failed native reads fail closed. This resolver supplies
identity only; it does not make a lobby member part of the mission quorum.

Fresh clients additionally wait before the game's EOS JoinLobby completion. The
host reads `EDF6AF_GetMissionAdmissionState` (ABI 1) independently of transport
readiness: Unknown, Lobby, Loading, Sealed or Invalid, a world epoch, and the frozen
participants. Only an explicit verified Lobby with an empty frozen roster permits
a fresh completion. A reconnect using a participant's PUID still has no old world
state and waits too. AF must derive Lobby from the native mission lifecycle;
`ready == 0`, a missing actor, or an empty list is never a Lobby signal.

The authenticated host's Room control message carries this metadata after the
reserved removed-ID delimiter (`#world-v1:<phase>:<epoch>`, `#world-member:<PUID>`).
It reaches lobby-only DirectNet clients without adding them to the support quorum
or requiring a ready extension/spawn channel. Older Room readers never mistake
these reserved strings for native slots or actual kicked PUIDs. Missing, malformed,
partially received, superseded, disconnected-host and old-host manifests cannot
release admission. Active-world waits remain parked until Lobby; a lost host or
unavailable authority fails the join instead of the ordinary no-host fallback.
Virtual joins use the same fresh-client policy. No unsafe player constructor is
replaced with a global null return.

The same stamped Room message also travels over EOS P2P (the room feed, `src/room_view.h`):
socket `EDF6ROOM`, channel `0x50`, sent by the lobby owner to every member publishing
`EDF6DN_ROOM`, and announced on the lobby as `EDF6DN_ROOMFEED = <owner PUID>`. EOS
authenticates the sending ProductUserId, and a member follows only the current lobby
owner's messages, so a host without a direct link (`Mode=off`, the default) is an
authority too. Each message carries `#room-v1:<epoch>:<seq>` after the removed-ID
delimiter (older readers skip it). The epoch is 64 random bits drawn when the owner begins
hosting, not a clock: a member orders an owner's sessions by when it first heard of each
(the one heard of last is current, every earlier one is over), and within the current one
by `seq`. A member takes a copy only when its stamp is newer than the one it holds from
that owner, whichever path brought it, and a newer EOS
message seen in part suspends the older one. A host whose EDF6Coop predates the room
feed and hosts no direct link can never provide a manifest: an All Forces client's
join into its room fails after `kParkedEntryMs` with a log line naming that cause.

Room/Roster pagination requires exact contiguous coverage, bounded totals and
monotonic versions. Learning any page of a newer Room snapshot suspends an older
admission grant until the full snapshot arrives. Host identity changes, new
handshakes and dropped links clear cached grants and paging state.

The extension inbox is separate from the game's inbox. Either reserved socket
or reserved channel removes a packet from game delivery, including malformed or
unavailable-extension packets and wildcard game reads. Valid messages enter only
the extension queue. A short poll buffer leaves its packet queued. There are at
most 4096 queued transactions (at most 4 MiB payload); overflow invalidates the
extension generation instead of losing part of an acknowledged transaction silently.
Queued state messages (at most one per tracked (sender, key)) never overflow it.

`EDF6CoopPollExtensionBatch(expectedGeneration, messages, maxMessages, buffer,
bufferBytes, outCount)` takes up to `maxMessages` queued messages under one transport
lock and one readiness refresh. Payloads are packed into `buffer` in arrival order;
each 20-byte `EDF6CoopExtensionMessage` gives the sender as the index `api.peer`
enumerates for that generation, the payload's offset and size, and the flags and key
(state bit 0 and its key; 0 and 0 for a transaction). A message that
does not fit stays queued. It returns 1 with a count (possibly 0), or 0 when the
transport is unavailable or the generation is stale.

**Overflow recovery contract (capacity bit 2).** An overflow is a generation
boundary, never a room boundary. Inbox overflow, and a legacy send or host relay
that would exceed either backlog ceiling, drop the queued inbox and renew the
generation at once, so every consumer cancels its in-flight work exactly as after
any readiness blip. The transport then stays not ready until every participant
link's real extension backlog (its own ordered domain; game traffic and state
messages are not in it) is at most half of the ceiling
(512 KiB and 4096 packets), and becomes ready again by itself under a further new
generation; no room change is needed. Earlier DLLs latched the room closed until
the room authority changed, which in practice meant the rest of the mission. On the
host, a relay overflow renews the host's generation; the host consumer's new epoch
reaches every joiner through its own handshake, so a dropped relayed message is
never mistaken for a delivered one.

Every query/send/poll revalidates route identities, reconnect link IDs and readiness
while holding DirectNet's mutex. Direct and relayed extension sends preflight the
actual encoded datagram before allocating a sequence: extension admission never
takes pending wire bytes above 1 MiB, and the 8192-packet reliable-link ceiling also
applies, both to the extension domain alone. Game traffic has a backlog of its own
and never counts here; congestion alone does not change a generation or cancel a
mission. The capacity query does not reserve payload space. `send` and the host
relay of transactions renew the generation if they would exceed either ceiling
(the recovery contract above). Only the explicit TrySend API reports transient
pressure as would-block without a new generation. Inbox overflow renews the
generation the same way; loss of identity/room authority closes the room.
Pending reliable game packets are never trimmed by extension resets.
The game routing heuristic (256 KiB backlog or a measured RTT increase over 150 ms)
does not invalidate this stable extension route; its ordinary game routing remains
unchanged. Room facts expire after two
seconds without an EOS tick; a link unheard for three seconds is not ready.
Participant membership, key, host, world and room changes invalidate generations and clear queued
messages. Loss detection is bounded by the freshness window, not instantaneous.
A successful send means accepted by the reliable transport, not application
execution or acknowledgement. Consumers need transactional acknowledgements and
timeouts and must not execute an old mission request after a new handshake.

Validation uses real localhost UDP with three independently keyed peers, the
production retransmission and relay implementation, ABI callbacks, EOS's existing
test SDK, and a test-only All Forces readiness DLL. `ExtensionTransport` covers
ordered repair, authenticated sender, reserved-channel isolation, bounds, stale
generations, key rebinding, reconnect, queue overflow and its recovery (inbox
overflow ready again at once under a new generation; relay overflow held while the
link stays above the resume mark, then ready by itself after it drains, with no
sequence hole in the extension domain), batch poll packing, sender indexes, flags and
keys, and room lease expiry. State messages: only the newest per (sender, key) is
queued, older or duplicate sequences are dropped (also when reordered while unread),
keys are per sender, a flood twice the extension ceiling under total loss leaves both
backlogs empty and readiness unchanged and is never retransmitted, joiner-to-joiner
state goes through the host relay, the (sender, key) cap holds without a fault, and
a new generation forgets the sequences. Head-of-line: with one extension transaction
lost, a later state message is delivered at once while the later transaction waits
for the repair; with one game packet lost, a later extension transaction is
delivered (and acknowledged) at once while the game's later packet waits. Its
selective transport-ACK-loss fixture delivers every application reply while real
pending bytes remain charged, then verifies would-block without sequence or
generation changes, game backlog above the extension ceiling without any effect on
the extension's queue or admission, and each domain's backlog retired by its own
acknowledgement once ACKs return. Fatal send/relay byte and packet boundaries remain
independently covered.
`ExtensionProfile` covers old/missing/mismatched/current member capabilities,
missing room details, readiness failure and normal Coop marker publication.
It also calls the exact production `publishExtensionRoom` with the real marker,
AF ABI and UDP links: an unmarked/unconnected lobby-only member can join and leave
without changing the existing participants' readiness or generation; missing host
address alone does not alter authority, while real host migration and participant
capability loss close it. `MissionAdmission` tests the native call wrapper's
allow/deny branches; `PatchTablesMatchEDF` verifies the installed game call target,
both null-result guards and mission-index/PUID lookup fixtures.
`WorldAdmission` covers frozen/malformed/missing manifests, next-lobby release,
same-PUID reconnect, host changes, missing/overlapping/reordered pages, and real
authenticated UDP Room delivery under loss to a nonparticipant. `GameNet_worldadmission`
loads the real EDF.dll and Coop DLL in private test processes, drives the actual
EOS imports, and verifies that the production parked callback stays pending with
host slots present while the host is Sealed, then succeeds exactly once on Lobby.
Its AF lifecycle source is an explicit fixture; that test does not validate AF's
native mission-end observer itself.
`WorldAdmissionNativeTests <EDF.dll> <production EDF6VehicleCrew.dll>` separately
maps the private game image with `DONT_RESOLVE_DLL_REFERENCES`, runs the signature-
verified native `Network_SetLocation` at `70F500`, and queries the production AF
ABI before plugin initialization. The returned state runs through the production
Coop Room encoder/parser/admission policy: initial Room, Loading, unsealed Playing,
return to Room, next Loading, Lobby and Boot. It never calls `EML6_Load` or opens
game UI; the network manager belongs to that short-lived test process.
Real game and two-machine mission execution are not validated by these tests.
