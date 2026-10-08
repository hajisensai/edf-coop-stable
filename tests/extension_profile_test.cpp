#include "../src/direct_net.h"
#include "../src/lobby_marker.h"
#include "../src/extension_bridge.h"
#include "../src/extension_room.h"
#include "../src/room_view.h"
#include <cstdio>
#include <string>
#include <chrono>
#include <thread>
namespace {
int failures = 0, checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
template<typename T> T function(HMODULE module, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(module, name));
}
}
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    HMODULE eos = LoadLibraryA(argv[1]);
    if (!eos) return 2;
    const auto reset = function<void(*)(const char*)>(eos, "FakeEos_Reset");
    const auto lobby = function<void(*)(const char*, const char*)>(eos, "FakeEos_EnterLobby");
    const auto user = function<EOS_ProductUserId(*)(const char*)>(eos, "FakeEos_User");
    const auto tick = function<void(*)(void*)>(eos, "EOS_Platform_Tick");
    const auto text = function<const char*(*)(const char*, const char*)>(eos, "FakeEos_Text");
    const auto setText = function<void(*)(const char*, const char*, const char*)>(eos, "FakeEos_SetText");
    const auto clear = function<void(*)(const char*)>(eos, "FakeEos_ClearTexts");
    const auto add = function<void(*)(const char*)>(eos, "FakeEos_AddMember");
    const auto remove = function<void(*)(const char*)>(eos, "FakeEos_RemoveMember");
    const auto owner = function<void(*)(const char*)>(eos, "FakeEos_SetOwner");
    const auto copyFails = function<void(*)(int)>(eos, "FakeEos_SetCopyFails");
    if (!reset || !lobby || !user || !tick || !text || !setText || !clear || !add || !remove || !owner || !copyFails) return 2;
    reset("self"); lobby("room", "self");
    dn::LobbyMarker marker;
    CHECK(marker.init(eos));
    marker.entered(reinterpret_cast<EOS_HLobby>(1), "room", user("self"), true);
    marker.setIdentity("0123456789abcdef0123456789abcdef");
    marker.tick(); tick(nullptr);
    auto published = [&](const char* key) { const char* value = text("self", key); return value ? std::string(value) : ""; };
    // EOS rejects empty text attributes atomically: disabled extensions must not
    // break vanilla Coop's identity/host-address publication.
    CHECK(published("EDF6DN_EXT") == "disabled");
    CHECK(published("EDF6DN_ID") == "0123456789abcdef0123456789abcdef");
    CHECK(!marker.extensionCompatible());
    uint64_t worldEpoch = 0;
    std::vector<std::string> participants;
    CHECK(!dn::readMissionParticipants(worldEpoch, participants));
    HMODULE af = LoadLibraryA(argv[2]);
    if (!af) return 2;
    const auto ready = function<void(*)(bool)>(af, "FakeAF_SetReady");
    const auto version = function<void(*)(unsigned)>(af, "FakeAF_SetVersion");
    const auto mission = function<void(*)(unsigned)>(af, "FakeAF_SetMission");
    if (!ready || !version || !mission) return 2;
    CHECK(!dn::readMissionParticipants(worldEpoch, participants)); // patched native gate mandatory
    dn::setExtensionMissionGateReady(true);
    CHECK(dn::readMissionParticipants(worldEpoch, participants) && worldEpoch == 7 &&
        participants == std::vector<std::string>({"peer", "self"}));
    for (unsigned malformed = 1; malformed <= 6; ++malformed) {
        mission(malformed);
        CHECK(!dn::readMissionParticipants(worldEpoch, participants) && worldEpoch == 0 && participants.empty());
    }
    mission(0);
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(published("EDF6DN_EXT") == "af-support/2");
    CHECK(marker.extensionCompatible());
    version(3); CHECK(!marker.extensionCompatible()); version(2);
    add("peer");
    CHECK(!marker.extensionCompatible()); // older Coop/absent extension
    CHECK(marker.extensionCompatible({"self"})); // lobby-only peer is not part of this mission
    CHECK(!marker.extensionCompatible({"self", "absent"}));
    setText("peer", "EDF6DN_EXT", "af-support/3");
    CHECK(!marker.extensionCompatible());
    setText("peer", "EDF6DN_EXT", "af-support/1");
    CHECK(!marker.extensionCompatible());
    setText("peer", "EDF6DN_EXT", "af-support/2");
    CHECK(marker.extensionCompatible());
    clear("peer");
    CHECK(!marker.extensionCompatible()); // no cached capability after it disappears
    setText("peer", "EDF6DN_EXT", "af-support/2");
    copyFails(1); CHECK(!marker.extensionCompatible()); copyFails(0);
    ready(false); CHECK(!marker.extensionCompatible());
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(published("EDF6DN_EXT") == "disabled");
    ready(true); CHECK(!marker.extensionCompatible()); // own current marker still disabled
    marker.memberJoined(); marker.tick(); tick(nullptr);
    CHECK(marker.extensionCompatible());

    // Drive the actual EOS-tick publisher, not a hand-fed DirectNet room snapshot.
    // AF supplies a real atomic ABI snapshot; marker reads the fake SDK; live
    // routes use independent signed identities and production UDP handshakes.
    auto net = std::make_shared<dn::DirectNet>();
    dn::DirectNet remote;
    dn::DirectOptions hostOptions;
    hostOptions.mode = dn::Mode::Host; hostOptions.listenPort = 0;
    hostOptions.identity = dn::Identity::generate();
    auto peerIdentity = dn::Identity::generate();
    hostOptions.memberIds = {{"peer", peerIdentity->commitment()}};
    CHECK(net->start(hostOptions)); net->setLocalUser("self");
    dn::DirectOptions joinOptions;
    joinOptions.mode = dn::Mode::Join; joinOptions.listenPort = 0;
    joinOptions.hostAddress = "127.0.0.1:" + std::to_string(net->boundPort());
    joinOptions.roomOwner = "self"; joinOptions.roomOwnerIdentity = hostOptions.identity->commitment();
    joinOptions.identity = peerIdentity;
    CHECK(remote.start(joinOptions)); remote.setLocalUser("peer");
    dn::RoomView view; view.reset("self", {"self", "peer"});
    auto publish = [&] {
        dn::publishExtensionRoom(net, marker, view, "room", false);
        EDF6CoopSnapshot state{}; net->extensionSnapshot(state); return state;
    };
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!publish().ready && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto stable = publish();
    CHECK(stable.ready && stable.peerCount == 1);
    add("lobby-only"); view.admit("lobby-only", dn::kJoined);
    CHECK(!view.awaitingHost());
    auto after = publish();
    CHECK(after.ready && after.generation == stable.generation && after.peerCount == 1);
    setText("lobby-only", "EDF6DN_EXT", "af-support/1");
    after = publish(); CHECK(after.ready && after.generation == stable.generation);
    // A missing advertised address does not mean that the room owner changed.
    setText("self", "EDF6DN_ADDR", "");
    after = publish(); CHECK(after.ready && after.generation == stable.generation);
    dn::invalidateExtensionParticipant("lobby-only");
    after = publish(); CHECK(after.ready && after.generation == stable.generation);
    remove("lobby-only"); view.admit("lobby-only", dn::kLeft);
    after = publish(); CHECK(after.ready && after.generation == stable.generation);
    clear("peer");
    CHECK(!publish().ready); // actual participant loses its capability: closed
    setText("peer", "EDF6DN_EXT", "af-support/2"); CHECK(publish().ready);
    owner("peer"); view.promoted("peer", GetTickCount64());
    CHECK(!publish().ready); // genuine host migration is not an ignored lobby change
    dn::invalidateExtensionTransport(); remote.stop(); net->stop();
    marker.left(); CHECK(!marker.extensionCompatible());
    FreeLibrary(af);
    if (argc == 4) {
        const HMODULE legacy = LoadLibraryA(argv[3]);
        CHECK(legacy != nullptr);
        if (legacy) {
            marker.entered(reinterpret_cast<EOS_HLobby>(1), "room", user("self"), true);
            marker.tick(); tick(nullptr);
            // Protocol 2 without the successful-creation observer is incompatible.
            CHECK(published("EDF6DN_EXT") == "disabled" && !marker.extensionCompatible());
            FreeLibrary(legacy);
        }
    }
    FreeLibrary(eos);
    printf("extension profile: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
