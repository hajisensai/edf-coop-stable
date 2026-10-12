#include "memberattr.h"

#include <algorithm>
#include <cstring>

#include "log.h"
#include "member_attribute_api.h"

namespace multislot::memberattr {
namespace {

bool KeyChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

bool IsExtensionKey(const std::string& key) { return key.rfind(kKeyPrefix, 0) == 0; }

}  // namespace

bool ValidKey(const char* key) {
    if (!key) return false;
    const std::size_t prefix = sizeof(kKeyPrefix) - 1;
    const std::size_t length = strnlen(key, kMaxKeyLength + 1);
    if (length <= prefix || length > kMaxKeyLength || std::strncmp(key, kKeyPrefix, prefix) != 0) return false;
    return std::all_of(key + prefix, key + length, KeyChar);
}

// Printable ASCII only: a text EOS refuses takes every other attribute of our member update down with it.
bool ValidValue(const char* value) {
    if (!value) return false;
    const std::size_t length = strnlen(value, kMaxValueLength + 1);
    return length <= kMaxValueLength && std::all_of(value, value + length, [](char c) { return c >= 0x20 && c <= 0x7E; });
}

std::optional<std::string> Decode(const std::string& wire) {
    if (wire.empty() || wire[0] != kPresent) return std::nullopt;
    return wire.substr(1);
}

void Store::Attach(Sinks sinks) {
    AcquireSRWLockExclusive(&lock_);
    sinks_ = std::move(sinks);
    attached_ = true;
    if (sinks_.watch)
        for (const std::string& key : keys_) sinks_.watch(key);
    ReleaseSRWLockExclusive(&lock_);
}

// Caller holds lock_ (exclusive). Our own sinks only: syncmarker never calls back into us under its lock.
bool Store::AdmitKeyLocked(const std::string& key) {
    if (std::find(keys_.begin(), keys_.end(), key) != keys_.end()) return true;
    if (keys_.size() >= kMaxKeys) return false;
    keys_.push_back(key);
    if (sinks_.watch) sinks_.watch(key);
    return true;
}

// Caller holds lock_ (exclusive): publishes stay in call order.
bool Store::SendLocked(const std::string& key, const std::string& wire) {
    std::string& own = own_[key];
    if (own == wire) return true;
    own = wire;
    ++generation_;
    if (sinks_.publish) sinks_.publish(key, wire);
    return true;
}

bool Store::Publish(const char* key, const char* value) {
    if (!ValidKey(key) || !ValidValue(value)) return false;
    AcquireSRWLockExclusive(&lock_);
    const bool ok = attached_ && AdmitKeyLocked(key) && SendLocked(key, kPresent + std::string(value));
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}

bool Store::Clear(const char* key) {
    if (!ValidKey(key)) return false;
    AcquireSRWLockExclusive(&lock_);
    const bool published = own_.find(key) != own_.end();
    const bool ok = !published || SendLocked(key, kWithdrawn);
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}

void Store::Observe(const LobbyView& view) {
    AcquireSRWLockExclusive(&lock_);
    if (view.lobbyId != lobby_) {
        members_.clear();
        nextOrdinal_ = 0;
    }
    std::vector<Member> members;
    for (const LobbyView::Member& seen : view.members) {
        const auto known = std::find_if(members_.begin(), members_.end(), [&](const Member& m) { return m.id == seen.id; });
        Member& member = members.emplace_back(Member{seen.id, known != members_.end() ? known->ordinal : nextOrdinal_++, {}});
        if (seen.id == view.self) continue;  // ours come from own_: the lobby's echo of them changes nothing
        for (const auto& [key, wire] : seen.texts)
            if (IsExtensionKey(key)) member.wire[key] = wire;
    }
    std::sort(members.begin(), members.end(), [](const Member& a, const Member& b) { return a.ordinal < b.ordinal; });
    const auto same = [](const Member& a, const Member& b) { return a.id == b.id && a.wire == b.wire; };
    const bool changed = view.lobbyId != lobby_ || view.self != self_ || view.owner != owner_ ||
                         !std::equal(members.begin(), members.end(), members_.begin(), members_.end(), same);
    lobby_ = view.lobbyId;
    self_ = view.lobbyId.empty() ? std::string() : view.self;
    owner_ = view.lobbyId.empty() ? std::string() : view.owner;
    members_ = std::move(members);
    if (changed) ++generation_;
    ReleaseSRWLockExclusive(&lock_);
}

RoomFacts Store::Room() const {
    AcquireSRWLockShared(&lock_);
    RoomFacts facts{!lobby_.empty(), !owner_.empty() && owner_ == self_, static_cast<std::uint32_t>(members_.size()),
                    generation_, self_, owner_};
    ReleaseSRWLockShared(&lock_);
    return facts;
}

// Ours from what we published (the lobby's copy lags a beat); everyone else's from the lobby. Caller holds lock_.
std::optional<std::string> Store::ValueLocked(const Member& member, const std::string& key) const {
    const auto& texts = member.id == self_ ? own_ : member.wire;
    const auto wire = texts.find(key);
    return wire == texts.end() ? std::nullopt : Decode(wire->second);
}

bool Store::Read(const char* key, std::vector<MemberValue>& members, std::uint64_t& generation) {
    if (!ValidKey(key)) return false;
    AcquireSRWLockExclusive(&lock_);
    const bool ok = AdmitKeyLocked(key);
    for (const Member& member : ok ? members_ : std::vector<Member>())
        members.push_back({member.id, member.ordinal, !owner_.empty() && member.id == owner_, member.id == self_,
                           ValueLocked(member, key)});
    generation = generation_;
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}

Store& MemberAttributes() {
    static Store store;
    return store;
}

void StartMemberAttributes(bool lobbyGlue) {
    WatchMemberTexts({}, [](const LobbyView& view) { MemberAttributes().Observe(view); });
    if (!lobbyGlue) {
        Log("MEMBER attributes: no lobby glue, extensions cannot publish room member attributes");
        return;
    }
    MemberAttributes().Attach({[](const std::string& key, const std::string& wire) { PublishMemberText(key, wire); },
                               [](const std::string& key) { WatchMemberKeys({key}); }});
}

}  // namespace multislot::memberattr

