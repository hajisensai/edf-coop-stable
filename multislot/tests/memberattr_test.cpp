// Room member attributes for other plugins (memberattr.h, member_attribute_api.h) without EOS: key and value
// validation, publish/clear on the wire, publishes kept until the lobby glue attaches, separate publish and read key
// limits, the value budget, remote values outside the contract, snapshots of fake rooms, the generation counter, the
// lock surviving a throwing sink, error codes, the forward-compatible room() and version negotiation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../src/memberattr.h"
#include "../src/member_attribute_api.h"

extern "C" const EDF6CoopMemberAttributeApi* EDF6COOP_MA_CALL EDF6CoopGetMemberAttributeApi(uint32_t version) noexcept;

using namespace multislot;
using namespace multislot::memberattr;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

struct Recorder {
    std::vector<std::pair<std::string, std::string>> published;
    std::vector<std::string> watched;
    Sinks MakeSinks() {
        return {[this](const std::string& k, const std::string& w) { published.emplace_back(k, w); },
                [this](const std::string& k) { watched.push_back(k); }};
    }
};

LobbyView Room(const std::string& lobby, const std::string& self, const std::string& owner,
               std::vector<std::pair<std::string, std::string>> members, const std::string& key = "EDF6X_VC") {
    LobbyView view{lobby, self, owner, {}, {}};
    for (auto& [id, wire] : members) {
        LobbyView::Member& member = view.members.emplace_back();
        member.id = id;
        if (!wire.empty()) member.texts[key] = wire;
        member.texts["EDF6MS_SEQ"] = "1";  // Coop's own keys never show through
    }
    return view;
}

void KeysAndValues() {
    Check(ValidKey("EDF6X_VC"), "short extension key");
    Check(ValidKey("EDF6X_VEHICLE_CREW_VARIANTS_1234"), "32-character key");
    Check(!ValidKey("EDF6X_VEHICLE_CREW_VARIANTS_12345"), "33-character key refused");
    Check(!ValidKey("EDF6X_"), "bare prefix refused");
    Check(!ValidKey("EDF6MS_SPLITSYNC"), "Coop's own key refused");
    Check(!ValidKey("EDF6CO_HD"), "host data key refused");
    Check(!ValidKey("EDF6X_vc"), "lower case refused");
    Check(!ValidKey("EDF6X_V-C"), "dash refused");
    Check(!ValidKey(nullptr), "null key refused");
    Check(ValidValue(""), "empty value allowed (goes out as \"=\")");
    Check(ValidValue(std::string(999, 'a').c_str()), "999 bytes allowed");
    Check(!ValidValue(std::string(1000, 'a').c_str()), "1000 bytes refused");
    Check(!ValidValue("tab\there"), "control character refused");
    Check(!ValidValue("\xE3\x81\x82"), "non-ASCII refused");
    Check(!ValidValue(nullptr), "null value refused");
    Check(Decode("=abc") == std::optional<std::string>("abc"), "decode value");
    Check(Decode("=") == std::optional<std::string>(""), "decode empty value");
    Check(!Decode("-"), "decode withdrawn");
    Check(!Decode("abc"), "decode foreign text as none");
    Check(Decode("=" + std::string(999, 'a')) == std::optional<std::string>(std::string(999, 'a')), "999 bytes decode");
    Check(!Decode("=" + std::string(1000, 'a')), "a remote value over 999 bytes reads as none");
    Check(!Decode("=tab\there") && !Decode("=\x7F") && !Decode("=\xE3\x81\x82"),
          "a remote value outside printable ASCII reads as none");
    Check(!Decode(std::string("=a\0b", 4)), "a remote value with a NUL reads as none");
}

bool Ok(Error error) { return error == Error::kNone; }

void PublishAndClear() {
    Store store;
    Recorder rec;
    Check(Ok(store.Publish("EDF6X_VC", "a")), "publish accepted before the lobby glue");
    Check(Ok(store.Publish("EDF6X_GONE", "g")) && Ok(store.Clear("EDF6X_GONE")), "and cleared before it");
    std::vector<MemberValue> members;
    std::uint64_t g = 0;
    Check(Ok(store.Read("EDF6X_R", members, g)), "read before the lobby glue");
    store.Attach(rec.MakeSinks());
    Check(rec.published == std::vector<std::pair<std::string, std::string>>{{"EDF6X_VC", "=a"}},
          "attaching sends what was published before (and not what was cleared before)");
    Check(rec.watched == std::vector<std::string>{"EDF6X_R"}, "attaching watches what was read before");
    Check(Ok(store.Publish("EDF6X_VC", "a,b")), "publish");
    Check(Ok(store.Publish("EDF6X_VC", "a,b")), "same value again");
    Check(rec.published.size() == 2 && rec.published[1].second == "=a,b", "one wire text, value marked");
    Check(rec.watched == std::vector<std::string>{"EDF6X_R"}, "publishing watches nothing");
    Check(store.Publish("EDF6MS_SEQ", "9") == Error::kInvalidKey, "Coop key refused");
    Check(store.Publish("EDF6X_VC", "\t") == Error::kInvalidValue, "bad value refused");
    Check(Ok(store.Clear("EDF6X_VC")) && rec.published.back().second == "-", "clear withdraws");
    Check(Ok(store.Clear("EDF6X_OTHER")) && rec.published.size() == 3, "clear of an unpublished key sends nothing");
}

