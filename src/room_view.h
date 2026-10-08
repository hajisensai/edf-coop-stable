// Who the game has in its room, kept in step with what it was told, and the host's say over it.
//
// The game learns its room's members from the lobby: the members listed when it enters, then one
// status event per change. Epic's lobby service is one source of those events; the room's host is the
// other. The host's game is what decides who plays (it kicks, it admits), so with direct links its view
// of the room is sent to every member (the Room message) and each member's game follows it. That is
// what lets a player come back into a room while Epic's lobby service is down: the host's plugin sees
// the player's direct link come back and tells its game "joined", and every other member hears it from
// the host.
//
// Epic speaks first. While Epic's lobby service works, the game must see exactly what it sees without the
// plugin: Epic's events, when Epic sends them, with Epic's copy of the lobby read alongside. So a status
// the plugin would tell the game goes out only after it stayed wanted for a while (settle()): if Epic
// reports it meanwhile, Epic's event is the one the game gets, and ours is no longer wanted. Two sources
// can still say the same thing (Epic late), so every event goes through admit(): a member already joined
// does not join again, one already gone does not leave again. Pure bookkeeping, no EOS calls. Not
// thread-safe: used on the EOS tick only.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dn {

enum class WorldPhase : uint32_t { Unknown = 0, Lobby = 1, Loading = 2, Sealed = 3, Invalid = 4 };
struct WorldAdmission {
    WorldPhase phase = WorldPhase::Unknown;
    uint64_t epoch = 0;
    std::vector<std::string> participants;
    bool present = false;
};
// A fresh process has no state from the sealed world, even when it reuses a participant's PUID.
bool freshWorldEntryAllowed(const WorldAdmission& world);
void appendWorldAdmission(std::vector<std::string>& message, const WorldAdmission& world);
WorldAdmission parseWorldAdmission(const std::vector<std::string>& message);

// EOS_ELobbyMemberStatus.
enum : int32_t { kJoined = 0, kLeft = 1, kDisconnected = 2, kKicked = 3, kPromoted = 4, kClosed = 5 };

struct StatusChange {
    std::string target;
    int32_t status = kJoined;
    bool operator==(const StatusChange& o) const { return target == o.target && status == o.status; }
};

// A member with a direct link up to the host, and which link (DirectNet::linkId: a new one for every link
// that comes up).
struct Linked {
    std::string member;
    uint64_t link = 0;
    bool epic = false;  // Epic's lobby lists it: its join is Epic's to report
};

class RoomView {
public:
    // The game entered a room with `members` (it lists itself among them; `self` is added if not).
    void reset(const std::string& self, const std::vector<std::string>& members);
    // The game left its room: nothing is filtered or followed until the next reset().
    void clear();
    bool active() const { return active_; }

    // A status about to reach the game. True: deliver it (the view now includes it). False: the game
    // already has it that way, so it is swallowed. Statuses about ourselves, promotions and closes
    // always pass: only joins and departures of other members can repeat.
    bool admit(const std::string& target, int32_t status);
    bool has(const std::string& member) const { return members_.count(member) != 0; }
    // In the order the game added them: the list it entered with, then each join as it was admitted.
    const std::vector<std::string>& members() const { return order_; }

    // The member slots of the room (see the file comment of hostSlots below). The host's game numbers its members by
    // the slot eos::Users::Add gave each (the first empty one: a member that leaves frees its slot and the next one
    // to come takes it, nothing moves up), and that number (the network index) goes in the game's packets and its
    // mission start sync. So every game must have every member in the slot the host's game has it in. The host's
    // Room list is its game's slots: index = slot, "" = an empty one.
    // The slot the host's game gave `member`, -1 when the host's list does not have it (or nothing was heard).
    int hostSlot(const std::string& member) const;
    // The host's members in slot order, without the empty slots.
    std::vector<std::string> hostMembers() const;
    // A member that heard the host's slots, which list it: its game takes members in the host's slots (and no
    // member before the host's game has it).
    bool slotted() const { return active_ && heard_ && !self_.empty() && hostNow_.count(self_) != 0; }
    // The room's host changed (a PROMOTED reached the game): the slots the old host said are no longer the room's,
    // and neither is whom it ever listed (a member the new host's list does not have yet is not told to leave for
    // that). Becoming the host ourselves, our game's own slots are the room's from now on (we publish them) and the
    // members the old host removed stay removed; the joins we hold back are returned with the slot the old host's
    // last list gave each (-1: none), in that slot order then the order they came, for our game to take them in those
    // slots before we publish ours (eos_hooks hostRoomTick). Another member becoming the host: until it says its slots,
    // a member that followed the old host's slots holds joins back (awaitingHost) instead of numbering them by Epic's
    // order, and only the new host's list is heard from now on.
    std::vector<std::pair<std::string, int>> promoted(const std::string& newHost, uint64_t nowMs);
    // Joins wait for the new host's slots: we followed the old host's and the new one has not said its own yet. Until
    // releaseHeld() ends the wait (it ran past its time: a new host without a direct link never says).
    bool awaitingHost() const { return active_ && awaiting_ && !heard_; }

