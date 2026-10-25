#pragma once
#include <Windows.h>
#include <cstdint>
namespace dn {
// All Forces' room namespace (its src/mod_room.h). It publishes SEARCH_TYPE as prefix + the game's value (0..0xFF) and
// a durable marker AF_PROFILE = its room protocol, and both change with every room protocol, which no older or newer
// All Forces may share. Coop reads the two from All Forces itself (EDF6AF_RoomPrefix / EDF6AF_RoomProfile, exported by
// EDF6VehicleCrew.dll since room protocol 2) instead of keeping its own copy. An All Forces without those exports is a
// build from before them, and nothing here is compatible with it: its rooms are refused and, while it is loaded, so is
// every room (no fallback to the namespace it had). Without All Forces loaded nothing changes.
// Every All Forces room protocol's prefix lies in this family ('AF' << 16), so a machine without All Forces, or with
// another protocol, knows such a room as one it cannot join.
constexpr std::int64_t kAllForcesFamily=0x41460000;
constexpr bool IsAllForcesFamily(std::int64_t value) {
    return value>=kAllForcesFamily && value<kAllForcesFamily+0x10000;
}
struct AllForcesRooms {
    bool loaded=false;     // EDF6VehicleCrew.dll is in this process
    bool published=false;  // and it exported a valid namespace
    std::int64_t prefix=0,profile=0;
    // A SEARCH_TYPE of this namespace.
    constexpr bool Wire(std::int64_t value) const { return published && value>=prefix && value<prefix+0x100; }
};
// What All Forces said: a prefix on a 0x100 boundary inside the family, and a protocol number of at least 1.
constexpr AllForcesRooms AllForcesNamespace(bool loaded,bool exported,std::int64_t prefix,std::int64_t profile) {
    AllForcesRooms rooms{};
    rooms.loaded=loaded;
    if(!loaded || !exported || (prefix&0xFF) || !IsAllForcesFamily(prefix) || profile<1)return rooms;
    rooms.published=true;
    rooms.prefix=prefix;
    rooms.profile=profile;
    return rooms;
}
using RoomNamespaceFn=std::int64_t(*)();
inline AllForcesRooms AllForcesNamespaceOf(HMODULE module) {
    if(!module)return {};
    const auto prefix=reinterpret_cast<RoomNamespaceFn>(GetProcAddress(module,"EDF6AF_RoomPrefix"));
    const auto profile=reinterpret_cast<RoomNamespaceFn>(GetProcAddress(module,"EDF6AF_RoomProfile"));
    return AllForcesNamespace(true,prefix && profile,prefix ? prefix() : 0,profile ? profile() : 0);
}
// This process's All Forces, as it is now (EDFModLoader loads it after EDF6Coop, so this is asked each time).
inline AllForcesRooms LocalAllForces() { return AllForcesNamespaceOf(GetModuleHandleW(L"EDF6VehicleCrew.dll")); }
inline bool AllForcesActive() { return GetModuleHandleW(L"EDF6VehicleCrew.dll")!=nullptr; }
// The log's words for a refusal on All Forces' account.
inline const char* AllForcesRoomRefusal(const AllForcesRooms& local) {
    if(local.loaded && !local.published)
        return "All Forces (EDF6VehicleCrew.dll) here exports no room protocol (EDF6AF_RoomPrefix / EDF6AF_RoomProfile): "
               "it is a build from before them, which this EDF6Coop joins no room with; update All Forces";
    return "its All Forces room protocol does not match this machine's";
}
inline bool RoomIsolationAvailable() {
    const AllForcesRooms local=LocalAllForces();
    if(!local.loaded)return true;
    if(!local.published)return false;
    const auto module=GetModuleHandleW(L"EDF6VehicleCrew.dll");
    using Ready=bool(*)();
    const auto ready=reinterpret_cast<Ready>(GetProcAddress(module,"EDF6AF_RoomIsolationReady"));
    return ready && ready();
}
// SEARCH_TYPE as the game reads it: this machine's All Forces namespace decoded, anything else as it is.
inline std::int64_t GameRoomType(std::int64_t value) {
    const AllForcesRooms local=LocalAllForces();
    return local.Wire(value) ? value-local.prefix : value;
}
// Persisted/virtual rooms can carry a type already decoded for the game. The separately published marker keeps removal
// of All Forces from bypassing admission. A room is All Forces' when it carries AF_PROFILE or an All Forces family
// SEARCH_TYPE; it is ours only with our protocol in both. A machine without All Forces takes no All Forces room, one
// whose All Forces published no namespace takes no room at all.
template<class Attributes>
bool CompatibleRoom(const Attributes& attributes,const AllForcesRooms& local) {
    bool marked=false,ours=true;
    for(const auto& a:attributes) {
        if(a.key=="AF_PROFILE") {
            marked=true;
            if(a.type!=1 || !local.published || a.integer!=local.profile)ours=false;
        }
        if(a.key=="SEARCH_TYPE" && a.type==1 && IsAllForcesFamily(a.integer)) {
            marked=true;
            if(!local.Wire(a.integer))ours=false;
        }
    }
    if(!local.loaded)return !marked;
    return local.published && marked && ours;
}
// All Forces' mission packs (its docs/mod-room-isolation.md): a room on one of its pack contents carries the pack's
// identity (AF_PACK) beside the game's content id (DLC), and All Forces refuses a room whose pack differs from the one
// installed here in its native JoinLobby hook. A room Coop persisted (REJOIN, a virtual room, the full-lobby fallback)
// never reaches that hook with an EOS handle, so Coop asks All Forces itself with the attributes it kept:
// `int EDF6AF_RoomPackCompatible(int64_t dlc, const char* afPack)` (afPack nullptr when the room has none), 1 joinable,
// 0 not (All Forces logs why). An All Forces without that export (from before its mission packs) has nothing to check.
// It is called on the game's thread - inside the game's JoinLobby, and in the room search's and the join's EOS
// callbacks on the EOS tick (no lock of Coop's held) - so it must answer at once: whatever All Forces tells the player
// about a refusal it shows on a later frame of its own, never in a dialog or a wait inside this call.
enum class PackCheck { Ok, NoExport, Refused, NoContent };
inline const char* PackCheckName(PackCheck c) {
    switch(c) {
        case PackCheck::Ok:return "its mission pack matches";
        case PackCheck::NoExport:return "All Forces has no mission-pack check (a version from before its packs)";
        case PackCheck::Refused:return "All Forces refuses its mission pack (another pack, or one this machine lacks)";
        default:return "the room has no content id (DLC) to check its mission pack by";
    }
}
using RoomPackCompatibleFn=int(*)(std::int64_t dlc,const char* afPack);
template<class Attributes>
PackCheck PackCompatibleRoom(const Attributes& attributes) {
    if(!AllForcesActive())return PackCheck::NoExport;
    const auto check=reinterpret_cast<RoomPackCompatibleFn>(
        GetProcAddress(GetModuleHandleW(L"EDF6VehicleCrew.dll"),"EDF6AF_RoomPackCompatible"));
    if(!check)return PackCheck::NoExport;
    bool content=false;
    std::int64_t dlc=0;
    const char* pack=nullptr;
    for(const auto& a:attributes) {
        if(a.key=="DLC" && a.type==1) {content=true;dlc=a.integer;}
        if(a.key=="AF_PACK" && a.type==3)pack=a.text.c_str();
    }
    // The game publishes DLC with every room: one without it is no room All Forces' own hook would let in either.
    if(!content)return PackCheck::NoContent;
    return check(dlc,pack)==1 ? PackCheck::Ok : PackCheck::Refused;
}
inline bool PackAllowed(PackCheck c) { return c==PackCheck::Ok || c==PackCheck::NoExport; }
// `pack` (optional): what the mission-pack check said, for the caller's log line.
template<class Attributes>
bool CompatibleLocalRoom(const Attributes& attributes,PackCheck* pack=nullptr) {
    if(pack)*pack=PackCheck::NoExport;
    if(!RoomIsolationAvailable() || !CompatibleRoom(attributes,LocalAllForces()))return false;
    const PackCheck c=PackCompatibleRoom(attributes);
    if(pack)*pack=c;
    return PackAllowed(c);
}
}
