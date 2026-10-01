#include "hold.h"

#include <algorithm>

namespace dn {

bool DisconnectHold::isTransient(int32_t reason) {
    switch (reason) {
        case 0:   // Unknown
        case 3:   // TimedOut
        case 7:   // ConnectionFailed
        case 8:   // ConnectionClosed
        case 9:   // NegotiationFailed
        case 10:  // UnexpectedError
            return true;
        default:  // ClosedByLocalUser, ClosedByPeer, TooManyConnections, InvalidMessage, InvalidData
            return false;
    }
}

size_t DisconnectHold::dropLocked(const std::string& remote) {
    auto gone = std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
    size_t n = static_cast<size_t>(held_.end() - gone);
    held_.erase(gone, held_.end());
    return n;
}

size_t DisconnectHold::onEstablished(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    established_.insert(remote);
    return dropLocked(remote);
}

bool DisconnectHold::offer(const std::string& remote, int32_t reason, bool directPeer, bool pluginPeer, uint64_t nowMs,
                           std::function<void()> forward, std::function<void()> reaccept) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!isTransient(reason)) {
        // A deliberate close (the peer left, or dropped us) makes earlier held events meaningless;
        // the game gets this event instead, exactly once.
        dropLocked(remote);
        established_.erase(remote);
        return false;
    }
    // A first handshake that fails must reach the game, so plain EOS peers need a prior connection.
    bool eligible = directPeer || ((pluginPeer || opt_.holdAll) && established_.count(remote));
    if (!eligible) return false;
    Held h;
    h.remote = remote;
    h.reason = reason;
    h.heldAtMs = nowMs;
    h.forward = std::move(forward);
    h.reaccept = std::move(reaccept);
    held_.push_back(std::move(h));  // first reconnect request on the next poll, outside the EOS callback
    return true;
}

size_t DisconnectHold::release(const std::string& remote) {
    std::vector<Held> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = held_.begin(); it != held_.end();) {
            if (it->remote != remote) {
                ++it;
                continue;
            }
            due.push_back(std::move(*it));
            it = held_.erase(it);
        }
        established_.erase(remote);
    }
    for (auto& h : due)
        if (h.forward) h.forward();
    return due.size();
}

size_t DisconnectHold::releaseAll() {
    std::vector<Held> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        due.swap(held_);
        established_.clear();
    }
    for (auto& h : due)
        if (h.forward) h.forward();
    return due.size();
}

size_t DisconnectHold::onGameClosed(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    established_.erase(remote);
    return dropLocked(remote);
}

size_t DisconnectHold::onGameClosedAll() {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = held_.size();
    held_.clear();
    established_.clear();
    return n;
}

bool DisconnectHold::isHeld(const std::string& remote) const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
}

size_t DisconnectHold::heldCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return held_.size();
}

std::vector<std::string> DisconnectHold::poll(uint64_t nowMs,
                                              const std::function<bool(const std::string&)>& directAlive) {
    std::vector<std::string> remotes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& h : held_) remotes.push_back(h.remote);
    }
    // Ask the direct transport outside our lock (it has its own).
    std::unordered_set<std::string> alive;
    for (const auto& r : remotes)
        if (directAlive && directAlive(r)) alive.insert(r);

    std::vector<std::function<void()>> reaccepts;
    std::vector<Held> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = held_.begin(); it != held_.end();) {
            if (alive.count(it->remote)) it->heldAtMs = nowMs;  // still reachable directly: never expire
            if (nowMs - it->heldAtMs >= opt_.graceMs) {
                established_.erase(it->remote);
                due.push_back(std::move(*it));
                it = held_.erase(it);
                continue;
            }
            if (it->reaccept && (!it->reacceptedOnce || nowMs - it->lastReacceptMs >= opt_.reacceptIntervalMs)) {
                it->reacceptedOnce = true;
                it->lastReacceptMs = nowMs;
                reaccepts.push_back(it->reaccept);
            }
            ++it;
        }
    }
    // Callbacks run unlocked: the game's handler may call straight back into EOS and into us.
    for (auto& r : reaccepts) r();
    std::vector<std::string> expired;
    for (auto& h : due) {
        expired.push_back(h.remote);
        if (h.forward) h.forward();
    }
    return expired;
}

bool LobbyStatusHold::offer(const std::string& remote, bool reachable, uint64_t nowMs, std::function<void()> deliver) {
    return offer(remote, remote, reachable, graceMs_, false, nowMs, std::move(deliver));
}

