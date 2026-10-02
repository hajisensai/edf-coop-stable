// Who the game has in its room, kept in step with what it was told, and the host's say over it.
//
// The game learns its room's members from the lobby: the members listed when it enters, then one
// status event per change. Epic's lobby service is one source of those events; the room's host is the
// other. The host's game is what decides who plays (it kicks, it admits), so with direct links its view
// of the room is sent to every member (the Room message) and each member's game follows it. That is
// what lets a player come back into a room while Epic's lobby service is down: the host's plugin sees
// the player's direct link come back and tells its game "joined", and every other member hears it from
// the host. Two sources can say the same thing (Epic and the host both report a member who left), so
// every event goes through admit(): a member already joined does not join again, one already gone does
// not leave again. Pure bookkeeping, no EOS calls. Not thread-safe: used on the EOS tick only.
#pragma once
#include <cstdint>
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

    // A member of the room: what the host's game has in the room, as the host last said. Returns what
    // our game must be told to have the same. The first list after reset() only adds: our game entered
    // with Epic's member list, which may be ahead of the host's (a player whose join the host's game has
    // not seen yet). Later lists are followed change by change: whom the host added joins, whom it
    // dropped leaves, and our own removal is a kick. Nothing is applied here: deliver each through
    // admit().
    std::vector<StatusChange> followHost(const std::vector<std::string>& hostMembers);

    // The host: whom to tell our game "joined". `linked`: members with a direct link to us now. A
    // member with a direct link is playing in this room (a player's plugin dials the host only while
    // its game is in the room), so one the game does not have is let in, unless it was kicked.
    std::vector<StatusChange> hostJoins(const std::vector<std::string>& linked) const;
    // The host: whom to tell our game "left". A member Epic does not list (`inEpicLobby` false) has only
    // the direct link to be in the room by; once that is down (`linkAlive` false) it is gone.
    template <typename InEpic, typename Alive>
    std::vector<StatusChange> hostLeaves(InEpic inEpicLobby, Alive linkAlive) const {
        std::vector<StatusChange> out;
        for (const std::string& m : members_)
            if (m != self_ && !inEpicLobby(m) && !linkAlive(m)) out.push_back({m, kLeft});
        return out;
    }

    // The game removes `member` from the room itself (KickMember). Kicked for this room: it is not let
    // back in by its direct link (a JOINED that reaches the game, i.e. a join through Epic's lobby, lifts
    // that). True when the game still had it, i.e. this is a kick and not the
    // game tidying up after a member that already left (it calls KickMember for those too).
    bool kick(const std::string& member);
    bool banned(const std::string& member) const { return banned_.count(member) != 0; }

private:
    bool active_ = false;
    std::string self_;
    std::set<std::string> members_;
    std::set<std::string> banned_;
    bool followed_ = false;
    std::set<std::string> hostSaid_;  // the host's last list
};

}  // namespace dn
