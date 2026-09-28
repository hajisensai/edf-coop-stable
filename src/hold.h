// Hides transient EOS P2P connection loss from the game.
//
// When EOS reports that a connection closed for a transient reason (timeout, network failure),
// the game would normally drop that player at once. Instead the event is held back for a grace
// period while the plugin asks EOS to reconnect. If EOS re-establishes the connection in time the
// game never learns about the hiccup; otherwise the original event is handed to the game.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace dn {

class DisconnectHold {
public:
    struct Options {
        bool enabled = true;
        uint32_t graceMs = 30000;
        uint32_t reacceptIntervalMs = 2000;
    };

    explicit DisconnectHold(Options options) : opt_(options) {}

    // Reasons that can heal by reconnecting (EOS_EConnectionClosedReason values).
    static bool isTransient(int32_t reason);

    // A connection to `remote` was (re-)established. Returns the number of held events it resolved.
    size_t onEstablished(const std::string& remote, uint64_t nowMs);

    // Offers a close event. Returns true when it is held; `forward` then runs later only if the
    // connection does not come back, `reaccept` runs periodically to ask EOS to reconnect.
    bool offer(const std::string& remote, int32_t reason, uint64_t nowMs, std::function<void()> forward,
               std::function<void()> reaccept);

    // The game closed the connection itself: drop held events for `remote` without forwarding.
    size_t onGameClosed(const std::string& remote);
    size_t onGameClosedAll();

    bool isHeld(const std::string& remote) const;
    size_t heldCount() const { return held_.size(); }

    // Runs reaccept timers and forwards expired events. Returns the remotes whose events expired.
    std::vector<std::string> poll(uint64_t nowMs);

private:
    struct Held {
        std::string remote;
        int32_t reason = 0;
        uint64_t heldAtMs = 0;
        uint64_t lastReacceptMs = 0;
        bool reacceptedOnce = false;
        std::function<void()> forward;
        std::function<void()> reaccept;
    };

    Options opt_;
    std::unordered_set<std::string> established_;  // peers that had a working connection before
    std::vector<Held> held_;
};

}  // namespace dn
