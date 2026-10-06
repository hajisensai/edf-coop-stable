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
constexpr std::int32_t kUtf8 = 3;
constexpr std::size_t kMaxTextLength = 1000;  // EOS_LOBBYMODIFICATION_MAX_ATTRIBUTE_LENGTH
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
struct GetLobbyOwnerOptions {
    std::int32_t ApiVersion;
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
using GetLobbyOwnerFn = const void* (*)(void* details, const GetLobbyOwnerOptions*);
using ReleaseDetailsFn = void (*)(void* details);
using ReleaseAttributeFn = void (*)(Attribute* attribute);
using IsCompleteFn = std::int32_t (*)(EosResult);
struct KickMemberOptions {  // EOS_Lobby_KickMemberOptions
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LocalUserId;
    const void* TargetUserId;
};
using KickMemberFn = void (*)(void* lobby, const KickMemberOptions*, void* clientData, LobbyIdCallback);
using IdFromStringFn = const void* (*)(const char*);

struct Api {
    UpdateModificationFn updateModification = nullptr;
    AddMemberAttributeFn addMemberAttribute = nullptr;
    ReleaseModificationFn releaseModification = nullptr;
    UpdateLobbyFn updateLobby = nullptr;
    CopyDetailsFn copyDetails = nullptr;
    GetMemberCountFn memberCount = nullptr;
    GetMemberByIndexFn memberByIndex = nullptr;
    CopyMemberAttributeFn copyMemberAttribute = nullptr;
    GetLobbyOwnerFn lobbyOwner = nullptr;
    ReleaseDetailsFn releaseDetails = nullptr;
    ReleaseAttributeFn releaseAttribute = nullptr;
    IsCompleteFn isComplete = nullptr;
    KickMemberFn kickMember = nullptr;      // optional: KickLobbyMember
    IdFromStringFn idFromString = nullptr;  // optional: KickLobbyMember
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
    // Everything we do in the lobby - observing, publishing, publishing again after a failure - happens on one beat,
    // once a second and never on the frame we enter: EDF6DirectNet publishes its own marker on that frame.
    ULONGLONG nextBeat = 0;
    bool failureLogged = false;  // in this lobby
    bool splitReader = false;  // kSplitSyncKey is ours to publish
    std::map<std::string, std::string, std::less<>> texts;  // PublishMemberText
    std::vector<std::string> watched;                       // WatchMemberTexts: every watcher's keys
    // The watched texts each member of this lobby has shown. Our copy of the lobby drops a member's attributes when
    // that member's game updates the lobby (EOS logs "Lobby backend has attributes missing from client"): a text
    // once seen stays until its member publishes another value or leaves, like the split marker.
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> seen;
    // Every watcher and tick listener (host data, the netcode version gate): each registration adds one.
    std::vector<LobbyObserver> observers;
    std::vector<std::function<void(void*)>> tickListeners;
} local;

// A pending CreateLobby / JoinLobby of the game; EOS runs its completion until the result is final.
struct LobbyCall {
    LobbyIdCallback callback;
    void* clientData;
    void* lobby;
    const void* user;
};

// The marker did not get in: it is still owed, and goes out again on the next beat. Caller holds local.lock.
void PublishFailedLocked(EosResult result) {
    local.dirty = true;
    if (local.failureLogged) return;
    local.failureLogged = true;
    Log("MISSION sync: publishing our split marker failed (EOS result %d); trying again every second while we are "
        "in this room", result);
}

void Published(const LobbyIdCallbackInfo* info) {
    if (info->ResultCode == kEosSuccess) {
        AcquireSRWLockShared(&local.lock);
        const bool split = local.splitReader;
        ReleaseSRWLockShared(&local.lock);
        if (split)
            Log("MISSION sync: published that this machine reads a split start message (lobby member attribute %s)",
                kSplitSyncKey);
        return;
    }
    if (api.isComplete && !api.isComplete(info->ResultCode)) return;  // EOS retries by itself
    AcquireSRWLockExclusive(&local.lock);
    if (info->LobbyId && local.lobbyId == info->LobbyId) PublishFailedLocked(info->ResultCode);
    ReleaseSRWLockExclusive(&local.lock);
}

// Caller holds local.lock.
void PublishLocked() {
    UpdateModificationOptions options{1, local.user, local.lobbyId.c_str()};
    void* modification = nullptr;
    EosResult result = api.updateModification(local.lobby, &options, &modification);
    if (result != kEosSuccess || !modification) {
        PublishFailedLocked(result != kEosSuccess ? result : -1);
        return;
    }
    std::vector<AttributeData> attributes;
    if (local.splitReader) {
        AttributeData& marker = attributes.emplace_back();
        marker.ApiVersion = 1;
        marker.Key = kSplitSyncKey;
        marker.Value.AsInt64 = kSplitSyncFormat;
        marker.ValueType = kInt64;
    }
    for (const auto& [key, value] : local.texts) {
        AttributeData& text = attributes.emplace_back();
        text.ApiVersion = 1;
        text.Key = key.c_str();
        text.Value.AsUtf8 = value.c_str();
        text.ValueType = kUtf8;
    }
    AttributeData& seq = attributes.emplace_back();
    seq.ApiVersion = 1;
    seq.Key = kSplitSyncSeqKey;
    seq.Value.AsInt64 = ++local.seq;
    seq.ValueType = kInt64;
    for (const AttributeData& attribute : attributes) {
        const AddMemberAttributeOptions add{1, &attribute, 0};
        result = api.addMemberAttribute(modification, &add);
        if (result != kEosSuccess) break;
    }
    if (result == kEosSuccess) {
        const UpdateLobbyOptions update{1, modification};
        api.updateLobby(local.lobby, &update, nullptr, &Published);  // EOS copies the modification
        local.dirty = false;
    } else {
        PublishFailedLocked(result);
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

// A text attribute of `member`, or nothing.
void ReadText(void* details, const void* member, const std::string& key,
              std::map<std::string, std::string, std::less<>>& texts) {
    const CopyMemberAttributeOptions options{1, member, key.c_str()};
    Attribute* attribute = nullptr;
    if (api.copyMemberAttribute(details, &options, &attribute) == kEosSuccess && attribute && attribute->Data &&
        attribute->Data->ValueType == kUtf8 && attribute->Data->Value.AsUtf8)
        texts[key] = std::string(attribute->Data->Value.AsUtf8, strnlen(attribute->Data->Value.AsUtf8, kMaxTextLength));
    if (attribute) api.releaseAttribute(attribute);
}

std::string UserText(const void* user) {
    char id[40]{};
    return user ? std::string(ProductUserIdText(user, id, sizeof(id))) : std::string();
}

// The members our copy of the lobby lists, and the view of it the observer gets. False when there is no copy
// (then nothing is learnt). Caller holds local.lock.
bool ReadMembersLocked(std::vector<LobbyMember>& members, LobbyView& view) {
    const CopyDetailsOptions options{1, local.lobbyId.c_str(), local.user};
    void* details = nullptr;
    if (api.copyDetails(local.lobby, &options, &details) != kEosSuccess || !details) return false;
    const GetLobbyOwnerOptions ownerOptions{1};
    view.lobbyId = local.lobbyId;
    view.self = UserText(local.user);
    view.owner = UserText(api.lobbyOwner(details, &ownerOptions));
    const GetMemberCountOptions countOptions{1};
    const std::uint32_t count = std::min(api.memberCount(details, &countOptions), kMaxMembersRead);
    decltype(local.seen) present;  // only members still in the lobby are remembered
    for (std::uint32_t i = 0; i < count; ++i) {
        const GetMemberByIndexOptions memberOptions{1, i};
        const void* member = api.memberByIndex(details, &memberOptions);
        const std::string id = UserText(member);
        if (id.empty()) continue;
        members.push_back({id, ReadMarker(details, member)});
        LobbyView::Member& seen = view.members.emplace_back();
        seen.id = id;
        for (const std::string& key : local.watched) ReadText(details, member, key, seen.texts);
        auto& known = local.seen[id];
        for (const auto& [key, value] : seen.texts) known[key] = value;
        seen.texts = known;
        present[id] = std::move(known);
    }
    local.seen = std::move(present);
    api.releaseDetails(details);
    return true;
}

// One beat: read who is in the lobby, then publish our attributes if they are owed. True when `view` was read.
// Caller holds local.lock.
bool BeatLocked(LobbyView& view) {
    std::vector<LobbyMember> members;
    const bool read = ReadMembersLocked(members, view);
    if (read && SplitSync().Observe(members)) local.dirty = true;  // a newcomer needs them again
    if (local.dirty) PublishLocked();
    return read;
}

void TickHook(void* platform) {
    api.tick(platform);
    AcquireSRWLockExclusive(&local.lock);
    const ULONGLONG now = GetTickCount64();
    LobbyView view;
    bool beat = false;
    if (!local.lobbyId.empty() && now >= local.nextBeat) {
        local.nextBeat = now + kObserveIntervalMs;
        beat = BeatLocked(view);
    }
    const std::vector<LobbyObserver> observers = beat ? local.observers : std::vector<LobbyObserver>();
    const std::vector<std::function<void(void*)>> listeners = local.tickListeners;
    ReleaseSRWLockExclusive(&local.lock);
    for (const LobbyObserver& observer : observers) observer(view);
    for (const auto& listener : listeners) listener(platform);
}

void Entered(const LobbyCall& call, const char* lobbyId) {
    AcquireSRWLockExclusive(&local.lock);
    local.lobby = call.lobby;
    local.user = call.user;
    local.lobbyId = lobbyId;
    local.seen.clear();
    local.dirty = true;
    local.nextBeat = GetTickCount64() + kObserveIntervalMs;
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
    local.seen.clear();
    local.dirty = false;
    const std::vector<LobbyObserver> observers = local.observers;
    ReleaseSRWLockExclusive(&local.lock);
    SplitSync().Left();
    ClearRecords();
    for (const LobbyObserver& observer : observers) observer(LobbyView{});
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
    std::vector<std::string> late;  // refused a split message before their marker showed up
    for (const LobbyMember& member : lobby_.empty() ? std::vector<LobbyMember>() : members) {
        if (!Contains(members_, member.id)) {
            members_.push_back(member.id);
            newcomer = true;
        }
        if (!member.marked || Contains(marked_, member.id)) continue;
        marked_.push_back(member.id);
        if (Contains(refused_, member.id)) late.push_back(member.id);
    }
    ReleaseSRWLockExclusive(&lock_);
    for (const std::string& id : late)
        Log("MISSION sync: EOS %s now shows that it reads a split start message", id.c_str());
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
        Log("MISSION sync: no marker seen yet from EOS %s that it reads a split start message; its loadout records go "
            "to it all the same. If it joined in the last few seconds its marker may still be on its way (a line "
            "follows when it shows up); otherwise it runs no MultiSlot 1.5.15 or later, cannot read this start "
            "message and starts the mission without the other players' loadouts",
            id);
    return marked;
}

void WatchMemberTexts(std::vector<std::string> keys, LobbyObserver observer) {
    AcquireSRWLockExclusive(&local.lock);
    for (std::string& key : keys)
        if (std::find(local.watched.begin(), local.watched.end(), key) == local.watched.end())
            local.watched.push_back(std::move(key));
    local.observers.push_back(std::move(observer));
    ReleaseSRWLockExclusive(&local.lock);
}

void PublishMemberText(const std::string& key, const std::string& value) {
    if (value.empty()) {
        Log("MISSION sync: not publishing an empty %s (EOS would refuse our whole lobby member update)", key.c_str());
        return;
    }
    const std::string text = value.substr(0, kMaxTextLength);
    AcquireSRWLockExclusive(&local.lock);
    std::string& published = local.texts[key];
    if (published != text) {
        published = text;
        local.dirty = local.dirty || !local.lobbyId.empty();
    }
    ReleaseSRWLockExclusive(&local.lock);
}

void ListenToTicks(std::function<void(void* platform)> listener) {
    AcquireSRWLockExclusive(&local.lock);
    local.tickListeners.push_back(std::move(listener));
    ReleaseSRWLockExclusive(&local.lock);
}

void Kicked(const LobbyIdCallbackInfo* info) {
    if (api.isComplete && !api.isComplete(info->ResultCode)) return;
    if (info->ResultCode != kEosSuccess) Log("NETCODE removing a member from the room failed (EOS result %d)", info->ResultCode);
}

bool KickLobbyMember(const std::string& id) {
    AcquireSRWLockShared(&local.lock);
    void* const lobby = local.lobby;
    const void* const user = local.user;
    const std::string lobbyId = local.lobbyId;
    ReleaseSRWLockShared(&local.lock);
    const void* target = api.idFromString && !id.empty() ? api.idFromString(id.c_str()) : nullptr;
    if (!api.kickMember || !lobby || !user || lobbyId.empty() || !target) return false;
    const KickMemberOptions options{1, lobbyId.c_str(), user, target};
    api.kickMember(lobby, &options, nullptr, &Kicked);
    return true;
}

bool InstallSyncMarker(HMODULE game, ImportRedirect redirect, bool splitReader) {
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
    ok &= Resolve(eos, "EOS_LobbyDetails_GetLobbyOwner", api.lobbyOwner);
    ok &= Resolve(eos, "EOS_LobbyDetails_Release", api.releaseDetails);
    ok &= Resolve(eos, "EOS_Lobby_Attribute_Release", api.releaseAttribute);
    ok &= Resolve(eos, "EOS_EResult_IsOperationComplete", api.isComplete);
    if (!ok) return false;
    // Only KickLobbyMember needs these; without them it reports false.
    api.kickMember = reinterpret_cast<KickMemberFn>(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_Lobby_KickMember")));
    api.idFromString =
        reinterpret_cast<IdFromStringFn>(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_ProductUserId_FromString")));
    AcquireSRWLockExclusive(&local.lock);
    local.splitReader = splitReader;
    ReleaseSRWLockExclusive(&local.lock);
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
