#include "lobby_marker.h"

#include <cstdio>

#include "log.h"

namespace dn {
namespace {

template <typename T>
bool resolve(HMODULE eos, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(eos, name));
    if (!out) logf("EOS export %s not found; lobby plugin detection and direct auto-connect are disabled", name);
    return out != nullptr;
}

void onPublished(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    bool ok = i->ResultCode == EOS_Success;
    logf("LOBBY published our plugin info: %s (result %d)", ok ? "ok" : "FAILED, other players treat us as vanilla",
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
    ok &= resolve(eos, "EOS_LobbyDetails_GetLobbyOwner", getOwner_);
    ok &= resolve(eos, "EOS_LobbyDetails_Release", releaseDetails_);
    ok &= resolve(eos, "EOS_Lobby_Attribute_Release", releaseAttribute_);
    getMemberAttributeCount_ = reinterpret_cast<PFN_EOS_LobbyDetails_GetMemberAttributeCount>(
        GetProcAddress(eos, "EOS_LobbyDetails_GetMemberAttributeCount"));
    getMemberCount_ = reinterpret_cast<PFN_EOS_LobbyDetails_GetMemberCount>(
        GetProcAddress(eos, "EOS_LobbyDetails_GetMemberCount"));
    ready_ = ok;
    return ok;
}

void LobbyMarker::entered(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId localUser, bool owner) {
    if (!lobby || !lobbyId || !localUser) return;
    std::lock_guard<std::mutex> lock(mu_);
    lobby_ = lobby;
    lobbyId_ = lobbyId;
    localUser_ = localUser;
    owner_ = owner;
    dirty_ = true;
}

void LobbyMarker::left() {
    std::lock_guard<std::mutex> lock(mu_);
    lobbyId_.clear();
    owner_ = false;
    dirty_ = false;
}

void LobbyMarker::promoted() {
    std::lock_guard<std::mutex> lock(mu_);
    if (lobbyId_.empty() || owner_) return;
    owner_ = true;
    dirty_ = !address_.empty();  // now we advertise our address, if we host one
}

void LobbyMarker::memberJoined() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!lobbyId_.empty()) dirty_ = true;
}

void LobbyMarker::setAddress(const std::string& address) {
    std::lock_guard<std::mutex> lock(mu_);
    if (address_ == address) return;
    address_ = address;
    dirty_ = !lobbyId_.empty();
}

void LobbyMarker::tick() {
    if (!ready_) return;
    std::lock_guard<std::mutex> lock(mu_);
    if (!dirty_ || lobbyId_.empty()) return;
    dirty_ = false;
    EOS_Lobby_UpdateLobbyModificationOptions mo{1, localUser_, lobbyId_.c_str()};
    EOS_HLobbyModification mod = nullptr;
    EOS_EResult r = updateModification_(lobby_, &mo, &mod);
    if (r != EOS_Success || !mod) {
        logf("LOBBY cannot edit our lobby member (result %d); other players treat us as vanilla", r);
        return;
    }
    EOS_Lobby_AttributeData marker{};
    marker.ApiVersion = 1;
    marker.Key = kKey;
    marker.Value.AsInt64 = kVersion;
    marker.ValueType = 1;  // int64
    EOS_LobbyModification_AddMemberAttributeOptions ao{1, &marker, 0 /* public */};
    r = addMemberAttribute_(mod, &ao);
    EOS_Lobby_AttributeData seq{};
    seq.ApiVersion = 1;
    seq.Key = kSeqKey;
    seq.Value.AsInt64 = ++seq_;
    seq.ValueType = 1;  // int64
    EOS_LobbyModification_AddMemberAttributeOptions as{1, &seq, 0};
    if (r == EOS_Success) r = addMemberAttribute_(mod, &as);
    // Only a host advertises an address; a player hosting nothing publishes none.
    if (r == EOS_Success && owner_ && !address_.empty()) {
        EOS_Lobby_AttributeData addr{};
        addr.ApiVersion = 1;
        addr.Key = kAddressKey;
        addr.Value.AsUtf8 = address_.c_str();
        addr.ValueType = 3;  // string
        EOS_LobbyModification_AddMemberAttributeOptions aa{1, &addr, 0};
        r = addMemberAttribute_(mod, &aa);
    }
    if (r == EOS_Success) {
        EOS_Lobby_UpdateLobbyOptions uo{1, mod};
        update_(lobby_, &uo, nullptr, onPublished);  // EOS copies the modification
        if (owner_ && !address_.empty()) logf("LOBBY advertising our direct-link address: %s", address_.c_str());
    } else {
        logf("LOBBY cannot set our plugin info (result %d); other players treat us as vanilla", r);
    }
    releaseModification_(mod);
}

