// Hides transient EOS P2P connection loss from the game.
//
// When EOS reports that a connection closed for a transient reason (timeout, network failure),
// the game would normally drop that player at once. Instead the event is held back while the plugin
// asks EOS to reconnect. If EOS re-establishes the connection in time the game never learns about the
// hiccup; otherwise the original event is handed to the game.
//
// Holding is only safe when the other side does the same: a player without the plugin drops us for
// real, and hiding that would leave a ghost player on our side. EDF6 reads every EOS channel as game
// data, so there is no safe way to probe for the plugin over EOS. Eligible peers are therefore:
//   * direct-link members (they run the plugin by definition), held for as long as the direct link
//     is alive because the game's traffic to them no longer flows over EOS at all;
//   * with `holdAll` (the players promise everyone runs the plugin): any peer that had a working
//     EOS connection before, held for `graceMs`.
// All methods are thread-safe; callbacks run without the internal lock held.
#pragma once
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace dn {

class DisconnectHold {
public:
    struct Options {
        uint32_t graceMs = 30000;
        uint32_t reacceptIntervalMs = 2000;
        bool holdAll = false;
    };

    explicit DisconnectHold(Options options) : opt_(options) {}

    // Reasons that can heal by reconnecting (EOS_EConnectionClosedReason values).
    static bool isTransient(int32_t reason);

    // A connection to `remote` was (re-)established. Returns the number of held events it resolved.
    size_t onEstablished(const std::string& remote);

    // Offers a close event. Returns true when it is held; `forward` then runs later only if the
    // connection does not come back, `reaccept` runs periodically to ask EOS to reconnect.
    // A non-transient close supersedes (discards) events already held for `remote`.
    bool offer(const std::string& remote, int32_t reason, bool directPeer, uint64_t nowMs,
               std::function<void()> forward, std::function<void()> reaccept);

    // Authoritative news that `remote` is gone (it left or was removed from the lobby): hand its held
    // events to the game right away instead of waiting. Returns the number forwarded.
    size_t release(const std::string& remote);
    size_t releaseAll();

    // The game closed the connection itself: drop held events for `remote` without forwarding.
    size_t onGameClosed(const std::string& remote);
    size_t onGameClosedAll();

    bool isHeld(const std::string& remote) const;
    size_t heldCount() const;

    // Runs reaccept timers and forwards expired events. Events of peers for which `directAlive`
    // returns true never expire. Returns the remotes whose events were forwarded.
    std::vector<std::string> poll(uint64_t nowMs, const std::function<bool(const std::string&)>& directAlive);

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

    size_t dropLocked(const std::string& remote);

    Options opt_;
    mutable std::mutex mu_;
    std::unordered_set<std::string> established_;  // peers that had a working connection before
    std::vector<Held> held_;
};

}  // namespace dn
