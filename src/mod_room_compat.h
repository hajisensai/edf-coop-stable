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
template<class Attributes>
bool CompatibleLocalRoom(const Attributes& attributes) {
    return RoomIsolationAvailable() && CompatibleRoom(attributes,AllForcesActive());
}
}
