#include "room_view.h"

#include <algorithm>
#include <charconv>

namespace dn {

namespace {
constexpr const char* kWorldMarker = "#world-v1:";
constexpr const char* kWorldMember = "#world-member:";
}

bool freshWorldEntryAllowed(const WorldAdmission& world) {
    return world.present && world.phase == WorldPhase::Lobby && world.participants.empty();
}

void appendWorldAdmission(std::vector<std::string>& message, const WorldAdmission& world) {
    // Old readers treat these reserved strings as removed IDs, never as player slots.
    if (std::find(message.begin(), message.end(), kRemovedMarker) == message.end()) message.push_back(kRemovedMarker);
    message.push_back(std::string(kWorldMarker) + std::to_string(static_cast<uint32_t>(world.phase)) + ":" +
                      std::to_string(world.epoch));
    for (const auto& id : world.participants) message.push_back(std::string(kWorldMember) + id);
}

WorldAdmission parseWorldAdmission(const std::vector<std::string>& message) {
    WorldAdmission out;
    bool removed = false;
    for (const auto& entry : message) {
        if (entry == kRemovedMarker) { removed = true; continue; }
        if (!removed) continue;
        if (entry.starts_with(kWorldMarker)) {
            if (out.present) return {};
            out.present = true;
            const std::string text = entry.substr(std::char_traits<char>::length(kWorldMarker));
            const auto colon = text.find(':');
            if (colon == std::string::npos) return {};
            uint32_t phase = 0;
            const auto p = std::from_chars(text.data(), text.data() + colon, phase);
            const auto e = std::from_chars(text.data() + colon + 1, text.data() + text.size(), out.epoch);
            if (p.ec != std::errc{} || p.ptr != text.data() + colon || e.ec != std::errc{} ||
                e.ptr != text.data() + text.size() || phase > 4) return {};
            out.phase = static_cast<WorldPhase>(phase);
        } else if (entry.starts_with(kWorldMember)) {
            if (!out.present || out.participants.size() >= 1024) return {};
            const std::string id = entry.substr(std::char_traits<char>::length(kWorldMember));
            if (id.empty() || id.size() > 64 || id[0] == '#') return {};
            out.participants.push_back(id);
        }
    }
    std::sort(out.participants.begin(), out.participants.end());
    if (std::adjacent_find(out.participants.begin(), out.participants.end()) != out.participants.end() ||
        (out.phase == WorldPhase::Lobby && !out.participants.empty()) ||
        ((out.phase == WorldPhase::Loading || out.phase == WorldPhase::Sealed) && !out.epoch)) return {};
    return out;
}

namespace {
void eraseFrom(std::vector<std::string>& order, const std::string& member) {
    order.erase(std::remove(order.begin(), order.end(), member), order.end());
}
}  // namespace

ParkedEntryOutcome decideParkedEntry(uint64_t waitedMs, bool slotted, bool gone, bool progressing, uint64_t noHostMs) {
    if (slotted) return ParkedEntryOutcome::Slotted;
    if (gone) return ParkedEntryOutcome::Gone;
    if (noHostMs >= kParkedNoHostMs) return ParkedEntryOutcome::NoHost;
    if (waitedMs >= kParkedEntryCapMs || (waitedMs >= kParkedEntryMs && !progressing)) return ParkedEntryOutcome::GiveUp;
    return ParkedEntryOutcome::Wait;
}

ParkedEntryOutcome decideParkedWorldEntry(uint64_t waitedMs, bool slotted, bool gone, bool progressing,
                                         uint64_t noHostMs, bool required, const WorldAdmission& world) {
    if (required && !freshWorldEntryAllowed(world)) {
        if (gone) return ParkedEntryOutcome::Gone;
        // An authenticated host explicitly running/loading a world: remain outside its native world until lobby.
        // A lost host or missing/malformed manifest must never turn into the old NoHost permissive fallback.
        if (progressing && world.present && world.phase != WorldPhase::Unknown) return ParkedEntryOutcome::Wait;
        if (waitedMs >= kParkedEntryCapMs || (waitedMs >= kParkedEntryMs && !progressing))
            return ParkedEntryOutcome::GiveUp;
        return ParkedEntryOutcome::Wait;
    }
    return decideParkedEntry(waitedMs, slotted, gone, progressing, noHostMs);
}

InheritedPlacement decideInheritedPlacement(int oldSlot, const std::vector<std::string>& ours, bool late) {
    size_t next = 0;  // the slot our game gives next
    while (next < ours.size() && !ours[next].empty()) ++next;
    const bool fits = oldSlot < 0 || ours.empty() || static_cast<size_t>(oldSlot) == next;
    if (!fits && !late && static_cast<size_t>(oldSlot) > next) return {false, false};
    return {true, fits};
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
        } else if (removed && !entry.empty() && entry[0] != '#') {
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
            std::erase_if(held_, [&](const Held& h) { return h.member == target; });
            releasing_.erase(target);
            if (!members_.insert(target).second) return false;
            order_.push_back(target);
            return true;
        case kLeft:
        case kDisconnected:
        case kKicked: {
            // Gone before its held join reached our game: nothing to bring any more.
            std::erase_if(held_, [&](const Held& h) { return h.member == target; });
            releasing_.erase(target);
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

void RoomView::heardHost(const std::vector<std::string>& message, const std::string& from) {
    if (!active_) return;
    if (!expectedHost_.empty() && !from.empty() && from != expectedHost_) return;  // the old host's, after a change
    heard_ = true;
    awaiting_ = false;
    parseRoomMessage(message, &hostSlots_, &hostBanned_);
    hostWorld_ = parseWorldAdmission(message);
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

std::vector<std::pair<std::string, int>> RoomView::promoted(const std::string& newHost, uint64_t nowMs) {
    std::vector<std::pair<std::string, int>> placements;
    if (!active_ || newHost.empty()) return placements;
    const bool followed = heard_ || awaiting_;
    const std::vector<std::string> oldSlots = hostSlots_;
    heard_ = false;
    hostSlots_.clear();
    hostWorld_ = {};
    hostNow_.clear();
    // Whom the old host listed says nothing about the new host's room: a member its list does not have (yet) is not
    // told to leave for that (followHost).
    hostEver_.clear();
    pending_.clear();
    expectedHost_ = newHost;
    if (newHost == self_) {
        // Our game's slots are the room's now; whom the old host removed stays removed.
        banned_.insert(hostBanned_.begin(), hostBanned_.end());
        hostBanned_.clear();
        awaiting_ = false;
        // The joins we held: in the old host's slots where it had them (slot order), the others after (as they came).
        for (const Held& h : held_) {
            const auto at = std::find(oldSlots.begin(), oldSlots.end(), h.member);
            placements.push_back({h.member, at == oldSlots.end() ? -1 : static_cast<int>(at - oldSlots.begin())});
        }
        std::stable_sort(placements.begin(), placements.end(), [](const auto& a, const auto& b) {
            if ((a.second < 0) != (b.second < 0)) return a.second >= 0;
            return a.second >= 0 && a.second < b.second;
        });
        held_.clear();
        return placements;
    }
    awaiting_ = followed;
    awaitingSinceMs_ = nowMs;
    return placements;
}

void RoomView::holdJoin(const std::string& member, uint64_t nowMs) {
    if (!active_ || member.empty() || members_.count(member) || held(member)) return;
    held_.push_back({member, nowMs});
}

bool RoomView::held(const std::string& member) const {
    return std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.member == member; });
}

std::vector<std::string> RoomView::releaseHeld(uint64_t nowMs, uint64_t waitMs, uint64_t capMs) {
    std::vector<std::string> out;
    if (!active_) return out;
    const bool waitOver = awaiting_ && !heard_ && nowMs - awaitingSinceMs_ >= waitMs;
    if (waitOver) awaiting_ = false;
    std::vector<Held> still;
    for (const Held& h : held_) {
        if (waitOver || nowMs - h.sinceMs >= capMs) {
            out.push_back(h.member);
            releasing_.insert(h.member);
        } else {
            still.push_back(h);
        }
    }
    held_ = std::move(still);
    return out;
}

bool RoomView::consumeRelease(const std::string& member) { return releasing_.erase(member) != 0; }

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
