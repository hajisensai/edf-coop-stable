#pragma once
#include <array>

#include "patches.h"

namespace multislot {

// One track for every supported player. An inactive object yields its slot first when a new player arrives.
template <typename Track>
class PlayerTracks {
public:
    Track* Find(const void* soldier, double now, bool create) {
        Entry* oldest = &entries_[0];
        for (auto& entry : entries_) {
            if (entry.soldier == soldier) {
                entry.touched = now;
                return &entry.track;
            }
            if (entry.touched < oldest->touched) oldest = &entry;
        }
        if (!create) return nullptr;
        *oldest = Entry{soldier, now, Track{}};
        return &oldest->track;
    }

private:
    struct Entry {
        const void* soldier = nullptr;
        double touched = -1.0;
        Track track{};
    };
    std::array<Entry, kMaxPlayers> entries_{};
};

}  // namespace multislot
