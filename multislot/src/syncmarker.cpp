#include "syncmarker.h"

#include <algorithm>
#include <cstring>

#include "identity.h"
#include "log.h"

namespace multislot {
namespace {

bool Contains(const std::vector<std::string>& list, const std::string& id) {
    return std::find(list.begin(), list.end(), id) != list.end();
}

// --- EOS lobby API (eos_lobby.h, eos_lobby_types.h; the layouts EDF6DirectNet's eos_min.h uses) ---
using EosResult = std::int32_t;
constexpr EosResult kEosSuccess = 0;

struct CreateLobbyOptionsHead {  // leading fields of EOS_Lobby_CreateLobbyOptions
    std::int32_t ApiVersion;
    const void* LocalUserId;
};
struct JoinLobbyOptionsHead {  // leading fields of EOS_Lobby_JoinLobbyOptions
    std::int32_t ApiVersion;
    void* LobbyDetailsHandle;
    const void* LocalUserId;
};
struct LobbyIdCallbackInfo {  // Create/Join/UpdateLobby completions share this layout
    EosResult ResultCode;
    void* ClientData;
    const char* LobbyId;
};
using LobbyIdCallback = void (*)(const LobbyIdCallbackInfo*);
using CreateLobbyFn = void (*)(void* lobby, const CreateLobbyOptionsHead*, void* clientData, LobbyIdCallback);
using JoinLobbyFn = void (*)(void* lobby, const JoinLobbyOptionsHead*, void* clientData, LobbyIdCallback);
using LeaveOrDestroyFn = void (*)(void* lobby, const void* options, void* clientData, void* callback);
using PlatformTickFn = void (*)(void* platform);

struct UpdateModificationOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct UpdateLobbyOptions {
    std::int32_t ApiVersion;
    void* LobbyModificationHandle;
};
struct CopyDetailsOptions {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LocalUserId;
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
    std::int32_t ValueType;  // 0 bool, 1 int64, 2 double, 3 string
};
constexpr std::int32_t kInt64 = 1;
struct Attribute {
    std::int32_t ApiVersion;
    AttributeData* Data;
    std::int32_t Visibility;  // 0 public
};
struct AddMemberAttributeOptions {
    std::int32_t ApiVersion;
    const AttributeData* Attribute;
    std::int32_t Visibility;
};
struct GetMemberCountOptions {
    std::int32_t ApiVersion;
};
struct GetMemberByIndexOptions {
    std::int32_t ApiVersion;
    std::uint32_t MemberIndex;
};
struct CopyMemberAttributeOptions {
    std::int32_t ApiVersion;
    const void* TargetUserId;
    const char* AttrKey;
};
static_assert(sizeof(AttributeData) == 32 && offsetof(AttributeData, ValueType) == 24, "EOS_Lobby_AttributeData");
static_assert(offsetof(LobbyIdCallbackInfo, LobbyId) == 16, "EOS_Lobby_LobbyIdCallbackInfo");

using UpdateModificationFn = EosResult (*)(void* lobby, const UpdateModificationOptions*, void** modification);
using AddMemberAttributeFn = EosResult (*)(void* modification, const AddMemberAttributeOptions*);
using ReleaseModificationFn = void (*)(void* modification);
using UpdateLobbyFn = void (*)(void* lobby, const UpdateLobbyOptions*, void* clientData, LobbyIdCallback);
using CopyDetailsFn = EosResult (*)(void* lobby, const CopyDetailsOptions*, void** details);
using GetMemberCountFn = std::uint32_t (*)(void* details, const GetMemberCountOptions*);
using GetMemberByIndexFn = const void* (*)(void* details, const GetMemberByIndexOptions*);
using CopyMemberAttributeFn = EosResult (*)(void* details, const CopyMemberAttributeOptions*, Attribute** out);
using ReleaseDetailsFn = void (*)(void* details);
using ReleaseAttributeFn = void (*)(Attribute* attribute);
using IsCompleteFn = std::int32_t (*)(EosResult);

struct Api {
    UpdateModificationFn updateModification = nullptr;
    AddMemberAttributeFn addMemberAttribute = nullptr;
    ReleaseModificationFn releaseModification = nullptr;
    UpdateLobbyFn updateLobby = nullptr;
    CopyDetailsFn copyDetails = nullptr;
    GetMemberCountFn memberCount = nullptr;
    GetMemberByIndexFn memberByIndex = nullptr;
    CopyMemberAttributeFn copyMemberAttribute = nullptr;
    ReleaseDetailsFn releaseDetails = nullptr;
    ReleaseAttributeFn releaseAttribute = nullptr;
    IsCompleteFn isComplete = nullptr;
    // What the game's imports pointed at before us (EOS, or another plugin's hook in front of it).
    CreateLobbyFn createLobby = nullptr;
    JoinLobbyFn joinLobby = nullptr;
    LeaveOrDestroyFn leaveLobby = nullptr;
    LeaveOrDestroyFn destroyLobby = nullptr;
    PlatformTickFn tick = nullptr;
} api;

constexpr ULONGLONG kObserveIntervalMs = 1000;
constexpr std::uint32_t kMaxMembersRead = 64;  // an EOS lobby holds at most 64

// Our own lobby membership, as the EOS calls on the tick need it. Only the tick and the lobby completions (which
// EOS runs inside the tick) touch it, but the game may call LeaveLobby from elsewhere: guarded.
struct Local {
    SRWLOCK lock = SRWLOCK_INIT;
    void* lobby = nullptr;  // EOS_HLobby
    const void* user = nullptr;  // EOS_ProductUserId
    std::string lobbyId;
    bool dirty = false;  // our marker is not (known to be) published in this lobby
    std::int64_t seq = 0;
    ULONGLONG nextObserve = 0;
    ULONGLONG nextPublish = 0;  // after a failed publish: not before the next observation
    bool failureLogged = false;
} local;

// A pending CreateLobby / JoinLobby of the game; EOS runs its completion until the result is final.
struct LobbyCall {
    LobbyIdCallback callback;
    void* clientData;
    void* lobby;
    const void* user;
};

void Published(const LobbyIdCallbackInfo* info) {
    if (info->ResultCode == kEosSuccess) {
        Log("MISSION sync: published that this machine reads a split start message (lobby member attribute %s)",
            kSplitSyncKey);
        return;
    }
    if (api.isComplete && !api.isComplete(info->ResultCode)) return;  // EOS retries by itself
    // Not published: the marker is still owed, published again with the next observation (about a second away).
    AcquireSRWLockExclusive(&local.lock);
    const bool ours = info->LobbyId && local.lobbyId == info->LobbyId;
    const bool log = ours && !local.failureLogged;
    if (ours) {
        local.dirty = true;
        local.nextPublish = GetTickCount64() + kObserveIntervalMs;
        local.failureLogged = true;
    }
    ReleaseSRWLockExclusive(&local.lock);
    if (log)
        Log("MISSION sync: publishing our split marker failed (EOS result %d); published again every second until "
            "it is in", info->ResultCode);
}

// Caller holds local.lock.
void PublishLocked() {
    UpdateModificationOptions options{1, local.user, local.lobbyId.c_str()};
    void* modification = nullptr;
    EosResult result = api.updateModification(local.lobby, &options, &modification);
    if (result != kEosSuccess || !modification) {
        Log("MISSION sync: cannot edit our lobby member (EOS result %d); hosts will not send us a split start message",
            result);
        return;
    }
    AttributeData marker{};
    marker.ApiVersion = 1;
    marker.Key = kSplitSyncKey;
    marker.Value.AsInt64 = kSplitSyncFormat;
    marker.ValueType = kInt64;
    AttributeData seq{};
    seq.ApiVersion = 1;
    seq.Key = kSplitSyncSeqKey;
    seq.Value.AsInt64 = ++local.seq;
    seq.ValueType = kInt64;
    const AddMemberAttributeOptions addMarker{1, &marker, 0};
    const AddMemberAttributeOptions addSeq{1, &seq, 0};
    result = api.addMemberAttribute(modification, &addMarker);
    if (result == kEosSuccess) result = api.addMemberAttribute(modification, &addSeq);
    if (result == kEosSuccess) {
        const UpdateLobbyOptions update{1, modification};
        api.updateLobby(local.lobby, &update, nullptr, &Published);  // EOS copies the modification
        local.dirty = false;
    } else {
        Log("MISSION sync: cannot set our split marker (EOS result %d)", result);
    }
    api.releaseModification(modification);
}

bool ReadMarker(void* details, const void* member) {
    const CopyMemberAttributeOptions options{1, member, kSplitSyncKey};
    Attribute* attribute = nullptr;
    const bool marked = api.copyMemberAttribute(details, &options, &attribute) == kEosSuccess && attribute &&
                        attribute->Data && attribute->Data->ValueType == kInt64 &&
                        attribute->Data->Value.AsInt64 >= kSplitSyncFormat;
    if (attribute) api.releaseAttribute(attribute);
    return marked;
}

// The members our copy of the lobby lists. False when there is no copy (then nothing is learnt).
// Caller holds local.lock.
bool ReadMembersLocked(std::vector<LobbyMember>& members) {
    const CopyDetailsOptions options{1, local.lobbyId.c_str(), local.user};
    void* details = nullptr;
    if (api.copyDetails(local.lobby, &options, &details) != kEosSuccess || !details) return false;
    const GetMemberCountOptions countOptions{1};
    const std::uint32_t count = std::min(api.memberCount(details, &countOptions), kMaxMembersRead);
    for (std::uint32_t i = 0; i < count; ++i) {
        const GetMemberByIndexOptions memberOptions{1, i};
        const void* member = api.memberByIndex(details, &memberOptions);
        char id[40]{};
        if (member && *ProductUserIdText(member, id, sizeof(id))) members.push_back({id, ReadMarker(details, member)});
    }
    api.releaseDetails(details);
    return true;
}

void ObserveLocked(ULONGLONG now) {
    if (now < local.nextObserve) return;
    local.nextObserve = now + kObserveIntervalMs;
    std::vector<LobbyMember> members;
    if (!ReadMembersLocked(members)) return;
    if (SplitSync().Observe(members)) local.dirty = true;  // a newcomer gets our marker sent again
}

void TickHook(void* platform) {
    api.tick(platform);
    AcquireSRWLockExclusive(&local.lock);
    const ULONGLONG now = GetTickCount64();
    if (!local.lobbyId.empty()) {
        ObserveLocked(now);
        if (local.dirty && now >= local.nextPublish) PublishLocked();
    }
    ReleaseSRWLockExclusive(&local.lock);
}

void Entered(const LobbyCall& call, const char* lobbyId) {
    AcquireSRWLockExclusive(&local.lock);
    local.lobby = call.lobby;
    local.user = call.user;
    local.lobbyId = lobbyId;
    local.dirty = true;
    local.nextObserve = 0;
    local.nextPublish = 0;
    local.failureLogged = false;
    ReleaseSRWLockExclusive(&local.lock);
    SplitSync().Entered(lobbyId);
}

void LobbyEntered(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<LobbyCall*>(info->ClientData);
    if (info->ResultCode == kEosSuccess && info->LobbyId && call->lobby && call->user) Entered(*call, info->LobbyId);
    LobbyIdCallbackInfo copy = *info;
    copy.ClientData = call->clientData;
    const LobbyIdCallback callback = call->callback;
    // EOS runs the completion again after a result that is not final: keep the call until the last run.
    if (!api.isComplete || api.isComplete(info->ResultCode)) delete call;
    callback(&copy);
}

void CreateLobbyHook(void* lobby, const CreateLobbyOptionsHead* options, void* clientData, LobbyIdCallback callback) {
    if (!callback || !options) return api.createLobby(lobby, options, clientData, callback);
    api.createLobby(lobby, options, new LobbyCall{callback, clientData, lobby, options->LocalUserId}, &LobbyEntered);
}

void JoinLobbyHook(void* lobby, const JoinLobbyOptionsHead* options, void* clientData, LobbyIdCallback callback) {
    if (!callback || !options) return api.joinLobby(lobby, options, clientData, callback);
    api.joinLobby(lobby, options, new LobbyCall{callback, clientData, lobby, options->LocalUserId}, &LobbyEntered);
}

void LeftLobby() {
    AcquireSRWLockExclusive(&local.lock);
    local.lobbyId.clear();
    local.dirty = false;
    ReleaseSRWLockExclusive(&local.lock);
    SplitSync().Left();
    ClearRecords();
}

void LeaveLobbyHook(void* lobby, const void* options, void* clientData, void* callback) {
    LeftLobby();
    api.leaveLobby(lobby, options, clientData, callback);
}

void DestroyLobbyHook(void* lobby, const void* options, void* clientData, void* callback) {
    LeftLobby();
    api.destroyLobby(lobby, options, clientData, callback);
}

template <typename T>
bool Resolve(HMODULE eos, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(eos, name)));
    if (!out) Log("MISSION sync: EOS export %s not found", name);
    return out != nullptr;
}

