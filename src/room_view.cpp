#include "room_view.h"

namespace dn {

void RoomView::reset(const std::string& self, const std::vector<std::string>& members) {
    active_ = true;
    self_ = self;
    members_ = {members.begin(), members.end()};
    if (!self.empty()) members_.insert(self);
    banned_.clear();
    followed_ = false;
    hostSaid_.clear();
}

void RoomView::clear() {
    active_ = false;
    self_.clear();
    members_.clear();
    banned_.clear();
    followed_ = false;
    hostSaid_.clear();
}

bool RoomView::admit(const std::string& target, int32_t status) {
    if (!active_ || target.empty() || target == self_) return true;
    switch (status) {
        case kJoined:
            banned_.erase(target);  // back in through Epic's lobby: the game let it in again
            return members_.insert(target).second;
        case kLeft:
        case kDisconnected:
        case kKicked:
            return members_.erase(target) != 0;
        default:
            return true;
    }
}

std::vector<StatusChange> RoomView::followHost(const std::vector<std::string>& hostMembers) {
    std::vector<StatusChange> out;
    if (!active_) return out;
    std::set<std::string> now(hostMembers.begin(), hostMembers.end());
    for (const std::string& m : now)
        if (m != self_ && !members_.count(m) && (!followed_ || !hostSaid_.count(m))) out.push_back({m, kJoined});
    if (followed_) {
        for (const std::string& m : hostSaid_) {
            if (now.count(m)) continue;
            if (m == self_)
                out.push_back({m, kKicked});
            else if (members_.count(m))
                out.push_back({m, kLeft});
        }
    }
    followed_ = true;
    hostSaid_ = std::move(now);
    return out;
}

std::vector<StatusChange> RoomView::hostJoins(const std::vector<std::string>& linked) const {
    std::vector<StatusChange> out;
    if (!active_) return out;
    for (const std::string& m : linked)
        if (!m.empty() && m != self_ && !members_.count(m) && !banned_.count(m)) out.push_back({m, kJoined});
    return out;
}

bool RoomView::kick(const std::string& member) {
    if (!active_ || member.empty() || member == self_ || !members_.count(member)) return false;
    banned_.insert(member);
    return true;
}

}  // namespace dn