// Reads and publishes count against limits of their own.
void KeyLimits() {
    Store store;
    Recorder rec;
    store.Attach(rec.MakeSinks());
    std::vector<MemberValue> members;
    std::uint64_t g = 0;
    for (int i = 0; i < 16; ++i)
        Check(Ok(store.Read(("EDF6X_R" + std::to_string(i)).c_str(), members, g)), "16 read keys");
    Check(store.Read("EDF6X_R16", members, g) == Error::kWatchKeyLimit, "the 17th read key is refused");
    Check(Ok(store.Read("EDF6X_R3", members, g)), "a read key already watched still reads");
    for (int i = 0; i < 16; ++i)
        Check(Ok(store.Publish(("EDF6X_P" + std::to_string(i)).c_str(), "x")), "16 published keys besides 16 read ones");
    Check(store.Publish("EDF6X_P16", "x") == Error::kPublishKeyLimit, "the 17th published key is refused");
    Check(Ok(store.Publish("EDF6X_P3", "y")), "a published key can change");
    Check(store.Read("EDF6X_P0", members, g) == Error::kWatchKeyLimit, "reading a published key needs a read slot");
    Check(rec.watched.size() == 16, "only read keys are watched");
}

// All our values together stay within kMaxTotalValueBytes.
void ValueBudget() {
    Store store;
    Recorder rec;
    store.Attach(rec.MakeSinks());
    const std::string big(999, 'v');
    for (int i = 0; i < 4; ++i)
        Check(Ok(store.Publish(("EDF6X_B" + std::to_string(i)).c_str(), big.c_str())), "3996 bytes");
    Check(store.Publish("EDF6X_B4", std::string(101, 'v').c_str()) == Error::kValueBudget, "4097 bytes refused");
    Check(Ok(store.Publish("EDF6X_B4", std::string(100, 'v').c_str())), "4096 bytes accepted");
    Check(Ok(store.Publish("EDF6X_B0", std::string(999, 'w').c_str())), "a value replaced counts once");
    Check(Ok(store.Clear("EDF6X_B1")) && Ok(store.Publish("EDF6X_B5", big.c_str())), "a cleared value frees its bytes");
    const std::size_t sent = rec.published.size();
    Check(store.Publish("EDF6X_B6", "z") == Error::kValueBudget && rec.published.size() == sent,
          "a refused value is not sent");
}

// A sink that throws (std::bad_alloc in the plugin) leaves the store's lock free: the next call does not hang.
struct Probe {
    Store* store;
    bool done;
};

DWORD WINAPI ProbeLock(void* p) {
    auto* probe = static_cast<Probe*>(p);
    probe->store->Observe(LobbyView{"L", "S", "S", {}, {}});
    probe->done = probe->store->Room().inRoom;
    return 0;
}

void LockSurvivesThrow() {
    Store store;
    store.Attach({[](const std::string&, const std::string&) { throw std::runtime_error("sink"); },
                  [](const std::string&) { throw std::runtime_error("watch"); }});
    bool threw = false;
    try {
        store.Publish("EDF6X_T", "x");
    } catch (const std::runtime_error&) { threw = true; }
    Check(threw, "the publish sink's exception reaches the caller");
    threw = false;
    std::vector<MemberValue> members;
    std::uint64_t g = 0;
    try {
        store.Read("EDF6X_T", members, g);
    } catch (const std::runtime_error&) { threw = true; }
    Check(threw, "the watch sink's exception reaches the caller");
    Probe probe{&store, false};
    const HANDLE thread = CreateThread(nullptr, 0, &ProbeLock, &probe, 0, nullptr);
    Check(thread && WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0 && probe.done,
          "the lock is free after the exceptions (another thread gets it)");
    if (thread) CloseHandle(thread);
}

