#include "lobby_marker.h"

#include "log.h"

namespace dn {
namespace {

template <typename T>
bool resolve(HMODULE eos, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(eos, name));
    if (!out) logf("EOS export %s not found; plugin detection in the lobby is disabled", name);
    return out != nullptr;
}

void onMarkerPublished(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    bool ok = i->ResultCode == EOS_Success;
    logf("LOBBY published our plugin marker: %s (result %d)", ok ? "ok" : "FAILED, other players treat us as vanilla",
         i->ResultCode);
}

}  // namespace

bool LobbyMarker::init(HMODULE eos) {
    bool ok = resolve(eos, "EOS_Lobby_UpdateLobbyModification", updateModification_);
    ok &= resolve(eos, "EOS_Lobby_UpdateLobby", update_);
    ok &= resolve(eos, "EOS_LobbyModification_AddMemberAttribute", addMemberAttribute_);
    ok &= resolve(eos, "EOS_LobbyModification_Release", releaseModification_);
    ok &= resolve(eos, "EOS_Lobby_CopyLobbyDetailsHandle", copyDetails_);
    ok &= resolve(eos, "EOS_LobbyDetails_CopyMemberAttributeByKey", copyMemberAttribute_);
    ok &= resolve(eos, "EOS_LobbyDetails_Release", releaseDetails_);
    ok &= resolve(eos, "EOS_Lobby_Attribute_Release", releaseAttribute_);
    if (!ok) updateModification_ = nullptr;  // entered() and hasMarker() check this one pointer
    return ok;
}

void LobbyMarker::entered(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId localUser) {
    if (!updateModification_ || !lobby || !lobbyId || !localUser) return;
    {
        std::lock_guard<std::mutex> lock(mu_);
        lobby_ = lobby;
        lobbyId_ = lobbyId;
        localUser_ = localUser;
    }
    EOS_Lobby_UpdateLobbyModificationOptions mo{1, localUser, lobbyId};
    EOS_HLobbyModification mod = nullptr;
    EOS_EResult r = updateModification_(lobby, &mo, &mod);
    if (r != EOS_Success || !mod) {
        logf("LOBBY cannot edit our lobby member (result %d); other players will treat us as vanilla", r);
        return;
    }
    EOS_Lobby_AttributeData data{};
    data.ApiVersion = 1;
    data.Key = kKey;
    data.Value.AsInt64 = kVersion;
    data.ValueType = 1;  // int64
    EOS_LobbyModification_AddMemberAttributeOptions ao{1, &data, 0 /* public */};
    r = addMemberAttribute_(mod, &ao);
    if (r == EOS_Success) {
        EOS_Lobby_UpdateLobbyOptions uo{1, mod};
        update_(lobby, &uo, nullptr, onMarkerPublished);  // EOS copies the modification
    } else {
        logf("LOBBY cannot add our plugin marker (result %d); other players will treat us as vanilla", r);
    }
    releaseModification_(mod);
}

bool LobbyMarker::hasMarker(EOS_ProductUserId remote) const {
    if (!updateModification_ || !remote) return false;
    std::lock_guard<std::mutex> lock(mu_);
    if (!lobby_ || lobbyId_.empty()) return false;
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{1, lobbyId_.c_str(), localUser_};
    EOS_HLobbyDetails details = nullptr;
    if (copyDetails_(lobby_, &o, &details) != EOS_Success || !details) return false;
    EOS_LobbyDetails_CopyMemberAttributeByKeyOptions ko{1, remote, kKey};
    EOS_Lobby_Attribute* attr = nullptr;
    bool found = copyMemberAttribute_(details, &ko, &attr) == EOS_Success && attr && attr->Data;
    if (attr) releaseAttribute_(attr);
    releaseDetails_(details);
    return found;
}

}  // namespace dn
