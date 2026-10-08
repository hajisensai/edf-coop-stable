#include "../src/mod_room_compat.h"
#include "../src/fake_lobby.h"
#include <cstdio>
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
    if(argc>1) {
        const auto module=LoadLibraryExA(argv[1],nullptr,DONT_RESOLVE_DLL_REFERENCES);
        if(!module || !dn::AllForcesActive() || dn::RoomIsolationAvailable())++failures;
        // DLL loaded but its namespace did not initialize: never treat it as a
        // vanilla installation and never bypass its closed gates via DirectNet.
        const std::vector<dn::LobbyAttribute> af{{"AF_PROFILE",1,1}};
        if(dn::CompatibleLocalRoom(af) || dn::CompatibleLocalRoom(std::vector<dn::LobbyAttribute>{}))++failures;
    }
    std::printf("persisted/virtual room profile matrix: %s\n",failures?"FAIL":"PASS");
    return failures?1:0;
}
