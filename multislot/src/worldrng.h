#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace multislot {

// Enemy random state sync (netcode rewrite W4, docs/net-re/world.md section 4): the pure part, no game memory.
//
// Every enemy draws its AI's random numbers from a per-object LCG at object +0x490 (and +0x3E8), seeded the same
// way on every machine. Only the insects (ants, bees, spiders, Nephila, small squids) have the owner send that state
// to the copies; every other enemy keeps step only as long as each machine draws exactly as often, and one extra
// draw (an AI branch taken on one machine because a player stood elsewhere there) splits the copies for the rest of
// the enemy's life. The owner now sends both states every EnemyRngSyncMs, at the frame its target choice runs,
// and the copies take them over.

// The message: a block after the small-int type kRngSyncTag, through the object's own NetworkObject (+0x80), as
// the game's HP messages travel (docs/net-re/damage.md section 3).
constexpr std::int8_t kRngSyncTag = 14;  // GameObjectBase takes 0-3, VehicleBase 4-5, hit authority 13
constexpr std::uint8_t kRngSyncMagic0 = 'R', kRngSyncMagic1 = 'S', kRngSyncVersion = 1;
constexpr std::size_t kRngSyncBytes = 3 + 4 + 8 + 8;

struct RngSync {
    std::uint32_t seq = 0;     // per sending machine, one more for every message
    std::uint64_t state = 0;   // object +0x490
    std::uint64_t state2 = 0;  // object +0x3E8
};

// Little endian; returns the bytes written (kRngSyncBytes) or 0 when `capacity` is too small.
std::size_t WriteRngSync(const RngSync& sync, std::uint8_t* out, std::size_t capacity);
// False for anything that is not exactly one message of this version.
bool ReadRngSync(const std::uint8_t* data, std::size_t size, RngSync& out);

// NetworkObject vtables (RVAs, at object +0x120) of the classes whose own replication already carries +0x490:
// their slot 6 reads it (GiantAnt 408400, Bee 41D6A0, Spider 4365A0, Nephila 4DDEB0, SquidSmall 3DEDA0). They are
// left to the game.
bool RngReplicatedByGame(std::uint32_t networkVtableRva);

constexpr std::int32_t kEnemyTeam = 1;  // object +0x314 (54EE88), what the Create*Enemy* natives give

struct RngSendView {
    bool active = false;                // NetFeatureActive(WorldAuthority): everyone in the room reads the message
    bool online = false;                // InSession (7748F0)
    std::uint16_t netFlags = 0;         // object +0x128
    std::int32_t team = 0;              // object +0x314
    std::uint32_t networkVtableRva = 0;  // *(object +0x120) - image base
};
// The owner of a registered enemy whose class the game does not sync already.
bool ShouldSyncRng(const RngSendView& view);

// When each owned object last sent (sender side), and the newest sequence taken per object (receiver side), by
// object address. Entries of objects not seen for kForgetMs are dropped, so a freed address that is reused starts
// over. Not thread-safe: the caller locks.
class RngSchedule {
public:
    static constexpr std::uint64_t kForgetMs = 30000;
    // True (and noted) when `object` has not sent within `intervalMs`.
    bool Due(std::uint64_t object, std::uint64_t nowMs, std::uint32_t intervalMs);
    std::size_t size() const { return last_.size(); }

private:
    void Prune(std::uint64_t nowMs);
    std::unordered_map<std::uint64_t, std::uint64_t> last_;
    std::uint64_t pruned_ = 0;
};

class RngReceiver {
public:
    static constexpr std::uint64_t kForgetMs = 30000;
    // True (and noted) when `seq` is newer than the last one taken for `object` (sequences compared by signed
    // difference, so they may wrap); the first message for an object is always taken.
    bool Accept(std::uint64_t object, std::uint32_t seq, std::uint64_t nowMs);
    std::size_t size() const { return last_.size(); }

private:
    struct Entry {
        std::uint32_t seq;
        std::uint64_t at;
    };
    void Prune(std::uint64_t nowMs);
    std::unordered_map<std::uint64_t, Entry> last_;
    std::uint64_t pruned_ = 0;
};

}  // namespace multislot
