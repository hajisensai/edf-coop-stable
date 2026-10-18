// The room feed (src/room_view.h): the host's Room message over EOS P2P next to the direct link, stamped so that a
// member that gets it both ways never lets an older copy replace a newer one.
#include "../src/room_view.h"

#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)

std::vector<std::string> stamped(std::vector<std::string> slots, uint64_t epoch, uint64_t seq,
                                 std::set<std::string> removed = {}) {
    std::vector<std::string> out = dn::roomMessage(slots, removed);
    dn::appendRoomStamp(out, {epoch, seq, true});
    return out;
}

// Every page of `message` as it would cross EOS.
std::vector<dn::RoomFeedPage> pagesOf(const std::string& room, const std::vector<std::string>& message, uint64_t epoch,
                                      uint64_t seq) {
    std::vector<dn::RoomFeedPage> out;
    for (const auto& bytes : dn::encodeRoomFeed(room, message, {epoch, seq, true})) {
        dn::RoomFeedPage page;
        CHECK(bytes.size() <= 1170);  // EOS_P2P_MAX_PACKET_SIZE
        CHECK(dn::decodeRoomFeedPage(bytes.data(), bytes.size(), &page));
        out.push_back(page);
    }
    return out;
}

void stamps() {
    // A stamp sits after the removed marker: a reader of an older version sees the same slots and removed members.
    const auto message = stamped({"host", "", "b"}, 1700000000000ull, 3, {"kicked"});
    std::vector<std::string> slots;
    std::set<std::string> removed;
    dn::parseRoomMessage(message, &slots, &removed);
    CHECK(slots == std::vector<std::string>({"host", "", "b"}));
    CHECK(removed == std::set<std::string>{"kicked"});
    CHECK(!dn::parseWorldAdmission(message).present);
    dn::RoomStamp s;
    CHECK(dn::parseRoomStamp(message, &s) && s.present && s.epoch == 1700000000000ull && s.seq == 3);
    // With All Forces' world admission in the same message, both read as before.
    auto world = dn::roomMessage({"host", "b"}, {});
    dn::appendWorldAdmission(world, {dn::WorldPhase::Lobby, 0, {}, true});
    dn::appendRoomStamp(world, {5, 1, true});
    CHECK(dn::freshWorldEntryAllowed(dn::parseWorldAdmission(world)));
    CHECK(dn::parseRoomStamp(world, &s) && s.epoch == 5 && s.seq == 1);
    CHECK(std::count(world.begin(), world.end(), std::string(dn::kRemovedMarker)) == 1);
    dn::RoomView view;
    view.reset("b", {"host", "b"});
    view.heardHost(message, "host");
    CHECK(view.hostSlot("b") == 2 && view.hostSlot("host") == 0 && view.banned("kicked"));
    // No stamp: a host of an older version. Malformed ones are no host's.
    CHECK(dn::parseRoomStamp(dn::roomMessage({"host"}, {}), &s) && !s.present);
    CHECK(!dn::parseRoomStamp({"host", "#removed", "#room-v1:1:1", "#room-v1:1:2"}, &s));
    CHECK(!dn::parseRoomStamp({"host", "#removed", "#room-v1:1"}, &s));
    CHECK(!dn::parseRoomStamp({"host", "#removed", "#room-v1:0:1"}, &s));
    CHECK(!dn::parseRoomStamp({"host", "#removed", "#room-v1:1:2x"}, &s));
    // A slot named like a stamp before the removed marker is a slot, not a stamp.
    CHECK(dn::parseRoomStamp({"#room-v1:1:1"}, &s) && !s.present);
    // Newer: a stamp beats none; in one session the later message; across two, the session heard of later - whatever
    // the numbers say (an epoch is random: 11 heard of before 10 is the older session).
    dn::RoomSessions sessions;
    const dn::RoomStamp none, a{10, 7, true}, b{10, 8, true}, c{11, 1, true};
    CHECK(sessions.newer(a, none) && !sessions.newer(none, a));
    CHECK(sessions.newer(b, a) && !sessions.newer(a, b) && !sessions.newer(a, a));
    sessions.heard(11);
    sessions.heard(10);
    CHECK(sessions.newest() == 10);
    CHECK(sessions.newer(a, c) && !sessions.newer(c, b));
    sessions.heard(11);  // heard of again: it keeps its place
    CHECK(sessions.newest() == 10 && sessions.newer(b, c));
    sessions.heard(0);  // no session
    CHECK(sessions.newest() == 10);
    // Bounded: the oldest falls off.
    for (uint64_t e = 1000; e < 1000 + dn::RoomSessions::kKept; ++e) sessions.heard(e);
    CHECK(sessions.newest() == 1000 + dn::RoomSessions::kKept - 1);
    CHECK(sessions.newer(dn::RoomStamp{1000, 1, true}, a));  // 10 fell off: never heard of, older than any
}