    // A join held back from our game (eos_hooks admitStatus) is remembered, in the order they came, so that it is
    // never lost: the host's say brings it when the host's game has it in a slot our game has free, and releaseHeld()
    // when that does not happen in time. A member that leaves (or joins) is no longer held.
    void holdJoin(const std::string& member, uint64_t nowMs);
    bool held(const std::string& member) const;
    size_t heldCount() const { return held_.size(); }
    // The held joins that must reach our game now, in the order they came: every one once the wait for a new host's
    // slots ran past `waitMs` (that wait ends), and any held for `capMs` (the host's game never took it in a slot our
    // game has free). Each goes once through admit (consumeRelease lets it past the hold).
    std::vector<std::string> releaseHeld(uint64_t nowMs, uint64_t waitMs, uint64_t capMs);
    // True once for a member releaseHeld returned: its join is not held back again.
    bool consumeRelease(const std::string& member);
    // The game now has exactly the host's members (it entered with them, hostMembers): what it is told follows.
    void adoptHost();

    // Holds back what the plugin would tell the game until Epic had its chance: returns the changes of
    // `wanted` that were wanted, the same way, in every call for the last `delayMs` (0: at once). A change
    // missing from `wanted` starts over. Deliver each through admit().
    std::vector<StatusChange> settle(const std::vector<StatusChange>& wanted, uint64_t nowMs, uint64_t delayMs);

    // A member of the room: the host's newest Room message (roomMessage): who its game has in the room, by slot ("" an
    // empty one), and whom the room removed.
    // `from`: the host that sent it; after a change of host, only the new host's list is heard ("" any).
    void heardHost(const std::vector<std::string>& message, const std::string& from = std::string());
    const WorldAdmission& hostWorld() const { return hostWorld_; }
    // A member: what our game must be told to have what the host's game has. Departures first: whom the host listed
    // before and lists no longer leaves (our own removal is a kick); a member only Epic told us of, which the host never
    // listed (its game may simply not have seen the join yet), stays. Then whom the host lists joins, in slot order -
    // after the departures, so that a member taking the slot of one that left finds it empty (a join whose slot is
    // still taken here waits: eos_hooks admitStatus).
    std::vector<StatusChange> followHost() const;

    // The host: whom to tell our game "joined". `linked`: members with a direct link to us now. A member with a direct link is playing in this room (a
    // player's plugin dials the host only while its game is in this room), so one the game does not have
    // Epic does not list is let in while the room has space (`capacity` members in all), unless it was kicked, or it left
    // and the link is still the one it had in the room (a link not yet timed out is no sign of a player
    // coming back).
    std::vector<StatusChange> hostJoins(const std::vector<Linked>& linked, size_t capacity);
    // The host: whom to tell our game "left". `inEpicLobby(m)` false: m has only its direct link to be in
    // the room by; once that is down (`linkAlive` false) it is gone.
    template <typename InEpic, typename Alive>
    std::vector<StatusChange> hostLeaves(InEpic inEpicLobby, Alive linkAlive) const {
        std::vector<StatusChange> out;
        for (const std::string& m : members_)
            if (m != self_ && !inEpicLobby(m) && !linkAlive(m)) out.push_back({m, kLeft});
        return out;
    }
    // The host: `member` came into the room by its direct link (hostJoins), not through Epic's lobby.
    void markDirect(const std::string& member);
    bool direct(const std::string& member) const { return direct_.count(member) != 0; }

