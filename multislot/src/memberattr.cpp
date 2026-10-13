#include "memberattr.h"

#include <algorithm>
#include <cstring>

#include "log.h"
#include "member_attribute_api.h"

static_assert(sizeof(EDF6CoopRoomState) == EDF6COOP_ROOM_STATE_V1_SIZE, "EDF6CoopRoomState is ABI v1's");

namespace multislot::memberattr {
namespace {

// The store's lock for one scope: released however the scope ends, an exception (std::bad_alloc) included - a lock
// left held would hang the game's next call.
class Exclusive {
public:
    explicit Exclusive(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~Exclusive() { ReleaseSRWLockExclusive(&lock_); }
    Exclusive(const Exclusive&) = delete;
    Exclusive& operator=(const Exclusive&) = delete;

private:
    SRWLOCK& lock_;
};

class Shared {
public:
    explicit Shared(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockShared(&lock_); }
    ~Shared() { ReleaseSRWLockShared(&lock_); }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;

private:
    SRWLOCK& lock_;
};

bool KeyChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool Printable(char c) { return c >= 0x20 && c <= 0x7E; }

bool IsExtensionKey(const std::string& key) { return key.rfind(kKeyPrefix, 0) == 0; }

bool Contains(const std::vector<std::string>& keys, const std::string& key) {
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

}  // namespace

bool ValidKey(const char* key) {
    if (!key) return false;
    const std::size_t prefix = sizeof(kKeyPrefix) - 1;
    const std::size_t length = strnlen(key, kMaxKeyLength + 1);
    if (length <= prefix || length > kMaxKeyLength || std::strncmp(key, kKeyPrefix, prefix) != 0) return false;
    return std::all_of(key + prefix, key + length, KeyChar);
}

// Printable ASCII only: a text EOS refuses takes every other attribute of the update down with it.
bool ValidValue(const char* value) {
    if (!value) return false;
    const std::size_t length = strnlen(value, kMaxValueLength + 1);
    return length <= kMaxValueLength && std::all_of(value, value + length, Printable);
}

std::optional<std::string> Decode(const std::string& wire) {
    if (wire.empty() || wire[0] != kPresent || wire.size() - 1 > kMaxValueLength ||
        !std::all_of(wire.begin() + 1, wire.end(), Printable))
        return std::nullopt;
    return wire.substr(1);
}

void Store::Attach(Sinks sinks) {
    Exclusive guard(lock_);
    sinks_ = std::move(sinks);
    attached_ = true;
    if (sinks_.watch)
        for (const std::string& key : watched_) sinks_.watch(key);
    if (sinks_.publish)
        for (const auto& [key, wire] : own_) sinks_.publish(key, wire);
}

// Caller holds lock_ (exclusive): publishes stay in call order.
void Store::SendLocked(const std::string& key, std::string wire) {
    std::string& own = own_[key];
    if (own == wire) return;
    own = std::move(wire);
    ++generation_;
    if (attached_ && sinks_.publish) sinks_.publish(key, own);
}

// Bytes of our current values but `except`'s. Caller holds lock_.
std::size_t Store::ValueBytesLocked(const std::string& except) const {
    std::size_t bytes = 0;
    for (const auto& [key, wire] : own_)
        if (key != except && wire.size() > 0 && wire[0] == kPresent) bytes += wire.size() - 1;
    return bytes;
}

Error Store::Publish(const char* key, const char* value) {
    if (!ValidKey(key)) return Error::kInvalidKey;
    if (!ValidValue(value)) return Error::kInvalidValue;
    const std::string name = key;
    std::string wire = kPresent + std::string(value);
    Exclusive guard(lock_);
    const bool known = Contains(published_, name);
    if (!known && published_.size() >= kMaxPublishedKeys) return Error::kPublishKeyLimit;
    if (ValueBytesLocked(name) + (wire.size() - 1) > kMaxTotalValueBytes) return Error::kValueBudget;
    if (!known) published_.push_back(name);
    SendLocked(name, std::move(wire));
    return Error::kNone;
}

Error Store::Clear(const char* key) {
    if (!ValidKey(key)) return Error::kInvalidKey;
    const std::string name = key;
    Exclusive guard(lock_);
    const auto own = own_.find(name);
    if (own == own_.end()) return Error::kNone;
    if (!attached_) {  // never sent: forgotten, and its key no longer counts
        own_.erase(own);
        published_.erase(std::find(published_.begin(), published_.end(), name));
        ++generation_;
        return Error::kNone;
    }
    SendLocked(name, kWithdrawn);
    return Error::kNone;
}

void Store::Observe(const LobbyView& view) {
    // Everything the view says, taken out before the lock; only the ordinals need the members we know.
    std::vector<Member> members;
    members.reserve(view.members.size());
    for (const LobbyView::Member& seen : view.members) {
        Member& member = members.emplace_back(Member{seen.id, 0, {}});
        if (seen.id == view.self) continue;  // ours come from own_: the lobby's echo of them changes nothing
        for (const auto& [key, wire] : seen.texts)
            if (IsExtensionKey(key)) member.wire.emplace(key, wire);
    }
    const std::string self = view.lobbyId.empty() ? std::string() : view.self;
    const std::string owner = view.lobbyId.empty() ? std::string() : view.owner;
    const auto same = [](const Member& a, const Member& b) { return a.id == b.id && a.wire == b.wire; };

    Exclusive guard(lock_);
    if (view.lobbyId != lobby_) {
        members_.clear();
        nextOrdinal_ = 0;
    }
    for (Member& member : members) {
        const auto known = std::find_if(members_.begin(), members_.end(), [&](const Member& m) { return m.id == member.id; });
        member.ordinal = known != members_.end() ? known->ordinal : nextOrdinal_++;
    }
    std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.ordinal < b.ordinal; });
    const bool changed = view.lobbyId != lobby_ || self != self_ || owner != owner_ ||
                         !std::equal(members.begin(), members.end(), members_.begin(), members_.end(), same);
    lobby_ = view.lobbyId;
    self_ = self;
    owner_ = owner;
    members_.swap(members);
    if (changed) ++generation_;
    // `members` (the old list) is freed after the lock.
}

RoomFacts Store::Room() const {
    RoomFacts facts;
    Shared guard(lock_);
    facts.inRoom = !lobby_.empty();
    facts.host = !owner_.empty() && owner_ == self_;
    facts.members = static_cast<std::uint32_t>(members_.size());
    facts.generation = generation_;
    facts.self = self_;
    facts.owner = owner_;
    return facts;
}

// Ours from what we published (the lobby's copy lags a beat); everyone else's from the lobby. Caller holds lock_.
std::optional<std::string> Store::ValueLocked(const Member& member, const std::string& key) const {
    const auto& texts = member.id == self_ ? own_ : member.wire;
    const auto wire = texts.find(key);
    return wire == texts.end() ? std::nullopt : Decode(wire->second);
}

Error Store::Read(const char* key, std::vector<MemberValue>& members, std::uint64_t& generation) {
    if (!ValidKey(key)) return Error::kInvalidKey;
    const std::string name = key;
    Exclusive guard(lock_);
    if (!Contains(watched_, name)) {
        if (watched_.size() >= kMaxWatchedKeys) return Error::kWatchKeyLimit;
        watched_.push_back(name);
        if (attached_ && sinks_.watch) sinks_.watch(name);
    }
    members.reserve(members_.size());
    for (const Member& member : members_)  // only this key's values are copied
        members.push_back({member.id, member.ordinal, !owner_.empty() && member.id == owner_, member.id == self_,
                           ValueLocked(member, name)});
    generation = generation_;
    return Error::kNone;
}

Store& MemberAttributes() {
    static Store store;
    return store;
}

void StartMemberAttributes(bool lobbyGlue) {
    WatchMemberTexts({}, [](const LobbyView& view) { MemberAttributes().Observe(view); });
    // No copy of the lobby we are in: EOS has none, we are not in it (any more). Shown as no room until a beat reads
    // one again (ordinals start again then).
    WatchLobbyUnreadable([] { MemberAttributes().Observe(LobbyView{}); });
    if (!lobbyGlue) {
        Log("MEMBER attributes: no lobby glue, extensions' room member attributes are kept but never sent");
        return;
    }
    MemberAttributes().Attach({[](const std::string& key, const std::string& wire) { PublishExtensionText(key, wire); },
                               [](const std::string& key) { WatchMemberKeys({key}); }});
}

}  // namespace multislot::memberattr

