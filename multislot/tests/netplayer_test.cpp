// W2 player sync without the game (netplayer.h): the block format, the game's bin element inside an
// eos::Serialize-shaped buffer, the sender's velocity, and a simulated remote copy driven by StepRemote through
// latency, jitter, a jump, a stop and a warp. The game's own Serialize functions are exercised against the same
// format by GameNet_playersync.
//   NetPlayerTests
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../src/netfeature.h"
#include "../src/netplayer.h"

using namespace multislot;

namespace {

int checks = 0, failures = 0;
void Check(bool ok, const std::string& label) {
    ++checks;
    if (!ok) {
        std::printf("FAIL: %s\n", label.c_str());
        ++failures;
    }
}
bool Near(float a, float b, float tolerance) { return std::fabs(a - b) <= tolerance; }
bool Same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

struct alignas(16) FakeSerialize {
    unsigned char bytes[0x600]{};
    void SetEnd(std::uint64_t end) { std::memcpy(bytes + kSerializeEnd, &end, sizeof(end)); }
    void SetPos(std::uint64_t pos) { std::memcpy(bytes + kSerializeReadPos, &pos, sizeof(pos)); }
    std::uint64_t End() const {
        std::uint64_t v = 0;
        std::memcpy(&v, bytes + kSerializeEnd, sizeof(v));
        return v;
    }
    std::uint64_t Pos() const {
        std::uint64_t v = 0;
        std::memcpy(&v, bytes + kSerializeReadPos, sizeof(v));
        return v;
    }
};

void TestBlock() {
    PlayerSample sample;
    sample.seq = 0xBEEF;
    sample.senderMs = 0xFFFFFFF0u;
    sample.position = {1234.5678f, -12.25f, 987.0625f};
    sample.velocity = {6.5f, 9.8f, -3.25f};
    std::uint8_t block[64]{};
    Check(EncodePlayerBlock(sample, block, kPlayerBlockBytes - 1) == 0, "encode refuses a short buffer");
    Check(EncodePlayerBlock(sample, block, sizeof(block)) == kPlayerBlockBytes, "encode writes kPlayerBlockBytes");
    PlayerSample out;
    Check(DecodePlayerBlock(block, kPlayerBlockBytes, out), "decode");
    Check(out.seq == sample.seq && out.senderMs == sample.senderMs && Same(out.position, sample.position) &&
              Same(out.velocity, sample.velocity),
          "decode gives back every field exactly (full precision, unlike the game's half floats)");
    Check(!DecodePlayerBlock(block, kPlayerBlockBytes - 1, out), "decode refuses a short block");
    block[0] = 2;
    Check(!DecodePlayerBlock(block, kPlayerBlockBytes, out), "decode refuses another version");
    block[0] = kPlayerBlockVersion;
    const float nan = std::nanf("");
    std::memcpy(block + 7, &nan, sizeof(nan));
    Check(!DecodePlayerBlock(block, kPlayerBlockBytes, out), "decode refuses a non-finite position");
}

void TestBin() {
    FakeSerialize s;
    s.SetEnd(5);  // other fields before ours
    const std::uint8_t data[31] = {1, 2, 3};
    Check(WriteBinElement(s.bytes, data, sizeof(data)), "bin written");
    Check(s.End() == 5 + 2 + 31, "bin length is header + data");
    Check(s.bytes[kSerializeData + 5] == 0xA0 && s.bytes[kSerializeData + 6] == 31, "bin tag as EDF+12B5200 writes it");
    s.SetPos(5);
    std::uint8_t out[64]{};
    std::size_t size = 0;
    Check(ReadBinElement(s.bytes, out, sizeof(out), size) && size == 31 && out[2] == 3, "bin read back");
    Check(s.Pos() == s.End(), "read moved to the end");
    Check(!ReadBinElement(s.bytes, out, sizeof(out), size), "nothing left to read");
    // A truncated element, a wrong tag and a too small buffer move nothing.
    s.SetPos(5);
    s.SetEnd(5 + 2 + 30);
    Check(!ReadBinElement(s.bytes, out, sizeof(out), size) && s.Pos() == 5, "truncated element refused");
    s.SetEnd(5 + 2 + 31);
    Check(!ReadBinElement(s.bytes, out, 30, size) && s.Pos() == 5, "element larger than the buffer refused");
    s.bytes[kSerializeData + 5] = 0xC2;  // the game's full float tag
    Check(!ReadBinElement(s.bytes, out, sizeof(out), size) && s.Pos() == 5, "another element type refused");
    FakeSerialize full;
    full.SetEnd(kSerializeCapacity - 10);
    Check(!WriteBinElement(full.bytes, data, sizeof(data)) && full.End() == kSerializeCapacity - 10,
          "a write that does not fit writes nothing");
}

void TestSender() {
    SendTrack track;
    PlayerSample a = MakeSample(track, {0, 0, 0}, 1000.0);
    Check(a.seq == 1 && Length(a.velocity) == 0.0f, "first sample: no velocity");
    PlayerSample b = MakeSample(track, {0.2f, 0.1f, 0}, 1033.3333);
    Check(b.seq == 2 && Near(b.velocity.x, 6.0f, 0.01f) && Near(b.velocity.y, 3.0f, 0.01f), "velocity over 33 ms");
    PlayerSample c = MakeSample(track, {50.0f, 0.1f, 0}, 1066.6666);
    Check(Length(c.velocity) == 0.0f, "a warp carries no velocity");
    MakeSample(track, {50.0f, 0.1f, 0}, 2000.0);
    PlayerSample d = MakeSample(track, {51.0f, 0.1f, 0}, 2600.0);
    Check(Length(d.velocity) == 0.0f, "a long gap carries no velocity");
    Check(d.senderMs == 2600u, "sender time in ms");
}

void TestAcceptAndClock() {
    NetPlayerParams p;
    RemoteTrack t;
    PlayerSample s;
    s.seq = 10;
    s.senderMs = 0xFFFFFF00u;
    Check(AcceptSample(t, s, 5000.0, p), "first sample");
    s.seq = 10;
    Check(!AcceptSample(t, s, 5010.0, p), "repeated seq dropped");
    s.seq = 9;
    Check(!AcceptSample(t, s, 5020.0, p), "older seq dropped");
    s.seq = 11;
    s.senderMs = 0xFFFFFF00u + 0x100u;  // wraps
    Check(AcceptSample(t, s, 5000.0 + 256.0 + 30.0, p), "newer seq across the sender clock wrap");
    // offset: first 5000 - senderTime; second delay 30 ms more than the first, so the baseline creeps 0.5 ms
    Check(Near(static_cast<float>(t.offset - (5000.0 - static_cast<double>(0xFFFFFF00u))), p.clockCreepMs, 0.01f),
          "a slower sample moves the latency baseline only by the creep");
    s.seq = 12;
    s.senderMs += 100;
    AcceptSample(t, s, 5000.0 + 356.0 - 10.0, p);
    Check(Near(static_cast<float>(t.offset - (5000.0 - static_cast<double>(0xFFFFFF00u))), -10.0f, 0.01f),
          "a faster sample sets the baseline");
    s.seq = 1;  // a new life of that player after a pause: the old sequence does not matter
    Check(AcceptSample(t, s, 9000.0, p) && t.last.seq == 1, "stale track starts over");
    Check(TrackFresh(t, 9000.0 + p.staleMs, p) && !TrackFresh(t, 9001.0 + p.staleMs, p), "freshness window");
}

void TestEstimate() {
    NetPlayerParams p;
    RemoteTrack t;
    PlayerSample s;
    s.seq = 1;
    s.senderMs = 100000;
    s.position = {10, 0, 0};
    s.velocity = {5, 0, 0};
    AcceptSample(t, s, 200000.0, p);
    Check(Near(EstimatePosition(t, 200000.0, p).x, 10.0f, 1e-4f), "estimate at arrival is the sample");
    Check(Near(EstimatePosition(t, 200100.0, p).x, 10.5f, 1e-3f), "100 ms later: +0.5 m");
    Check(Near(EstimatePosition(t, 201000.0, p).x, 11.0f, 1e-3f), "extrapolation capped at maxExtrapolateMs");
    s.seq = 2;
    s.senderMs = 100050;
    s.velocity = {};
    s.position = {10.25f, 0, 0};
    AcceptSample(t, s, 200050.0, p);
    Check(Near(EstimatePosition(t, 200150.0, p).x, 10.25f, 1e-4f), "a stopped player is estimated where it stopped");
}

// The remote copy as the game moves it: its own motion (from replicated input, possibly wrong) plus what StepRemote
// adds each frame. The true player sends samples every `sendMs`, which arrive `latencyMs` (+ jitter) later.
struct Sim {
    NetPlayerParams p;
    RemoteTrack track;
    double now = 0.0;
    Vec3 copy;
    std::uint16_t seq = 0;
    struct Pending {
        double arrive;
        PlayerSample sample;
    };
    std::vector<Pending> inFlight;
    int warps = 0;

