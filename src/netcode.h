// The transport side of the netcode rewrite (docs/netcode-rewrite-plan.md W1), as the room part and the later
// workstreams use it. Implemented in eos_hooks.cpp, which owns the game's EOS calls.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace dn {

// Capability bits a member publishes with its netcode protocol (multislot netfeature.h is their one list).
enum NetCap : uint32_t {
    kCapTrafficClasses = 1u << 0,  // state unreliable and replaceable, events on two paths, no double resends
    kCapMesh = 1u << 1,            // guests link to each other directly, the host relays only what cannot
    kCapFragments = 1u << 2,       // datagrams above EOS's 1170 bytes and bulk messages travel in fragments
    kCapCompression = 1u << 3,     // reserved: XPRESS of the game's plaintext (not enabled, see netfeature.h)
};

struct NetcodeOptions {
    bool trafficClasses = true;  // [Netcode] TrafficClasses
    bool mesh = true;            // [Netcode] Mesh: joiners link to each other (while the whole room runs it)
    bool fragments = true;       // [Netcode] Fragments
    // [Netcode] ShedState: a state datagram that does not fit the path's budget (linkBudgetBytesPerSec) is
    // dropped instead of queued - the next one replaces it anyway.
    bool shedState = true;
    uint32_t statsIntervalMs = 60000;  // [Netcode] StatsSeconds: the STATS/NETTYPE lines
};
void setNetcodeOptions(const NetcodeOptions& options);
NetcodeOptions netcodeOptions();

// Whether `cap` is on for the whole room (every member publishes it and runs our protocol). Answered by the room
// part (netfeature.cpp); unset: nothing is.
using RoomCapQuery = bool (*)(uint32_t cap);
void setRoomCapQuery(RoomCapQuery query);

// Interest management (W6) decides per (observer, subject) whether a state datagram of `bytes` goes now, over a path
// to `observer` that carries `budget` bytes per second (its congestion controller; the caller measures it, so the
// filter never calls back into the transport that asks it).
// `observer`: the member it would go to; `subject`: whose state it carries - the sender's own (this machine's EOS id)
// when this machine sends it, the original sender's when a room host relays it between two joiners. Asked on the
// game's thread for our own datagrams and on the direct link's thread for relayed ones. Unset: always.
using StateSendFilter = bool (*)(const std::string& observer, const std::string& subject, uint32_t bytes, uint32_t budget,
                                 uint64_t nowMs);
void setStateSendFilter(StateSendFilter filter);

// The room's real size (up to 1024; multislot lobbystate.h CurrentLobbyCapacity) where Epic's lobby holds at most 64:
// a host lets that many in over the direct link. Returns 0 when the room part does not know (then Epic's MaxMembers).
using RoomCapacitySource = uint32_t (*)();
void setRoomCapacitySource(RoomCapacitySource source);

// Member slots (room_view.h): the game numbers each member by the eos::Users slot it was added in (the first empty
// one), and every game must have every member in the slot the host's game has it in.
// This machine's game's slots (multislot userslots.h): index = slot, "" = empty. What a host sends as its Room list.
using GameSlotsSource = std::vector<std::string> (*)();
void setGameSlotsSource(GameSlotsSource source);
// In someone else's room: the slot its host's game has `member` in, where our game must add it; -1 when not known
// (the host's slots not heard, or they do not list it).
int hostSlotOf(const std::string& member);

// Bytes per second the path to `peer` carries now, from its congestion controller (direct link: the path in
// use; EOS only: the game's own budget, as nobody measures Epic's path). 0: `peer` is not known.
uint32_t linkBudgetBytesPerSec(const std::string& peer);

// Messages of the plugin's own that may be larger than one packet (up to kMaxBulkBytes), sent reliably in
// fragments over the best path to `remote` and handed to the handler of `tag` on the receiving machine, on the
// thread that receives game packets. False when `remote` cannot get them (it does not read fragments, or there is
// no path). Thread-safe.
constexpr size_t kMaxBulkBytes = 1u << 20;
using BulkHandler = void (*)(const std::string& src, uint16_t tag, const uint8_t* data, size_t size);
bool sendBulk(const std::string& remote, uint16_t tag, const uint8_t* data, size_t size);
void setBulkHandler(BulkHandler handler);
// A bulk message goes again until its receiver acknowledges it (5 tries, the wait doubling); this many were given up
// since start (each logged).
uint64_t bulkUndelivered();

// --- Rooms above Epic's 64 (I1) ---
// What a room owner puts on the lobby itself (not a member: a searcher reads the lobby's attributes only) so that a
// player Epic turns away from a full lobby can come in over the direct link: the address list it hosts on and its
// identity commitment. False when this machine hosts no direct link.
constexpr const char* kHostAddressKey = "EDF6DN_HOSTADDR";
constexpr const char* kHostIdentityKey = "EDF6DN_HOSTID";
bool hostAdvertisement(std::string& address, std::string& identity);
// Our netcode protocol and features, said in every direct-link hello; a host refuses another protocol when
// `refuseOthers`. The host's view of what each member beyond Epic's lobby runs (for the version gate).
void setNetcodeIdentity(uint32_t protocol, uint32_t caps, bool refuseOthers);
std::map<std::string, std::pair<uint32_t, uint32_t>> directMemberNetcode();  // EOS id -> (protocol, caps)
// Tests only ([Test] LoopbackHosts): a room host may advertise a loopback address (gamenet runs on one machine).
void setTestLoopbackHosts(bool on);

// Tests only ([Test] PeerBlockAfterMs / PeerBlockForMs): the links between joiners lose everything from `afterMs`
// from now for `forMs` (UINT32_MAX: for good), as when the NAT between them stops letting it through.
void setTestPeerBlock(uint32_t afterMs, uint32_t forMs);

}  // namespace dn