// The host's clock went back between two hosting sessions of the same room (or it drew a lower epoch): its members
// follow the new session from its first message, and an old session's message coming late changes nothing.
void sessionsBackwards() {
    const std::string room = "0123456789abcdef0123456789abcdef";
    std::vector<std::string> out;
    dn::RoomFeedAssembler in;
    const auto first = stamped({"host", "a"}, 900, 7), second = stamped({"host", "a", "b"}, 100, 1);
    CHECK(in.add("host", pagesOf(room, first, 900, 7)[0], &out) && out == first);
    CHECK(in.add("host", pagesOf(room, second, 100, 1)[0], &out) && out == second);
    CHECK(!in.add("host", pagesOf(room, stamped({"host"}, 900, 8), 900, 8)[0], &out));  // the session that is over
    const auto third = stamped({"host", "", "b"}, 100, 2);
    CHECK(in.add("host", pagesOf(room, third, 100, 2)[0], &out) && out == third);

    dn::HostRoomFeed f;
    uint64_t version = 0;
    std::string from;
    f.setOwner("host");
    CHECK(f.eos("host", first));
    CHECK(f.eos("host", second));
    CHECK(f.current(&version, &from) == second);
    CHECK(!f.eos("host", stamped({"host"}, 900, 8)));
    f.direct("host", first);  // the direct link still holds the first session's: EOS has the newer one
    CHECK(f.current(&version, &from) == second);
    f.pending("host", {900, 9, true});  // a page of the session that is over: no newer message partly here
    CHECK(f.current(&version, &from) == second);
    f.direct("host", third);  // the new session over the direct link, ahead of EOS
    CHECK(f.current(&version, &from) == third);
    CHECK(!f.eos("host", second));
}

