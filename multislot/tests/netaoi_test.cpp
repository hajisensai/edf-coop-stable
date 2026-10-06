// Interest management on the wire (netaoi.h): with budget to spare every state datagram goes; under a tight budget
// near and engaged players go more often than far ones; nobody goes more than a second without an update.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "../src/netaoi.h"

using namespace multislot;

namespace {

int failures = 0, checks = 0;
void Check(bool condition, const std::string& what) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

struct Run {
    std::map<std::string, int> sent, asked;
    std::map<std::string, std::uint64_t> gap, last;
};

// `subjects` send a `bytes` state datagram each to one observer every 90 ms (each at its own phase) for `ms`.
Run Play(InterestGate& gate, const std::vector<std::string>& subjects, std::uint32_t bytes, std::uint64_t startMs,
         std::uint64_t ms) {
    Run r;
    for (std::uint64_t t = startMs; t < startMs + ms; t += 10)
        for (std::size_t i = 0; i < subjects.size(); ++i) {
            if ((t / 10 + i * 3) % 9 != 0) continue;  // every 90 ms, phases spread
            const std::string& s = subjects[i];
            ++r.asked[s];
            if (!gate.Allow("observer", s, bytes, t)) continue;
            ++r.sent[s];
            if (r.last.count(s)) r.gap[s] = std::max(r.gap[s], t - r.last[s]);
            r.last[s] = t;
        }
    return r;
}

void TestBudgetDriven() {
    std::uint32_t budget = 64 * 1024;
    std::map<std::string, MemberPlace> where;
    MemberPlace self;
    where["observer"] = self;
    std::vector<std::string> subjects;
    for (int i = 0; i < 10; ++i) {
        const std::string name = "s" + std::to_string(i);
        subjects.push_back(name);
        MemberPlace p;
        p.position = interest::Vec3{0, 0, 20.0f + 60.0f * static_cast<float>(i)};  // s0 near ... s9 far
        where[name] = p;
    }
    where["s9"].engaged = false;
    where["s1"].engaged = true;
    InterestGate gate([&](const std::string&) { return budget; },
                      [&](const std::string& m) -> std::optional<MemberPlace> {
                          auto it = where.find(m);
                          return it == where.end() ? std::nullopt : std::optional<MemberPlace>(it->second);
                      });
    // 10 subjects x 120 bytes x 11/s = 13 KB/s: 64 KB/s carries them all, every one of them.
    Run full = Play(gate, subjects, 120, 1000, 5000);
    for (const auto& s : subjects)
        Check(full.sent[s] == full.asked[s], s + ": with budget to spare every datagram goes (" + std::to_string(full.sent[s]) +
                                                 " of " + std::to_string(full.asked[s]) + ")");
    // 3 KB/s: about a quarter of what they make.
    budget = 3 * 1024;
    Run tight = Play(gate, subjects, 120, 6000, 10000);
    int total = 0;
    for (const auto& s : subjects) total += tight.sent[s];
    std::printf("tight budget: near s0 %d, engaged s1 %d, far s8 %d, far s9 %d of %d each; longest gaps %llu / %llu ms\n",
                tight.sent["s0"], tight.sent["s1"], tight.sent["s8"], tight.sent["s9"], tight.asked["s0"],
                static_cast<unsigned long long>(tight.gap["s0"]), static_cast<unsigned long long>(tight.gap["s9"]));
    Check(total < full.asked["s0"] * 10 * 10 / 5 / 2, "under a tight budget fewer go");
    Check(tight.sent["s0"] > tight.sent["s9"] && tight.sent["s1"] > tight.sent["s8"],
          "near and engaged players go more often than far ones");
    for (const auto& s : subjects) {
        Check(tight.sent[s] > 0, s + " is never starved");
        Check(tight.gap[s] <= interest::kMaxIntervalMs, s + " never goes more than a second without an update (longest " +
                                                            std::to_string(tight.gap[s]) + " ms)");
    }
    // Budget back: full rate again.
    budget = 64 * 1024;
    Run back = Play(gate, subjects, 120, 16000, 3000);
    // (The tick that began under the old budget ends first: one datagram each at most.)
    for (const auto& s : subjects)
        Check(back.sent[s] + 1 >= back.asked[s], s + ": the budget back, everything goes again (" +
                                                     std::to_string(back.sent[s]) + " of " + std::to_string(back.asked[s]) + ")");
    // Nothing at all to spend: still once a second each.
    budget = 0;
    Run none = Play(gate, subjects, 120, 19000, 5000);
    for (const auto& s : subjects)
        Check(none.sent[s] >= 4 && none.gap[s] <= interest::kMaxIntervalMs, s + ": no budget, still at least once a second");
}

void TestUnknownPlaces() {
    // Nobody says where anyone is: everyone counts the same, nobody starves, nobody hogs.
    std::uint32_t budget = 2 * 1024;
    InterestGate gate([&](const std::string&) { return budget; }, [](const std::string&) { return std::nullopt; });
    std::vector<std::string> subjects = {"a", "b", "c", "d", "e", "f"};
    Run r = Play(gate, subjects, 120, 1000, 10000);
    const auto [lo, hi] = std::minmax_element(subjects.begin(), subjects.end(),
                                              [&](const std::string& x, const std::string& y) { return r.sent[x] < r.sent[y]; });
    std::printf("unknown places: %d..%d sends each of %d\n", r.sent[*lo], r.sent[*hi], r.asked["a"]);
    Check(r.sent[*lo] > 0 && r.sent[*hi] <= r.sent[*lo] * 2, "without places every subject gets a fair share");
    for (const auto& s : subjects) Check(r.gap[s] <= interest::kMaxIntervalMs, s + ": at most a second apart");
}

}  // namespace

int main() {
    TestBudgetDriven();
    TestUnknownPlaces();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
