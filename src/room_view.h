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
    std::vector<std::string> members() const { return {members_.begin(), members_.end()}; }

    // Holds back what the plugin would tell the game until Epic had its chance: returns the changes of
    // `wanted` that were wanted, the same way, in every call for the last `delayMs` (0: at once). A change
    // missing from `wanted` starts over. Deliver each through admit().
    std::vector<StatusChange> settle(const std::vector<StatusChange>& wanted, uint64_t nowMs, uint64_t delayMs);

    // A member of the room: the host's newest list of who its game has in the room.
    void heardHost(const std::vector<std::string>& hostMembers);
    // A member: what our game must be told to have what the host's game has. Whom the host lists joins.
    // Whom the host listed before and lists no longer leaves (our own removal is a kick); a member only
    // Epic told us of, which the host never listed (its game may simply not have seen the join yet), stays.
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
    bool banned(const std::string& member) const { return banned_.count(member) != 0; }

private:
    struct Pending {
        int32_t status = kJoined;
        uint64_t sinceMs = 0;
    };
    bool active_ = false;
    std::string self_;
    std::set<std::string> members_;
    std::set<std::string> banned_;
    std::set<std::string> direct_;              // came in by their direct link only
    std::map<std::string, uint64_t> links_;     // the host: each member's direct link while in the room
    std::map<std::string, uint64_t> departed_;  // the host: the link a member had when it left (0: none)
    std::map<std::string, Pending> pending_;    // settle(): what was wanted, since when
    bool heard_ = false;
    std::set<std::string> hostNow_;   // the host's newest list
    std::set<std::string> hostEver_;  // everyone the host listed since we entered
};

}  // namespace dn
