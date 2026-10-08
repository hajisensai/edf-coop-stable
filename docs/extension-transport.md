# Authenticated extension transport, ABI v1

`src/extension_api.h` is the Windows x64 C ABI shared with All Forces. Resolve
`EDF6CoopGetExtensionApi` dynamically from `EDF6Coop.dll`, request version 1 and
the exact API structure size. Callers initialize `Snapshot.size` before querying.
All callbacks return `uint32_t` 0/1 and catch C++ exceptions at the boundary.

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
outside EOS's 64-member lobby are unavailable in v1. The ABI reserves room for
1024 remote peers without promising those paths are currently supported.

The profile also requires `EDF6AF_MissionPlayerCreated`. After the safe native
creation call succeeds, Coop notifies AF with the exact object and its weak-self
control block (`+0x30`), only while the object is not deleted (`+0x18 & 4 == 0`)
and the control has positive strong uses (`+8`). Failed creation, bad reads and
expired identities never notify; admission attempts alone cannot seal a roster.

Messages have 1–1024 bytes and use socket `EDF6CoopExt1`, channel `0xAF`.
The sender always uses the host link (or, as host, the recipient's link), reliable
ordered delivery, and exactly one path. Joiner-to-joiner messages are relayed by
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

Room/Roster pagination requires exact contiguous coverage, bounded totals and
monotonic versions. Learning any page of a newer Room snapshot suspends an older
admission grant until the full snapshot arrives. Host identity changes, new
handshakes and dropped links clear cached grants and paging state.

The extension inbox is separate from the game's inbox. Either reserved socket
or reserved channel removes a packet from game delivery, including malformed or
unavailable-extension packets and wildcard game reads. Valid messages enter only
the extension queue. A short poll buffer leaves its packet queued. There are at
most 256 queued messages (at most 256 KiB payload); overflow invalidates the
extension session instead of losing part of an acknowledged transaction silently.
Overflow remains closed until the room authority is explicitly invalidated.

Every query/send/poll revalidates route identities, reconnect link IDs, readiness,
and congestion while holding DirectNet's mutex. Room facts expire after two
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
generations, key rebinding, reconnect, queue overflow and room lease expiry.
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