void pages() {
    // A big room: 40 slots, removed members and All Forces' frozen participants - several packets.
    std::vector<std::string> slots;
    for (int i = 0; i < 40; ++i) slots.push_back(i % 7 == 3 ? std::string() : "member" + std::to_string(i) + std::string(20, 'x'));
    auto message = dn::roomMessage(slots, {"gone1", "gone2"});
    dn::appendWorldAdmission(message, {dn::WorldPhase::Sealed, 9, {"member0" + std::string(20, 'x'), "member1" + std::string(20, 'x')}, true});
    dn::appendRoomStamp(message, {77, 5, true});
    const std::string room = "0123456789abcdef0123456789abcdef";
    auto all = pagesOf(room, message, 77, 5);
    CHECK(all.size() == (message.size() + dn::kRoomFeedPageEntries - 1) / dn::kRoomFeedPageEntries);
    CHECK(all.size() > 2);
    CHECK(all[0].room == room && all[0].total == message.size());
    // In order, and in any order: the same message once its last page is in.
    dn::RoomFeedAssembler in;
    std::vector<std::string> out;
    for (size_t i = 0; i + 1 < all.size(); ++i) CHECK(!in.add("host", all[i], &out));
    CHECK(in.pending("host").present && in.pending("host").seq == 5);
    CHECK(in.add("host", all.back(), &out) && out == message);
    CHECK(!in.pending("host").present);
    CHECK(!in.add("host", all[0], &out));  // that message is whole already: a copy of a page does nothing
    dn::RoomFeedAssembler reversed;
    for (size_t i = all.size(); i-- > 1;) CHECK(!reversed.add("host", all[i], &out));
    CHECK(reversed.add("host", all[0], &out) && out == message);

    // A newer message begun drops the older one's pages; a page of an older one coming late is dropped.
    auto newer = message;
    newer[1] = "someone";
    newer.back() = "#room-v1:77:6";
    const auto next = pagesOf(room, newer, 77, 6);
    dn::RoomFeedAssembler race;
    CHECK(!race.add("host", all[0], &out));
    CHECK(!race.add("host", next[0], &out));
    for (size_t i = 1; i < all.size(); ++i) CHECK(!race.add("host", all[i], &out));  // seq 5 is moot now
    for (size_t i = 1; i < next.size(); ++i) {
        const bool whole = race.add("host", next[i], &out);
        CHECK(whole == (i + 1 == next.size()));
    }
    CHECK(out == newer);
    for (const auto& page : all) CHECK(!race.add("host", page, &out));  // older than the newest whole one

    // Pages and message must agree on the stamp; malformed packets are not decoded.
    auto lying = pagesOf(room, stamped({"host"}, 3, 9), 3, 8);  // header says 8, the message says 9
    dn::RoomFeedAssembler strict;
    CHECK(lying.size() == 1 && !strict.add("host", lying[0], &out));
    const auto bytes = dn::encodeRoomFeed(room, stamped({"host"}, 3, 1), {3, 1, true});
    dn::RoomFeedPage page;
    CHECK(bytes.size() == 1 && dn::decodeRoomFeedPage(bytes[0].data(), bytes[0].size(), &page));
    CHECK(!dn::decodeRoomFeedPage(bytes[0].data(), bytes[0].size() - 1, &page));  // cut short
    auto extra = bytes[0];
    extra.push_back(0);
    CHECK(!dn::decodeRoomFeedPage(extra.data(), extra.size(), &page));  // trailing bytes
    auto magic = bytes[0];
    magic[0] = 'X';
    CHECK(!dn::decodeRoomFeedPage(magic.data(), magic.size(), &page));
    auto format = bytes[0];
    format[4] = 2;
    CHECK(!dn::decodeRoomFeedPage(format.data(), format.size(), &page));  // a later format: not ours to read
    CHECK(dn::encodeRoomFeed(room, {}, {1, 1, true}).empty());
    CHECK(dn::encodeRoomFeed(room, stamped({"host"}, 1, 1), {}).empty());
    CHECK(dn::encodeRoomFeed(room, {std::string(65, 'x')}, {1, 1, true}).empty());  // no EOS id is that long
}