void Snapshots() {
    Store store;
    Recorder rec;
    store.Attach(rec.MakeSinks());
    std::vector<MemberValue> members;
    std::uint64_t g0 = 0, g = 0;
    Check(Ok(store.Read("EDF6X_VC", members, g0)) && members.empty() && g0 != 0, "outside a room: no members");
    Check(!store.Room().inRoom, "not in a room");
    store.Observe(Room("L1", "B", "A", {{"A", "=v1"}, {"B", ""}, {"C", "-"}, {"D", "junk"}}));
    const RoomFacts facts = store.Room();
    Check(facts.inRoom && !facts.host && facts.members == 4 && facts.owner == "A" && facts.self == "B", "room facts");
    members.clear();
    store.Read("EDF6X_VC", members, g);
    Check(g > g0, "generation moved on entering");
    Check(members.size() == 4 && members[0].id == "A" && members[0].host && members[0].value == std::optional<std::string>("v1"),
          "host's value");
    {
        LobbyView view = Room("L1", "B", "A", {{"A", "=v1"}, {"B", ""}, {"C", "=bad\x01value"}, {"D", "junk"}});
        view.members[0].texts["EDF6X_OTHER"] = "=not asked for";
        store.Observe(view);
        std::vector<MemberValue> other;
        std::uint64_t go = 0;
        store.Read("EDF6X_VC", other, go);
        Check(other.size() == 4 && !other[2].value && other[0].value == std::optional<std::string>("v1"),
              "a remote value outside the contract reads as none; the key asked for only");
        store.Observe(Room("L1", "B", "A", {{"A", "=v1"}, {"B", ""}, {"C", "-"}, {"D", "junk"}}));
        members.clear();
        store.Read("EDF6X_VC", members, g);
    }
    Check(members[1].self && !members[1].value, "self without a value");
    Check(!members[2].value && !members[3].value, "withdrawn and foreign texts read as none");
    store.Publish("EDF6X_VC", "mine");
    members.clear();
    std::uint64_t g2 = 0;
    store.Read("EDF6X_VC", members, g2);
    Check(g2 > g && members[1].value == std::optional<std::string>("mine"), "own value shows at once");
    store.Observe(Room("L1", "B", "A", {{"A", "=v1"}, {"B", "=mine"}, {"C", "-"}, {"D", "junk"}}));
    Check(store.Room().generation == g2, "unchanged beat keeps the generation");
    store.Observe(Room("L1", "B", "A", {{"E", "=new"}, {"A", "=v1"}, {"B", "=mine"}}));
    members.clear();
    store.Read("EDF6X_VC", members, g);
    Check(g > g2 && members.size() == 3, "leavers dropped, generation moved");
    Check(members[0].id == "A" && members[0].ordinal == 0 && members[2].id == "E" && members[2].ordinal == 4,
          "ordinals by first appearance, stable across beats");
    store.Observe(Room("L1", "B", "B", {{"A", "=v1"}, {"B", "=mine"}}));
    Check(store.Room().host, "host migrated to us");
    store.Observe(LobbyView{});
    Check(!store.Room().inRoom && store.Room().members == 0 && store.Room().self.empty() && store.Room().owner.empty(),
          "left the room (kicked, closed or unreadable: syncmarker hands the same empty view)");
    members.clear();
    store.Read("EDF6X_VC", members, g);
    Check(members.empty(), "no members after leaving");
    store.Observe(Room("L2", "B", "A", {{"C", "=x"}, {"B", ""}}));
    members.clear();
    store.Read("EDF6X_VC", members, g);
    Check(members[0].id == "C" && members[0].ordinal == 0, "ordinals restart in a new room");
}

struct Seen {
    std::vector<std::string> lines;
};

void EDF6COOP_MA_CALL Collect(void* context, const EDF6CoopMemberValue* member) {
    auto* seen = static_cast<Seen*>(context);
    seen->lines.push_back(std::string(member->memberId) + "#" + std::to_string(member->ordinal) +
                          (member->isHost ? "H" : "") + (member->isSelf ? "S" : "") +
                          (member->hasValue ? "=" + std::string(member->value, member->valueBytes) : "-"));
}