    // The game removes `member` from the room itself (KickMember). Kicked for this room: it is not let
    // back in by its direct link (a JOINED that reaches the game, i.e. a join through Epic's lobby, lifts
    // that). True for the first kick of a member the game has; false for the game's repeats (it calls
    // KickMember over and over until KICKED comes back) and for a member already gone (the game tidies
    // up after members that left, too).
    bool kick(const std::string& member);
    bool banned(const std::string& member) const { return banned_.count(member) != 0 || hostBanned_.count(member) != 0; }
    // Everyone removed from this room: by our game, and by the host's (its Room message). The direct link keeps them
    // out too, and a host sends them with its slots so that whoever hosts the room next keeps them out.
    std::set<std::string> bannedMembers() const;

private:
    struct Pending {
        int32_t status = kJoined;
        uint64_t sinceMs = 0;
    };
    bool active_ = false;
    std::string self_;
    std::set<std::string> members_;
    std::vector<std::string> order_;  // members_, in the order they were added
    std::set<std::string> banned_;
    std::set<std::string> direct_;              // came in by their direct link only
    std::map<std::string, uint64_t> links_;     // the host: each member's direct link while in the room
    std::map<std::string, uint64_t> departed_;  // the host: the link a member had when it left (0: none)
    std::map<std::string, Pending> pending_;    // settle(): what was wanted, since when
    bool heard_ = false;
    bool awaiting_ = false;           // promoted(): waiting for the new host's slots
    uint64_t awaitingSinceMs_ = 0;
    std::string expectedHost_;        // promoted(): whose list is heard ("" anyone's)
    struct Held {
        std::string member;
        uint64_t sinceMs = 0;
    };
    std::vector<Held> held_;          // joins held back from our game, in the order they came
    std::set<std::string> releasing_;  // releaseHeld(): let past the hold once
    std::set<std::string> hostBanned_;  // whom the host's newest Room message says the room removed
    std::set<std::string> hostNow_;   // the host's newest list
    std::vector<std::string> hostSlots_;  // the same by slot, "" for an empty one
    WorldAdmission hostWorld_;
    std::set<std::string> hostEver_;  // everyone the host listed since we entered
};

// A join into a room whose host hosts a direct link enters the game once the host's member slots are here (eos_hooks
// parkedEntryTick). How it ends: still waiting; with the slots; the room went meanwhile; given up. It is given up when the
// link to the host got nowhere for kParkedEntryMs, or after kParkedEntryCapMs whatever it did: entering without the
// slots would number the members by Epic's order, and nothing renumbers a member later.
// NoHost: the room's host (its owner changed meanwhile) has advertised no direct link for kParkedNoHostMs: nobody says
// slots in that room, every other game goes as it would without the plugin, and so does ours.
enum class ParkedEntryOutcome { Wait, Slotted, Gone, GiveUp, NoHost };
constexpr uint64_t kParkedEntryMs = 10000;
constexpr uint64_t kParkedEntryCapMs = 45000;
constexpr uint64_t kParkedNoHostMs = 5000;
// noHostMs: how long the room's owner has advertised no direct link (0: it does).
ParkedEntryOutcome decideParkedEntry(uint64_t waitedMs, bool slotted, bool gone, bool progressing, uint64_t noHostMs = 0);
ParkedEntryOutcome decideParkedWorldEntry(uint64_t waitedMs, bool slotted, bool gone, bool progressing,
                                         uint64_t noHostMs, bool required, const WorldAdmission& world);

// Becoming the room's host, one join we held back (RoomView::promoted) and the slot the old host's game had it in
// (`oldSlot`, -1 none): whether our game is told it now, and whether it then takes that slot. Our game gives the first
// empty slot of `ours` (index = slot, "" empty; nothing known: any). It goes now when that is its old slot (or it had
// none); it waits while a lower slot is still empty (a later join fills it), unless `late`; a slot already taken by
// another member cannot be given, and then it goes as it came.
struct InheritedPlacement {
    bool tell = false;
    bool fits = false;
};
InheritedPlacement decideInheritedPlacement(int oldSlot, const std::vector<std::string>& ours, bool late);

// The Room message a host sends: its game's slots (index = slot, "" an empty one), then kRemovedMarker and the members
// the room removed (kicked), so that every member keeps them and a member that becomes the host keeps them out. No
// EOS id starts with '#'.
constexpr const char* kRemovedMarker = "#removed";
std::vector<std::string> roomMessage(const std::vector<std::string>& slots, const std::set<std::string>& removed);
void parseRoomMessage(const std::vector<std::string>& message, std::vector<std::string>* slots,
                      std::set<std::string>* removed);

// What a host sends as its Room list while its game's slots are not known (no eos::Users seen yet): Epic's lobby in
// Epic's order, then the members Epic does not list in the order the game has them (`view`, RoomView::members). A
// member of `epic` the view does not have is left out. Its members take the slots in this order.
std::vector<std::string> roomOrder(const std::vector<std::string>& epic, const std::vector<std::string>& view);

}  // namespace dn
