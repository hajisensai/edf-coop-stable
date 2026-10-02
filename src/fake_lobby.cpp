#include "fake_lobby.h"

namespace dn {

struct FakeLobbies::OwnedAttribute {
    std::string key, text;
    EOS_Lobby_AttributeData data{};
    EOS_Lobby_Attribute attribute{};
};

struct FakeLobbies::OwnedInfo {
    std::string roomId;
    EOS_LobbyDetails_Info info{};
};

FakeLobbies::FakeLobbies() = default;
FakeLobbies::~FakeLobbies() = default;

EOS_HLobbyDetails FakeLobbies::make(FakeDetails details) {
    auto owned = std::make_unique<FakeDetails>(std::move(details));
    void* handle = owned.get();
    std::lock_guard<std::mutex> lock(mu_);
    handles_.emplace(handle, std::move(owned));
    return static_cast<EOS_HLobbyDetails>(handle);
}

bool FakeLobbies::lookup(EOS_HLobbyDetails handle, FakeDetails* out) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = handles_.find(handle);
    if (it == handles_.end()) return false;
    if (out) *out = *it->second;
    return true;
}

bool FakeLobbies::owns(EOS_HLobbyDetails handle) const { return lookup(handle, nullptr); }

bool FakeLobbies::release(EOS_HLobbyDetails handle) {
    std::lock_guard<std::mutex> lock(mu_);
    return handles_.erase(handle) != 0;
}

EOS_Lobby_Attribute* FakeLobbies::copyAttribute(const LobbyAttribute& a) {
    auto owned = std::make_unique<OwnedAttribute>();
    owned->key = a.key;
    owned->text = a.text;
    owned->data.ApiVersion = 1;
    owned->data.Key = owned->key.c_str();
    owned->data.ValueType = a.type;
    switch (a.type) {
        case 0: owned->data.Value.AsBool = a.integer != 0; break;
        case 1: owned->data.Value.AsInt64 = a.integer; break;
        case 2: owned->data.Value.AsDouble = a.number; break;
        default: owned->data.Value.AsUtf8 = owned->text.c_str(); break;
    }
    owned->attribute.ApiVersion = 1;
    owned->attribute.Data = &owned->data;
    owned->attribute.Visibility = a.visibility;
    EOS_Lobby_Attribute* out = &owned->attribute;
    std::lock_guard<std::mutex> lock(mu_);
    attributes_.emplace(out, std::move(owned));
    return out;
}

bool FakeLobbies::releaseAttribute(EOS_Lobby_Attribute* attribute) {
    std::lock_guard<std::mutex> lock(mu_);
    return attributes_.erase(attribute) != 0;
}

EOS_LobbyDetails_Info* FakeLobbies::copyInfo(const FakeDetails& details, EOS_ProductUserId owner) {
    auto owned = std::make_unique<OwnedInfo>();
    owned->roomId = details.roomId;
    EOS_LobbyDetails_Info& i = owned->info;
    i.ApiVersion = EOS_LOBBYDETAILS_INFO_API_LATEST;
    i.LobbyId = owned->roomId.c_str();
    i.LobbyOwnerUserId = owner;
    i.MaxMembers = details.maxMembers;
    const auto members = static_cast<uint32_t>(details.members.size());
    i.AvailableSlots = details.maxMembers > members ? details.maxMembers - members : 0;
    i.bAllowInvites = 1;
    i.bAllowHostMigration = 1;
    EOS_LobbyDetails_Info* out = &owned->info;
    std::lock_guard<std::mutex> lock(mu_);
    infos_.emplace(out, std::move(owned));
    return out;
}

bool FakeLobbies::releaseInfo(EOS_LobbyDetails_Info* info) {
    std::lock_guard<std::mutex> lock(mu_);
    return infos_.erase(info) != 0;
}

size_t FakeLobbies::liveHandles() const {
    std::lock_guard<std::mutex> lock(mu_);
    return handles_.size();
}

size_t FakeLobbies::liveCopies() const {
    std::lock_guard<std::mutex> lock(mu_);
    return attributes_.size() + infos_.size();
}

}  // namespace dn