bool LobbyMarker::inLobby() const {
    std::lock_guard<std::mutex> lock(mu_);
    return !lobbyId_.empty();
}

bool LobbyMarker::isOwner() const {
    std::lock_guard<std::mutex> lock(mu_);
    return owner_;
}

EOS_HLobbyDetails LobbyMarker::copyDetailsLocked() const {
    if (!ready_ || !lobby_ || lobbyId_.empty()) return nullptr;
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{1, lobbyId_.c_str(), localUser_};
    EOS_HLobbyDetails details = nullptr;
    return copyDetails_(lobby_, &o, &details) == EOS_Success ? details : nullptr;
}

bool LobbyMarker::readAttribute(EOS_HLobbyDetails details, EOS_ProductUserId member, const char* key,
                                std::string* value) const {
    EOS_LobbyDetails_CopyMemberAttributeByKeyOptions ko{1, member, key};
    EOS_Lobby_Attribute* attr = nullptr;
    bool found = copyMemberAttribute_(details, &ko, &attr) == EOS_Success && attr && attr->Data;
    if (found && value && attr->Data->ValueType == 3 && attr->Data->Value.AsUtf8) *value = attr->Data->Value.AsUtf8;
    if (attr) releaseAttribute_(attr);
    return found;
}

bool LobbyMarker::hasMarker(EOS_ProductUserId remote) const {
    if (!remote) return false;
    std::lock_guard<std::mutex> lock(mu_);
    EOS_HLobbyDetails details = copyDetailsLocked();
    if (!details) return false;
    bool found = readAttribute(details, remote, kKey, nullptr);
    releaseDetails_(details);
    return found;
}

std::string LobbyMarker::describeOwner() const {
    std::lock_guard<std::mutex> lock(mu_);
    EOS_HLobbyDetails details = copyDetailsLocked();
    if (!details) return "no local copy of the lobby";
    EOS_LobbyDetails_GetLobbyOwnerOptions oo{1};
    EOS_ProductUserId id = getOwner_(details, &oo);
    bool marker = id && readAttribute(details, id, kKey, nullptr);
    EOS_LobbyDetails_GetMemberAttributeCountOptions ac{1, id};
    EOS_LobbyDetails_GetMemberCountOptions mc{1};
    long attrs = id && getMemberAttributeCount_ ? static_cast<long>(getMemberAttributeCount_(details, &ac)) : -1;
    long members = getMemberCount_ ? static_cast<long>(getMemberCount_(details, &mc)) : -1;
    releaseDetails_(details);
    char buf[160];
    snprintf(buf, sizeof(buf), "owner %s, plugin marker %s, owner attributes visible %ld, members %ld",
             id ? "known" : "UNKNOWN", marker ? "yes" : "no", attrs, members);
    return buf;
}

std::string LobbyMarker::ownerAddress(EOS_ProductUserId* owner) const {
    std::lock_guard<std::mutex> lock(mu_);
    EOS_HLobbyDetails details = copyDetailsLocked();
    if (!details) return {};
    EOS_LobbyDetails_GetLobbyOwnerOptions oo{1};
    EOS_ProductUserId id = getOwner_(details, &oo);
    std::string address;
    if (id) readAttribute(details, id, kAddressKey, &address);
    releaseDetails_(details);
    if (owner) *owner = id;
    return address;
}

}  // namespace dn
