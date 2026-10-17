#include "../src/mod_room_compat.h"
#include "../src/fake_lobby.h"
#include <cstdio>
#include <string>
int main(int argc,char** argv){
    int failures=0;
    for(bool client:{false,true})for(bool host:{false,true})for(bool normalized:{false,true}) {
        std::vector<dn::LobbyAttribute> a;
        a.push_back({"SEARCH_TYPE",1,host && !normalized ? dn::kAllForcesPrefix+0x13 : 0x13});
        if(host)a.push_back({"AF_PROFILE",1,1});
        if(dn::CompatibleRoom(a,client)!=(host==client))++failures;
    }
    std::vector<dn::LobbyAttribute> future{{"AF_PROFILE",1,2}};
    if(dn::CompatibleRoom(future,true)||dn::CompatibleRoom(future,false))++failures;
    if(argc>1 && std::string(argv[1])!="-") {
        const auto module=LoadLibraryExA(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if(!module || !dn::AllForcesActive() || dn::RoomIsolationAvailable())++failures;
        // DLL loaded but its namespace did not initialize: never treat it as a
        // vanilla installation and never bypass its closed gates via DirectNet.
        const std::vector<dn::LobbyAttribute> af{{"AF_PROFILE",1,1}};
        if(dn::CompatibleLocalRoom(af) || dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{}))++failures;
    }
    // argv[2], argv[3]: All Forces with its mission-pack check (fake profile), and one from before it. The persisted
    // DLC / AF_PACK of a remembered or virtual room decide, as All Forces' own JoinLobby hook decides for EOS rooms.
    if(argc>3) {
        const auto packs=LoadLibraryA(argv[2]);
        if(!packs || !dn::AllForcesActive() || !dn::RoomIsolationAvailable())++failures;
        const auto lastContent=reinterpret_cast<long long(*)()>(GetProcAddress(packs,"FakeAF_LastPackContent"));
        const auto lastPack=reinterpret_cast<const char*(*)()>(GetProcAddress(packs,"FakeAF_LastPack"));
        auto room=[](long long dlc,const char* pack,bool withDlc=true) {
            std::vector<dn::LobbyAttribute> a{{"AF_PROFILE",1,1},{"SEARCH_TYPE",1,dn::kAllForcesPrefix+0x13}};
            if(withDlc)a.push_back({"DLC",1,dlc});
            if(pack){dn::LobbyAttribute t;t.key="AF_PACK";t.type=3;t.text=pack;a.push_back(t);}
            return a;
        };
        dn::PackCheck c{};
        const bool same=dn::CompatibleLocalRoom(room(65,"P65"),&c);
        if(!same || c!=dn::PackCheck::Ok || !lastContent || lastContent()!=65 || std::string(lastPack())!="P65")++failures;
        if(dn::CompatibleLocalRoom(room(65,"P66"),&c) || c!=dn::PackCheck::Refused)++failures;   // another pack
        if(dn::CompatibleLocalRoom(room(65,nullptr),&c) || c!=dn::PackCheck::Refused)++failures;  // an older host's room
        if(std::string(lastPack())!="(null)")++failures;                                         // absent: nullptr
        if(dn::CompatibleLocalRoom(room(66,"P66"),&c) || c!=dn::PackCheck::Refused)++failures;   // a pack not here
        if(!dn::CompatibleLocalRoom(room(1,nullptr),&c) || c!=dn::PackCheck::Ok)++failures;      // stock content
        if(dn::CompatibleLocalRoom(room(0,nullptr,false),&c) || c!=dn::PackCheck::NoContent)++failures;
        // The room profile still decides first: a vanilla room is no All Forces room, whatever its pack.
        if(dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{{"DLC",1,65}},&c))++failures;
        FreeLibrary(packs);
        // All Forces from before its packs: no export, nothing to check, the room profile alone decides.
        const auto old=LoadLibraryA(argv[3]);
        if(!old || GetProcAddress(old,"EDF6AF_RoomPackCompatible"))++failures;
        if(!dn::CompatibleLocalRoom(room(65,"P66"),&c) || c!=dn::PackCheck::NoExport)++failures;
        FreeLibrary(old);
    }
    std::printf("persisted/virtual room profile matrix: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
