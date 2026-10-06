#pragma once
// Interest management on the wire (I1): W6's scheduler (interest.h) decides which state datagrams go, with W1's
// path budgets (src/netcode.h linkBudgetBytesPerSec).
//
// The transport sees datagrams, not objects: a state datagram carries what one machine owns - its player above all -
// and goes to one member. So per datagram: observer = the member it goes to, subject = whose state it carries (the
// sender itself; when a host relays between two joiners, the original sender, see src/netcode.h StateSendFilter).
// The game makes one per sync beat (90 ms, or W2's own) per (sender, member) pair.
//
// InterestGate turns the per-tick scheduler into per-datagram answers, per observer:
//   - every tickMs a new tick starts with the observer's budget for it (TickBudgetBytes of the path's budget), and
//     PickSendsThisTick picks among every subject that asked within kActiveMs - those past the maximum interval first,
//     then by accumulated priority - as far as the budget goes. Each subject picked gets a permission that lasts
//     until its next datagram uses it (a datagram comes once per beat, a tick does not wait for it);
//   - a datagram of a subject with a permission goes and uses it up; any other goes while what the picks left of the
//     tick's budget lasts (so with budget to spare everything goes, at full rate);
//   - otherwise it is held back: the subject's next datagram carries its newest state.
// The scheduler's interval is kMaxIntervalMs less two ticks: a permission given at the interval's end is used by the
// subject's next datagram at most a beat later, so no subject goes more than kMaxIntervalMs without an update.
//
// Relevance needs where observer and subject are (positions, facing, team, engaged). NoteMember gives them (W2's
// player sync knows remote players' positions); a member nobody told about counts as a medium-relevance subject at
// kNearDistance in view, so a missing input never starves anyone and never ranks them first.
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>

#include "interest.h"

namespace multislot {

struct MemberPlace {
    interest::Vec3 position;
    interest::Vec3 facing{0, 0, 1};
    std::uint32_t team = 0;
    bool engaged = false;
};

class InterestGate {
public:
    static constexpr std::uint64_t kTickMs = 90;
    static constexpr std::uint64_t kActiveMs = 3000;   // a subject silent this long is no longer scheduled
    static constexpr std::uint64_t kObserverIdleMs = 10000;
    using Budget = std::function<std::uint32_t(const std::string& observer)>;
    using Place = std::function<std::optional<MemberPlace>(const std::string& member)>;
    InterestGate(Budget budget, Place place, std::uint64_t tickMs = kTickMs,
                 std::uint64_t maxIntervalMs = interest::kMaxIntervalMs);
    // Whether a state datagram of `bytes` for `subject` goes to `observer` now, the path to it carrying `budget` bytes a
    // second (0: ask the Budget function). Thread-safe; calls nothing outside the gate but the Place function.
    bool Allow(const std::string& observer, const std::string& subject, std::uint32_t bytes, std::uint32_t budget,
               std::uint64_t nowMs);

private:
    struct SubjectState {
        interest::SubjectId id = 0;
        std::uint32_t bytes = 0;
        std::uint64_t lastAskMs = 0;
        bool permitted = false;
    };
    struct View {
        interest::PeerId id = 0;
        std::uint64_t tickMs = 0;
        bool ticked = false;
        double spare = 0;  // bytes of this tick's budget the picks left
        std::uint64_t lastAskMs = 0;
        std::map<std::string, SubjectState> subjects;
    };
    void StartTick(const std::string& observer, View& view, std::uint32_t budget, std::uint64_t nowMs);
    interest::SubjectId SubjectIdOf(const std::string& member);

    Budget budget_;
    Place place_;
    std::uint64_t tickMs_;
    interest::Scheduler scheduler_;
    std::mutex mu_;
    std::map<std::string, View> views_;
    std::map<std::string, interest::SubjectId> ids_;  // stable small ids for members
    interest::PeerId nextPeer_ = 1;
};

// Where members are, for relevance (W2's player sync, or anything that knows). Any thread.
void NoteMember(const std::string& member, const MemberPlace& place);
void ForgetMembers();
// One member whose player is gone: back to the medium-relevance default.
void ForgetMember(const std::string& member);
// Puts the gate between the game and the wire: interest::SetLinkBudgetSource reads W1's path budgets, and every
// state datagram (our own and those a host relays) asks the gate first ([Netcode] Interest=1, the default; it acts
// only while the room runs the traffic classes).
void InstallNetInterest(bool enabled);

}  // namespace multislot