bool LobbyStatusHold::offer(const std::string& key, const std::string& probe, bool reachable, uint32_t graceMs,
                            bool replace, uint64_t nowMs, std::function<void()> deliver) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& h : held_) {
        if (h.remote != key) continue;
        if (replace) {  // the newer status supersedes; the link check carries on
            h.probe = probe;
            h.graceMs = graceMs;
            h.deliver = std::move(deliver);
        }
        return true;  // a repeat; the first one is still hidden
    }
    if (!reachable) return false;
    held_.push_back(Held{key, probe, graceMs, nowMs, false, std::move(deliver)});
    return true;
}

bool LobbyStatusHold::discard(const std::string& key) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find_if(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == key; });
    if (it == held_.end()) return false;
    held_.erase(it);
    return true;
}

bool LobbyStatusHold::onStatus(const std::string& remote, int32_t status) {
    constexpr int32_t kJoined = 0, kPromoted = 4;
    std::function<void()> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::find_if(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
        if (it == held_.end()) return false;
        if (status != kJoined && status != kPromoted) due = std::move(it->deliver);
        held_.erase(it);
    }
    if (due) due();
    return status == kJoined;
}

bool LobbyStatusHold::abandon(const std::string& remote) {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto& h : held_)
        if (h.remote == remote) {
            h.abandoned = true;
            return true;
        }
    return false;
}

size_t LobbyStatusHold::releaseAll() {
    std::vector<Held> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        due.swap(held_);
    }
    for (auto& h : due)
        if (h.deliver) h.deliver();
    return due.size();
}

size_t LobbyStatusHold::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    size_t n = held_.size();
    held_.clear();
    return n;
}

bool LobbyStatusHold::isHeld(const std::string& remote) const {
    std::lock_guard<std::mutex> lock(mu_);
    return std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
}

size_t LobbyStatusHold::heldCount() const {
    std::lock_guard<std::mutex> lock(mu_);
    return held_.size();
}

std::vector<std::string> LobbyStatusHold::poll(uint64_t nowMs,
                                               const std::function<bool(const std::string&)>& reachable) {
    std::vector<std::string> probes;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& h : held_) probes.push_back(h.probe);
    }
    // Ask the direct transport outside our lock (it has its own).
    std::unordered_set<std::string> alive;
    for (const auto& r : probes)
        if (reachable && reachable(r)) alive.insert(r);

    std::vector<Held> due;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto it = held_.begin(); it != held_.end();) {
            const bool up = alive.count(it->probe) != 0;
            if (up) it->reachableAtMs = nowMs;
            if (!it->abandoned && (up || nowMs - it->reachableAtMs < it->graceMs)) {
                ++it;
                continue;
            }
            due.push_back(std::move(*it));
            it = held_.erase(it);
        }
    }
    std::vector<std::string> delivered;
    for (auto& h : due) {
        delivered.push_back(h.remote);
        if (h.deliver) h.deliver();  // unlocked: it may call straight back into us
    }
    return delivered;
}

void LobbyOwnerPin::entered(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mu_);
    pinned_ = owner;
    epic_ = owner;
}

void LobbyOwnerPin::left() {
    std::lock_guard<std::mutex> lock(mu_);
    pinned_.clear();
    epic_.clear();
}

LobbyOwnerPin::Promotion LobbyOwnerPin::onPromoted(const std::string& target, const std::string& self,
                                                   bool pinnedReachable) {
    std::lock_guard<std::mutex> lock(mu_);
    epic_ = target;
    Promotion p;
    if (pinned_.empty()) {  // the owner was not known when we joined: Epic's word is all there is
        pinned_ = target;
        return p;
    }
    if (target == pinned_) {
        p.hide = true;  // the game never saw the owner change, so it has nothing to undo
        return p;
    }
    if (!pinnedReachable) {
        pinned_ = target;  // the pinned owner is gone for real: the room follows Epic
        return p;
    }
    p.hide = true;
    p.promoteBack = !self.empty() && target == self;
    return p;
}

bool LobbyOwnerPin::onPinnedJoined(const std::string& self) const {
    std::lock_guard<std::mutex> lock(mu_);
    return !self.empty() && !pinned_.empty() && epic_ == self && pinned_ != self;
}

void LobbyOwnerPin::follow(const std::string& owner) {
    std::lock_guard<std::mutex> lock(mu_);
    pinned_ = owner;
}

bool LobbyOwnerPin::kickAuthorized() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pinned_.empty() || epic_ == pinned_;
}

std::string LobbyOwnerPin::pinned() const {
    std::lock_guard<std::mutex> lock(mu_);
    return pinned_;
}

std::string LobbyOwnerPin::usurper() const {
    std::lock_guard<std::mutex> lock(mu_);
    return epic_ != pinned_ ? epic_ : std::string();
}

}  // namespace dn
