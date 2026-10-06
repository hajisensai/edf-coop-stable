// The netcode version gate (netfeature.h): which features are on, from what every member publishes.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../src/netfeature.h"

using namespace multislot;

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

const std::string kHost = "host0000000000000000000000000001";
const std::string kGuest = "guest000000000000000000000000002";
const std::string kOther = "guest000000000000000000000000003";

LobbyView::Member Member(const std::string& id, const char* protocol, const char* caps) {
    LobbyView::Member m;
    m.id = id;
    if (protocol) m.texts[kNetProtocolKey] = protocol;
    if (caps) m.texts[kNetCapsKey] = caps;
    return m;
}

LobbyView View(const std::string& self, std::vector<LobbyView::Member> members) {
    LobbyView view;
    view.lobbyId = "lobby1";
    view.self = self;
    view.owner = kHost;
    view.members = std::move(members);
    return view;
}

const std::uint32_t kMesh = static_cast<std::uint32_t>(NetFeature::Mesh);
const std::uint32_t kFragments = static_cast<std::uint32_t>(NetFeature::Fragments);
const std::string kProto = std::to_string(kNetProtocol);

void TestRoomAgrees() {
    NetRoom room;
    Check(!room.Active(kMesh), "outside a room nothing is on");
    room.Observe(View(kGuest, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, kProto.c_str(), "7")}));
    Check(room.InRoom() && !room.Owner(), "a guest in the room");
    Check(room.Active(kMesh) && room.Active(kMesh | kFragments), "everyone runs protocol and features: on");
    // A member that turned a feature off keeps that one off for the room, the others stay on.
    room.Observe(View(kGuest, {Member(kHost, kProto.c_str(), "3"), Member(kGuest, kProto.c_str(), "7")}));
    Check(room.Active(kMesh) && !room.Active(kFragments), "a feature off at one member is off for the room");
    Check(room.WhyOff(kFragments).find(kHost) != std::string::npos, "the log names who keeps it off");
}

void TestUnpublishedMember() {
    NetRoom room;
    // The game as it ships (or an older EDF6Coop): no attributes. Not refused - everyone plays the old netcode.
    const auto fresh = room.Observe(View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, nullptr, nullptr)}));
    Check(fresh.empty(), "a member without the attributes is not refused");
    Check(!room.Active(kMesh), "a member without the attributes turns every feature off");
    Check(room.WhyOff(kMesh).find("publishes no netcode protocol") != std::string::npos, "and the log says why");
    // Its attributes arrive (Epic relays them late): on.
    room.Observe(View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, kProto.c_str(), "7")}));
    Check(room.Active(kMesh), "once its attributes arrive the features come on");
}

void TestMismatchedProtocol() {
    NetRoom room;
    const std::string other = std::to_string(kNetProtocol + 1);
    auto fresh = room.Observe(View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, kProto.c_str(), "7"),
                                           Member(kOther, other.c_str(), "7")}));
    Check(room.Owner(), "the host owns the room");
    Check(fresh.size() == 1 && fresh[0].id == kOther && fresh[0].protocol == kNetProtocol + 1,
          "a member of another protocol is reported to refuse");
    Check(!room.Active(kMesh), "while it is in the room nothing is on");
    fresh = room.Observe(View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, kProto.c_str(), "7"),
                                      Member(kOther, other.c_str(), "7")}));
    Check(fresh.empty(), "reported once per room");
    // It was removed: on again.
    room.Observe(View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kGuest, kProto.c_str(), "7")}));
    Check(room.Active(kMesh), "once it is gone the features come back");
    // Left the room: off, and a new room reports again.
    room.Observe(LobbyView{});
    Check(!room.InRoom() && !room.Active(kMesh), "out of the room: off");
    auto view = View(kHost, {Member(kHost, kProto.c_str(), "7"), Member(kOther, other.c_str(), "7")});
    view.lobbyId = "lobby2";
    Check(room.Observe(view).size() == 1, "another room reports it again");
}

void TestSelfListed() {
    NetRoom room;
    room.Observe(View(kGuest, {Member(kHost, kProto.c_str(), "7")}));
    Check(!room.Active(kMesh), "until our own entry is listed the room is not known");
}

void TestCaps() {
    Check(ParseCaps("7") == 7 && ParseCaps("700") == 0x700 && ParseCaps("") == 0 && ParseCaps("x1") == 0, "caps parse as hex");
    Check(FormatCaps(7) == "TrafficClasses,Mesh,Fragments" && FormatCaps(0) == "none" &&
              FormatCaps(0x10007) == "TrafficClasses,Mesh,Fragments,0x10000",
          "caps format by name");
    Check(!NetFeatureActive(NetFeature::Mesh), "not started: NetFeatureActive is false");
}

}  // namespace

int main() {
    TestRoomAgrees();
    TestUnpublishedMember();
    TestMismatchedProtocol();
    TestSelfListed();
    TestCaps();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
