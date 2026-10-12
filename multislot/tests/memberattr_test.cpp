// Room member attributes for other plugins (memberattr.h, member_attribute_api.h) without EOS: key and value
// validation, publish/clear on the wire, snapshots of fake rooms, the generation counter and version negotiation.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdint>
#include <cstdio>
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
}

void PublishAndClear() {
    Store store;
    Recorder rec;
    Check(!store.Publish("EDF6X_VC", "a"), "publish refused before the lobby glue");
    store.Attach(rec.MakeSinks());
    Check(store.Publish("EDF6X_VC", "a,b"), "publish");
    Check(store.Publish("EDF6X_VC", "a,b"), "same value again");
    Check(rec.published.size() == 1 && rec.published[0].second == "=a,b", "one wire text, value marked");
    Check(rec.watched == std::vector<std::string>{"EDF6X_VC"}, "published key watched");
    Check(!store.Publish("EDF6MS_SEQ", "9"), "Coop key refused");
    Check(store.Clear("EDF6X_VC") && rec.published.back().second == "-", "clear withdraws");
    Check(store.Clear("EDF6X_OTHER") && rec.published.size() == 2, "clear of an unpublished key sends nothing");
    for (int i = 0; i < 15; ++i) store.Publish(("EDF6X_K" + std::to_string(i)).c_str(), "x");
    Check(!store.Publish("EDF6X_ONEMORE", "x"), "key limit");
}

void Snapshots() {
    Store store;
    Recorder rec;
    store.Attach(rec.MakeSinks());
    std::vector<MemberValue> members;
    std::uint64_t g0 = 0, g = 0;
    Check(store.Read("EDF6X_VC", members, g0) && members.empty() && g0 != 0, "outside a room: no members");
    Check(!store.Room().inRoom, "not in a room");
    store.Observe(Room("L1", "B", "A", {{"A", "=v1"}, {"B", ""}, {"C", "-"}, {"D", "junk"}}));
    const RoomFacts facts = store.Room();
    Check(facts.inRoom && !facts.host && facts.members == 4 && facts.owner == "A" && facts.self == "B", "room facts");
    members.clear();
    store.Read("EDF6X_VC", members, g);
    Check(g > g0, "generation moved on entering");
    Check(members.size() == 4 && members[0].id == "A" && members[0].host && members[0].value == std::optional<std::string>("v1"),
          "host's value");
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
    Check(!store.Room().inRoom && store.Room().members == 0 && store.Room().self.empty(), "left the room");
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
    Check(!api->publish("EDF6X_VC", "a"), "publish refused without the lobby glue");
    Recorder rec;
    MemberAttributes().Attach(rec.MakeSinks());
    Check(api->publish("EDF6X_VC", "ab") == 1, "publish through the ABI");
    Check(!api->publish("EDF6X_VC", nullptr) && !api->publish(nullptr, "a"), "null arguments refused");
    EDF6CoopRoomState state{};
    Check(!api->room(&state) && !api->room(nullptr), "room needs size");
    state.size = sizeof(state);
    Check(api->room(&state) && !state.inRoom && state.generation != 0, "room outside a lobby");
    MemberAttributes().Observe(Room("L", "S1", "H1", {{"H1", "=host"}, {"S1", "=ab"}, {"X", ""}}));
    Check(api->room(&state) && state.inRoom && state.memberCount == 3 && !state.isHost &&
              std::string(state.self) == "S1" && std::string(state.host) == "H1",
          "room inside a lobby");
    Seen seen;
    std::uint64_t generation = 0;
    Check(api->snapshot("EDF6X_VC", &Collect, &seen, &generation) == 1 && generation == state.generation,
          "snapshot generation matches room");
    Check(seen.lines == std::vector<std::string>{"H1#0H=host", "S1#1S=ab", "X#2-"}, "snapshot rows");
    Check(!api->snapshot("EDF6CO_HD", &Collect, &seen, nullptr), "snapshot refuses Coop keys");
    Check(!api->snapshot("EDF6X_VC", nullptr, nullptr, nullptr), "snapshot needs a callback");
    Check(api->clear("EDF6X_VC") == 1 && rec.published.back().second == "-", "clear through the ABI");
    seen.lines.clear();
    api->snapshot("EDF6X_VC", &Collect, &seen, nullptr);
    Check(seen.lines[1] == "S1#1S-", "own value withdrawn at once");
}

}  // namespace

int main() {
    KeysAndValues();
    PublishAndClear();
    Snapshots();
    Abi();
    if (failures) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("member attribute tests passed\n");
    return 0;
}