void Abi() {
    Check(EDF6CoopGetMemberAttributeApi(0) == nullptr, "version 0 unknown");
    Check(EDF6CoopGetMemberAttributeApi(2) == nullptr, "version 2 unknown");
    const EDF6CoopMemberAttributeApi* api = EDF6CoopGetMemberAttributeApi(EDF6COOP_MEMBER_ATTRIBUTE_VERSION);
    Check(api && api->size == sizeof(*api) && api->version == 1, "version 1 table");
    if (!api) return;
    Check(EDF6COOP_MA_HAS(api, lastError) && api->lastError, "the table has lastError");
    Check(api->publish("EDF6X_EARLY", "e") == 1 && api->lastError() == EDF6COOP_MA_OK,
          "publish before the lobby glue is accepted");
    Recorder rec;
    MemberAttributes().Attach(rec.MakeSinks());
    Check(!rec.published.empty() && rec.published[0] == std::pair<std::string, std::string>("EDF6X_EARLY", "=e"),
          "and sent when the glue attaches");
    Check(api->publish("EDF6X_VC", "ab") == 1, "publish through the ABI");
    Check(!api->publish("EDF6X_VC", nullptr) && api->lastError() == EDF6COOP_MA_INVALID_VALUE, "null value: invalid value");
    Check(!api->publish(nullptr, "a") && api->lastError() == EDF6COOP_MA_INVALID_KEY, "null key: invalid key");
    Check(!api->publish("EDF6X_VC", std::string(4000, 'x').c_str()) && api->lastError() == EDF6COOP_MA_INVALID_VALUE,
          "too long: invalid value");
    for (int i = 0; i < 4; ++i) api->publish(("EDF6X_F" + std::to_string(i)).c_str(), std::string(999, 'f').c_str());
    Check(!api->publish("EDF6X_F4", std::string(999, 'f').c_str()) && api->lastError() == EDF6COOP_MA_VALUE_BUDGET,
          "budget: value budget");
    for (int i = 0; i < 4; ++i) api->clear(("EDF6X_F" + std::to_string(i)).c_str());
    Check(api->publish("EDF6X_F4", "small") == 1 && api->lastError() == EDF6COOP_MA_OK, "success resets the code");
    EDF6CoopRoomState state{};
    Check(!api->room(&state) && api->lastError() == EDF6COOP_MA_BAD_ARGUMENT, "room needs size");
    Check(!api->room(nullptr) && api->lastError() == EDF6COOP_MA_BAD_ARGUMENT, "room needs out");
    state.size = EDF6COOP_ROOM_STATE_V1_SIZE - 1;
    Check(!api->room(&state), "room refuses a size below v1's");
    state.size = sizeof(state);
    Check(api->room(&state) && !state.inRoom && state.generation != 0 && state.size == EDF6COOP_ROOM_STATE_V1_SIZE,
          "room outside a lobby");
    // A caller built against a later, longer state: v1's part is written, the rest left alone, size says so.
    struct Later {
        EDF6CoopRoomState v1;
        std::uint64_t later[4];
    } later;
    std::memset(&later, 0xCC, sizeof(later));
    later.v1.size = sizeof(later);
    Check(api->room(&later.v1) && later.v1.size == EDF6COOP_ROOM_STATE_V1_SIZE && later.v1.generation != 0 &&
              later.later[0] == 0xCCCCCCCCCCCCCCCCull && later.later[3] == 0xCCCCCCCCCCCCCCCCull,
          "a longer state gets v1's bytes and size = bytes written");
    MemberAttributes().Observe(Room("L", "S1", "H1", {{"H1", "=host"}, {"S1", "=ab"}, {"X", ""}}));
    Check(api->room(&state) && state.inRoom && state.memberCount == 3 && !state.isHost &&
              std::string(state.self) == "S1" && std::string(state.host) == "H1",
          "room inside a lobby");
    Seen seen;
    std::uint64_t generation = 0;
    Check(api->snapshot("EDF6X_VC", &Collect, &seen, &generation) == 1 && generation == state.generation,
          "snapshot generation matches room");
    Check(seen.lines == std::vector<std::string>{"H1#0H=host", "S1#1S=ab", "X#2-"}, "snapshot rows");
    Check(!api->snapshot("EDF6CO_HD", &Collect, &seen, nullptr) && api->lastError() == EDF6COOP_MA_INVALID_KEY,
          "snapshot refuses Coop keys");
    Check(!api->snapshot("EDF6X_VC", nullptr, nullptr, nullptr) && api->lastError() == EDF6COOP_MA_BAD_ARGUMENT,
          "snapshot needs a callback");
    Check(api->clear("EDF6X_VC") == 1 && rec.published.back().second == "-", "clear through the ABI");
    seen.lines.clear();
    api->snapshot("EDF6X_VC", &Collect, &seen, nullptr);
    Check(seen.lines.size() == 3 && seen.lines[1] == "S1#1S-", "own value withdrawn at once");
    Seen ignored;
    int reads = 1;  // EDF6X_VC
    while (reads < 40 && api->snapshot(("EDF6X_W" + std::to_string(reads)).c_str(), &Collect, &ignored, nullptr)) ++reads;
    Check(reads == 16 && api->lastError() == EDF6COOP_MA_WATCH_KEY_LIMIT, "the read key limit: watch key limit");
    Check(api->publish("EDF6X_AFTER_READS", "p") == 1, "reads use no publish slot");
}

}  // namespace

int main() {
    KeysAndValues();
    PublishAndClear();
    KeyLimits();
    ValueBudget();
    LockSurvivesThrow();
    Snapshots();
    Abi();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("member attribute tests passed\n");
    return 0;
}
