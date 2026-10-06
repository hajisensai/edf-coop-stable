#include "netaoi.h"

#include <algorithm>
#include <vector>

#include "log.h"
#include "netfeature.h"
#include "src/netcode.h"

namespace multislot {
namespace {

// The medium-relevance stand-in for a member nobody told about (see netaoi.h).
MemberPlace Unknown(bool observer) {
    MemberPlace p;
    if (!observer) p.position = interest::Vec3{0, 0, interest::kNearDistance};
    return p;
}

struct Places {
    std::mutex mu;
    std::map<std::string, MemberPlace> byMember;
    std::map<interest::PeerId, std::string> peers;  // the gate's observer ids, for interest::LinkBudgetBytesPerSec
} places;

std::optional<MemberPlace> PlaceOf(const std::string& member) {
    std::lock_guard<std::mutex> lock(places.mu);
    auto it = places.byMember.find(member);
    if (it == places.byMember.end()) return std::nullopt;
    return it->second;
}

std::uint32_t PathBudget(const std::string& observer) { return dn::linkBudgetBytesPerSec(observer); }

// interest.h's budget source: W1's estimate for the member behind a gate's observer id.
std::uint32_t PeerBudget(interest::PeerId peer) {
    std::string member;
    {
        std::lock_guard<std::mutex> lock(places.mu);
        auto it = places.peers.find(peer);
        if (it == places.peers.end()) return interest::kStubLinkBudget;
        member = it->second;
    }
    return PathBudget(member);
}

InterestGate& Gate() {
    static InterestGate gate(&PathBudget, &PlaceOf);
    return gate;
}

bool Filter(const std::string& observer, const std::string& subject, std::uint32_t bytes, std::uint64_t nowMs) {
    // Only where the room runs the classes: a state datagram is one the transport may replace with the next.
    if (!NetFeatureActive(NetFeature::TrafficClasses)) return true;
    return Gate().Allow(observer, subject, bytes, nowMs);
}

}  // namespace

InterestGate::InterestGate(Budget budget, Place place, std::uint64_t tickMs, std::uint64_t maxIntervalMs)
    : budget_(std::move(budget)), place_(std::move(place)), tickMs_(tickMs),
      scheduler_(maxIntervalMs > 2 * tickMs ? maxIntervalMs - 2 * tickMs : maxIntervalMs) {}

interest::SubjectId InterestGate::SubjectIdOf(const std::string& member) {
    auto it = ids_.find(member);
    if (it != ids_.end()) return it->second;
    const auto id = static_cast<interest::SubjectId>(ids_.size() + 1);
    ids_[member] = id;
    return id;
}

void InterestGate::StartTick(const std::string& observer, View& view, std::uint64_t nowMs) {
    view.tickMs = nowMs;
    view.ticked = true;
    // Subjects that stopped asking are no longer scheduled (the scheduler forgets what it is not listed).
    for (auto it = view.subjects.begin(); it != view.subjects.end();)
        it = nowMs - it->second.lastAskMs > kActiveMs ? view.subjects.erase(it) : std::next(it);
    const std::optional<MemberPlace> seen = place_ ? place_(observer) : std::nullopt;
    const MemberPlace o = seen ? *seen : Unknown(true);
    interest::Observer obs{view.id, o.position, o.facing, o.team};
    std::vector<interest::Subject> list;
    std::map<interest::SubjectId, std::string> names;
    for (const auto& [member, s] : view.subjects) {
        const std::optional<MemberPlace> at = place_ ? place_(member) : std::nullopt;
        const MemberPlace p = at ? *at : Unknown(false);
        list.push_back({s.id, p.position, p.team, p.engaged, s.bytes});
        names[s.id] = member;
    }
    const std::uint32_t budget = interest::TickBudgetBytes(budget_ ? budget_(observer) : interest::kStubLinkBudget,
                                                           static_cast<std::uint32_t>(tickMs_));
    double spare = budget;
    for (interest::SubjectId id : scheduler_.PickSendsThisTick(obs, list, budget, nowMs)) {
        SubjectState& s = view.subjects[names[id]];
        // A permission still unused was paid for already: it is not paid again.
        if (!s.permitted) spare -= s.bytes;
        s.permitted = true;
    }
    view.spare = std::max(0.0, spare);
}

bool InterestGate::Allow(const std::string& observer, const std::string& subject, std::uint32_t bytes, std::uint64_t nowMs) {
    std::lock_guard<std::mutex> lock(mu_);
    // Observers gone quiet: their state goes with them.
    for (auto it = views_.begin(); it != views_.end();) {
        if (it->first != observer && nowMs - it->second.lastAskMs > kObserverIdleMs) {
            scheduler_.Forget(it->second.id);
            it = views_.erase(it);
        } else {
            ++it;
        }
    }
    auto [vit, fresh] = views_.try_emplace(observer);
    View& view = vit->second;
    if (fresh) {
        view.id = nextPeer_++;
        std::lock_guard<std::mutex> plock(places.mu);
        places.peers[view.id] = observer;
    }
    view.lastAskMs = nowMs;
    auto [sit, newSubject] = view.subjects.try_emplace(subject);
    SubjectState& s = sit->second;
    if (newSubject) s.id = SubjectIdOf(subject);
    s.bytes = bytes;
    s.lastAskMs = nowMs;
    if (!view.ticked || nowMs - view.tickMs >= tickMs_) StartTick(observer, view, nowMs);
    if (newSubject) return true;  // a subject's first datagram always goes (the scheduler sends the never-sent first)
    if (s.permitted) {
        s.permitted = false;
        return true;
    }
    if (view.spare >= bytes) {
        view.spare -= bytes;
        return true;
    }
    return false;
}

void NoteMember(const std::string& member, const MemberPlace& place) {
    std::lock_guard<std::mutex> lock(places.mu);
    places.byMember[member] = place;
}

void ForgetMembers() {
    std::lock_guard<std::mutex> lock(places.mu);
    places.byMember.clear();
}

void InstallNetInterest(bool enabled) {
    interest::SetLinkBudgetSource(&PeerBudget);
    if (!enabled) {
        Log("NETCODE interest management: off ([Netcode] Interest=0), every state datagram goes");
        return;
    }
    dn::setStateSendFilter(&Filter);
    Log("NETCODE interest management: state datagrams follow each path's budget (W6 scheduler, at most %llu ms between "
        "two updates of anyone)",
        static_cast<unsigned long long>(interest::kMaxIntervalMs));
}

}  // namespace multislot
