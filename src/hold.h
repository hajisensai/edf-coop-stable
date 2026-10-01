// Hides transient EOS P2P connection loss from the game.
//
// When EOS reports that a connection closed for a transient reason (timeout, network failure),
// the game would normally drop that player at once. Instead the event is held back while the plugin
// asks EOS to reconnect. If EOS re-establishes the connection in time the game never learns about the
// hiccup; otherwise the original event is handed to the game.
//
// Holding is only safe when the other side does the same: a player without the plugin drops us for
// real, and hiding that would leave a ghost player on our side. EDF6 reads every EOS channel as game
// data, so the plugin is detected through the lobby instead (see lobby_marker.h). Eligible peers:
//   * direct-link members (they run the plugin by definition), held for as long as the direct link
//     is alive because the game's traffic to them no longer flows over EOS at all;
//   * peers that carry the plugin's lobby marker, or any peer with `holdAll`, provided they had a
//     working EOS connection before; held for `graceMs`.
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
    bool offer(const std::string& remote, int32_t reason, bool directPeer, bool pluginPeer, uint64_t nowMs,
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

// Hides "member disconnected" lobby statuses while the direct link shows the member still plays.
//
// That status means the member lost Epic's lobby service, not the game: its game traffic runs over
// the direct link and may be perfectly fine, yet the game removes it at once. EOS puts such a member
// back into the lobby (a new "joined") when its lobby connection recovers; that pair is swallowed and
// the game never learns about it. The disconnect reaches the game only after the member's direct link
// has been silent for `graceMs`, or when the member leaves or is kicked for real.
// All methods are thread-safe; `deliver` callbacks run without the internal lock held.
class LobbyStatusHold {
public:
    explicit LobbyStatusHold(uint32_t graceMs) : graceMs_(graceMs) {}

    // A "disconnected" status for `remote`. Held (true) when `reachable` over the direct link, or when
    // one is already held (the first one stays); `deliver` then runs later only if it does not return.
    bool offer(const std::string& remote, bool reachable, uint64_t nowMs, std::function<void()> deliver);
    // The general form. `key` names the held status (a member, or a room-wide slot such as "#room");
    // `probe` is whose direct link poll() checks. The status is delivered once that link has been down
    // for `graceMs` (0: on the first poll that finds it down). A held key keeps its first status unless
    // `replace`, which swaps in the newer one (a newer owner supersedes the older).
    bool offer(const std::string& key, const std::string& probe, bool reachable, uint32_t graceMs, bool replace,
               uint64_t nowMs, std::function<void()> deliver);
    // Forgets the status held under `key` without delivering it (it no longer applies). False if none.
    bool discard(const std::string& key);
    // Any other status for `remote`. Returns true when that status must be swallowed: the member joined
    // again while its disconnect was hidden. A promotion ends the hold and still reaches the game.
    // Anything else (left, kicked) delivers the hidden disconnect first, in the order EOS reported.
    bool onStatus(const std::string& remote, int32_t status);
    // The game itself gave up on `remote` (it is kicking it): deliver its hidden disconnect on the next
    // poll whatever the direct link says. Not delivered right here: the caller runs inside the game's
    // own call. Returns false when nothing was hidden.
    bool abandon(const std::string& remote);
    // Delivers every hidden disconnect (the room was closed). Returns the number delivered.
    size_t releaseAll();
    // We left the room: forget the hidden disconnects without delivering them.
    size_t clear();

    bool isHeld(const std::string& remote) const;
    size_t heldCount() const;

    // Delivers the disconnects of members `reachable` has reported unreachable for `graceMs`.
    // Returns those members.
    std::vector<std::string> poll(uint64_t nowMs, const std::function<bool(const std::string&)>& reachable);

private:
    struct Held {
        std::string remote;          // the key
        std::string probe;           // whose direct link keeps it held
        uint32_t graceMs = 0;
        uint64_t reachableAtMs = 0;  // last time the direct link showed the member alive
        bool abandoned = false;
        std::function<void()> deliver;
    };

    uint32_t graceMs_;
    mutable std::mutex mu_;
    std::vector<Held> held_;
};

// Who owns the room, as far as the game is concerned.
//
// Epic hands the lobby to another member when the owner loses Epic's lobby service, although the
// owner still hosts the game over the direct link. The game would follow Epic: a new host, and that
// host's game removing players (it kicked the real host itself). The room keeps the owner it was
// created with (the pin) for as long as the pinned owner's direct link is alive; Epic's ownership is
// tracked separately so a legitimate kick (by the pinned owner) is still told from a usurper's.
// Pure bookkeeping, no EOS calls. Thread-safe.
class LobbyOwnerPin {
public:
    // Entered a room owned by `owner` ("" when not known yet: pinned by the first promotion seen).
    void entered(const std::string& owner);
    void left();

    struct Promotion {
        bool hide = false;         // the game must not see this promotion
        bool promoteBack = false;  // we were made owner over the pinned owner: hand the lobby back
    };
    // Epic made `target` the owner. `pinnedReachable`: the pinned owner's direct link is alive.
    Promotion onPromoted(const std::string& target, const std::string& self, bool pinnedReachable);
    // The pinned owner is back in the lobby. True when Epic's owner is `self`: hand the lobby back now.
    bool onPinnedJoined(const std::string& self) const;
    // A hidden promotion reached the game after all (the pinned owner's link died): the room follows it.
    void follow(const std::string& owner);
    // A kick reaches the game only when Epic's owner, who issued it, is the pinned owner.
    bool kickAuthorized() const;

    std::string pinned() const;
    // Epic's owner when it is not the pinned one ("" otherwise).
    std::string usurper() const;

private:
    mutable std::mutex mu_;
    std::string pinned_, epic_;
};

}  // namespace dn