void feed() {
    using V = dn::RoomVia;
    uint64_t version = 0, seen = 0;
    std::string from;
    V via = V::None;

    // The player's room: its host hosts no direct link. The slots come over EOS alone.
    {
        dn::HostRoomFeed f;
        f.setOwner("host");
        f.direct("", {});
        CHECK(f.current(&version, &from).empty());
        CHECK(f.eos("host", stamped({"host", "guest"}, 100, 1)));
        CHECK(f.current(&version, &from, &via) == stamped({"host", "guest"}, 100, 1) && from == "host" && via == V::Eos);
        CHECK(!f.eos("host", stamped({"host", "guest"}, 100, 1)));  // the same message again changes nothing
        CHECK(!f.eos("host", dn::roomMessage({"host"}, {})));        // nothing comes over EOS without a stamp
    }

    // A host of an older version: its unstamped list over the direct link is followed as before.
    {
        dn::HostRoomFeed f;
        f.setOwner("host");
        f.direct("host", {"host", "guest"});
        CHECK(f.current(&version, &from, &via) == std::vector<std::string>({"host", "guest"}) && via == V::Direct);
        seen = version;
        f.direct("host", {"host", "guest"});
        f.current(&version, &from);
        CHECK(version == seen);  // nothing new: the game is not told anything again
        f.direct("host", {});    // the link went down: nothing usable from it
        CHECK(f.current(&version, &from).empty() && version != seen);
    }

    // Both paths, each with the same stamped messages, arriving in every order: the newer one always wins.
    {
        dn::HostRoomFeed f;
        f.setOwner("host");
        const auto m4 = stamped({"host", "a"}, 100, 4), m5 = stamped({"host", "a", "b"}, 100, 5),
                   m6 = stamped({"host", "", "b"}, 100, 6);
        CHECK(f.eos("host", m5));
        f.direct("host", m4);  // the slower path still holds the older one
        CHECK(f.current(&version, &from, &via) == m5 && via == V::Eos);
        f.direct("host", m5);  // the same message over the other path
        seen = version;
        CHECK(f.current(&version, &from) == m5 && version == seen);
        f.direct("host", m6);  // the direct link is ahead now
        CHECK(f.current(&version, &from, &via) == m6 && via == V::Direct);
        CHECK(!f.eos("host", m4));  // late and older: dropped
        CHECK(!f.eos("host", stamped({"host", "a"}, 100, 3)));  // an earlier message of the session: older
        CHECK(f.current(&version, &from) == m6);
        f.direct("host", {});  // the link drops: EOS's newest (5) is all there is, and it is a real host message
        CHECK(f.current(&version, &from, &via) == m5 && via == V::Eos);
        CHECK(f.eos("host", m6));
        CHECK(f.current(&version, &from, &via) == m6 && via == V::Eos);
        // A later hosting session of the same host (it hosted the room again): newer whatever its message number.
        const auto again = stamped({"", "host", "b"}, 101, 1);
        CHECK(f.eos("host", again));
        f.direct("host", m6);
        CHECK(f.current(&version, &from) == again);
        CHECK(!f.eos("host", stamped({"host", "a"}, 100, 7)));  // the session before is over, whatever the number
        // An unstamped list over the direct link never replaces a stamped one.
        f.direct("host", {"host", "x"});
        CHECK(f.current(&version, &from) == again);
    }

    // A newer message partly here over EOS: the older one is superseded, nothing is followed until it is whole.
    {
        dn::HostRoomFeed f;
        f.setOwner("host");
        CHECK(f.eos("host", stamped({"host", "a"}, 100, 1)));
        f.pending("host", {100, 2, true});
        CHECK(f.current(&version, &from).empty());
        f.pending("host", {100, 1, true});  // not newer than what is whole: changes nothing
        CHECK(f.current(&version, &from).empty());
        CHECK(f.eos("host", stamped({"host", "a", "b"}, 100, 2)));
        CHECK(f.current(&version, &from) == stamped({"host", "a", "b"}, 100, 2));
        // The direct link brought that newer message whole already: it is followed.
        f.pending("host", {100, 3, true});
        f.direct("host", stamped({"host", "", "b"}, 100, 3));
        CHECK(f.current(&version, &from) == stamped({"host", "", "b"}, 100, 3));
    }

    // Only the room's owner is followed. A member that becomes the owner may have sent its slots before we knew: they
    // are kept, and followed from the moment it owns the room.
    {
        dn::HostRoomFeed f;
        f.setOwner("old");
        CHECK(f.eos("new", stamped({"", "new", "b"}, 200, 1)));
        f.direct("new", stamped({"", "new", "b"}, 200, 1));
        CHECK(f.current(&version, &from).empty());
        CHECK(f.eos("old", stamped({"old", "new", "b"}, 100, 3)));
        CHECK(f.current(&version, &from) == stamped({"old", "new", "b"}, 100, 3) && from == "old");
        f.setOwner("new");
        CHECK(f.current(&version, &from) == stamped({"", "new", "b"}, 200, 1) && from == "new");
        // Owner not known (a room Epic does not know we are in): the direct link's host is the room's.
        f.setOwner("");
        f.direct("virtual", {"virtual", "me"});
        CHECK(f.current(&version, &from) == std::vector<std::string>({"virtual", "me"}) && from == "virtual");
    }
}
}  // namespace

int main() {
    stamps();
    pages();
    sessionsBackwards();
    feed();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
