#include "lobbystate.h"

#include <atomic>
#include <cstring>
#include <string>

#include "crashlog.h"
#include "identity.h"
#include "log.h"
#include "patches.h"

namespace multislot {
namespace {

// --- EOS lobby API (eos_lobby.h, eos_lobby_types.h) ---
using EosResult = std::int32_t;
constexpr EosResult kEosSuccess = 0;
constexpr EosResult kEosLobbyAlreadyExists = 9002;  // EOS_Lobby_LobbyAlreadyExists: still a member of it

struct CreateLobbyOptionsHead {  // leading fields of EOS_Lobby_CreateLobbyOptions
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxLobbyMembers;
};
// EOS_Lobby_JoinLobbyOptions as EDF.dll fills it (7430BE: ApiVersion 4, no RTC options). Only this version is
// copied for a join that has to wait (JoinLobbyHook); any other is passed on as it is.
struct JoinLobbyOptions {
    std::int32_t ApiVersion;
    void* LobbyDetailsHandle;
    const void* LocalUserId;
    std::int32_t bPresenceEnabled;
    const void* LocalRTCOptions;
    std::int32_t bCrossplayOptOut;
};
constexpr std::int32_t kJoinOptionsVersion = 4;
static_assert(sizeof(JoinLobbyOptions) == 0x30, "EOS_Lobby_JoinLobbyOptions version 4");
// EOS_Lobby_LeaveLobbyOptions and EOS_Lobby_DestroyLobbyOptions: the same fields, both at version 1.
struct LeaveOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct LobbyIdCallbackInfo {  // every lobby completion used here shares this layout
    EosResult ResultCode;
    void* ClientData;
    const char* LobbyId;
};
using LobbyIdCallback = void (*)(const LobbyIdCallbackInfo*);
using LobbyCallFn = void (*)(void* lobby, const void* options, void* clientData, LobbyIdCallback);
using PlatformTickFn = void (*)(void* platform);

struct CopyDetailsOptions {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LocalUserId;
};
struct VersionOnly {
    std::int32_t ApiVersion;
};
struct CopyAttributeOptions {
    std::int32_t ApiVersion;
    const char* AttrKey;
};
// EOS_LobbyDetails_Info, leading fields only (rooms.cpp).
struct LobbyDetailsInfo {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LobbyOwnerUserId;
    std::int32_t PermissionLevel;
    std::uint32_t AvailableSlots;
    std::uint32_t MaxMembers;
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
    std::int32_t Visibility;
};
static_assert(offsetof(LobbyIdCallbackInfo, LobbyId) == 16, "EOS_Lobby_*CallbackInfo");
static_assert(offsetof(LobbyDetailsInfo, MaxMembers) == 0x20, "EOS_LobbyDetails_Info");

using CopyDetailsFn = EosResult (*)(void* lobby, const CopyDetailsOptions*, void** details);
using CopyInfoFn = EosResult (*)(void* details, const VersionOnly*, LobbyDetailsInfo** info);
using ReleaseInfoFn = void (*)(LobbyDetailsInfo* info);
using GetOwnerFn = const void* (*)(void* details, const VersionOnly*);
using CopyAttributeFn = EosResult (*)(void* details, const CopyAttributeOptions*, Attribute** out);
using ReleaseAttributeFn = void (*)(Attribute* attribute);
using ReleaseDetailsFn = void (*)(void* details);
using IsCompleteFn = std::int32_t (*)(EosResult);
using ToStringFn = const char* (*)(EosResult);

struct Api {
    bool ready = false;  // every export below resolved: the lobby can be read
    CopyDetailsFn copyDetails = nullptr;
    CopyInfoFn copyInfo = nullptr;
    ReleaseInfoFn releaseInfo = nullptr;
    GetOwnerFn owner = nullptr;
    CopyAttributeFn copyAttribute = nullptr;
    ReleaseAttributeFn releaseAttribute = nullptr;
    ReleaseDetailsFn releaseDetails = nullptr;
    IsCompleteFn isComplete = nullptr;
    ToStringFn toString = nullptr;
    // What the game's imports pointed at before us: EOS itself, as this module is installed first.
    LobbyCallFn leaveLobby = nullptr;
    LobbyCallFn destroyLobby = nullptr;
    LobbyCallFn updateLobby = nullptr;
    LobbyCallFn createLobby = nullptr;
    LobbyCallFn joinLobby = nullptr;
    PlatformTickFn tick = nullptr;
} api;

constexpr const char* kSearchTypeKey = "SEARCH_TYPE";
constexpr ULONGLONG kBeatMs = 1000;
constexpr std::size_t kIdChars = 40;

bool Final(EosResult result) { return !api.isComplete || api.isComplete(result); }
const char* ResultName(EosResult result) {
    const char* name = api.toString ? api.toString(result) : nullptr;
    return name ? name : "?";
}

std::string UserText(const void* user) {
    char id[kIdChars]{};
    ProductUserIdText(user, id, sizeof(id));
    return id;
}

// Lobby ids come from EOS or the game: bounded and printable in the log.
std::string IdText(const char* id) {
    std::string text;
    Probing([&] {
        __try {
            for (std::size_t i = 0; id && id[i] && i < 64; ++i) text.push_back(id[i] > 0x20 && id[i] < 0x7F ? id[i] : '?');
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            text = "(unreadable)";
        }
    });
    return text.empty() ? std::string("?") : text;
}

// Our copy of `lobbyId` for `user`; EOS has one only while that user is a member of it.
void* CopyDetails(void* lobby, const void* user, const char* lobbyId) {
    if (!api.ready || !lobby || !user || !lobbyId || !*lobbyId) return nullptr;
    const CopyDetailsOptions options{1, lobbyId, user};
    void* details = nullptr;
    return api.copyDetails(lobby, &options, &details) == kEosSuccess ? details : nullptr;
}

// Owner and kind from a lobby details handle.
void ReadDetails(void* details, LobbyFacts& facts, std::string* owner) {
    const VersionOnly version{1};
    LobbyDetailsInfo* info = nullptr;
    if (api.copyInfo(details, &version, &info) == kEosSuccess && info) {
        facts.maxMembers = info->MaxMembers;
        api.releaseInfo(info);
    }
    const CopyAttributeOptions key{1, kSearchTypeKey};
    Attribute* attribute = nullptr;
    if (api.copyAttribute(details, &key, &attribute) == kEosSuccess && attribute && attribute->Data &&
        attribute->Data->ValueType == kInt64) {
        facts.hasSearchType = true;
        facts.searchType = attribute->Data->Value.AsInt64;
    }
    if (attribute) api.releaseAttribute(attribute);
    if (owner) *owner = UserText(api.owner(details, &version));
}

// False when EOS has no copy of that lobby for that user.
bool ReadLobby(void* lobby, const void* user, const char* lobbyId, LobbyFacts& facts, std::string* owner) {
    void* details = CopyDetails(lobby, user, lobbyId);
    if (!details) return false;
    ReadDetails(details, facts, owner);
    api.releaseDetails(details);
    return true;
}

std::string LobbyIdOf(void* details) {
    if (!api.ready || !details) return std::string();
    const VersionOnly version{1};
    LobbyDetailsInfo* info = nullptr;
    std::string id;
    if (api.copyInfo(details, &version, &info) == kEosSuccess && info) {
        if (info->LobbyId) id = info->LobbyId;
        api.releaseInfo(info);
    }
    return id;
}

std::string KindText(LobbyKind kind, int capacity) {
    char text[48]{};
    if (kind == LobbyKind::MultiSlot)
        _snprintf_s(text, _TRUNCATE, "a MultiSlot room for %d", capacity);
    else if (kind == LobbyKind::Normal)
        _snprintf_s(text, _TRUNCATE, "a normal %d-player room", kVanillaPlayers);
    else
        _snprintf_s(text, _TRUNCATE, "of an unknown kind");
    return text;
}

std::string FactsText(const LobbyFacts& facts) {
    char text[64]{};
    if (facts.hasSearchType)
        _snprintf_s(text, _TRUNCATE, "MaxMembers %u, SEARCH_TYPE 0x%llX", facts.maxMembers,
                    static_cast<unsigned long long>(facts.searchType));
    else
        _snprintf_s(text, _TRUNCATE, "MaxMembers %u, no SEARCH_TYPE yet", facts.maxMembers);
    return text;
}

// The lobby the game is in. The tick, the lobby calls and the room update all run on the game's thread, but the
// menu reads the kind from its own: guarded, and the two answers it needs are atomics.
struct Current {
    SRWLOCK lock = SRWLOCK_INIT;
    void* lobby = nullptr;
    const void* user = nullptr;
    std::string id;
    std::uint32_t createdCapacity = 0;  // 0: joined
    ULONGLONG nextBeat = 0;
    std::string owner;   // as last read
    bool copyLost = false;
    LobbyKind kind = LobbyKind::Unknown;
    int capacity = 0;
} current;
std::atomic<int> currentKind{static_cast<int>(LobbyKind::Unknown)};
std::atomic<int> currentCapacity{0};  // current.capacity in a MultiSlot room, else 0 (read without the lock)
std::atomic<bool> createdCurrent{false};

// The room update's last decision, logged when it changes.
SRWLOCK decisionLock = SRWLOCK_INIT;
std::string decidedLobby;
LobbyKind decidedKind = LobbyKind::Unknown;
int decidedCapacity = -1;
const char* decidedSource = nullptr;

void NoteDecision(const std::string& lobbyId, const RoomUpdate& update, const char* source, const std::string& detail) {
    AcquireSRWLockExclusive(&decisionLock);
    const bool changed = decidedLobby != lobbyId || decidedKind != update.kind || decidedCapacity != update.capacity ||
                         decidedSource != source;
    if (changed) {
        decidedLobby = lobbyId;
        decidedKind = update.kind;
        decidedCapacity = update.capacity;
        decidedSource = source;
    }
    ReleaseSRWLockExclusive(&decisionLock);
    if (!changed) return;
    if (update.kind == LobbyKind::Unknown)
        Log("ROOM UPDATE of lobby %s: %s, so the game's own values (%d players, vanilla SEARCH_TYPE) are published",
            lobbyId.c_str(), source, kVanillaPlayers);
    else
        Log("ROOM UPDATE of lobby %s keeps it %s (%s: %s)", lobbyId.c_str(), KindText(update.kind, update.capacity).c_str(),
            source, detail.c_str());
}

// MSVC std::string: the characters in place while capacity < 16, else a pointer to them; then size, capacity.
constexpr std::size_t kRoomLobbyOffset = 0x20;    // EOS_HLobby (12BC9D0 builds UpdateLobbyModification from these)
constexpr std::size_t kRoomUserOffset = 0x60;     // EOS_ProductUserId
constexpr std::size_t kRoomLobbyIdOffset = 0x68;  // std::string
struct RoomLobby {
    void* lobby = nullptr;
    const void* user = nullptr;
    char id[72]{};
};

bool ReadRoomLobby(std::uintptr_t room, RoomLobby& out) {
    if (!room) return false;
    return Probing([&]() -> bool {
        __try {
            out.lobby = *reinterpret_cast<void* const*>(room + kRoomLobbyOffset);
            out.user = *reinterpret_cast<const void* const*>(room + kRoomUserOffset);
            const auto size = *reinterpret_cast<const std::size_t*>(room + kRoomLobbyIdOffset + 0x10);
            const auto capacity = *reinterpret_cast<const std::size_t*>(room + kRoomLobbyIdOffset + 0x18);
            if (!size || size >= sizeof(out.id) || size > capacity) return false;
            const char* chars = capacity >= 16 ? *reinterpret_cast<const char* const*>(room + kRoomLobbyIdOffset)
                                               : reinterpret_cast<const char*>(room + kRoomLobbyIdOffset);
            std::memcpy(out.id, chars, size);
            out.id[size] = 0;
            return out.lobby && out.user;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    });
}

// --- the lobby the game is in ---

void Beat() {
    AcquireSRWLockShared(&current.lock);
    void* lobby = current.lobby;
    const void* user = current.user;
    const std::string id = current.id;
    ReleaseSRWLockShared(&current.lock);
    LobbyFacts facts;
    std::string owner;
    const bool read = ReadLobby(lobby, user, id.c_str(), facts, &owner);
    const std::string self = UserText(user);
    const LobbyKind kind = read ? KindOf(facts) : LobbyKind::Unknown;
    const int capacity = kind == LobbyKind::MultiSlot ? CapacityToKeep(facts) : kVanillaPlayers;

    AcquireSRWLockExclusive(&current.lock);
    if (current.id != id) {  // left or entered another lobby meanwhile
        ReleaseSRWLockExclusive(&current.lock);
        return;
    }
    const bool lost = !read && !current.copyLost;
    const bool back = read && current.copyLost;
    current.copyLost = !read;
    const std::string previousOwner = current.owner;
    const bool ownerChanged = read && !owner.empty() && owner != current.owner;
    if (ownerChanged) current.owner = owner;
    const bool kindChanged = read && kind != LobbyKind::Unknown && (kind != current.kind || capacity != current.capacity);
    if (kindChanged) {
        current.kind = kind;
        current.capacity = capacity;
        // Capacity before kind: the menu thread reads the kind first and must not see a MultiSlot room of size 0.
        currentCapacity.store(kind == LobbyKind::MultiSlot ? capacity : 0);
        currentKind.store(static_cast<int>(kind));
    }
    ReleaseSRWLockExclusive(&current.lock);

    if (lost)
        Log("LOBBY %s: EOS has no copy of this lobby for this machine any more (closed, or this machine was removed); "
            "the game is still in its room", id.c_str());
    if (back) Log("LOBBY %s: EOS has a copy of this lobby for this machine again", id.c_str());
    if (ownerChanged && previousOwner.empty())
        Log("LOBBY %s owner: EOS %s%s", id.c_str(), owner.c_str(), owner == self ? " (this machine)" : "");
    else if (ownerChanged)
        Log("LOBBY %s owner changed: EOS %s -> EOS %s%s", id.c_str(), previousOwner.c_str(), owner.c_str(),
            owner == self ? " (this machine is the owner now)" : previousOwner == self ? " (this machine no longer owns it)" : "");
    if (kindChanged) Log("LOBBY %s is %s (%s)", id.c_str(), KindText(kind, capacity).c_str(), FactsText(facts).c_str());
}

void TickHook(void* platform) {
    api.tick(platform);
    AcquireSRWLockExclusive(&current.lock);
    const ULONGLONG now = GetTickCount64();
    const bool due = !current.id.empty() && now >= current.nextBeat;
    if (due) current.nextBeat = now + kBeatMs;
    ReleaseSRWLockExclusive(&current.lock);
    if (due) Beat();
}

// --- completions ---

// A game call whose completion is watched on its way back to the game.
struct GameCall {
    LobbyIdCallback callback;
    void* clientData;
    void* lobby;
    const void* user;
    std::string lobbyId;      // the lobby it is about
    std::uint32_t capacity;   // CreateLobby: MaxLobbyMembers
};

// Hands one run of EOS's completion to the game, with its own ClientData; the call goes after the final run.
void ToGame(const LobbyIdCallbackInfo* info, GameCall* call) {
    LobbyIdCallbackInfo copy = *info;
    copy.ClientData = call->clientData;
    const LobbyIdCallback callback = call->callback;
    if (Final(info->ResultCode)) delete call;
    if (callback) callback(&copy);
}

// A leave of this module's own (no game callback behind it).
struct OwnLeave {
    std::string lobbyId;
    const char* why;
};

void OwnLeft(const LobbyIdCallbackInfo* info) {
    auto* leave = static_cast<OwnLeave*>(info->ClientData);
    if (!Final(info->ResultCode)) return;
    Log("LOBBY LeaveLobby %s (%s): result %d (%s)", leave->lobbyId.c_str(), leave->why, info->ResultCode,
        ResultName(info->ResultCode));
    delete leave;
}

void LeaveOwn(void* lobby, const void* user, const std::string& lobbyId, const char* why) {
    const LeaveOptions options{1, user, lobbyId.c_str()};
    api.leaveLobby(lobby, &options, new OwnLeave{lobbyId, why}, &OwnLeft);
}

void GameLeft(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    if (Final(info->ResultCode))
        Log("LOBBY LeaveLobby %s: result %d (%s)", call->lobbyId.c_str(), info->ResultCode, ResultName(info->ResultCode));
    ToGame(info, call);
}

void DestroyedAsLeave(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    if (Final(info->ResultCode))
        Log("LOBBY LeaveLobby %s in place of the game's DestroyLobby: result %d (%s)", call->lobbyId.c_str(),
            info->ResultCode, ResultName(info->ResultCode));
    ToGame(info, call);
}

void GameDestroyed(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    if (Final(info->ResultCode)) {
        Log("LOBBY DestroyLobby %s: result %d (%s)", call->lobbyId.c_str(), info->ResultCode, ResultName(info->ResultCode));
        // The lobby is still there, and EOS may still list this machine in it: a member nobody can reach, which the
        // next join into this lobby would trip over (9002). Leaving is what the game would have done as a guest.
        if (info->ResultCode != kEosSuccess) {
            Log("LOBBY DestroyLobby %s failed, so this machine may still be a member of it: leaving it", call->lobbyId.c_str());
            LeaveOwn(call->lobby, call->user, call->lobbyId, "after a failed DestroyLobby");
        }
    }
    ToGame(info, call);
}

void GameUpdated(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    if (Final(info->ResultCode))
        Log("LOBBY UpdateLobby %s: result %d (%s)", info->LobbyId ? IdText(info->LobbyId).c_str() : call->lobbyId.c_str(),
            info->ResultCode, ResultName(info->ResultCode));
    ToGame(info, call);
}

void Entered(const LobbyIdCallbackInfo* info, const GameCall& call, const char* how) {
    if (info->ResultCode != kEosSuccess || !info->LobbyId) return;
    NoteLobbyEntered(call.lobby, call.user, info->LobbyId, call.capacity);
    if (call.capacity)
        Log("LOBBY %s %s for %u players by this machine", IdText(info->LobbyId).c_str(), how, call.capacity);
    else
        Log("LOBBY %s %s", IdText(info->LobbyId).c_str(), how);
}

void Created(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    Entered(info, *call, "created");
    ToGame(info, call);
}

void Joined(const LobbyIdCallbackInfo* info) {
    auto* call = static_cast<GameCall*>(info->ClientData);
    Entered(info, *call, "joined");
    if (info->ResultCode == kEosLobbyAlreadyExists && Final(info->ResultCode) && !call->lobbyId.empty()) {
        // EOS keeps this machine in that lobby from an earlier session (its own copy said nothing, or JoinLobbyHook
        // would have left first). This join fails; leaving clears the stale membership, so the next one gets in.
        Log("LOBBY JoinLobby %s: EOS says this machine is still a member of it (%d, %s); leaving it so the next join "
            "starts clean", call->lobbyId.c_str(), info->ResultCode, ResultName(info->ResultCode));
        LeaveOwn(call->lobby, call->user, call->lobbyId, "a stale membership that refused a join");
    }
    ToGame(info, call);
}

// A join into a lobby EOS still lists this machine in: leave, then join with our own copy of its details (the
// game's handle is only good for the call it was passed to).
struct JoinAfterLeave {
    JoinLobbyOptions options;
    void* details;
    GameCall* call;
};

void LeftBeforeJoin(const LobbyIdCallbackInfo* info) {
    auto* pending = static_cast<JoinAfterLeave*>(info->ClientData);
    if (!Final(info->ResultCode)) return;
    GameCall* call = pending->call;
    Log("LOBBY LeaveLobby %s before joining it again: result %d (%s); joining now", call->lobbyId.c_str(),
        info->ResultCode, ResultName(info->ResultCode));
    pending->options.LobbyDetailsHandle = pending->details;
    api.joinLobby(call->lobby, &pending->options, call, &Joined);  // EOS copies the options and the details
    api.releaseDetails(pending->details);
    delete pending;
}

// --- the game's lobby calls ---

void LeaveLobbyHook(void* lobby, const void* options, void* clientData, LobbyIdCallback callback) {
    const auto* leave = static_cast<const LeaveOptions*>(options);
    const std::string id = leave ? IdText(leave->LobbyId) : std::string("?");
    Log("LOBBY LeaveLobby %s requested by the game", id.c_str());
    NoteLobbyLeft();
    api.leaveLobby(lobby, options, new GameCall{callback, clientData, lobby, leave ? leave->LocalUserId : nullptr, id, 0},
                   &GameLeft);
}

void DestroyLobbyHook(void* lobby, const void* options, void* clientData, LobbyIdCallback callback) {
    const auto* destroy = static_cast<const LeaveOptions*>(options);
    const void* user = destroy ? destroy->LocalUserId : nullptr;
    const std::string id = destroy ? IdText(destroy->LobbyId) : std::string("?");
    LobbyFacts facts;
    std::string owner;
    const bool read = destroy && ReadLobby(lobby, user, destroy->LobbyId, facts, &owner);
    const std::string self = UserText(user);
    NoteLobbyLeft();
    auto* call = new GameCall{callback, clientData, lobby, user, id, 0};
    if (read && !owner.empty() && !self.empty() && owner != self) {
        // EOS refuses a destroy from anyone but the owner and the machine would stay a member. The game destroys
        // only a room it believes it hosts alone, so leaving is what it means; the options have the same fields.
        Log("LOBBY DestroyLobby %s requested by the game, but EOS %s owns it, not this machine (EOS %s): leaving it "
            "instead", id.c_str(), owner.c_str(), self.c_str());
        api.leaveLobby(lobby, options, call, &DestroyedAsLeave);
        return;
    }
    Log("LOBBY DestroyLobby %s requested by the game (owner: %s)", id.c_str(),
        read ? (owner == self ? "this machine" : "unknown") : "unknown, EOS has no copy of the lobby");
    api.destroyLobby(lobby, options, call, &GameDestroyed);
}

void UpdateLobbyHook(void* lobby, const void* options, void* clientData, LobbyIdCallback callback) {
    AcquireSRWLockShared(&current.lock);
    const std::string id = current.id.empty() ? std::string("?") : current.id;
    ReleaseSRWLockShared(&current.lock);
    api.updateLobby(lobby, options, new GameCall{callback, clientData, lobby, nullptr, id, 0}, &GameUpdated);
}

void CreateLobbyHook(void* lobby, const void* options, void* clientData, LobbyIdCallback callback) {
    const auto* create = static_cast<const CreateLobbyOptionsHead*>(options);
    if (!create) return api.createLobby(lobby, options, clientData, callback);
    api.createLobby(lobby, options,
                    new GameCall{callback, clientData, lobby, create->LocalUserId, std::string(), create->MaxLobbyMembers},
                    &Created);
}

void JoinLobbyHook(void* lobby, const void* options, void* clientData, LobbyIdCallback callback) {
    const auto* join = static_cast<const JoinLobbyOptions*>(options);
    if (!join) return api.joinLobby(lobby, options, clientData, callback);
    const std::string target = LobbyIdOf(join->LobbyDetailsHandle);
    auto* call = new GameCall{callback, clientData, lobby, join->LocalUserId, target, 0};
    void* mine = join->ApiVersion == kJoinOptionsVersion && !join->LocalRTCOptions
                     ? CopyDetails(lobby, join->LocalUserId, target.c_str())
                     : nullptr;
    if (!mine) {
        api.joinLobby(lobby, options, call, &Joined);
        return;
    }
    // EOS has a copy of the lobby for this machine, so it still counts it as a member: left behind by an earlier
    // session in it, where the other members see someone who never answers. Joining would fail with 9002.
    Log("LOBBY JoinLobby %s: EOS still lists this machine as a member of it (left behind earlier); leaving it first, "
        "then joining", target.c_str());
    auto* pending = new JoinAfterLeave{*join, mine, call};
    const LeaveOptions leave{1, join->LocalUserId, call->lobbyId.c_str()};
    api.leaveLobby(lobby, &leave, pending, &LeftBeforeJoin);
}

template <typename T>
bool Resolve(HMODULE eos, const char* name, T& out) {
    out = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(eos, name)));
    if (!out) Log("LOBBY: EOS export %s not found", name);
    return out != nullptr;
}

template <typename T>
bool Redirect(HMODULE game, ImportRedirect redirect, const char* name, T replacement, T& original) {
    const bool ok = redirect(game, "EOSSDK-Win64-Shipping.dll", name, reinterpret_cast<void*>(replacement),
                             reinterpret_cast<void**>(&original));
    if (!ok) Log("LOBBY: EDF.dll import %s could not be redirected", name);
    return ok;
}

}  // namespace

