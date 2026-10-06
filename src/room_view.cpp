#include "room_view.h"

#include <algorithm>

namespace dn {

namespace {
void eraseFrom(std::vector<std::string>& order, const std::string& member) {
    order.erase(std::remove(order.begin(), order.end(), member), order.end());
}
}  // namespace

ParkedEntryOutcome decideParkedEntry(uint64_t waitedMs, bool slotted, bool gone, bool progressing) {
    if (slotted) return ParkedEntryOutcome::Slotted;
    if (gone) return ParkedEntryOutcome::Gone;
    if (waitedMs >= kParkedEntryCapMs || (waitedMs >= kParkedEntryMs && !progressing)) return ParkedEntryOutcome::GiveUp;
    return ParkedEntryOutcome::Wait;
}

std::vector<std::string> roomMessage(const std::vector<std::string>& slots, const std::set<std::string>& removed) {
    std::vector<std::string> out = slots;
    if (removed.empty()) return out;
    out.push_back(kRemovedMarker);
    out.insert(out.end(), removed.begin(), removed.end());
    return out;
}

void parseRoomMessage(const std::vector<std::string>& message, std::vector<std::string>* slots,
                      std::set<std::string>* removed) {
    if (slots) slots->clear();
    if (removed) removed->clear();
    bool past = false;
    for (const std::string& entry : message) {
        if (!past && entry == kRemovedMarker) {
            past = true;
            continue;
        }
        if (!past) {
            if (slots) slots->push_back(entry);
        } else if (removed && !entry.empty()) {
            removed->insert(entry);
        }
    }
}

std::vector<std::string> roomOrder(const std::vector<std::string>& epic, const std::vector<std::string>& view) {
    const std::set<std::string> inView(view.begin(), view.end()), inEpic(epic.begin(), epic.end());
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& m : epic)
        if (inView.count(m) && seen.insert(m).second) out.push_back(m);
    for (const std::string& m : view)
        if (!inEpic.count(m) && seen.insert(m).second) out.push_back(m);
    return out;
}

void RoomView::reset(const std::string& self, const std::vector<std::string>& members) {
    clear();
    active_ = true;
    self_ = self;
    for (const std::string& m : members)
        if (!m.empty() && members_.insert(m).second) order_.push_back(m);
    if (!self.empty() && members_.insert(self).second) order_.push_back(self);
}

void RoomView::clear() { *this = RoomView(); }

bool RoomView::admit(const std::string& target, int32_t status) {
    if (!active_ || target.empty() || target == self_) return true;
    switch (status) {
        case kJoined:
            banned_.erase(target);  // back in through Epic's lobby: the game let it in again
            if (!members_.insert(target).second) return false;
            order_.push_back(target);
            return true;
        case kLeft:
        case kDisconnected:
        case kKicked: {
            if (!members_.erase(target)) return false;
            eraseFrom(order_, target);
            auto link = links_.find(target);
            departed_[target] = link == links_.end() ? 0 : link->second;
            links_.erase(target);
            direct_.erase(target);
            return true;
        }
        default:
            return true;
    }
}

std::vector<StatusChange> RoomView::settle(const std::vector<StatusChange>& wanted, uint64_t nowMs, uint64_t delayMs) {
    std::map<std::string, Pending> still;
    std::vector<StatusChange> due;
    for (const StatusChange& c : wanted) {
        auto it = pending_.find(c.target);
        Pending p = it != pending_.end() && it->second.status == c.status ? it->second : Pending{c.status, nowMs};
        if (nowMs - p.sinceMs >= delayMs)
            due.push_back(c);
        else
            still[c.target] = p;
    }
    pending_ = std::move(still);
    return due;
}

void RoomView::heardHost(const std::vector<std::string>& message) {
    if (!active_) return;
    heard_ = true;
    awaiting_ = false;
    parseRoomMessage(message, &hostSlots_, &hostBanned_);
    hostNow_.clear();
    for (const std::string& m : hostSlots_)
        if (!m.empty()) hostNow_.insert(m);
    hostEver_.insert(hostNow_.begin(), hostNow_.end());
}

int RoomView::hostSlot(const std::string& member) const {
    if (!heard_ || member.empty()) return -1;
    for (size_t i = 0; i < hostSlots_.size(); ++i)
        if (hostSlots_[i] == member) return static_cast<int>(i);
    return -1;
}

std::vector<std::string> RoomView::hostMembers() const {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& m : hostSlots_)
        if (!m.empty() && seen.insert(m).second) out.push_back(m);
    return out;
}

void RoomView::promoted(const std::string& newHost, uint64_t nowMs) {
    if (!active_ || newHost.empty()) return;
    const bool followed = heard_;
    heard_ = false;
    hostSlots_.clear();
    hostNow_.clear();
    pending_.clear();
    if (newHost == self_) {
        // Our game's slots are the room's now; whom the old host removed stays removed.
        banned_.insert(hostBanned_.begin(), hostBanned_.end());
        hostBanned_.clear();
        awaiting_ = false;
        return;
    }
    awaiting_ = followed;
    awaitingSinceMs_ = nowMs;
}

std::set<std::string> RoomView::bannedMembers() const {
    std::set<std::string> out = banned_;
    out.insert(hostBanned_.begin(), hostBanned_.end());
    return out;
}

void RoomView::adoptHost() {
    if (!active_ || !heard_) return;
    members_.clear();
    order_.clear();
    for (const std::string& m : hostMembers())
        if (members_.insert(m).second) order_.push_back(m);
    if (!self_.empty() && members_.insert(self_).second) order_.push_back(self_);
    pending_.clear();
}

std::vector<StatusChange> RoomView::followHost() const {
    std::vector<StatusChange> out;
    if (!active_ || !heard_) return out;
    for (const std::string& m : order_)
        if (hostEver_.count(m) && !hostNow_.count(m)) out.push_back({m, m == self_ ? kKicked : kLeft});
    for (const std::string& m : hostSlots_)
        if (!m.empty() && m != self_ && !members_.count(m) &&
            std::none_of(out.begin(), out.end(), [&](const StatusChange& c) { return c.target == m; }))
            out.push_back({m, kJoined});
    return out;
}

std::vector<StatusChange> RoomView::hostJoins(const std::vector<Linked>& linked, size_t capacity) {
    std::vector<StatusChange> out;
    if (!active_) return out;
    for (const Linked& l : linked) {
        if (members_.count(l.member)) {
            links_[l.member] = l.link;
            continue;
        }
        if (l.epic || l.member.empty() || l.member == self_ || banned(l.member)) continue;
        auto gone = departed_.find(l.member);
        if (gone != departed_.end() && gone->second == l.link) continue;
        if (members_.size() + out.size() >= capacity) continue;
        out.push_back({l.member, kJoined});
    }
    return out;
}

void RoomView::markDirect(const std::string& member) {
    if (members_.count(member)) direct_.insert(member);
}

bool RoomView::kick(const std::string& member) {
    if (!active_ || member.empty() || member == self_ || !members_.count(member)) return false;
    return banned_.insert(member).second;
}

}  // namespace dn
