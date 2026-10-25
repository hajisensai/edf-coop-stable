#include "../src/mod_room_compat.h"
#include "../src/fake_lobby.h"
#include <cstdio>
#include <string>
// All Forces' room namespace as Coop reads it (mod_room_compat.h), for the three machines EDF6Coop 2.7.0 meets: no All
// Forces, an All Forces of room protocol 1 (no namespace exports: every All Forces released up to 2026-10, with which
// this EDF6Coop keeps 2.6.1's rooms), and one of protocol 2 (EDF6AF_RoomPrefix / EDF6AF_RoomProfile).
// argv: "-" or nothing; then, for the DLL part, All Forces with its namespace and mission packs (FakeExtensionProfile)
// and an All Forces from before both (FakeExtensionNoPacks: EDF6AF_DecodeRoomType, neither namespace export, no
// EDF6AF_RoomPackCompatible).
namespace {
int failures=0;
void Check(bool ok,const char* what){ if(!ok){++failures;std::printf("FAIL: %s\n",what);} }
constexpr std::int64_t kPrefix=0x41460200,kProfile=2;   // what FakeExtensionProfile exports (All Forces protocol 2)
constexpr std::int64_t kOldPrefix=0x41460100;           // All Forces up to 2026-10 (protocol 1, no exports)
std::vector<dn::LobbyAttribute> Room(std::int64_t type,std::int64_t profile) {
    std::vector<dn::LobbyAttribute> a{{"SEARCH_TYPE",1,type}};
    if(profile)a.push_back({"AF_PROFILE",1,profile});
    return a;
}
}
int main(int argc,char** argv){
    using dn::RoomRefusal;
    const dn::AllForcesRooms none{};
    const dn::AllForcesRooms ours=dn::AllForcesNamespace(true,true,kPrefix,kProfile);
    const dn::AllForcesRooms old=dn::AllForcesLegacyNamespace();
    Check(!none.loaded && ours.published && !ours.legacy && ours.prefix==kPrefix && ours.profile==kProfile,
          "namespaces as All Forces exports them");
    Check(old.loaded && old.published && old.legacy && old.prefix==kOldPrefix && old.profile==1,
          "an All Forces without the exports has room protocol 1's namespace, the one EDF6Coop 2.6.1 kept");
    // What the exports make of an EDF6VehicleCrew.dll.
    {
        const auto fresh=dn::AllForcesFromExports(true,true,true,kPrefix,kProfile);
        const auto fallback=dn::AllForcesFromExports(false,false,true,0,0);
        const auto vanilla=dn::AllForcesFromExports(false,false,false,0,0);
        const auto half=dn::AllForcesFromExports(true,false,true,kPrefix,0);
        const auto halfOther=dn::AllForcesFromExports(false,true,true,0,kProfile);
        Check(fresh.published && !fresh.legacy && fresh.prefix==kPrefix && fresh.profile==kProfile,
              "both exports: the namespace they say (the decoder is not asked)");
        Check(fallback.published && fallback.legacy && fallback.prefix==kOldPrefix && fallback.profile==1,
              "the decoder alone: room protocol 1");
        Check(!vanilla.loaded,"neither: no All Forces to the rooms, as in 2.6.1");
        Check(half.loaded && !half.published && halfOther.loaded && !halfOther.published,
              "one export without the other: no namespace, and no fallback to protocol 1");
    }
    // An export that is no namespace: off the 0x100 grid, outside the family, no protocol number.
    Check(!dn::AllForcesNamespace(true,true,kPrefix+1,kProfile).published &&
          !dn::AllForcesNamespace(true,true,0x13,kProfile).published &&
          !dn::AllForcesNamespace(true,true,kPrefix,0).published,"a malformed namespace counts as none");
    const dn::AllForcesRooms broken=dn::AllForcesNamespace(true,true,kPrefix+1,kProfile);
    // The matrix: rooms with the type encoded or already decoded, marked or not, for each local All Forces.
    for(bool host:{false,true})for(bool normalized:{false,true}) {
        const auto a=Room(host && !normalized ? kPrefix+0x13 : 0x13,host ? kProfile : 0);
        const auto b=Room(host && !normalized ? kOldPrefix+0x13 : 0x13,host ? 1 : 0);
        Check(dn::CompatibleRoom(a,none)==!host && dn::CompatibleRoom(b,none)==!host,
              "no All Forces: only rooms without All Forces");
        Check(dn::CompatibleRoom(a,ours)==host,"protocol 2: only rooms of its protocol");
        Check(dn::CompatibleRoom(b,old)==host,"protocol 1: only rooms of its protocol, as with 2.6.1");
        Check(!dn::CompatibleRoom(a,broken) && !dn::CompatibleRoom(b,broken),"a broken namespace: no room at all");
    }
    // Protocols 1 and 2 share no room, either way round: their prefixes and markers differ.
    for(const auto& a:{Room(kOldPrefix+0x13,1),Room(0x13,1),Room(kOldPrefix+0x13,0),Room(0x41460300+0x13,3),Room(0x13,3),
                      Room(kPrefix+0x13,1),Room(kOldPrefix+0x13,kProfile)}) {
        Check(!dn::CompatibleRoom(a,ours),"another All Forces protocol's room is refused by protocol 2");
        Check(!dn::CompatibleRoom(a,none),"and by a machine without All Forces");
    }
    for(const auto& a:{Room(kPrefix+0x13,kProfile),Room(0x13,kProfile),Room(kPrefix+0x13,0),Room(kPrefix+0x13,1),
                      Room(kOldPrefix+0x13,kProfile)})
        Check(!dn::CompatibleRoom(a,old),"and protocol 1 refuses protocol 2's rooms");
    Check(!dn::CompatibleRoom(std::vector<dn::LobbyAttribute>{{"AF_PROFILE",3,0}},ours),"a marker of the wrong type");
    // Why, for the log and the player: an older or newer host's All Forces is told as such.
    Check(dn::RoomRefusalOf(Room(0x13,0),ours)==RoomRefusal::VanillaRoom &&
          dn::RoomRefusalOf(Room(kPrefix+0x13,kProfile),none)==RoomRefusal::AllForcesRoom &&
          dn::RoomRefusalOf(Room(0x13,0),broken)==RoomRefusal::NoNamespace,"the refusal says what kind of room it was");
    Check(dn::RoomRefusalOf(Room(kOldPrefix+0x13,1),ours)==RoomRefusal::OlderProtocol &&
          dn::RoomRefusalOf(Room(kOldPrefix+0x13,0),ours)==RoomRefusal::OlderProtocol,
          "protocol 2 meets a room of protocol 1: its host's All Forces is older (marker or prefix alone)");
    Check(dn::RoomRefusalOf(Room(kPrefix+0x13,kProfile),old)==RoomRefusal::NewerProtocol &&
          dn::RoomRefusalOf(Room(kPrefix+0x13,0),old)==RoomRefusal::NewerProtocol,
          "protocol 1 meets a room of protocol 2: update All Forces");
    Check(dn::RoomRefusalOf(Room(kOldPrefix+0x13,kProfile),ours)==RoomRefusal::OtherProtocol,
          "a room whose prefix and marker disagree is another protocol's");
    Check(dn::RoomRefusalOf(Room(kPrefix+0x13,kProfile),ours)==RoomRefusal::None &&
          dn::RoomRefusalOf(Room(0x13,0),none)==RoomRefusal::None,"a joinable room is no refusal");
    for(const RoomRefusal r:{RoomRefusal::NotReady,RoomRefusal::NoNamespace,RoomRefusal::AllForcesRoom,
                             RoomRefusal::VanillaRoom,RoomRefusal::OlderProtocol,RoomRefusal::NewerProtocol,
                             RoomRefusal::OtherProtocol}) {
        const std::string notice=dn::RoomRefusalNotice(r);
        bool ascii=true;
        for(const char c:notice)ascii=ascii && static_cast<unsigned char>(c)<0x80;
        Check(notice.rfind("Room refused: ",0)==0 && notice.size()<=64 && ascii,
              "every refusal tells the player in one short ASCII line");
    }
    Check(std::string(dn::RoomRefusalNotice(RoomRefusal::NewerProtocol)).find("update All Forces")!=std::string::npos,
          "the player behind is told to update All Forces");
    Check(!dn::RoomRefusalNotice(RoomRefusal::None)[0],"nothing to tell for a joinable room");
    {
        dn::LocalRoomVerdict refused{RoomRefusal::OlderProtocol,dn::PackCheck::NoExport};
        dn::LocalRoomVerdict pack{RoomRefusal::None,dn::PackCheck::Refused};
        dn::LocalRoomVerdict content{RoomRefusal::None,dn::PackCheck::NoContent};
        Check(!refused.ok() && std::string(refused.notice())==dn::RoomRefusalNotice(RoomRefusal::OlderProtocol),
              "a namespace refusal is told");
        Check(!pack.ok() && !pack.notice()[0],"a pack All Forces refused it explains itself: not told twice");
        Check(!content.ok() && content.notice()[0],"a room without a content id is told");
    }
    // Decoding for the game, no All Forces loaded in this process yet: nothing is decoded and every room is open.
    Check(dn::GameRoomType(kPrefix+0x13)==kPrefix+0x13 && dn::GameRoomType(kOldPrefix+0x13)==kOldPrefix+0x13 &&
          dn::GameRoomType(0x13)==0x13,"no All Forces: nothing decoded");
    Check(dn::RoomIsolationAvailable() && !dn::AllForcesActive() && !dn::LocalAllForces().loaded,
          "no All Forces: rooms are open as without it");
    Check(dn::CompatibleLocalRoom(Room(0x13,0)) && !dn::CompatibleLocalRoom(Room(kPrefix+0x13,kProfile)) &&
          !dn::CompatibleLocalRoom(Room(kOldPrefix+0x13,1)),"no All Forces: vanilla rooms only, of either protocol none");

    if(argc>3) {
        // All Forces of room protocol 2: its exports.
        const auto packs=LoadLibraryA(argv[2]);
        const dn::AllForcesRooms local=dn::LocalAllForces();
        Check(packs && dn::AllForcesActive() && local.published && !local.legacy && local.prefix==kPrefix &&
              local.profile==kProfile,"the namespace comes from All Forces' exports");
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
        Check(dn::JudgeLocalRoom(room(1,nullptr,true,kOldPrefix,1)).refusal==RoomRefusal::OlderProtocol,
              "and the player hears that its host's All Forces is older");
        FreeLibrary(packs);
        // All Forces from before its room protocol exports (protocol 1): 2.6.1's namespace, its rooms as before.
        const auto older=LoadLibraryA(argv[3]);
        const dn::AllForcesRooms before=dn::LocalAllForces();
        Check(older && !GetProcAddress(older,"EDF6AF_RoomPrefix") && GetProcAddress(older,"EDF6AF_DecodeRoomType") &&
              before.loaded && before.published && before.legacy && before.prefix==kOldPrefix && before.profile==1,
              "an All Forces without the exports gets room protocol 1's namespace");
        Check(dn::AllForcesActive() && dn::RoomIsolationAvailable(),"its rooms are open (EDF6AF_RoomIsolationReady)");
        Check(dn::CompatibleLocalRoom(room(1,nullptr,true,kOldPrefix,1),&c) && c==dn::PackCheck::NoExport &&
              dn::CompatibleLocalRoom(room(65,"P66",true,kOldPrefix,1),&c) && c==dn::PackCheck::NoExport,
              "a room of protocol 1 is joinable, and without a pack check there is nothing more to ask");
        Check(dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{{"AF_PROFILE",1,1},{"SEARCH_TYPE",1,0x13}}),
              "also persisted with its type already decoded for the game");
        Check(!dn::CompatibleLocalRoom(room(1,nullptr)) && !dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{{"DLC",1,1}}),
              "protocol 2's rooms and vanilla rooms are not");
        Check(dn::JudgeLocalRoom(room(1,nullptr)).refusal==RoomRefusal::NewerProtocol &&
              std::string(dn::JudgeLocalRoom(room(1,nullptr)).notice()).find("update All Forces")!=std::string::npos,
              "a protocol 2 room tells this player to update All Forces");
        Check(dn::GameRoomType(kOldPrefix+0x13)==0x13 && dn::GameRoomType(kPrefix+0x13)==kPrefix+0x13,
              "protocol 1's types are decoded for the game, protocol 2's are not");
        // Its readiness still gates its rooms, as in 2.6.1.
        const auto setReady=reinterpret_cast<void(*)(bool)>(GetProcAddress(older,"FakeAF_SetReady"));
        Check(setReady!=nullptr,"the fixture can close All Forces' room isolation");
        if(setReady) {
            setReady(false);
            Check(!dn::RoomIsolationAvailable() && !dn::CompatibleLocalRoom(room(1,nullptr,true,kOldPrefix,1)) &&
                  dn::JudgeLocalRoom(room(1,nullptr,true,kOldPrefix,1)).refusal==RoomRefusal::NotReady,
                  "not ready: no room, and the player hears why");
            setReady(true);
        }
        FreeLibrary(older);
    }
    std::printf("All Forces room namespace: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
