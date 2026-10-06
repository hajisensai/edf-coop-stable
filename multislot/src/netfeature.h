#pragma once
// The netcode version gate (docs/netcode-rewrite-plan.md §2.1): which parts of the netcode rewrite are on in this
// room. A part that changes what goes over the network, or a rule of the game, may only run when every member of
// the room runs it the same way; otherwise one machine would play by other rules than the rest without anyone
// seeing why.
//
// Every machine publishes, on its own lobby member (syncmarker.h, where EDF.dll never looks):
//   EDF6NET_PROTO  kNetProtocol, as decimal text
//   EDF6NET_CAPS   the NetFeature bits this machine has on ([Netcode] in the INI), as hex text
// and reads everyone else's on the lobby beat (once a second).
//
//   NetFeatureActive(f) == f is on in this machine's INI, we are in a room, and every member the room lists
//                          publishes our kNetProtocol and has f on.
//
// A member without the attributes (the game as it ships, an EDF6Coop from before the gate, or one whose attributes
// Epic has not relayed yet) turns every feature off for the room: it plays the original netcode, and so does
// everyone else. A member that publishes another protocol (another EDF6Coop version) is refused by the room owner
// ([Netcode] RejectMismatched=1, the default): the owner removes it from the room and both sides log why. A room
// where the owner keeps it (RejectMismatched=0) runs with every feature off. Either way nobody plays by rules the
// others do not have.
//
// Usage (W2-W4 and the transport):
//     if (multislot::NetFeatureActive(multislot::NetFeature::HitAuthority)) { ...new rule... } else { ...game's... }
// Cheap (a shared lock and a few compares): call it where the decision is made, every time. Any thread.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "syncmarker.h"
#include "src/netcode.h"

namespace multislot {

// Bumped whenever a message, a wire format or a game rule of the netcode rewrite changes in a way an older build
// would read differently. Builds of different protocols never run new netcode together.
constexpr std::int64_t kNetProtocol = 1;
constexpr const char* kNetProtocolKey = "EDF6NET_PROTO";
constexpr const char* kNetCapsKey = "EDF6NET_CAPS";

enum class NetFeature : std::uint32_t {
    // W1, the transport (src/netcode.h).
    TrafficClasses = dn::kCapTrafficClasses,
    Mesh = dn::kCapMesh,
    Fragments = dn::kCapFragments,
    Compression = dn::kCapCompression,  // XPRESS of the game's plaintext (netcompress.h)
    // Reserved for the later workstreams; each gets its [Netcode] key when it lands.
    PlayerSync = 1u << 8,      // W2: player pose replication with velocity and extrapolation
    HitAuthority = 1u << 9,    // W3: the shooter decides hits
    WorldAuthority = 1u << 10, // W4: enemies, bullets and mission events from the host
    PluginObjects = 1u << 11,  // W5: all-forces objects
};

bool NetFeatureActive(NetFeature feature);
// Tests only: NetFeatureActive(feature) answers `active` whatever the room runs, until ClearNetFeatureForTest.
void SetNetFeatureForTest(NetFeature feature, bool active);
void ClearNetFeatureForTest();
// On in this machine's INI (whatever the room runs).
bool NetFeatureEnabledLocally(NetFeature feature);

// The room as the gate sees it, without EOS: the tests drive it directly. Thread-safe.
class NetRoom {
public:
    struct Member {
        std::string id;
        bool published = false;  // shows EDF6NET_PROTO
        std::int64_t protocol = 0;
        std::uint32_t caps = 0;
    };
    // What the lobby beat read (an empty view: we left). Returns the members whose protocol differs from ours and
    // that are new since the last call (each once per room), for the owner to refuse.
    std::vector<Member> Observe(const LobbyView& view);
    bool Active(std::uint32_t caps) const;  // every listed member publishes kNetProtocol and has all of `caps`
    bool InRoom() const;
    bool Owner() const;  // we own the room
    std::vector<Member> Members() const;
    // Why Active is false for `caps` ("" when it is not), for the log: the first member that keeps it off.
    std::string WhyOff(std::uint32_t caps) const;

private:
    mutable SRWLOCK lock_ = SRWLOCK_INIT;
    std::string lobby_, self_, owner_;
    std::vector<Member> members_;
    std::vector<std::string> reported_;  // mismatched members already returned, this room
};

NetRoom& NetGate();
// Parses the [Netcode] value of `caps` keys into NetFeature bits (tests and InitNetFeature).
std::uint32_t ParseCaps(const std::string& hex);
std::string FormatCaps(std::uint32_t caps);

// --- What the transport offers the other workstreams (src/netcode.h) ---
// Bytes per second the path to `peer` (EOS_ProductUserId text) carries now, from its congestion controller: the
// budget interest management (W6) schedules state by. The game's own budget (40 KB/s) for a member reached over
// EOS only; 0 for no member.
std::uint32_t LinkBudgetBytesPerSec(const std::string& peer);
// Interest management decides per (observer, subject) whether a state datagram goes now (src/netcode.h
// StateSendFilter). Unset (the default): always.
void SetStateSendFilter(dn::StateSendFilter filter);
// A message of up to dn::kMaxBulkBytes (about 1 MB) to `remote`, reliably in fragments; the receiver's handler
// for `tag` gets it whole (SetBulkHandler). False when the room does not read fragments (NetFeature::Fragments)
// or there is no path yet. Call it on the game's thread (it sends through EOS).
bool SendBulk(const std::string& remote, std::uint16_t tag, const void* data, std::size_t size);
void SetBulkHandler(dn::BulkHandler handler);
// The handler of one tag (several parts receive bulk messages; the one dn handler dispatches by tag).
void SetBulkHandlerForTag(std::uint16_t tag, dn::BulkHandler handler);

// Reads [Netcode] (the features this machine has on, RejectMismatched, StatsSeconds, ...) and hands the
// transport its options (src/netcode.h). Before StartNetFeature.
void InitNetFeature(const wchar_t* iniPath);
// Publishes our protocol and features and watches everyone's, once the lobby glue (InstallSyncMarker) is in. False
// without it: then no feature is ever active (we cannot tell what the room runs).
bool StartNetFeature(bool lobbyGlue);

}  // namespace multislot
