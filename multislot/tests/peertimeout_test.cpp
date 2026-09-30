// The decision behind KeepRoomOnPeerTimeout (peertimeout.h), on fake rooms laid out like the game's: a RoomImpl
// with its eos::Users and P2P link manager, the manager's list of links, links with their weak_ptr<eos::User>,
// deadline and timed-out flag. No game needed; the offsets are tied to EDF.dll by PatchTablesMatchEDF.
//   PeerTimeoutTests log-file
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

#include "../src/log.h"
#include "../src/peertimeout.h"

using namespace multislot;

namespace {

int failures = 0;
void Check(bool ok, const char* label) {
    if (!ok) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

template <typename T>
void Put(void* data, std::size_t offset, T value) {
    std::memcpy(static_cast<unsigned char*>(data) + offset, &value, sizeof(value));
}
std::uintptr_t Address(const void* p) { return reinterpret_cast<std::uintptr_t>(p); }

constexpr std::size_t kUsers = 6;

// One room: users[0] is this machine, users[1..] the others; one link per other user, in the manager's list.
struct FakeRoom {
    alignas(16) unsigned char room[0x170]{};
    alignas(16) unsigned char usersObject[0x20]{};
    alignas(16) unsigned char manager[0xE0]{};
    alignas(16) unsigned char sentinel[0x40]{};
    alignas(16) std::array<std::array<unsigned char, 0x40>, kUsers> nodes{};
    alignas(16) std::array<std::array<unsigned char, 0xB0>, kUsers> links{};
    alignas(16) std::array<std::array<unsigned char, 0xA0>, kUsers> users{};
    alignas(16) std::array<std::array<unsigned char, 0x10>, kUsers> controls{};

    FakeRoom() {
        Put(room, kRoomUsersOffset, Address(usersObject));
        Put(usersObject, kUsersLocalOffset, Address(users[0].data()));
        Put(room, kRoomLinksOwnerOffset, Address(manager));
        Put(manager, kLinkListOffset, Address(sentinel));
        // sentinel -> node[1] -> ... -> node[kUsers-1] -> sentinel
        Put(sentinel, 0, Address(nodes[1].data()));
        for (std::size_t i = 1; i < kUsers; ++i) {
            Put(nodes[i].data(), 0, i + 1 < kUsers ? Address(nodes[i + 1].data()) : Address(sentinel));
            Put(nodes[i].data(), kLinkNodeValueOffset, Address(links[i].data()));
            Put(links[i].data(), kLinkUserOffset, Address(users[i].data()));
            Put(links[i].data(), kLinkUserOffset + 8, Address(controls[i].data()));
            Put(links[i].data(), kLinkDeadlineOffset, 20000.0f);
            Put(controls[i].data(), 8, std::int32_t{1});
        }
        for (std::size_t i = 0; i < kUsers; ++i) Put(users[i].data(), kUserProductIdOffset, std::uintptr_t{0x1000 + i});
    }
    const void* User(std::size_t i) const { return users[i].data(); }
    void TimedOut(std::size_t i, bool on = true) { links[i][kLinkTimedOutOffset] = on ? 1 : 0; }
    void Gone(std::size_t i) { Put(controls[i].data(), 8, std::int32_t{0}); }
};

constexpr std::uint64_t kJoined = 1'000'000;      // this machine joined the room
constexpr std::uint64_t kMinute = 60'000;

// Everyone who was there when this machine joined comes in with it; `late` joined `after` ms later.
void Joins(const FakeRoom& room, std::size_t late, std::uint64_t after) {
    ForgetPeerJoins();
    NotePeerJoined(room.User(0), true, kJoined);
    for (std::size_t i = 1; i < kUsers; ++i) NotePeerJoined(room.User(i), false, i == late ? kJoined + after : kJoined + i);
}

std::string ReadText(const wchar_t* path) {
    LogFlush();
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t Count(const std::string& text, const char* needle) {
    std::size_t n = 0;
    for (auto at = text.find(needle); at != std::string::npos; at = text.find(needle, at + 1)) ++n;
    return n;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;

    // The 2026-09-30 room: a newcomer (user 5) joined a room this machine had been in for 15 minutes, and its
    // P2P link never came up. Everyone else is connected.
    {
        FakeRoom room;
        Joins(room, 5, 15 * kMinute);
        room.TimedOut(5);
        const auto decision = DecidePeerTimeout(room.room);
        Check(decision.verdict == PeerTimeoutVerdict::Stay, "an established member stays when a newcomer's handshake times out");
        Check(decision.timedOut == 1 && decision.peer == Address(room.User(5)) && decision.productId == 0x1005 &&
                  decision.joinKnown && decision.joinedAfterMs == static_cast<std::int64_t>(15 * kMinute) &&
                  decision.deadlineMs == 20000.0f,
              "the decision names the newcomer, when it joined and the link's deadline");
    }
    // The newcomer's own side: everyone it links to was there before it (added with it, within milliseconds).
    {
        FakeRoom room;
        Joins(room, 0, 0);
        room.TimedOut(3);
        const auto decision = DecidePeerTimeout(room.room);
        Check(decision.verdict == PeerTimeoutVerdict::Leave, "a newcomer whose own join failed leaves, as in the game");
        Check(decision.peer == Address(room.User(3)) && decision.joinKnown && decision.joinedAfterMs == 3,
              "and the decision names the member it could not reach");
    }
    // Two people joining close together: neither had finished its own join when the other came.
    {
        FakeRoom room;
        Joins(room, 2, 5000);
        room.TimedOut(2);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave,
              "someone who joined 5 s after this machine is not enough: this machine's own join window was still open");
    }
    // The boundary is the link's own deadline.
    {
        FakeRoom room;
        Joins(room, 4, 20000);
        room.TimedOut(4);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Stay, "joined exactly one deadline later: stay");
        Joins(room, 4, 19999);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "joined 1 ms short of the deadline: leave");
        Put(room.links[4].data(), kLinkDeadlineOffset, 30000.0f);
        Joins(room, 4, 25000);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "the deadline is read from the link");
        Put(room.links[4].data(), kLinkDeadlineOffset, 0.0f);
        Joins(room, 4, 19999);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave &&
                  DecidePeerTimeout(room.room).deadlineMs == 20000.0f,
              "a link without a sensible deadline falls back to the game's 20 s");
    }
    // Every timed-out link counts: one newcomer and one member this machine joined with.
    {
        FakeRoom room;
        Joins(room, 5, 15 * kMinute);
        room.TimedOut(5);
        room.TimedOut(1);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave,
              "a timed-out link to someone this machine joined with still makes it leave");
    }
    // Nothing known means the game's answer.
    {
        FakeRoom room;
        room.TimedOut(5);
        ForgetPeerJoins();
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "unknown join times: leave");
        NotePeerJoined(room.User(0), true, kJoined);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "unknown peer join time: leave");
        ForgetPeerJoins();
        NotePeerJoined(room.User(5), false, kJoined + 15 * kMinute);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "unknown local join time: leave");
        // A remote user recorded at the local user's address is not the local user's join.
        ForgetPeerJoins();
        NotePeerJoined(room.User(0), false, kJoined);
        NotePeerJoined(room.User(5), false, kJoined + 15 * kMinute);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "only a local add counts as this machine's join");
    }
    // A long evening: many people came and went after this machine joined; its own join time is kept.
    {
        FakeRoom room;
        Joins(room, 5, 15 * kMinute);
        for (int i = 0; i < 500; ++i) NotePeerJoined(reinterpret_cast<const void*>(std::uintptr_t{0x100000} + 0x100 * i), false, kJoined + i);
        NotePeerJoined(room.User(5), false, kJoined + 15 * kMinute);
        room.TimedOut(5);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Stay, "the local join survives 500 later joins");
        // A user object reused at the same address is a new join, and its newest time is what counts.
        NotePeerJoined(room.User(5), false, kJoined + 1);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "the newest join of an address counts");
    }
    // Links whose user is already gone (the lobby removed them; their link goes in the same call).
    {
        FakeRoom room;
        Joins(room, 0, 0);
        room.TimedOut(2);
        room.Gone(2);
        const auto decision = DecidePeerTimeout(room.room);
        Check(decision.verdict == PeerTimeoutVerdict::Stay && decision.timedOut == 0,
              "a timed-out link to someone already gone is no reason to leave");
        room.TimedOut(3);
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "but a live one still is");
    }
    // Memory that does not read like a room.
    {
        FakeRoom room;
        Joins(room, 5, 15 * kMinute);
        room.TimedOut(5);
        Put(room.nodes[kUsers - 1].data(), 0, Address(room.nodes[1].data()));  // a list that never returns to its sentinel
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "a list without an end is not trusted");
        Put(room.nodes[kUsers - 1].data(), 0, std::uintptr_t{0});
        Check(DecidePeerTimeout(room.room).verdict == PeerTimeoutVerdict::Leave, "a null node is not trusted");
        Check(DecidePeerTimeout(nullptr).verdict == PeerTimeoutVerdict::Leave, "no room: leave");
        auto* inaccessible = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
        Check(inaccessible && DecidePeerTimeout(inaccessible).verdict == PeerTimeoutVerdict::Leave,
              "an unreadable room is refused without crashing");
        FakeRoom other;
        Put(other.room, kRoomLinksOwnerOffset, Address(inaccessible));
        Check(DecidePeerTimeout(other.room).verdict == PeerTimeoutVerdict::Leave, "an unreadable manager too");
        if (inaccessible) VirtualFree(inaccessible, 0, MEM_RELEASE);
    }
    // Users::Add's hook records joins from the registers the game has at 12B80E0.
    {
        FakeRoom room;
        ForgetPeerJoins();
        std::uintptr_t made[2] = {Address(room.User(0)), 0};   // shared_ptr<eos::User> as 12B7610 returns it
        unsigned char argument[16]{};                            // {EOS_ProductUserId, bool remote}
        CpuContext context{};
        context.rax = Address(made);
        context.rbx = Address(argument);
        const auto before = GetTickCount64();
        PeerJoinedHandler(&context);                             // the local user
        made[0] = Address(room.User(5));
        argument[8] = 1;
        PeerJoinedHandler(&context);                             // a member
        room.TimedOut(5);
        const auto decision = DecidePeerTimeout(room.room);
        Check(decision.joinKnown && decision.joinedAfterMs >= 0 && decision.joinedAfterMs <= static_cast<std::int64_t>(GetTickCount64() - before) &&
                  decision.verdict == PeerTimeoutVerdict::Leave,
              "the hook records the local user and a member with the time they joined");
        argument[8] = 7;
        made[0] = Address(room.User(4));
        PeerJoinedHandler(&context);
        room.TimedOut(5, false);
        room.TimedOut(4);
        Check(!DecidePeerTimeout(room.room).joinKnown, "an add whose remote flag is not a bool records nothing");
        context.rax = 0x10;
        PeerJoinedHandler(&context);                             // unreadable: nothing recorded, nothing thrown
    }

    // What is logged: one line when it acts, then one per 30 s while the game keeps asking every frame.
    DeleteFileW(argv[1]);
    LogOpen(argv[1]);
    ForgetPeerTimeoutLog();
    {
        FakeRoom room;
        Joins(room, 5, 15 * kMinute);
        room.TimedOut(5);
        bool allStay = true;
        for (std::uint64_t frame = 0; frame < 600; ++frame) allStay = KeepRoomAfterPeerTimeout(room.room, 5'000'000 + frame * 16) && allStay;
        Check(allStay, "every frame of the timeout is answered with stay");
        auto text = ReadText(argv[1]);
        Check(Count(text, "HANDSHAKE TIMEOUT: this machine STAYS in the room") == 1, "ten seconds of frames log one line");
        Check(text.find("joined 900.0 s after this machine") != std::string::npos && text.find("deadline 20000 ms") != std::string::npos,
              "the line says when the newcomer joined and the deadline");
        Check(KeepRoomAfterPeerTimeout(room.room, 5'000'000 + 30'000), "still stay after 30 s");
        text = ReadText(argv[1]);
        Check(Count(text, "this machine STAYS") == 2 && text.find("599 more checks since the last line") != std::string::npos,
              "a reminder with the count of silent checks every 30 s");
        // The same room seen by a newcomer: a new verdict is logged at once.
        Joins(room, 0, 0);
        Check(!KeepRoomAfterPeerTimeout(room.room, 5'000'000 + 30'001), "a newcomer leaves");
        text = ReadText(argv[1]);
        Check(Count(text, "HANDSHAKE TIMEOUT: leaving the room as the game does: this machine joined the room with or after them") == 1,
              "the leave is logged at once, with why");
    }
    ForgetPeerTimeoutLog();
    ForgetPeerJoins();
    std::printf("peer timeout decisions: %d failures\n", failures);
    return failures ? 1 : 0;
}
