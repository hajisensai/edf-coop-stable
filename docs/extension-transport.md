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

Only current EOS rooms where the game roster exactly matches the EOS roster are
eligible. Each member must currently publish `EDF6DN_EXT = af-support/1`; no
cached capability is accepted. This marker is published only with All Forces
present, its room isolation initialized, and its
`EDF6AF_SupportProtocolVersion()` export returning 1. An absent extension publishes the
nonempty value `disabled` because EOS rejects empty string attributes, including
the rest of the same update. Virtual rooms, offline rejoin, and overflow members
outside EOS's 64-member lobby are unavailable in v1. The ABI reserves room for
1024 remote peers without promising those paths are currently supported.

Messages have 1–1024 bytes and use socket `EDF6CoopExt1`, channel `0xAF`.
The sender always uses the host link (or, as host, the recipient's link), reliable
ordered delivery, and exactly one path. Joiner-to-joiner messages are relayed by
the authenticated host. A peer's identity comes from DirectNet's signed handshake
and authenticated link; relayed sender identities trust the authenticated host,
not claims in an extension payload. Mesh packets in this namespace are refused.
The transport performs no game operations and creates no additional threads.

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
Membership, key, host and room changes invalidate generations and clear queued
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
Real game and two-machine mission execution are not validated by these tests.