LobbyKind KindOf(const LobbyFacts& facts) {
    if (facts.hasSearchType) {
        const auto value = facts.searchType;
        if (value >= static_cast<std::int64_t>(2 * kSearchTypeCenter - 0x94) &&
            value <= static_cast<std::int64_t>(2 * kSearchTypeCenter - 0x91))
            return LobbyKind::MultiSlot;
        if ((value & ~std::int64_t{0xF}) == 0x90) return LobbyKind::Normal;
    }
    if (!facts.maxMembers) return LobbyKind::Unknown;
    return facts.maxMembers > static_cast<std::uint32_t>(kVanillaPlayers) ? LobbyKind::MultiSlot : LobbyKind::Normal;
}

int CapacityToKeep(const LobbyFacts& facts) {
    const auto members = static_cast<int>(facts.maxMembers);
    return members > kVanillaPlayers && members <= kMaxPlayers ? members : kMaxPlayers;
}

RoomUpdate DecideRoomUpdate(std::uintptr_t room) {
    RoomLobby lobby;
    if (!ReadRoomLobby(room, lobby)) {
        const RoomUpdate update{LobbyKind::Unknown, 0};
        NoteDecision("?", update, "the room's lobby could not be read", std::string());
        return update;
    }
    const std::string id = IdText(lobby.id);
    LobbyFacts facts;
    if (ReadLobby(lobby.lobby, lobby.user, lobby.id, facts, nullptr) && KindOf(facts) != LobbyKind::Unknown) {
        const RoomUpdate update{KindOf(facts), CapacityToKeep(facts)};
        NoteDecision(id, update, "read from the lobby", FactsText(facts));
        return update;
    }
    // No copy of the lobby (the update itself will be refused then), or nothing in it yet. This machine knows
    // what it created the lobby as, and nothing else does.
    AcquireSRWLockShared(&current.lock);
    const std::uint32_t created = current.id == lobby.id ? current.createdCapacity : 0;
    ReleaseSRWLockShared(&current.lock);
    if (created) {
        LobbyFacts creation;
        creation.maxMembers = created;
        const RoomUpdate update{KindOf(creation), CapacityToKeep(creation)};
        char detail[48]{};
        _snprintf_s(detail, _TRUNCATE, "created for %u players", created);
        NoteDecision(id, update, "how this machine created it, as EOS has nothing on it", detail);
        return update;
    }
    const RoomUpdate update{LobbyKind::Unknown, 0};
    NoteDecision(id, update, "EOS has nothing on this lobby and this machine did not create it", std::string());
    return update;
}

