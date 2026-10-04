// A stand-in EOSSDK-Win64-Shipping.dll for SyncMarkerTests and LobbyStateTests: the lobby and id functions
// syncmarker.cpp, lobbystate.cpp and identity.cpp use, over one in-memory lobby. Completions run on EOS_Platform_Tick, as EOS runs them. The test
// drives it through the FakeEos_* exports.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#define EXPORT extern "C" __declspec(dllexport)

namespace {

struct User {
    char text[33];
};

struct AttributeData {
    std::int32_t ApiVersion;
    const char* Key;
    union {
        std::int64_t AsInt64;
        double AsDouble;
        std::int32_t AsBool;
        const char* AsUtf8;
    } Value;
    std::int32_t ValueType;
};
struct Attribute {
    std::int32_t ApiVersion;
    AttributeData* Data;
    std::int32_t Visibility;
};
struct AddMemberAttributeOptions {
    std::int32_t ApiVersion;
    const AttributeData* Attribute;
    std::int32_t Visibility;
};
struct LobbyIdCallbackInfo {
    std::int32_t ResultCode;
    void* ClientData;
    const char* LobbyId;
};
using LobbyIdCallback = void (*)(const LobbyIdCallbackInfo*);
struct UpdateLobbyOptions {
    std::int32_t ApiVersion;
    void* LobbyModificationHandle;
};
struct CopyMemberAttributeOptions {
    std::int32_t ApiVersion;
    const void* TargetUserId;
    const char* AttrKey;
};
struct GetMemberByIndexOptions {
    std::int32_t ApiVersion;
    std::uint32_t MemberIndex;
};
struct CopyDetailsOptions {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LocalUserId;
};
struct CopyAttributeOptions {
    std::int32_t ApiVersion;
    const char* AttrKey;
};
struct CreateLobbyOptionsHead {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxLobbyMembers;
};
struct JoinLobbyOptionsHead {
    std::int32_t ApiVersion;
    void* LobbyDetailsHandle;
};
struct LeaveOptions {  // Leave and Destroy
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct LobbyDetailsInfo {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LobbyOwnerUserId;
    std::int32_t PermissionLevel;
    std::uint32_t AvailableSlots;
    std::uint32_t MaxMembers;
};

// A copy of the lobby, as EOS hands one out; or, from FakeEos_Details, the handle of a lobby to join.
struct Details {
    std::string lobbyId;
    std::vector<std::string> members;
    std::string owner;
    std::uint32_t maxMembers = 0;
    std::map<std::string, std::int64_t> attributes;
};

constexpr std::int32_t kWillRetry = 0x99;  // this fake's not-final result
constexpr std::int32_t kNotFound = 18, kNotOwner = 9000, kAlreadyMember = 9002;

std::map<std::string, std::unique_ptr<User>> users;
std::vector<std::string> members;                                  // in the lobby, in order
std::map<std::string, std::map<std::string, std::int64_t>> attributes;  // member -> key -> int64
std::map<std::string, std::map<std::string, std::string>> texts;        // member -> key -> UTF-8
std::string self;
std::string lobbyId;
bool copyFails = false;
std::int32_t updateResult = 0;
std::int32_t modificationResult = 0;
int updates = 0;
int modifications = 0;
int leaves = 0;
int destroys = 0;
int joins = 0;
std::string owner;
std::uint32_t maxMembers = 0;
std::map<std::string, std::int64_t> lobbyAttributes;
std::int32_t leaveResult = 0;
std::int32_t destroyResult = 0;
std::deque<std::function<void()>> completions;
std::vector<std::int64_t> searchTypes;  // what a room search finds: each room's SEARCH_TYPE, -1 for none
int finds = 0;

struct Modification {
    std::map<std::string, std::int64_t> attributes;
    std::map<std::string, std::string> texts;
};

const User* Handle(const std::string& id) {
    auto& user = users[id];
    if (!user) {
        user = std::make_unique<User>();
        strncpy_s(user->text, id.c_str(), _TRUNCATE);
    }
    return user.get();
}

bool IsMember(const std::string& id) {
    for (const auto& member : members)
        if (member == id) return true;
    return false;
}

void Complete(LobbyIdCallback callback, void* clientData, std::int32_t result, const std::string& lobby) {
    if (!callback) return;
    completions.push_back([=]() {
        LobbyIdCallbackInfo info{result, clientData, lobby.c_str()};
        callback(&info);
    });
}

}  // namespace

// --- control ---
EXPORT void FakeEos_Reset(const char* selfId) {
    members.clear();
    attributes.clear();
    texts.clear();
    completions.clear();
    self = selfId;
    lobbyId.clear();
    copyFails = false;
    updateResult = 0;
    modificationResult = 0;
    updates = 0;
    modifications = 0;
    leaves = 0;
    destroys = 0;
    joins = 0;
    owner.clear();
    maxMembers = 0;
    lobbyAttributes.clear();
    leaveResult = 0;
    destroyResult = 0;
    searchTypes.clear();
    finds = 0;
}
EXPORT void FakeEos_SetSearchResults(const std::int64_t* values, int count) { searchTypes.assign(values, values + count); }
EXPORT int FakeEos_Finds() { return finds; }
// The lobby id of a details handle the fake handed out.
EXPORT const char* FakeEos_DetailsId(void* details) { return static_cast<Details*>(details)->lobbyId.c_str(); }
EXPORT const void* FakeEos_User(const char* id) { return Handle(id); }
EXPORT void FakeEos_AddMember(const char* id) { members.push_back(id); }
EXPORT void FakeEos_RemoveMember(const char* id) {
    for (auto it = members.begin(); it != members.end(); ++it)
        if (*it == id) {
            members.erase(it);
            break;
        }
}
EXPORT void FakeEos_SetAttribute(const char* member, const char* key, std::int64_t value) { attributes[member][key] = value; }
EXPORT void FakeEos_ClearAttributes(const char* member) { attributes.erase(member); }
EXPORT std::int64_t FakeEos_Attribute(const char* member, const char* key) {
    auto m = attributes.find(member);
    if (m == attributes.end()) return -1;
    auto a = m->second.find(key);
    return a == m->second.end() ? -1 : a->second;
}
EXPORT void FakeEos_SetText(const char* member, const char* key, const char* value) { texts[member][key] = value; }
EXPORT void FakeEos_ClearTexts(const char* member) { texts.erase(member); }
// The text `member` has under `key`, or null.
EXPORT const char* FakeEos_Text(const char* member, const char* key) {
    const auto m = texts.find(member);
    if (m == texts.end()) return nullptr;
    const auto t = m->second.find(key);
    return t == m->second.end() ? nullptr : t->second.c_str();
}
EXPORT void FakeEos_SetCopyFails(int fails) { copyFails = fails != 0; }
EXPORT void FakeEos_SetUpdateResult(std::int32_t result) { updateResult = result; }
EXPORT int FakeEos_Updates() { return updates; }
EXPORT void FakeEos_SetModificationResult(std::int32_t result) { modificationResult = result; }
EXPORT int FakeEos_Modifications() { return modifications; }
EXPORT int FakeEos_Leaves() { return leaves; }
EXPORT std::int32_t FakeEos_WillRetry() { return kWillRetry; }
EXPORT int FakeEos_Destroys() { return destroys; }
EXPORT int FakeEos_Joins() { return joins; }
EXPORT const char* FakeEos_LobbyId() { return lobbyId.c_str(); }
EXPORT int FakeEos_IsMember(const char* id) { return IsMember(id); }
EXPORT void FakeEos_SetOwner(const char* id) { owner = id; }
EXPORT void FakeEos_SetMaxMembers(std::uint32_t count) { maxMembers = count; }
EXPORT void FakeEos_SetLobbyAttribute(const char* key, std::int64_t value) { lobbyAttributes[key] = value; }
EXPORT void FakeEos_SetLeaveResult(std::int32_t result) { leaveResult = result; }
EXPORT void FakeEos_SetDestroyResult(std::int32_t result) { destroyResult = result; }
// This machine is in `id`, owned by `ownerId`, as EOS sees it, whatever the game thinks.
EXPORT void FakeEos_EnterLobby(const char* id, const char* ownerId) {
    lobbyId = id;
    owner = ownerId;
    members = {ownerId};
    if (self != ownerId) members.push_back(self);
    lobbyAttributes.clear();
}
// The handle of `id` as a search result hands it to the game for joining.
EXPORT void* FakeEos_Details(const char* id) {
    auto* details = new Details;
    details->lobbyId = id;
    return details;
}

// --- EOS ---
EXPORT std::int32_t EOS_ProductUserId_IsValid(const void* id) { return id != nullptr; }
EXPORT std::int32_t EOS_ProductUserId_ToString(const void* id, char* out, std::int32_t* length) {
    const auto* user = static_cast<const User*>(id);
    const auto needed = static_cast<std::int32_t>(std::strlen(user->text) + 1);
    if (*length < needed) return 10;
    std::memcpy(out, user->text, needed);
    *length = needed;
    return 0;
}
EXPORT std::int32_t EOS_EResult_IsOperationComplete(std::int32_t result) { return result != kWillRetry; }

EXPORT void EOS_Platform_Tick(void*) {
    std::deque<std::function<void()>> now;
    now.swap(completions);
    for (auto& completion : now) completion();
}

EXPORT const char* EOS_EResult_ToString(std::int32_t result) {
    switch (result) {
    case 0: return "EOS_Success";
    case kNotFound: return "EOS_NotFound";
    case kNotOwner: return "EOS_Lobby_NotOwner";
    case kAlreadyMember: return "EOS_Lobby_LobbyAlreadyExists";
    default: return "EOS_Unknown";
    }
}

EXPORT void EOS_Lobby_CreateLobby(void*, const CreateLobbyOptionsHead* options, void* clientData, LobbyIdCallback callback) {
    lobbyId = "lobby-created";
    members = {self};
    owner = self;
    maxMembers = options ? options->MaxLobbyMembers : 0;
    lobbyAttributes.clear();
    // A not-final run first: the plugin's wrapper must survive it.
    Complete(callback, clientData, kWillRetry, lobbyId);
    Complete(callback, clientData, 0, lobbyId);
}
// EOS refuses a join into a lobby this user is still a member of; otherwise this machine is in the target now.
EXPORT void EOS_Lobby_JoinLobby(void*, const JoinLobbyOptionsHead* options, void* clientData, LobbyIdCallback callback) {
    ++joins;
    const auto* target = options ? static_cast<const Details*>(options->LobbyDetailsHandle) : nullptr;
    const std::string id = target ? target->lobbyId : "lobby-joined";
    if (id == lobbyId && IsMember(self)) {
        Complete(callback, clientData, kAlreadyMember, id);
        return;
    }
    lobbyId = id;
    members.push_back(self);
    Complete(callback, clientData, 0, lobbyId);
}
EXPORT void EOS_Lobby_LeaveLobby(void*, const LeaveOptions* options, void* clientData, LobbyIdCallback callback) {
    ++leaves;
    const std::string id = options && options->LobbyId ? options->LobbyId : lobbyId;
    if (leaveResult == 0) {
        FakeEos_RemoveMember(self.c_str());
        attributes.erase(self);
        texts.erase(self);
    }
    Complete(callback, clientData, leaveResult, id);
}
// Only the owner may destroy a lobby.
EXPORT void EOS_Lobby_DestroyLobby(void*, const LeaveOptions* options, void* clientData, LobbyIdCallback callback) {
    ++destroys;
    const std::string id = options && options->LobbyId ? options->LobbyId : lobbyId;
    const std::int32_t result = !owner.empty() && owner != self ? kNotOwner : destroyResult;
    if (result == 0) {
        FakeEos_RemoveMember(self.c_str());
        attributes.erase(self);
        texts.erase(self);
    }
    Complete(callback, clientData, result, id);
}

EXPORT std::int32_t EOS_Lobby_UpdateLobbyModification(void*, const void*, void** modification) {
    ++modifications;
    if (modificationResult != 0) return modificationResult;
    *modification = new Modification;
    return 0;
}
EXPORT std::int32_t EOS_LobbyModification_AddMemberAttribute(void* modification, const AddMemberAttributeOptions* options) {
    if (!options->Attribute) return 10;
    auto* changes = static_cast<Modification*>(modification);
    if (options->Attribute->ValueType == 3 && options->Attribute->Value.AsUtf8) {
        changes->texts[options->Attribute->Key] = options->Attribute->Value.AsUtf8;
        return 0;
    }
    if (options->Attribute->ValueType != 1) return 10;
    changes->attributes[options->Attribute->Key] = options->Attribute->Value.AsInt64;
    return 0;
}
EXPORT void EOS_LobbyModification_Release(void* modification) { delete static_cast<Modification*>(modification); }
EXPORT void EOS_Lobby_UpdateLobby(void*, const UpdateLobbyOptions* options, void* clientData, LobbyIdCallback callback) {
    ++updates;
    const Modification copy = *static_cast<Modification*>(options->LobbyModificationHandle);  // EOS copies it
    const std::int32_t result = updateResult;
    const std::string lobby = lobbyId;
    completions.push_back([=]() {
        if (result != 0) {
            LobbyIdCallbackInfo info{result, clientData, lobby.c_str()};
            callback(&info);
            return;
        }
        for (const auto& [key, value] : copy.attributes) attributes[self][key] = value;
        for (const auto& [key, value] : copy.texts) texts[self][key] = value;
        LobbyIdCallbackInfo info{result, clientData, lobby.c_str()};
        callback(&info);
    });
}

// A copy exists only of the lobby this user is in.
EXPORT std::int32_t EOS_Lobby_CopyLobbyDetailsHandle(void*, const CopyDetailsOptions* options, void** details) {
    if (copyFails || lobbyId.empty() || !IsMember(self)) return kNotFound;
    if (options && options->LobbyId && lobbyId != options->LobbyId) return kNotFound;
    *details = new Details{lobbyId, members, owner, maxMembers, lobbyAttributes};
    return 0;
}
EXPORT std::uint32_t EOS_LobbyDetails_GetMemberCount(void* details, const void*) {
    return static_cast<std::uint32_t>(static_cast<Details*>(details)->members.size());
}
EXPORT const void* EOS_LobbyDetails_GetMemberByIndex(void* details, const GetMemberByIndexOptions* options) {
    const auto& list = static_cast<Details*>(details)->members;
    return options->MemberIndex < list.size() ? Handle(list[options->MemberIndex]) : nullptr;
}
EXPORT std::int32_t EOS_LobbyDetails_CopyMemberAttributeByKey(void*, const CopyMemberAttributeOptions* options,
                                                              Attribute** out) {
    if (const char* text = FakeEos_Text(static_cast<const User*>(options->TargetUserId)->text, options->AttrKey)) {
        auto* data = new AttributeData{1, options->AttrKey, {}, 3};
        const std::size_t size = std::string_view(text).size() + 1;
        auto* copy = new char[size];
        std::memcpy(copy, text, size);
        data->Value.AsUtf8 = copy;
        *out = new Attribute{1, data, 0};
        return 0;
    }
    const std::int64_t value = FakeEos_Attribute(static_cast<const User*>(options->TargetUserId)->text, options->AttrKey);
    if (value < 0) return kNotFound;
    auto* data = new AttributeData{1, options->AttrKey, {}, 1};
    data->Value.AsInt64 = value;
    *out = new Attribute{1, data, 0};
    return 0;
}
EXPORT std::int32_t EOS_LobbyDetails_CopyAttributeByKey(void* details, const CopyAttributeOptions* options, Attribute** out) {
    const auto& found = static_cast<Details*>(details)->attributes;
    const auto it = found.find(options->AttrKey);
    if (it == found.end()) return kNotFound;
    auto* data = new AttributeData{1, options->AttrKey, {}, 1};
    data->Value.AsInt64 = it->second;
    *out = new Attribute{1, data, 0};
    return 0;
}
EXPORT std::int32_t EOS_LobbyDetails_CopyInfo(void* details, const void*, LobbyDetailsInfo** out) {
    const auto* lobby = static_cast<Details*>(details);
    const auto taken = static_cast<std::uint32_t>(lobby->members.size());
    *out = new LobbyDetailsInfo{1, lobby->lobbyId.c_str(), lobby->owner.empty() ? nullptr : Handle(lobby->owner), 0,
                                lobby->maxMembers > taken ? lobby->maxMembers - taken : 0, lobby->maxMembers};
    return 0;
}
EXPORT void EOS_LobbyDetails_Info_Release(LobbyDetailsInfo* info) { delete info; }
EXPORT const void* EOS_LobbyDetails_GetLobbyOwner(void* details, const void*) {
    const auto* lobby = static_cast<Details*>(details);
    return lobby->owner.empty() ? nullptr : Handle(lobby->owner);
}
EXPORT void EOS_LobbyDetails_Release(void* details) { delete static_cast<Details*>(details); }
// A room search: every handle finds the results FakeEos_SetSearchResults set, by their SEARCH_TYPE.
struct SearchResultOptions {
    std::int32_t ApiVersion;
    std::uint32_t LobbyIndex;
};
EXPORT void EOS_LobbySearch_Find(void*, const void*, void*, void*) { ++finds; }
EXPORT std::uint32_t EOS_LobbySearch_GetSearchResultCount(void*, const void*) {
    return static_cast<std::uint32_t>(searchTypes.size());
}
EXPORT std::int32_t EOS_LobbySearch_CopySearchResultByIndex(void*, const SearchResultOptions* options, void** out) {
    if (!options || options->LobbyIndex >= searchTypes.size()) return kNotFound;
    auto* details = new Details;
    details->lobbyId = "room" + std::to_string(options->LobbyIndex);
    if (searchTypes[options->LobbyIndex] >= 0) details->attributes["SEARCH_TYPE"] = searchTypes[options->LobbyIndex];
    *out = details;
    return 0;
}
EXPORT void EOS_Lobby_Attribute_Release(Attribute* attribute) {
    if (attribute->Data->ValueType == 3) delete[] attribute->Data->Value.AsUtf8;
    delete attribute->Data;
    delete attribute;
}
