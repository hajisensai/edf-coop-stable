#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace multislot::interest {

// Interest management for rooms far larger than four (docs/net-re/roomsize.md): which other players' states an
// observer is sent this tick, driven by the bandwidth the link to that observer has, not by fixed distance tiers.
//
// Every (observer, subject) pair keeps an accumulated priority. Each tick it grows by the subject's relevance to the
// observer times the time since the last tick; the tick then sends subjects from the highest accumulated priority
// down until the tick's byte budget is spent, and a sent subject's priority starts over at zero. With enough
// bandwidth every subject goes out every tick (full rate); only when the budget runs short do near, engaged or
// friendly subjects naturally go out more often than far ones. Nothing is dropped: a subject passed over is sent its
// newest state later, and one whose last send is kMaxIntervalMs old goes out this tick whatever the budget.
//
// Pure and deterministic; one Scheduler per sending machine, used on the thread that sends (W1's send scheduling).

using PeerId = std::uint64_t;     // an observer: the machine a link goes to
using SubjectId = std::uint32_t;  // what is replicated (a player index, an object id)

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Observer {
    PeerId id = 0;
    Vec3 position;
    Vec3 facing{0, 0, 1};  // unit length; the camera's direction
    std::uint32_t team = 0;
};

struct Subject {
    SubjectId id = 0;
    Vec3 position;
    std::uint32_t team = 0;
    bool engaged = false;     // firing, hit or being hit lately: its state changes fast and matters
    std::uint32_t bytes = 0;  // what one update of it costs on the wire
};

// Weights, all multiplied: distance (1 at the observer, 1/2 at kNearDistance, falling with the square beyond), in
// view, engaged, same team. Never below kMinRelevance, so a subject anywhere still accumulates.
constexpr float kNearDistance = 60.0f;  // game units (metres)
constexpr float kInViewFactor = 2.0f;
constexpr float kInViewCos = 0.5f;      // within 60 degrees of the facing
constexpr float kEngagedFactor = 4.0f;
constexpr float kTeamFactor = 1.5f;
constexpr float kMinRelevance = 0.01f;
constexpr std::uint64_t kMaxIntervalMs = 1000;
float Relevance(const Observer& observer, const Subject& subject);

// A tick's share of a link budget (bytes per second over tickMs).
std::uint32_t TickBudgetBytes(std::uint32_t bytesPerSecond, std::uint32_t tickMs);

// The measured budget of the link to `peer`, bytes per second (W1's congestion control fills it in: RTT, loss).
// Until then a stub that answers kStubLinkBudget for every peer; SetLinkBudget overrides it per peer (tests).
constexpr std::uint32_t kStubLinkBudget = 64 * 1024;
std::uint32_t LinkBudgetBytesPerSec(PeerId peer);
void SetLinkBudget(PeerId peer, std::uint32_t bytesPerSecond);  // 0 removes the override
using LinkBudgetFn = std::uint32_t (*)(PeerId peer);
void SetLinkBudgetSource(LinkBudgetFn source);  // W1's estimator; nullptr returns to the stub

// The floor the maximum interval sets costs subjects * bytes / interval on every link whatever its budget: 1023
// subjects of 140 bytes once a second are 143 KB/s. Large rooms keep it affordable with compact updates for far
// subjects (W2) or a longer interval; the floor always wins over the budget, so nobody starves.
class Scheduler {
public:
    explicit Scheduler(std::uint64_t maxIntervalMs = kMaxIntervalMs) : maxIntervalMs_(maxIntervalMs) {}
    // The subjects to send `observer` this tick, in the order chosen: first those past the maximum interval (oldest
    // first), then subjects never sent yet, then by accumulated priority, while their bytes fit what is left of
    // `budgetBytes`. A subject that does not fit is skipped and smaller ones after it may still go. Subjects not
    // listed any more are forgotten; a new one counts its interval from when it was first listed.
    std::vector<SubjectId> PickSendsThisTick(const Observer& observer, const std::vector<Subject>& subjects,
                                             std::uint32_t budgetBytes, std::uint64_t nowMs);
    // The observer left: its state goes.
    void Forget(PeerId observer);
    // For tests and diagnostics.
    float Accumulated(PeerId observer, SubjectId subject) const;
    std::uint64_t LastSent(PeerId observer, SubjectId subject) const;

private:
    struct Pair {
        float accumulated = 0;
        std::uint64_t lastSentMs = 0;
        std::uint64_t firstSeenMs = 0;
        bool sent = false;  // ever sent
        bool seen = false;  // listed this tick
    };
    struct View {
        std::uint64_t lastTickMs = 0;
        bool ticked = false;
        std::unordered_map<SubjectId, Pair> pairs;
    };
    std::uint64_t maxIntervalMs_;
    std::unordered_map<PeerId, View> views_;
};

}  // namespace multislot::interest
