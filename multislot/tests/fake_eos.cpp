// A stand-in EOSSDK-Win64-Shipping.dll for SyncMarkerTests: the lobby and id functions syncmarker.cpp and
// identity.cpp use, over one in-memory lobby. Completions run on EOS_Platform_Tick, as EOS runs them. The test
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

constexpr std::int32_t kWillRetry = 0x99;  // this fake's not-final result

std::map<std::string, std::unique_ptr<User>> users;
std::vector<std::string> members;                                  // in the lobby, in order
std::map<std::string, std::map<std::string, std::int64_t>> attributes;  // member -> key -> int64
std::string self;
std::string lobbyId;
bool copyFails = false;
std::int32_t updateResult = 0;
std::int32_t modificationResult = 0;
int updates = 0;
int modifications = 0;
int leaves = 0;
std::deque<std::function<void()>> completions;

struct Modification {
    std::map<std::string, std::int64_t> attributes;
};

const User* Handle(const std::string& id) {
    auto& user = users[id];
    if (!user) {
        user = std::make_unique<User>();
        strncpy_s(user->text, id.c_str(), _TRUNCATE);
    }
    return user.get();
}

void Complete(LobbyIdCallback callback, void* clientData, std::int32_t result, const std::string& lobby) {
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
    completions.clear();
    self = selfId;
    lobbyId.clear();
    copyFails = false;
    updateResult = 0;
    modificationResult = 0;
    updates = 0;
    modifications = 0;
    leaves = 0;
}
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
EXPORT void FakeEos_SetCopyFails(int fails) { copyFails = fails != 0; }
EXPORT void FakeEos_SetUpdateResult(std::int32_t result) { updateResult = result; }
EXPORT int FakeEos_Updates() { return updates; }
EXPORT void FakeEos_SetModificationResult(std::int32_t result) { modificationResult = result; }
EXPORT int FakeEos_Modifications() { return modifications; }
EXPORT int FakeEos_Leaves() { return leaves; }
EXPORT std::int32_t FakeEos_WillRetry() { return kWillRetry; }

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

EXPORT void EOS_Lobby_CreateLobby(void*, const void*, void* clientData, LobbyIdCallback callback) {
    lobbyId = "lobby-created";
    members = {self};
    // A not-final run first: the plugin's wrapper must survive it.
    Complete(callback, clientData, kWillRetry, lobbyId);
    Complete(callback, clientData, 0, lobbyId);
}
EXPORT void EOS_Lobby_JoinLobby(void*, const void*, void* clientData, LobbyIdCallback callback) {
    lobbyId = "lobby-joined";
    members.push_back(self);
    Complete(callback, clientData, 0, lobbyId);
}
EXPORT void EOS_Lobby_LeaveLobby(void*, const void*, void*, void*) {
    ++leaves;
    FakeEos_RemoveMember(self.c_str());
    attributes.erase(self);
}
EXPORT void EOS_Lobby_DestroyLobby(void*, const void*, void*, void*) { EOS_Lobby_LeaveLobby(nullptr, nullptr, nullptr, nullptr); }

EXPORT std::int32_t EOS_Lobby_UpdateLobbyModification(void*, const void*, void** modification) {
    ++modifications;
    if (modificationResult != 0) return modificationResult;
    *modification = new Modification;
    return 0;
}
EXPORT std::int32_t EOS_LobbyModification_AddMemberAttribute(void* modification, const AddMemberAttributeOptions* options) {
    if (!options->Attribute || options->Attribute->ValueType != 1) return 10;
    static_cast<Modification*>(modification)->attributes[options->Attribute->Key] = options->Attribute->Value.AsInt64;
    return 0;
}
EXPORT void EOS_LobbyModification_Release(void* modification) { delete static_cast<Modification*>(modification); }
EXPORT void EOS_Lobby_UpdateLobby(void*, const UpdateLobbyOptions* options, void* clientData, LobbyIdCallback callback) {
    ++updates;
    const auto copy = static_cast<Modification*>(options->LobbyModificationHandle)->attributes;  // EOS copies it
    const std::int32_t result = updateResult;
    const std::string lobby = lobbyId;
    completions.push_back([=]() {
        if (result == 0)
            for (const auto& [key, value] : copy) attributes[self][key] = value;
        LobbyIdCallbackInfo info{result, clientData, lobby.c_str()};
        callback(&info);
    });
}

EXPORT std::int32_t EOS_Lobby_CopyLobbyDetailsHandle(void*, const void*, void** details) {
    if (copyFails || lobbyId.empty()) return 18;  // EOS_NotFound
    *details = new std::vector<std::string>(members);
    return 0;
}
EXPORT std::uint32_t EOS_LobbyDetails_GetMemberCount(void* details, const void*) {
    return static_cast<std::uint32_t>(static_cast<std::vector<std::string>*>(details)->size());
}
EXPORT const void* EOS_LobbyDetails_GetMemberByIndex(void* details, const GetMemberByIndexOptions* options) {
    const auto& list = *static_cast<std::vector<std::string>*>(details);
    return options->MemberIndex < list.size() ? Handle(list[options->MemberIndex]) : nullptr;
}
EXPORT std::int32_t EOS_LobbyDetails_CopyMemberAttributeByKey(void*, const CopyMemberAttributeOptions* options,
                                                              Attribute** out) {
    const std::int64_t value = FakeEos_Attribute(static_cast<const User*>(options->TargetUserId)->text, options->AttrKey);
    if (value < 0) return 18;
    auto* data = new AttributeData{1, options->AttrKey, {}, 1};
    data->Value.AsInt64 = value;
    *out = new Attribute{1, data, 0};
    return 0;
}
EXPORT void EOS_LobbyDetails_Release(void* details) { delete static_cast<std::vector<std::string>*>(details); }
EXPORT void EOS_Lobby_Attribute_Release(Attribute* attribute) {
    delete attribute->Data;
    delete attribute;
}
