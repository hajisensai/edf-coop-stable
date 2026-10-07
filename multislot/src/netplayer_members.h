#pragma once
// Where each member's player is, for interest management (netaoi.h NoteMember): member <-> player object <-> place.
//
// A player object knows its member: SoldierBase::CreateOnlinePlayerObject (EDF+591130) looks the player index up in
// the room's users (the lambda at EDF+5A35D0 compares eos::User+0x48 with it) and keeps that eos::User in the object
// at +0x1ED0 (EDF+591254); the member's EOS ProductUserId is at eos::User+0x18 (peertimeout.h). An online mission
// creates every player that way, this machine's own included (EDF+1DC525), so the same field names the member for the
// local player and every remote copy. An object without a user there (offline, a ghost copy) has no member, and the
// gate keeps counting that member as medium relevance (netaoi.h) - never as far away.
//
// Fed from the player's NetworkUpdate (netplayer_game.cpp), at most every kFeedIntervalMs per member: this machine's
// player by its own position, a remote player by W2's estimate when its machine sends the block (netplayer.h), by the
// copy's position otherwise. A member not seen for kForgetMs (its player gone) is forgotten again.
#include <cstdint>
#include <functional>
#include <map>
#include <string>

#include "netplayer.h"

namespace multislot {

constexpr std::size_t kPlayerUserOffset = 0x1ED0;  // SoldierBase -> shared_ptr<eos::User> (EDF+591254)
constexpr std::size_t kUserProductIdField = 0x18;  // eos::User -> EOS_ProductUserId

// The eos::User a player object belongs to, or nullptr.
const void* PlayerUser(const void* soldier);
// That user's EOS_ProductUserId, or nullptr.
const void* UserProductId(const void* user);

struct PlayerPlace {
    Vec3 position;
    Vec3 velocity;
    Vec3 facing{0, 0, 1};  // where it moves (the last direction while it stands)
    bool engaged = false;  // not known yet: no player state read says "in a fight"
};

class MemberPlaces {
public:
    static constexpr double kFeedIntervalMs = 100.0;
    static constexpr double kForgetMs = 2000.0;
    using Namer = std::function<std::string(const void* productId)>;  // EOS id -> its 32 characters ("" unknown)
    using Sink = std::function<void(const std::string& member, const PlayerPlace& place)>;
    using Forget = std::function<void(const std::string& member)>;
    MemberPlaces(Namer namer, Sink sink, Forget forget);

    // One frame of a player object that belongs to `user` (nullptr: none; nothing is fed). Feeds the member's place
    // when its interval has passed. Returns whether it fed.
    bool Observe(const void* user, Vec3 position, Vec3 velocity, double nowMs);
    // Forgets members not observed for kForgetMs. Returns how many.
    int Expire(double nowMs);
    std::size_t Known() const { return members_.size(); }

private:
    struct Member {
        std::string name;
        double fedMs = -1e300;
        double seenMs = 0.0;
        Vec3 facing{0, 0, 1};
    };
    Namer namer_;
    Sink sink_;
    Forget forget_;
    std::map<const void*, Member> members_;  // by eos::User
};

}  // namespace multislot
