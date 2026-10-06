#include "netplayer_members.h"

#include <cstring>
#include <utility>

namespace multislot {
namespace {

const void* Pointer(const void* object, std::size_t offset) {
    const void* value = nullptr;
    std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
    return value;
}

}  // namespace

const void* PlayerUser(const void* soldier) { return soldier ? Pointer(soldier, kPlayerUserOffset) : nullptr; }

const void* UserProductId(const void* user) { return user ? Pointer(user, kUserProductIdField) : nullptr; }

MemberPlaces::MemberPlaces(Namer namer, Sink sink, Forget forget)
    : namer_(std::move(namer)), sink_(std::move(sink)), forget_(std::move(forget)) {}

bool MemberPlaces::Observe(const void* user, Vec3 position, Vec3 velocity, double nowMs) {
    if (!user) return false;
    auto [it, fresh] = members_.try_emplace(user);
    Member& member = it->second;
    if (fresh || member.name.empty()) member.name = namer_ ? namer_(UserProductId(user)) : std::string();
    member.seenMs = nowMs;
    if (member.name.empty()) return false;  // no id to give the gate: it keeps the medium default
    // Facing follows the motion on the ground plane; standing keeps the last one.
    const Vec3 flat{velocity.x, 0.0f, velocity.z};
    const float speed = Length(flat);
    if (speed > 0.5f) member.facing = flat * (1.0f / speed);
    if (nowMs - member.fedMs < kFeedIntervalMs) return false;
    member.fedMs = nowMs;
    PlayerPlace place;
    place.position = position;
    place.velocity = velocity;
    place.facing = member.facing;
    if (sink_) sink_(member.name, place);
    return true;
}

int MemberPlaces::Expire(double nowMs) {
    int forgotten = 0;
    for (auto it = members_.begin(); it != members_.end();) {
        if (nowMs - it->second.seenMs <= kForgetMs) {
            ++it;
            continue;
        }
        if (forget_ && !it->second.name.empty()) forget_(it->second.name);
        it = members_.erase(it);
        ++forgotten;
    }
    return forgotten;
}

}  // namespace multislot
