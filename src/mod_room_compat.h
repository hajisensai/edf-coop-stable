#pragma once
#include <Windows.h>
#include <cstdint>
namespace dn {
constexpr std::int64_t kAllForcesPrefix=0x41460100;
inline bool AllForcesActive() {
    const auto module=GetModuleHandleW(L"EDF6VehicleCrew.dll");
    return module && GetProcAddress(module,"EDF6AF_DecodeRoomType");
}
inline bool RoomIsolationAvailable() {
    if(!AllForcesActive())return true;
    const auto module=GetModuleHandleW(L"EDF6VehicleCrew.dll");
    using Ready=bool(*)();
    const auto ready=reinterpret_cast<Ready>(GetProcAddress(module,"EDF6AF_RoomIsolationReady"));
    return ready && ready();
}
inline bool IsAllForcesType(std::int64_t value) {
    return value>=kAllForcesPrefix && value<kAllForcesPrefix+0x100;
}
inline std::int64_t GameRoomType(std::int64_t value) {
    return IsAllForcesType(value) && AllForcesActive() ? value-kAllForcesPrefix : value;
}
// Persisted/virtual rooms can carry a type already decoded for the game. The
// separately published marker keeps removal of All Forces from bypassing admission.
template<class Attributes>
bool CompatibleRoom(const Attributes& attributes,bool allForces) {
    bool marked=false;
    for(const auto& a:attributes) {
        if(a.key=="AF_PROFILE") {
            if(a.type!=1 || a.integer!=1)return false;
            marked=true;
        }
        if(a.key=="SEARCH_TYPE" && a.type==1 && IsAllForcesType(a.integer))marked=true;
    }
    return marked==allForces;
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
    if(!RoomIsolationAvailable() || !CompatibleRoom(attributes,AllForcesActive()))return false;
    const PackCheck c=PackCompatibleRoom(attributes);
    if(pack)*pack=c;
    return PackAllowed(c);
}
}
