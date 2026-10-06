// The transport side of the netcode rewrite (docs/netcode-rewrite-plan.md W1), as the room part and the later
// workstreams use it. Implemented in eos_hooks.cpp, which owns the game's EOS calls.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

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

// Interest management (W6) decides per (observer, subject) whether a state datagram goes now. `observer`: the
// member it would go to; `subject`: whose state it carries (this machine's EOS id: the game's state datagrams
// carry what this machine owns). Unset: always.
using StateSendFilter = bool (*)(const std::string& observer, const std::string& subject, uint64_t nowMs);
void setStateSendFilter(StateSendFilter filter);

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

}  // namespace dn