namespace {

using multislot::memberattr::Error;
using multislot::memberattr::MemberAttributes;

thread_local std::uint32_t lastError = EDF6COOP_MA_OK;

uint32_t Result(Error error) {
    lastError = static_cast<std::uint32_t>(error);
    return error == Error::kNone ? 1u : 0u;
}

void CopyId(char (&out)[EDF6COOP_MEMBER_ID_SIZE], const std::string& id) {
    strncpy_s(out, id.c_str(), _TRUNCATE);
}

uint32_t EDF6COOP_MA_CALL Publish(const char* key, const char* value) noexcept {
    try { return Result(MemberAttributes().Publish(key, value)); } catch (...) { return Result(Error::kInternal); }
}

uint32_t EDF6COOP_MA_CALL Clear(const char* key) noexcept {
    try { return Result(MemberAttributes().Clear(key)); } catch (...) { return Result(Error::kInternal); }
}

// Forward compatible: a caller built against a later, longer EDF6CoopRoomState gets the v1 part and learns from
// `size` how much of it was written.
uint32_t EDF6COOP_MA_CALL Room(EDF6CoopRoomState* out) noexcept {
    if (!out || out->size < EDF6COOP_ROOM_STATE_V1_SIZE) return Result(Error::kBadArgument);
    try {
        const multislot::memberattr::RoomFacts facts = MemberAttributes().Room();
        EDF6CoopRoomState state{sizeof(state), facts.inRoom ? 1u : 0u, facts.members, facts.host ? 1u : 0u,
                                facts.generation, {}, {}};
        CopyId(state.self, facts.self);
        CopyId(state.host, facts.owner);
        const uint32_t written = static_cast<uint32_t>(std::min<std::size_t>(out->size, sizeof(state)));
        std::memcpy(out, &state, written);
        out->size = written;
        return Result(Error::kNone);
    } catch (...) { return Result(Error::kInternal); }
}

// The callback runs on copies, outside the store's lock.
uint32_t EDF6COOP_MA_CALL Snapshot(const char* key, EDF6CoopMemberValueFn each, void* context,
                                   uint64_t* outGeneration) noexcept {
    if (!each) return Result(Error::kBadArgument);
    std::vector<multislot::memberattr::MemberValue> members;
    std::uint64_t generation = 0;
    try {
        const Error error = MemberAttributes().Read(key, members, generation);
        if (error != Error::kNone) return Result(error);
    } catch (...) { return Result(Error::kInternal); }
    if (outGeneration) *outGeneration = generation;
    for (const auto& member : members) {
        const EDF6CoopMemberValue view{member.id.c_str(), member.ordinal, member.host ? 1u : 0u, member.self ? 1u : 0u,
                                       member.value ? 1u : 0u, member.value ? member.value->c_str() : "",
                                       static_cast<uint32_t>(member.value ? member.value->size() : 0)};
        each(context, &view);
    }
    // Set after the callbacks: one that calls this table again must not leave its own code behind.
    return Result(Error::kNone);
}

uint32_t EDF6COOP_MA_CALL LastError() noexcept { return lastError; }

constexpr EDF6CoopMemberAttributeApi kApiV1{sizeof(EDF6CoopMemberAttributeApi), EDF6COOP_MEMBER_ATTRIBUTE_VERSION,
                                            &Publish, &Clear, &Room, &Snapshot, &LastError};

}  // namespace

extern "C" __declspec(dllexport) const EDF6CoopMemberAttributeApi* EDF6COOP_MA_CALL EDF6CoopGetMemberAttributeApi(
    uint32_t version) noexcept {
    return version == EDF6COOP_MEMBER_ATTRIBUTE_VERSION ? &kApiV1 : nullptr;
}