template <typename T>
bool Redirect(HMODULE game, ImportRedirect redirect, const char* name, T replacement, T& original) {
    const bool ok = redirect(game, "EOSSDK-Win64-Shipping.dll", name, reinterpret_cast<void*>(replacement),
                             reinterpret_cast<void**>(&original));
    if (!ok) Log("MISSION sync: EDF.dll import %s could not be redirected", name);
    return ok;
}

}  // namespace

void SplitSyncRoom::Entered(const std::string& lobbyId) {
    AcquireSRWLockExclusive(&lock_);
    lobby_ = lobbyId;
    members_.clear();
    marked_.clear();
    refused_.clear();
    ReleaseSRWLockExclusive(&lock_);
}

void SplitSyncRoom::Left() { Entered(std::string()); }

bool SplitSyncRoom::Observe(const std::vector<LobbyMember>& members) {
    AcquireSRWLockExclusive(&lock_);
    bool newcomer = false;
    for (const LobbyMember& member : lobby_.empty() ? std::vector<LobbyMember>() : members) {
        if (!Contains(members_, member.id)) {
            members_.push_back(member.id);
            newcomer = true;
        }
        if (member.marked && !Contains(marked_, member.id)) marked_.push_back(member.id);
    }
    ReleaseSRWLockExclusive(&lock_);
    return newcomer;
}

