#include "../src/mod_room_compat.h"
#include "../src/fake_lobby.h"
#include <cstdio>
#include <string>
// All Forces' room namespace as Coop reads it (mod_room_compat.h). argv: "-" or nothing; then, for the DLL part, All
// Forces with its namespace and mission packs (FakeExtensionProfile) and an All Forces from before both
// (FakeExtensionNoPacks: neither EDF6AF_RoomPrefix / EDF6AF_RoomProfile nor EDF6AF_RoomPackCompatible).
namespace {
int failures=0;
void Check(bool ok,const char* what){ if(!ok){++failures;std::printf("FAIL: %s\n",what);} }
constexpr std::int64_t kPrefix=0x41460200,kProfile=2;   // what FakeExtensionProfile exports (All Forces protocol 2)
constexpr std::int64_t kOldPrefix=0x41460100;           // All Forces up to 2026-10-25 (protocol 1)
std::vector<dn::LobbyAttribute> Room(std::int64_t type,std::int64_t profile) {
    std::vector<dn::LobbyAttribute> a{{"SEARCH_TYPE",1,type}};
    if(profile)a.push_back({"AF_PROFILE",1,profile});
    return a;
}
}
int main(int argc,char** argv){
    const dn::AllForcesRooms none{};
    const dn::AllForcesRooms ours=dn::AllForcesNamespace(true,true,kPrefix,kProfile);
    const dn::AllForcesRooms old=dn::AllForcesNamespace(true,false,0,0);
    Check(!none.loaded && ours.published && ours.prefix==kPrefix && ours.profile==kProfile && old.loaded && !old.published,
          "namespaces as All Forces exports them");
    // An export that is no namespace: off the 0x100 grid, outside the family, no protocol number.
    Check(!dn::AllForcesNamespace(true,true,kPrefix+1,kProfile).published &&
          !dn::AllForcesNamespace(true,true,0x13,kProfile).published &&
          !dn::AllForcesNamespace(true,true,kPrefix,0).published,"a malformed namespace counts as none");
    // The matrix: rooms with the type encoded or already decoded, marked or not, for each local All Forces.
    for(bool host:{false,true})for(bool normalized:{false,true}) {
        const auto a=Room(host && !normalized ? kPrefix+0x13 : 0x13,host ? kProfile : 0);
        Check(dn::CompatibleRoom(a,none)==!host,"no All Forces: only rooms without All Forces");
        Check(dn::CompatibleRoom(a,ours)==host,"All Forces: only rooms of its protocol");
        Check(!dn::CompatibleRoom(a,old),"an All Forces without the namespace exports: no room at all");
    }
    // The old namespace (0x41460100, AF_PROFILE 1) and a newer protocol's: never ours, and no vanilla room either.
    for(const auto& a:{Room(kOldPrefix+0x13,1),Room(0x13,1),Room(kOldPrefix+0x13,0),Room(0x41460300+0x13,3),Room(0x13,3),
                      Room(kPrefix+0x13,1),Room(kOldPrefix+0x13,kProfile)}) {
        Check(!dn::CompatibleRoom(a,ours),"another All Forces protocol's room is refused by this one");
        Check(!dn::CompatibleRoom(a,none),"and by a machine without All Forces");
    }
    Check(!dn::CompatibleRoom(std::vector<dn::LobbyAttribute>{{"AF_PROFILE",3,0}},ours),"a marker of the wrong type");
    // Decoding for the game: only this machine's namespace (no All Forces loaded in this process yet).
    Check(dn::GameRoomType(kPrefix+0x13)==kPrefix+0x13 && dn::GameRoomType(0x13)==0x13,"no All Forces: nothing decoded");
    Check(dn::RoomIsolationAvailable() && !dn::AllForcesActive(),"no All Forces: rooms are open as without it");

    if(argc>3) {
        const auto packs=LoadLibraryA(argv[2]);
        const dn::AllForcesRooms local=dn::LocalAllForces();
        Check(packs && dn::AllForcesActive() && local.published && local.prefix==kPrefix && local.profile==kProfile,
              "the namespace comes from All Forces' exports");
        Check(dn::RoomIsolationAvailable(),"and its rooms are open");
        Check(dn::GameRoomType(kPrefix+0x13)==0x13 && dn::GameRoomType(kOldPrefix+0x13)==kOldPrefix+0x13 &&
              dn::GameRoomType(0x13)==0x13,"only its own namespace is decoded for the game");
        const auto lastContent=reinterpret_cast<long long(*)()>(GetProcAddress(packs,"FakeAF_LastPackContent"));
        const auto lastPack=reinterpret_cast<const char*(*)()>(GetProcAddress(packs,"FakeAF_LastPack"));
        auto room=[](long long dlc,const char* pack,bool withDlc=true,std::int64_t prefix=kPrefix,std::int64_t profile=kProfile) {
            std::vector<dn::LobbyAttribute> a{{"AF_PROFILE",1,profile},{"SEARCH_TYPE",1,prefix+0x13}};
            if(withDlc)a.push_back({"DLC",1,dlc});
            if(pack){dn::LobbyAttribute t;t.key="AF_PACK";t.type=3;t.text=pack;a.push_back(t);}
            return a;
        };
        dn::PackCheck c{};
        const bool same=dn::CompatibleLocalRoom(room(65,"P65"),&c);
        Check(same && c==dn::PackCheck::Ok && lastContent && lastContent()==65 && std::string(lastPack())=="P65",
              "a room of its protocol and pack is joinable");
        Check(!dn::CompatibleLocalRoom(room(65,"P66"),&c) && c==dn::PackCheck::Refused,"another pack");
        Check(!dn::CompatibleLocalRoom(room(65,nullptr),&c) && c==dn::PackCheck::Refused,"an older host's room");
        Check(std::string(lastPack())=="(null)","absent: nullptr");
        Check(!dn::CompatibleLocalRoom(room(66,"P66"),&c) && c==dn::PackCheck::Refused,"a pack not here");
        Check(dn::CompatibleLocalRoom(room(1,nullptr),&c) && c==dn::PackCheck::Ok,"stock content");
        Check(!dn::CompatibleLocalRoom(room(0,nullptr,false),&c) && c==dn::PackCheck::NoContent,"no content id");
        // The room protocol decides first: a vanilla room, and the old namespace's rooms, whatever their pack.
        Check(!dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{{"DLC",1,65}},&c),"a vanilla room is no All Forces room");
        Check(!dn::CompatibleLocalRoom(room(1,nullptr,true,kOldPrefix,1)),"an old-protocol (0x41460100) room is refused");
        Check(!dn::CompatibleLocalRoom(room(1,nullptr,true,kPrefix,1)),"a room marked with the old protocol is refused");
        FreeLibrary(packs);
        // All Forces from before its room protocol exports: incompatible, its rooms and every other.
        const auto older=LoadLibraryA(argv[3]);
        const dn::AllForcesRooms before=dn::LocalAllForces();
        Check(older && !GetProcAddress(older,"EDF6AF_RoomPrefix") && before.loaded && !before.published,
              "an All Forces without the exports publishes no namespace");
        Check(!dn::RoomIsolationAvailable(),"its rooms are closed");
        Check(!dn::CompatibleLocalRoom(room(1,nullptr,true,kOldPrefix,1)) && !dn::CompatibleLocalRoom(room(1,nullptr)) &&
              !dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{{"DLC",1,1}}),
              "no room is joinable with it, old namespace, new or vanilla");
        Check(dn::GameRoomType(kOldPrefix+0x13)==kOldPrefix+0x13,"and nothing is decoded with the old constants");
        Check(std::string(dn::AllForcesRoomRefusal(before)).find("build from before")!=std::string::npos,
              "the log line says the All Forces build is too old");
        FreeLibrary(older);
    }
    std::printf("All Forces room namespace: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
