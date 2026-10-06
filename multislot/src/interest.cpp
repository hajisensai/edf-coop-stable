#include "interest.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

namespace multislot::interest {
namespace {

std::mutex overridesLock;
std::unordered_map<PeerId, std::uint32_t> overrides;
std::atomic<LinkBudgetFn> source{nullptr};

}  // namespace

float Relevance(const Observer& observer, const Subject& subject) {
    const float dx = subject.position.x - observer.position.x;
    const float dy = subject.position.y - observer.position.y;
    const float dz = subject.position.z - observer.position.z;
    const float squared = dx * dx + dy * dy + dz * dz;
    float weight = 1.0f / (1.0f + squared / (kNearDistance * kNearDistance));
    const float distance = std::sqrt(squared);
    if (distance > 0.0f) {
        const float cosine = (dx * observer.facing.x + dy * observer.facing.y + dz * observer.facing.z) / distance;
        if (cosine >= kInViewCos) weight *= kInViewFactor;
    } else {
        weight *= kInViewFactor;
    }
    if (subject.engaged) weight *= kEngagedFactor;
    if (subject.team == observer.team) weight *= kTeamFactor;
    return weight > kMinRelevance ? weight : kMinRelevance;
}

std::uint32_t TickBudgetBytes(std::uint32_t bytesPerSecond, std::uint32_t tickMs) {
    return static_cast<std::uint32_t>(static_cast<std::uint64_t>(bytesPerSecond) * tickMs / 1000);
}

std::uint32_t LinkBudgetBytesPerSec(PeerId peer) {
    {
        const std::lock_guard<std::mutex> lock(overridesLock);
        const auto found = overrides.find(peer);
        if (found != overrides.end()) return found->second;
    }
    const LinkBudgetFn measured = source.load();
    return measured ? measured(peer) : kStubLinkBudget;
}

void SetLinkBudget(PeerId peer, std::uint32_t bytesPerSecond) {
    const std::lock_guard<std::mutex> lock(overridesLock);
    if (bytesPerSecond)
        overrides[peer] = bytesPerSecond;
    else
        overrides.erase(peer);
}

void SetLinkBudgetSource(LinkBudgetFn measured) { source.store(measured); }

std::vector<SubjectId> Scheduler::PickSendsThisTick(const Observer& observer, const std::vector<Subject>& subjects,
                                                    std::uint32_t budgetBytes, std::uint64_t nowMs) {
    View& view = views_[observer.id];
    const float elapsed = view.ticked && nowMs > view.lastTickMs ? static_cast<float>(nowMs - view.lastTickMs) : 0.0f;
    view.lastTickMs = nowMs;
    view.ticked = true;
    for (auto& [id, pair] : view.pairs) pair.seen = false;

    struct Candidate {
        const Subject* subject;
        Pair* pair;
        bool forced;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(subjects.size());
    for (const Subject& subject : subjects) {
        const bool fresh = view.pairs.find(subject.id) == view.pairs.end();
        Pair& pair = view.pairs[subject.id];
        if (fresh) pair.firstSeenMs = nowMs;
        pair.seen = true;
        pair.accumulated += Relevance(observer, subject) * elapsed;
        const std::uint64_t since = pair.sent ? pair.lastSentMs : pair.firstSeenMs;
        const bool forced = nowMs - since >= maxIntervalMs_;
        candidates.push_back({&subject, &pair, forced});
    }
    std::erase_if(view.pairs, [](const auto& entry) { return !entry.second.seen; });

    // Forced first, the longest waiting first; then never sent; then the highest accumulated priority. Ties by id,
    // so the order is the same on every run.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        if (a.forced != b.forced) return a.forced;
        const std::uint64_t sinceA = a.pair->sent ? a.pair->lastSentMs : a.pair->firstSeenMs;
        const std::uint64_t sinceB = b.pair->sent ? b.pair->lastSentMs : b.pair->firstSeenMs;
        if (a.forced && sinceA != sinceB) return sinceA < sinceB;
        if (a.pair->sent != b.pair->sent) return !a.pair->sent;
        if (!a.forced && a.pair->accumulated != b.pair->accumulated) return a.pair->accumulated > b.pair->accumulated;
        return a.subject->id < b.subject->id;
    });

    std::vector<SubjectId> picked;
    std::uint64_t left = budgetBytes;
    for (const Candidate& candidate : candidates) {
        const std::uint32_t bytes = candidate.subject->bytes;
        if (!candidate.forced && bytes > left) continue;
        left = bytes > left ? 0 : left - bytes;
        candidate.pair->accumulated = 0;
        candidate.pair->lastSentMs = nowMs;
        candidate.pair->sent = true;
        picked.push_back(candidate.subject->id);
    }
    return picked;
}

void Scheduler::Forget(PeerId observer) { views_.erase(observer); }

float Scheduler::Accumulated(PeerId observer, SubjectId subject) const {
    const auto view = views_.find(observer);
    if (view == views_.end()) return 0;
    const auto pair = view->second.pairs.find(subject);
    return pair == view->second.pairs.end() ? 0 : pair->second.accumulated;
}

std::uint64_t Scheduler::LastSent(PeerId observer, SubjectId subject) const {
    const auto view = views_.find(observer);
    if (view == views_.end()) return 0;
    const auto pair = view->second.pairs.find(subject);
    return pair == view->second.pairs.end() ? 0 : pair->second.lastSentMs;
}

}  // namespace multislot::interest
