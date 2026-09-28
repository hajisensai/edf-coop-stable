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

bool DisconnectHold::offer(const std::string& remote, int32_t reason, bool directPeer, uint64_t nowMs,
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
    bool eligible = directPeer || (opt_.holdAll && established_.count(remote));
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

}  // namespace dn
