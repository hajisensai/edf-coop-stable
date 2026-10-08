#include "../src/direct_net.h"
#include "../src/room_view.h"
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
bool waitFor(const std::function<bool()>& predicate) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    do { if (predicate()) return true; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    while (std::chrono::steady_clock::now() < end);
    return false;
}
std::vector<std::string> message(dn::WorldAdmission world) {
    auto out = dn::roomMessage({"host", "participant", "fresh"}, {"kicked"});
    dn::appendWorldAdmission(out, world);
    return out;
}
}

int main() {
    using P = dn::WorldPhase;
    using O = dn::ParkedEntryOutcome;
    const dn::WorldAdmission lobby{P::Lobby, 8, {}, true};
    const dn::WorldAdmission sealed{P::Sealed, 7, {"host", "participant"}, true};
    auto parsed = dn::parseWorldAdmission(message(sealed));
    CHECK(parsed.present && parsed.epoch == 7 && parsed.phase == P::Sealed && parsed.participants == sealed.participants);
    std::vector<std::string> slots;
    std::set<std::string> removed;
    dn::parseRoomMessage(message(sealed), &slots, &removed);
    CHECK(slots == std::vector<std::string>({"host", "participant", "fresh"}));
    CHECK(removed == std::set<std::string>{"kicked"});
    CHECK(!dn::freshWorldEntryAllowed(sealed)); // same-PUID fresh reconnect cannot reconstruct old support state
    CHECK(dn::freshWorldEntryAllowed(dn::parseWorldAdmission(message(lobby))));
    CHECK(!dn::freshWorldEntryAllowed({}));
    CHECK(!dn::parseWorldAdmission({"#removed", "#world-v1:1:1", "#world-member:host"}).present);
    CHECK(!dn::parseWorldAdmission({"#removed", "#world-v1:1:1", "#world-v1:1:2"}).present);
    CHECK(!dn::parseWorldAdmission({"#removed", "#world-v1:1:1junk"}).present);
    CHECK(!dn::parseWorldAdmission({"#removed", "#world-v1:3:0"}).present);
    CHECK(!dn::parseWorldAdmission({"#removed", "#world-v1:3:7", "#world-member:host", "#world-member:host"}).present);
    CHECK(dn::decideParkedWorldEntry(900000, true, false, true, 0, true, sealed) == O::Wait);
    CHECK(dn::decideParkedWorldEntry(900000, true, false, true, 0, true, lobby) == O::Slotted);
    CHECK(dn::decideParkedWorldEntry(900000, true, false, false, 50000, true, {}) == O::GiveUp);
    CHECK(dn::decideParkedWorldEntry(100, true, false, true, 0, true, {}) == O::Wait);
    CHECK(dn::decideParkedWorldEntry(100, true, false, true, 0, false, {}) == O::Slotted);
    dn::RoomView view;
    view.reset("fresh", {"host", "fresh"});
    view.heardHost(message(lobby), "host");
    CHECK(dn::freshWorldEntryAllowed(view.hostWorld()));
    view.promoted("next-host", 10);
    CHECK(!dn::freshWorldEntryAllowed(view.hostWorld()));
    view.heardHost(message(lobby), "host");
    CHECK(!dn::freshWorldEntryAllowed(view.hostWorld()));
    view.heardHost(message(sealed), "next-host");
    CHECK(view.hostWorld().phase == P::Sealed);

    dn::ListPages pages;
    std::vector<std::string> assembled;
    CHECK(!dn::applyListPage(pages, 2, 4, 2, {"c", "d"}, assembled)); // final page arrives first
    CHECK(!dn::currentSinglePage(pages, 1)); // stale one-page Lobby must not bypass an in-flight sealed roster
    CHECK(!dn::applyListPage(pages, 1, 4, 0, {"old", "old"}, assembled));
    CHECK(dn::applyListPage(pages, 2, 4, 0, {"a", "b"}, assembled));
    CHECK(assembled == std::vector<std::string>({"a", "b", "c", "d"}));
    CHECK(!dn::applyListPage(pages, 2, 4, 0, {"old", "old"}, assembled));
    CHECK(!dn::applyListPage(pages, 3, 4, 0, {"a", "b", "c"}, assembled));
    CHECK(!dn::applyListPage(pages, 3, 4, 2, {"overlap", "d"}, assembled)); // sum == 5 used to falsely commit
    CHECK(pages.applied == 2);
    pages = {};
    CHECK(!dn::applyListPage(pages, 1, 4, 0, {"a"}, assembled));
    CHECK(!dn::applyListPage(pages, 1, 4, 2, {"c", "d"}, assembled));
    CHECK(pages.applied == 0); // missing offset 1 cannot authorize entry
    CHECK(dn::applyListPage(pages, 1, 4, 1, {"b"}, assembled));
    CHECK(!dn::applyListPage(pages, 2, 65000, 0, {"a"}, assembled));

    // Real authenticated UDP Room messages reach a lobby-only link without any extension participant/ready state.
    dn::DirectNet host, fresh;
    dn::DirectOptions ho;
    ho.mode = dn::Mode::Host; ho.listenPort = 0; ho.pingIntervalMs = 100;
    ho.identity = dn::Identity::generate();
    auto identity = dn::Identity::generate();
    ho.memberIds = {{"fresh", identity->commitment()}};
    ho.testDropRate = 0.2;
    CHECK(host.start(ho)); host.setLocalUser("host");
    dn::WorldAdmission large = sealed;
    large.participants.push_back("fresh"); // even a participant PUID reconnecting with no world must wait
    for (int i = 0; i < 70; ++i) large.participants.push_back("other" + std::to_string(i));
    host.setRoomMembers(message(large));
    dn::DirectOptions jo;
    jo.mode = dn::Mode::Join; jo.listenPort = 0; jo.pingIntervalMs = 100;
    jo.identity = identity; jo.roomOwner = "host"; jo.roomOwnerIdentity = ho.identity->commitment();
    jo.hostAddress = "127.0.0.1:" + std::to_string(host.boundPort());
    CHECK(fresh.start(jo)); fresh.setLocalUser("fresh");
    uint64_t version = 0;
    auto read = [&] { return dn::parseWorldAdmission(fresh.hostRoom(&version)); };
    CHECK(waitFor([&] { return read().phase == P::Sealed; }));
    CHECK(read().participants.size() == large.participants.size());
    CHECK(!dn::freshWorldEntryAllowed(read()));
    EDF6CoopSnapshot extension{}; fresh.extensionSnapshot(extension);
    CHECK(!extension.ready); // admission is deliberately independent of spawn-packet readiness
    host.setRoomMembers(message(lobby));
    CHECK(waitFor([&] { return dn::freshWorldEntryAllowed(read()); }));
    // A different owner invalidates even a previously allowed Lobby manifest immediately.
    fresh.setRoomOwner("next-host", ho.identity->commitment());
    CHECK(fresh.hostRoom(&version).empty());
    fresh.setRoomOwner("host", ho.identity->commitment());
    fresh.setActive(false);
    CHECK(fresh.hostRoom(&version).empty());
    host.setRoomMembers(message(sealed));
    fresh.setActive(true); // fresh process/session with same PUID: must get a new host snapshot
    CHECK(!dn::freshWorldEntryAllowed(read()));
    CHECK(waitFor([&] { return read().phase == P::Sealed; }));
    CHECK(!dn::freshWorldEntryAllowed(read()));
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