namespace {

using multislot::memberattr::MemberAttributes;

void CopyId(char (&out)[EDF6COOP_MEMBER_ID_SIZE], const std::string& id) {
    strncpy_s(out, id.c_str(), _TRUNCATE);
}

uint32_t EDF6COOP_MA_CALL Publish(const char* key, const char* value) noexcept {
    try { return MemberAttributes().Publish(key, value) ? 1u : 0u; } catch (...) { return 0; }
}

uint32_t EDF6COOP_MA_CALL Clear(const char* key) noexcept {
    try { return MemberAttributes().Clear(key) ? 1u : 0u; } catch (...) { return 0; }
}

uint32_t EDF6COOP_MA_CALL Room(EDF6CoopRoomState* out) noexcept {
    if (!out || out->size != sizeof(*out)) return 0;
    try {
        const multislot::memberattr::RoomFacts facts = MemberAttributes().Room();
        *out = {sizeof(*out), facts.inRoom ? 1u : 0u, facts.members, facts.host ? 1u : 0u, facts.generation, {}, {}};
        CopyId(out->self, facts.self);
        CopyId(out->host, facts.owner);
        return 1;
    } catch (...) { return 0; }
}

// The callback runs on copies, outside the store's lock.
uint32_t EDF6COOP_MA_CALL Snapshot(const char* key, EDF6CoopMemberValueFn each, void* context,
                                   uint64_t* outGeneration) noexcept {
    if (!each) return 0;
    std::vector<multislot::memberattr::MemberValue> members;
    std::uint64_t generation = 0;
    try {
        if (!MemberAttributes().Read(key, members, generation)) return 0;
    } catch (...) { return 0; }
    if (outGeneration) *outGeneration = generation;
    for (const auto& member : members) {
        const EDF6CoopMemberValue view{member.id.c_str(), member.ordinal, member.host ? 1u : 0u, member.self ? 1u : 0u,
                                       member.value ? 1u : 0u, member.value ? member.value->c_str() : "",
                                       static_cast<uint32_t>(member.value ? member.value->size() : 0)};
        each(context, &view);
    }
    return 1;
}

constexpr EDF6CoopMemberAttributeApi kApiV1{sizeof(EDF6CoopMemberAttributeApi), EDF6COOP_MEMBER_ATTRIBUTE_VERSION,
                                            &Publish, &Clear, &Room, &Snapshot};

}  // namespace

extern "C" __declspec(dllexport) const EDF6CoopMemberAttributeApi* EDF6COOP_MA_CALL EDF6CoopGetMemberAttributeApi(
    uint32_t version) noexcept {
    return version == EDF6COOP_MEMBER_ATTRIBUTE_VERSION ? &kApiV1 : nullptr;
}
