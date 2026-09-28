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

size_t DisconnectHold::onEstablished(const std::string& remote, uint64_t) {
    established_.insert(remote);
    auto gone = std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
    size_t n = static_cast<size_t>(held_.end() - gone);
    held_.erase(gone, held_.end());
    return n;
}

bool DisconnectHold::offer(const std::string& remote, int32_t reason, uint64_t nowMs, std::function<void()> forward,
                           std::function<void()> reaccept) {
    // Only peers that were connected before: a first handshake that fails must reach the game.
    if (!opt_.enabled || !isTransient(reason) || !established_.count(remote)) return false;
    Held h;
    h.remote = remote;
    h.reason = reason;
    h.heldAtMs = nowMs;
    h.reacceptedOnce = false;  // first reconnect request on the next poll, outside the EOS callback
    h.forward = std::move(forward);
    h.reaccept = std::move(reaccept);
    held_.push_back(std::move(h));
    return true;
}

size_t DisconnectHold::onGameClosed(const std::string& remote) {
    auto gone = std::remove_if(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
    size_t n = static_cast<size_t>(held_.end() - gone);
    held_.erase(gone, held_.end());
    established_.erase(remote);
    return n;
}

size_t DisconnectHold::onGameClosedAll() {
    size_t n = held_.size();
    held_.clear();
    established_.clear();
    return n;
}

bool DisconnectHold::isHeld(const std::string& remote) const {
    return std::any_of(held_.begin(), held_.end(), [&](const Held& h) { return h.remote == remote; });
}

std::vector<std::string> DisconnectHold::poll(uint64_t nowMs) {
    std::vector<std::string> expired;
    std::vector<Held> due;
    for (auto it = held_.begin(); it != held_.end();) {
        if (nowMs - it->heldAtMs >= opt_.graceMs) {
            due.push_back(std::move(*it));
            it = held_.erase(it);
            continue;
        }
        if (it->reaccept && (!it->reacceptedOnce || nowMs - it->lastReacceptMs >= opt_.reacceptIntervalMs)) {
            it->reacceptedOnce = true;
            it->lastReacceptMs = nowMs;
            it->reaccept();
        }
        ++it;
    }
    // Forward after the list is consistent: the game's handler may call back into EOS.
    for (auto& h : due) {
        established_.erase(h.remote);
        expired.push_back(h.remote);
        if (h.forward) h.forward();
    }
    return expired;
}

}  // namespace dn