LobbyKind CurrentLobbyKind() { return static_cast<LobbyKind>(currentKind.load()); }
int CurrentLobbyCapacity() { return currentCapacity.load(); }
bool CreatedCurrentLobby() { return createdCurrent.load(); }

void NoteLobbyEntered(void* lobby, const void* user, const char* lobbyId, std::uint32_t createdCapacity) {
    LobbyFacts creation;
    creation.maxMembers = createdCapacity;
    // A lobby just created is what it was created as until the first read says otherwise.
    const LobbyKind kind = createdCapacity ? KindOf(creation) : LobbyKind::Unknown;
    AcquireSRWLockExclusive(&current.lock);
    current.lobby = lobby;
    current.user = user;
    current.id = lobbyId ? lobbyId : "";
    current.createdCapacity = createdCapacity;
    current.nextBeat = GetTickCount64() + kBeatMs;
    current.owner.clear();
    current.copyLost = false;
    current.kind = kind;
    current.capacity = kind == LobbyKind::MultiSlot ? CapacityToKeep(creation) : kVanillaPlayers;
    currentCapacity.store(kind == LobbyKind::MultiSlot ? current.capacity : 0);  // before the kind, as above
    currentKind.store(static_cast<int>(kind));
    createdCurrent.store(createdCapacity != 0);
    ReleaseSRWLockExclusive(&current.lock);
}

