// Production LobbyMarker with deterministic EOS member snapshots; no game or network is launched.
#include "../src/lobby_marker.h"
#include <cstring>
#include <cstdio>

namespace {
bool localReady=true, detailsAvailable=true;
const char* local="local";
const char* remoteId="remote";
const char* identityA="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
const char* identityB="bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
std::map<std::string,std::map<std::string,std::string>> attributes;
std::vector<std::string> members{"local","remote"};
HMODULE WINAPI MarkerModule(LPCWSTR) { return reinterpret_cast<HMODULE>(1); }
bool ProfileGate() { return localReady; }
uint32_t ProfileVersion() { return 2; }
void Present() {}
FARPROC WINAPI MarkerProc(HMODULE,LPCSTR name) {
    if(!std::strcmp(name,"EDF6AF_RoomIsolationReady"))return reinterpret_cast<FARPROC>(&ProfileGate);
    if(!std::strcmp(name,"EDF6AF_SupportProtocolVersion"))return reinterpret_cast<FARPROC>(&ProfileVersion);
    return reinterpret_cast<FARPROC>(&Present);
}
}
#define GetModuleHandleW MarkerModule
#define GetProcAddress MarkerProc
#include "../src/lobby_marker.cpp"
#undef GetModuleHandleW
#undef GetProcAddress

namespace dn {
void logf(const char*,...) {}
struct LobbyMarkerFixture {
    static EOS_EResult Copy(EOS_HLobby,const EOS_Lobby_CopyLobbyDetailsHandleOptions*,EOS_HLobbyDetails* out) {
        *out=detailsAvailable ? reinterpret_cast<EOS_HLobbyDetails>(1) : nullptr;return detailsAvailable ? EOS_Success : 1;
    }
    static uint32_t Count(EOS_HLobbyDetails,const EOS_LobbyDetails_GetMemberCountOptions*) { return static_cast<uint32_t>(members.size()); }
    static EOS_ProductUserId Member(EOS_HLobbyDetails,const EOS_LobbyDetails_GetMemberByIndexOptions* o) {
        return o->MemberIndex<members.size() ? reinterpret_cast<EOS_ProductUserId>(members[o->MemberIndex].data()) : nullptr;
    }
    static EOS_EResult String(EOS_ProductUserId p,char* out,int32_t* count) {
        const auto s=reinterpret_cast<const char*>(p);const auto n=std::strlen(s)+1;
        if(*count<static_cast<int32_t>(n))return 1;std::memcpy(out,s,n);*count=static_cast<int32_t>(n);return EOS_Success;
    }
    static EOS_EResult Attribute(EOS_HLobbyDetails,const EOS_LobbyDetails_CopyMemberAttributeByKeyOptions* o,EOS_Lobby_Attribute** out) {
        static EOS_Lobby_AttributeData data{};static EOS_Lobby_Attribute attribute{};
        const auto who=attributes.find(reinterpret_cast<const char*>(o->TargetUserId));
        if(who==attributes.end()){*out=nullptr;return 1;}
        const auto value=who->second.find(o->AttrKey);
        if(value==who->second.end()){*out=nullptr;return 1;}
        data.ValueType=3;data.Value.AsUtf8=value->second.c_str();attribute.Data=&data;*out=&attribute;return EOS_Success;
    }
    static void Release(EOS_HLobbyDetails) {}
    static void ReleaseAttribute(EOS_Lobby_Attribute*) {}
    static void Bind(LobbyMarker& marker) {
        marker.ready_=true;marker.copyDetails_=&Copy;marker.getMemberCount_=&Count;marker.getMemberByIndex_=&Member;
        marker.idToString_=&String;marker.copyMemberAttribute_=&Attribute;marker.releaseDetails_=&Release;
        marker.releaseAttribute_=&ReleaseAttribute;
    }
};
}

int main() {
    int failed=0,checks=0;
    const auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::printf("FAIL %s\n",name);}};
    dn::LobbyMarker marker;dn::LobbyMarkerFixture::Bind(marker);
    const auto enter=[&](const char* room){marker.entered(reinterpret_cast<EOS_HLobby>(1),room,reinterpret_cast<EOS_ProductUserId>(const_cast<char*>(local)),false);};
    const std::vector<std::string> world{local,remoteId};
    enter("room-one");
    attributes[remoteId]={{dn::LobbyMarker::kIdentityKey,identityA},{"EDF6DN_EXT","af-support/2"}};
    attributes[local]={{dn::LobbyMarker::kIdentityKey,identityB},{"EDF6DN_EXT","af-support/2"}};
    marker.memberIdentities(); // observes the profile in the lobby, before a world has participants
    attributes[remoteId].erase("EDF6DN_EXT");attributes[local].erase("EDF6DN_EXT");
    check(marker.extensionCompatible(world),"known process survives missing EOS attributes after world sealing");
    check(!marker.extensionCompatible({local,"unknown"}),"missing actual participant is never admitted");
    attributes[remoteId]["EDF6DN_EXT"]="disabled";
    check(!marker.extensionCompatible(world),"explicit disable revokes the grant");
    attributes[remoteId].erase("EDF6DN_EXT");
    check(!marker.extensionCompatible(world),"missing after disable never resurrects an old grant");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";
    check(marker.extensionCompatible(world),"explicit compatible observation enables same process");
    attributes[remoteId].erase("EDF6DN_EXT");attributes[remoteId][dn::LobbyMarker::kIdentityKey]=identityB;
    marker.memberIdentities();
    check(!marker.extensionCompatible(world),"same PUID with a replacement identity cannot inherit a grant");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";marker.memberIdentities();
    check(marker.extensionCompatible(world),"new identity needs its own capability observation");
    marker.memberGone(remoteId);attributes[remoteId].erase("EDF6DN_EXT");
    check(!marker.extensionCompatible(world),"departed and rejoined member has no old grant");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";marker.memberIdentities();
    enter("room-two");attributes[remoteId].erase("EDF6DN_EXT");
    check(!marker.extensionCompatible(world),"room replacement clears grants even without a left callback");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";marker.memberIdentities();
    detailsAvailable=false;check(!marker.extensionCompatible(world),"no current lobby roster fails closed");detailsAvailable=true;
    localReady=false;check(!marker.extensionCompatible(world),"local dependency gate still required");localReady=true;
    marker.left();enter("room-two");attributes[remoteId].erase("EDF6DN_EXT");
    check(!marker.extensionCompatible(world),"leaving clears all capability observations");
    enter("room-three");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";attributes[remoteId].erase(dn::LobbyMarker::kIdentityKey);
    check(!marker.extensionCompatible(world),"profile with no observed identity cannot grant compatibility");
    attributes[remoteId][dn::LobbyMarker::kIdentityKey]=identityA;marker.memberIdentities();
    attributes[remoteId]["EDF6DN_EXT"]="af-support/3";
    check(!marker.extensionCompatible(world),"future incompatible profile is rejected");
    attributes[remoteId]["EDF6DN_EXT"]="af-support/2";marker.memberIdentities();
    attributes[remoteId][dn::LobbyMarker::kIdentityKey]="invalid";
    check(!marker.extensionCompatible(world),"explicit malformed identity revokes the old grant");
    attributes[remoteId][dn::LobbyMarker::kIdentityKey]=identityA;marker.memberIdentities();
    members={local};check(!marker.extensionCompatible(world),"cached profile never substitutes for a current roster member");
    std::printf("LobbyProfile: %d checks, %d failed\n",checks,failed);return failed ? 1 : 0;
}