bool SplitSyncRoom::InLobby() const {
    AcquireSRWLockShared(&lock_);
    const bool in = !lobby_.empty();
    ReleaseSRWLockShared(&lock_);
    return in;
}

std::string SplitSyncRoom::LobbyId() const {
    AcquireSRWLockShared(&lock_);
    std::string id = lobby_;
    ReleaseSRWLockShared(&lock_);
    return id;
}

bool SplitSyncRoom::Marked(const std::string& id) const {
    AcquireSRWLockShared(&lock_);
    const bool marked = Contains(marked_, id);
    ReleaseSRWLockShared(&lock_);
    return marked;
}

std::vector<std::string> SplitSyncRoom::Unmarked() const {
    AcquireSRWLockShared(&lock_);
    std::vector<std::string> unmarked;
    for (const std::string& id : members_)
        if (!Contains(marked_, id)) unmarked.push_back(id);
    ReleaseSRWLockShared(&lock_);
    return unmarked;
}

SplitSyncRoom& SplitSync() {
    static SplitSyncRoom room;
    return room;
}

bool PeerReadsSplitSync(const void* remote) {
    char id[40]{};
    if (!remote || !*ProductUserIdText(remote, id, sizeof(id))) return false;
    SplitSyncRoom& room = SplitSync();
    AcquireSRWLockExclusive(&room.lock_);
    const bool marked = Contains(room.marked_, id);
    const bool first = !marked && !Contains(room.refused_, id);
    if (first) room.refused_.push_back(id);
    ReleaseSRWLockExclusive(&room.lock_);
    if (first)
        Log("MISSION sync: EOS %s has not published that it reads a split start message (it runs no MultiSlot 1.5.15 "
            "or later); the start message is not sent to it, as before the split, so it cannot start this mission",
            id);
    return marked;
}