void NoteLobbyLeft() {
    AcquireSRWLockExclusive(&current.lock);
    current.lobby = nullptr;
    current.user = nullptr;
    current.id.clear();
    current.createdCapacity = 0;
    current.owner.clear();
    current.copyLost = false;
    current.kind = LobbyKind::Unknown;
    currentKind.store(static_cast<int>(LobbyKind::Unknown));
    currentCapacity.store(0);
    createdCurrent.store(false);
    ReleaseSRWLockExclusive(&current.lock);
}

bool InstallLobbyState(HMODULE game, ImportRedirect redirect) {
    const HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (!eos) {
        Log("LOBBY: EOSSDK-Win64-Shipping.dll is not loaded");
        return false;
    }
    bool ok = Resolve(eos, "EOS_Lobby_CopyLobbyDetailsHandle", api.copyDetails);
    ok &= Resolve(eos, "EOS_LobbyDetails_CopyInfo", api.copyInfo);
    ok &= Resolve(eos, "EOS_LobbyDetails_Info_Release", api.releaseInfo);
    ok &= Resolve(eos, "EOS_LobbyDetails_GetLobbyOwner", api.owner);
    ok &= Resolve(eos, "EOS_LobbyDetails_CopyAttributeByKey", api.copyAttribute);
    ok &= Resolve(eos, "EOS_Lobby_Attribute_Release", api.releaseAttribute);
    ok &= Resolve(eos, "EOS_LobbyDetails_Release", api.releaseDetails);
    ok &= Resolve(eos, "EOS_EResult_IsOperationComplete", api.isComplete);
    ok &= Resolve(eos, "EOS_EResult_ToString", api.toString);
    if (!ok) return false;
    api.ready = true;
    // Leaving first, entering last, as syncmarker.cpp does: a lobby entered can always be left again.
    ok = Redirect(game, redirect, "EOS_Lobby_LeaveLobby", &LeaveLobbyHook, api.leaveLobby);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_DestroyLobby", &DestroyLobbyHook, api.destroyLobby);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_UpdateLobby", &UpdateLobbyHook, api.updateLobby);
    ok = ok && Redirect(game, redirect, "EOS_Platform_Tick", &TickHook, api.tick);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_CreateLobby", &CreateLobbyHook, api.createLobby);
    ok = ok && Redirect(game, redirect, "EOS_Lobby_JoinLobby", &JoinLobbyHook, api.joinLobby);
    return ok;
}

}  // namespace multislot
