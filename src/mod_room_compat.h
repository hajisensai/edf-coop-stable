#pragma once
#include <Windows.h>
#include <cstdint>
namespace dn {
// All Forces' room namespace (its src/mod_room.h). It publishes SEARCH_TYPE as prefix + the game's value (0..0xFF) and
// a durable marker AF_PROFILE = its room protocol, and both change with every room protocol, which no older or newer
// All Forces may share. Coop reads the two from All Forces itself (EDF6AF_RoomPrefix / EDF6AF_RoomProfile, exported by
// EDF6VehicleCrew.dll since room protocol 2) instead of keeping its own copy.
// An All Forces without those exports but with EDF6AF_DecodeRoomType is a build of room protocol 1 (every All Forces
// released up to 2026-10): its namespace is the one EDF6Coop 2.6.1 kept for it (prefix 0x41460100, AF_PROFILE 1), and
// that is what it gets here, so its players keep their rooms with each other once this EDF6Coop has updated itself. It
// shares no room with a protocol 2 All Forces (neither prefix nor marker match): that split is All Forces' own. An
// EDF6VehicleCrew.dll with neither (from before All Forces isolated its rooms) is no All Forces to the rooms here, as
// it was to 2.6.1. One that exports only one of the two, or values that are no namespace, publishes none: while it is
// loaded no room is joined. Without All Forces loaded nothing changes.
// Every All Forces room protocol's prefix lies in this family ('AF' << 16), so a machine without All Forces, or with
// another protocol, knows such a room as one it cannot join. All Forces numbers its prefixes by protocol
// (0x41460000 + (protocol << 8)), so a SEARCH_TYPE without AF_PROFILE beside it still says which protocol it is.
constexpr std::int64_t kAllForcesFamily=0x41460000;
constexpr std::int64_t kAllForcesLegacyPrefix=0x41460100,kAllForcesLegacyProfile=1;  // room protocol 1, no exports
constexpr bool IsAllForcesFamily(std::int64_t value) {
    return value>=kAllForcesFamily && value<kAllForcesFamily+0x10000;
}
struct AllForcesRooms {
    bool loaded=false;     // an All Forces is in this process (EDF6VehicleCrew.dll with a room namespace of any kind)
    bool published=false;  // and its namespace is valid
    bool legacy=false;     // the namespace is room protocol 1's, taken because it exports none (kAllForcesLegacy*)
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
// An All Forces of room protocol 1 (EDF6AF_DecodeRoomType, no namespace exports): the namespace 2.6.1 kept for it.
constexpr AllForcesRooms AllForcesLegacyNamespace() {
    AllForcesRooms rooms=AllForcesNamespace(true,true,kAllForcesLegacyPrefix,kAllForcesLegacyProfile);
    rooms.legacy=true;
    return rooms;
}
// What an EDF6VehicleCrew.dll's exports make of it: `prefix` / `profile` whether the two namespace exports are there
// (their values `prefixValue` / `profileValue`), `decoder` whether EDF6AF_DecodeRoomType is. Either namespace export
// decides; without both, the decoder says room protocol 1; without it as well, there is no All Forces.
constexpr AllForcesRooms AllForcesFromExports(bool prefix,bool profile,bool decoder,std::int64_t prefixValue,
                                              std::int64_t profileValue) {
    if(prefix || profile)return AllForcesNamespace(true,prefix && profile,prefixValue,profileValue);
    if(decoder)return AllForcesLegacyNamespace();
    return {};
}
using RoomNamespaceFn=std::int64_t(*)();
inline AllForcesRooms AllForcesNamespaceOf(HMODULE module) {
    if(!module)return {};
    const auto prefix=reinterpret_cast<RoomNamespaceFn>(GetProcAddress(module,"EDF6AF_RoomPrefix"));
    const auto profile=reinterpret_cast<RoomNamespaceFn>(GetProcAddress(module,"EDF6AF_RoomProfile"));
    const bool decoder=GetProcAddress(module,"EDF6AF_DecodeRoomType")!=nullptr;
    const bool both=prefix && profile;
    return AllForcesFromExports(prefix!=nullptr,profile!=nullptr,decoder,both ? prefix() : 0,both ? profile() : 0);
}
// This process's All Forces, as it is now (EDFModLoader loads it after EDF6Coop, so this is asked each time).
inline AllForcesRooms LocalAllForces() { return AllForcesNamespaceOf(GetModuleHandleW(L"EDF6VehicleCrew.dll")); }
inline bool AllForcesActive() { return LocalAllForces().loaded; }
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
// Why a room is not this machine's on All Forces' account, for the log and for the player.
enum class RoomRefusal {
    None,           // it is
    NotReady,       // All Forces' room isolation is not up here (EDF6AF_RoomIsolationReady)
    NoNamespace,    // All Forces here exports a broken room namespace: no room is joined with it
    AllForcesRoom,  // an All Forces room, and no All Forces here
    VanillaRoom,    // no All Forces room, and All Forces here
    OlderProtocol,  // an All Forces room of an older room protocol than this machine's: its host's All Forces is older
    NewerProtocol,  // of a newer one: this machine's All Forces is older
    OtherProtocol,  // of another namespace, or one whose SEARCH_TYPE and AF_PROFILE disagree
};
// Persisted/virtual rooms can carry a type already decoded for the game. The separately published marker keeps removal
// of All Forces from bypassing admission. A room is All Forces' when it carries AF_PROFILE or an All Forces family
// SEARCH_TYPE; it is ours only with our protocol in both. A machine without All Forces takes no All Forces room, one
// whose All Forces published no valid namespace takes no room at all.
template<class Attributes>
RoomRefusal RoomRefusalOf(const Attributes& attributes,const AllForcesRooms& local) {
    bool marked=false,ours=true;
    std::int64_t marker=0,family=0;  // the room's protocol as its AF_PROFILE / its SEARCH_TYPE prefix says, 0 unsaid
    for(const auto& a:attributes) {
        if(a.key=="AF_PROFILE") {
            marked=true;
            if(a.type!=1 || !local.published || a.integer!=local.profile)ours=false;
            if(a.type==1)marker=a.integer;
        }
        if(a.key=="SEARCH_TYPE" && a.type==1 && IsAllForcesFamily(a.integer)) {
            marked=true;
            if(!local.Wire(a.integer))ours=false;
            family=(a.integer>>8)&0xFF;
        }
    }
    if(!local.loaded)return marked ? RoomRefusal::AllForcesRoom : RoomRefusal::None;
    if(!local.published)return RoomRefusal::NoNamespace;
    if(!marked)return RoomRefusal::VanillaRoom;
    if(ours)return RoomRefusal::None;
    const std::int64_t protocol=marker ? marker : family;
    if(protocol>0 && protocol<local.profile)return RoomRefusal::OlderProtocol;
    if(protocol>local.profile)return RoomRefusal::NewerProtocol;
    return RoomRefusal::OtherProtocol;
}
template<class Attributes>
bool CompatibleRoom(const Attributes& attributes,const AllForcesRooms& local) {
    return RoomRefusalOf(attributes,local)==RoomRefusal::None;
}
// The log's words for a refusal.
inline const char* RoomRefusalReason(RoomRefusal r) {
    switch(r) {
        case RoomRefusal::None:return "it is joinable";
        case RoomRefusal::NotReady:return "All Forces' room isolation is not ready here (EDF6AF_RoomIsolationReady)";
        case RoomRefusal::NoNamespace:
            return "All Forces (EDF6VehicleCrew.dll) here exports a broken room namespace (EDF6AF_RoomPrefix / "
                   "EDF6AF_RoomProfile: one without the other, or no valid prefix or protocol), so no room is joined";
        case RoomRefusal::AllForcesRoom:return "it is an All Forces room and All Forces is not loaded here";
        case RoomRefusal::VanillaRoom:return "it is no All Forces room and All Forces is loaded here";
        case RoomRefusal::OlderProtocol:
            return "its All Forces room protocol is older than this machine's (its host runs an older All Forces)";
        case RoomRefusal::NewerProtocol:
            return "its All Forces room protocol is newer than this machine's (update All Forces)";
        default:return "its All Forces room protocol does not match this machine's";
    }
}
// What the player is told (room_notice.h: the menu's lower left, outside rooms; one short ASCII line).
inline const char* RoomRefusalNotice(RoomRefusal r) {
    switch(r) {
        case RoomRefusal::None:return "";
        case RoomRefusal::NotReady:return "Room refused: All Forces is not ready";
        case RoomRefusal::NoNamespace:return "Room refused: All Forces room setup invalid, update All Forces";
        case RoomRefusal::AllForcesRoom:return "Room refused: All Forces room, All Forces not installed";
        case RoomRefusal::VanillaRoom:return "Room refused: not an All Forces room (All Forces is on)";
        case RoomRefusal::OlderProtocol:return "Room refused: host's All Forces is older";
        case RoomRefusal::NewerProtocol:return "Room refused: host's All Forces is newer, update All Forces";
        default:return "Room refused: All Forces version differs";
    }
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
// What this machine makes of a room Coop persisted: All Forces' room namespace first, then its mission pack.
struct LocalRoomVerdict {
    RoomRefusal refusal=RoomRefusal::None;
    PackCheck pack=PackCheck::NoExport;  // asked only when the namespace lets the room in
    bool ok() const { return refusal==RoomRefusal::None && PackAllowed(pack); }
    // The log's words.
    const char* reason() const { return refusal!=RoomRefusal::None ? RoomRefusalReason(refusal) : PackCheckName(pack); }
    // What the player is told, "" for nothing. A pack All Forces refused it explains itself (EDF6AF_RoomPackCompatible
    // shows the player why, on a frame of its own), so that one is not said twice.
    const char* notice() const {
        if(refusal!=RoomRefusal::None)return RoomRefusalNotice(refusal);
        return pack==PackCheck::NoContent ? "Room refused: the room has no content id (DLC)" : "";
    }
};
template<class Attributes>
LocalRoomVerdict JudgeLocalRoom(const Attributes& attributes) {
    LocalRoomVerdict v;
    const AllForcesRooms local=LocalAllForces();
    v.refusal=RoomRefusalOf(attributes,local);
    if(v.refusal==RoomRefusal::None && !RoomIsolationAvailable())v.refusal=RoomRefusal::NotReady;
    if(v.refusal==RoomRefusal::None)v.pack=PackCompatibleRoom(attributes);
    return v;
}
// `pack` (optional): what the mission-pack check said.
template<class Attributes>
bool CompatibleLocalRoom(const Attributes& attributes,PackCheck* pack=nullptr) {
    const LocalRoomVerdict v=JudgeLocalRoom(attributes);
    if(pack)*pack=v.pack;
    return v.ok();
}
}