bool InstallSyncMarker(HMODULE game, ImportRedirect redirect) {
    const HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (!eos) {
        Log("MISSION sync: EOSSDK-Win64-Shipping.dll is not loaded");
        return false;
    }
    bool ok = Resolve(eos, "EOS_Lobby_UpdateLobbyModification", api.updateModification);
    ok &= Resolve(eos, "EOS_LobbyModification_AddMemberAttribute", api.addMemberAttribute);
    ok &= Resolve(eos, "EOS_LobbyModification_Release", api.releaseModification);
    ok &= Resolve(eos, "EOS_Lobby_UpdateLobby", api.updateLobby);
    ok &= Resolve(eos, "EOS_Lobby_CopyLobbyDetailsHandle", api.copyDetails);
    ok &= Resolve(eos, "EOS_LobbyDetails_GetMemberCount", api.memberCount);
    ok &= Resolve(eos, "EOS_LobbyDetails_GetMemberByIndex", api.memberByIndex);
    ok &= Resolve(eos, "EOS_LobbyDetails_CopyMemberAttributeByKey", api.copyMemberAttribute);
    ok &= Resolve(eos, "EOS_LobbyDetails_Release", api.releaseDetails);
    ok &= Resolve(eos, "EOS_Lobby_Attribute_Release", api.releaseAttribute);
    ok &= Resolve(eos, "EOS_EResult_IsOperationComplete", api.isComplete);
    if (!ok) return false;
    // Leaving first, entering last: a lobby entered can always be left again. The tick goes in before the lobby
    // completions, which EOS runs inside it.
    ok = Redirect(game, redirect, "EOS_Lobby_LeaveLobby", &LeaveLobbyHook, api.leaveLobby);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_DestroyLobby", &DestroyLobbyHook, api.destroyLobby);
    ok = ok && Redirect(game, redirect, "EOS_Platform_Tick", &TickHook, api.tick);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_CreateLobby", &CreateLobbyHook, api.createLobby);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_JoinLobby", &JoinLobbyHook, api.joinLobby);
    return ok;
}

}  // namespace multislot