    // truth(t), the copy's own velocity(t)
    template <typename Truth, typename Own>
    float Run(double untilMs, Truth truth, Own own, double sendMs, double latencyMs, bool jitter, double measureFromMs,
              Vec3 truthVel(double)) {
        float worst = 0.0f;
        double nextSend = now;
        while (now < untilMs) {
            if (now >= nextSend) {
                PlayerSample s;
                s.seq = ++seq;
                s.senderMs = static_cast<std::uint32_t>(now + 1000000.0);
                s.position = truth(now);
                s.velocity = truthVel(now);
                const double extra = jitter ? (seq * 7919 % 40) : 0.0;
                inFlight.push_back({now + latencyMs + extra, s});
                nextSend += sendMs;
            }
            for (auto it = inFlight.begin(); it != inFlight.end();) {
                if (it->arrive <= now) {
                    AcceptSample(track, it->sample, now, p);
                    it = inFlight.erase(it);
                } else {
                    ++it;
                }
            }
            const double dt = 1000.0 / 60.0;
            if (TrackFresh(track, now, p)) {
                const RemoteStep step = StepRemote(track, copy, now, p);
                if (step.warp) {
                    copy = step.target;
                    ++warps;
                } else {
                    copy = copy + own(now) * static_cast<float>(dt / 1000.0) + step.add;
                }
                copy.y = (std::max)(copy.y, 0.0f);  // the ground, as the proxy's collision keeps it
            } else {
                copy = copy + own(now) * static_cast<float>(dt / 1000.0);
            }
            now += dt;
            // Compare with where the player was one latency ago at the most: what any extrapolation can know.
            if (now >= measureFromMs) worst = (std::max)(worst, Length(copy - truth(now - latencyMs)));
        }
        return worst;
    }
};

Vec3 Zero(double) { return {}; }

void TestConverge() {
    // A player runs at 6 m/s; the copy stands still on its own (its input never arrived). With latency the copy
    // must still track the true position closely once the latency baseline is learnt.
    {
        Sim sim;
        sim.copy = {0, 0, 0};
        auto truth = [](double t) { return Vec3{static_cast<float>(6.0 * t / 1000.0), 0, 0}; };
        const float worst = sim.Run(
            3000.0, truth, [](double) { return Vec3{}; }, 33.0, 60.0, false, 1000.0, +[](double) { return Vec3{6, 0, 0}; });
        // The receiver cannot know the one-way latency (60 ms): it shows the player ~latency behind, 0.36 m.
        Check(worst < 0.12f, "running player, copy idle: error vs the position one latency ago (" + std::to_string(worst) + " m)");
    }
    // The copy runs at 6 m/s on its own, the player stops at t=1 s: the copy must stop and settle, not slide on.
    {
        Sim sim;
        auto truth = [](double t) { return Vec3{static_cast<float>(6.0 * (std::min)(t, 1000.0) / 1000.0), 0, 0}; };
        sim.Run(
            1500.0, truth, [](double) { return Vec3{6, 0, 0}; }, 33.0, 40.0, false, 1e9,
            +[](double t) { return t < 1000.0 ? Vec3{6, 0, 0} : Vec3{}; });
        const float after = Length(sim.copy - Vec3{6, 0, 0});
        Check(after < 0.05f, "player stopped: copy that would run on is held within 5 cm after 0.5 s (" +
                                  std::to_string(after) + " m)");
    }
    // A jump the copy does not reproduce (no vertical motion of its own): the vertical axis is corrected too.
    {
        Sim sim;
        auto truth = [](double t) {
            const double s = t / 1000.0 - 1.0;
            const double y = s > 0.0 && s < 1.0 ? 8.0 * s - 8.0 * s * s : 0.0;  // 2 m high, 1 s long
            return Vec3{0, static_cast<float>(y), 0};
        };
        const float worst = sim.Run(
            3000.0, truth, [](double) { return Vec3{}; }, 33.0, 30.0, false, 500.0, +[](double t) {
                const double s = t / 1000.0 - 1.0;
                return Vec3{0, static_cast<float>(s > 0.0 && s < 1.0 ? 8.0 - 16.0 * s : 0.0), 0};
            });
        // What is left is the send interval: a sample is up to 33 ms old when the velocity flips (take-off,
        // landing), 8 m/s * 33 ms = 0.26 m for one interval.
        Check(worst < 0.3f, "unreproduced jump: vertical error stays small (" + std::to_string(worst) + " m)");
        Check(std::fabs(sim.copy.y) <= sim.p.deadband + 0.005f, "and the copy lands with the player (within the deadband)");
    }
    // Jitter (0-40 ms on 50 ms latency) and the copy's own motion 30% too fast: no oscillation, bounded error.
    {
        Sim sim;
        auto truth = [](double t) { return Vec3{static_cast<float>(10.0 * t / 1000.0), 0, 0}; };
        const float worst = sim.Run(
            4000.0, truth, [](double) { return Vec3{13, 0, 0}; }, 33.0, 50.0, true, 1500.0,
            +[](double) { return Vec3{10, 0, 0}; });
        Check(worst < 0.3f, "jitter + a copy 30% too fast: bounded (" + std::to_string(worst) + " m)");
        Check(sim.warps == 0, "no warps for small errors");
    }
    // Far apart (a respawn): one warp, then tracking.
    {
        Sim sim;
        sim.copy = {100, 0, 0};
        sim.Run(
            500.0, [](double) { return Vec3{0, 0, 0}; }, [](double) { return Vec3{}; }, 33.0, 20.0, false, 1e9, &Zero);
        Check(sim.warps == 1 && Length(sim.copy) < 0.01f, "a far copy warps once to the estimate");
    }
    // Samples stop: the track goes stale, the game's own path takes over (StepRemote is no longer called).
    {
        NetPlayerParams p;
        RemoteTrack t;
        PlayerSample s;
        s.seq = 1;
        AcceptSample(t, s, 0.0, p);
        Check(!TrackFresh(t, p.staleMs + 1.0, p), "no samples: stale, back to the game's sync");
    }
    // Step size cap.
    {
        NetPlayerParams p;
        RemoteTrack t;
        PlayerSample s;
        s.seq = 1;
        s.position = {5, 0, 0};
        AcceptSample(t, s, 0.0, p);
        const RemoteStep step = StepRemote(t, {0, 0, 0}, 0.0, p);
        Check(!step.warp && Length(step.add) <= p.maxSpeed / 60.0f + 1e-4f, "one step adds at most maxSpeed * dt");
    }
}

// Review fixes (2026-10-07).
void TestReviewFixes() {
    // 1. The flush interval follows this machine's own PlayerSync switch.
    Check(EffectiveFlushIntervalMs(45, false) == 0, "PlayerSync off locally: the game's 90 ms flush stays");
    Check(EffectiveFlushIntervalMs(45, true) == 45, "PlayerSync on: 45 ms");
    Check(EffectiveFlushIntervalMs(0, true) == 0 && EffectiveFlushIntervalMs(90, true) == 0, "0 or 90: leave it");
    Check(EffectiveFlushIntervalMs(5, true) == 16 && EffectiveFlushIntervalMs(200, true) == 90, "clamped to 16..90");
    // 2. Samples stop arriving while the player ran at 5 m/s: once the estimate stops at the extrapolation limit,
    //    the copy must stop there too, not run on by the old velocity and get pulled back.
    {
        NetPlayerParams p;
        RemoteTrack t;
        PlayerSample s;
        s.seq = 1;
        s.velocity = {5, 0, 0};
        AcceptSample(t, s, 0.0, p);
        Vec3 copy{};
        float furthest = 0.0f;
        for (double now = 0.0; now < p.staleMs; now += 1000.0 / 60.0) {
            const RemoteStep step = StepRemote(t, copy, now, p);
            copy = copy + step.add;  // the copy has no motion of its own
            furthest = (std::max)(furthest, copy.x);
        }
        const float limit = 5.0f * p.maxExtrapolateMs / 1000.0f;
        Check(furthest <= limit + 0.02f, "no overshoot past the extrapolation limit (" + std::to_string(furthest) +
                                             " m, estimate stops at " + std::to_string(limit) + " m)");
        Check(std::fabs(copy.x - limit) <= p.deadband + 0.01f, "and it settles on the stopped estimate");
    }
    // 3. The warp threshold grows with speed: a 40 m/s boost may be 9 m off for a moment without a warp.
    {
        NetPlayerParams p;
        RemoteTrack t;
        PlayerSample s;
        s.seq = 1;
        AcceptSample(t, s, 0.0, p);
        Check(WarpDistance(t, p) == p.snapDistance, "standing: warp above snapDistance");
        Check(StepRemote(t, {9, 0, 0}, 0.0, p).warp, "standing, 9 m off: warp");
        RemoteTrack fast;
        s.velocity = {40, 0, 0};
        AcceptSample(fast, s, 0.0, p);
        Check(std::fabs(WarpDistance(fast, p) - 10.0f) < 1e-4f, "40 m/s: warp above 10 m");
        Check(!StepRemote(fast, {-9, 0, 0}, 0.0, p).warp, "40 m/s, 9 m off: no warp");
        RemoteTrack fast2;
        AcceptSample(fast2, s, 0.0, p);
        Check(StepRemote(fast2, {-11, 0, 0}, 0.0, p).warp, "40 m/s, 11 m off: warp");
    }
}

void TestFeature() {
    // An INI without [Netcode]: every switch at its default.
    InitNetFeature(L"Z:\\no-such-folder\\netplayer_test.ini");
    Check(NetFeatureEnabledLocally(NetFeature::PlayerSync), "PlayerSync on by default");
    // Outside a room the gate cannot know what the others run: no feature is active.
    Check(!NetFeatureActive(NetFeature::PlayerSync), "not in a room: PlayerSync not active");
    SetNetFeatureForTest(NetFeature::PlayerSync, true);
    Check(NetFeatureActive(NetFeature::PlayerSync), "forced on");
    SetNetFeatureForTest(NetFeature::PlayerSync, false);
    Check(!NetFeatureActive(NetFeature::PlayerSync), "forced off");
    ClearNetFeatureForTest();
}

}  // namespace

int main() {
    TestBlock();
    TestBin();
    TestSender();
    TestAcceptAndClock();
    TestEstimate();
    TestConverge();
    TestReviewFixes();
    TestFeature();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
