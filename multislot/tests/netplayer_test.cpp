// W2 player sync without the game (netplayer.h): the block format, the game's bin element inside an
// eos::Serialize-shaped buffer, the sender's velocity, and a simulated remote copy driven by StepRemote through
// latency, jitter, a jump, a stop and a warp. The game's own Serialize functions are exercised against the same
// format by GameNet_playersync.
//   NetPlayerTests
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../src/netfeature.h"
#include "../src/netaoi.h"
#include "../src/netplayer.h"
#include "../src/netplayer_members.h"
#include "../src/netplayer_tracks.h"

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

// Interest management's inputs (netplayer_members.h): player object -> eos::User -> member id -> place, fed to the
// gate, which then sends a near guest's state more often than a far one's under a tight budget, and keeps a member it
// knows nothing about at medium relevance.
struct alignas(16) FakePlayer {
    unsigned char bytes[0x2000]{};
    void SetUser(const void* user) { std::memcpy(bytes + kPlayerUserOffset, &user, sizeof(user)); }
};
struct alignas(16) FakeUser {
    unsigned char bytes[0x80]{};
    void SetId(const void* id) { std::memcpy(bytes + kUserProductIdField, &id, sizeof(id)); }
};

void TestMemberPlaces() {
    static const int idA = 1, idB = 2, idNameless = 3;
    FakeUser userA, userB, nameless;
    userA.SetId(&idA);
    userB.SetId(&idB);
    nameless.SetId(&idNameless);
    FakePlayer local, remote, ghost;
    local.SetUser(userA.bytes);
    remote.SetUser(userB.bytes);
    Check(PlayerUser(local.bytes) == userA.bytes && PlayerUser(remote.bytes) == userB.bytes, "player -> its eos::User");
    Check(PlayerUser(ghost.bytes) == nullptr && PlayerUser(nullptr) == nullptr, "no user: no member");
    Check(UserProductId(userA.bytes) == &idA, "user -> ProductUserId");

    std::map<std::string, PlayerPlace> fed;
    std::vector<std::string> forgotten;
    int named = 0;
    MemberPlaces places(
        [&](const void* id) {
            ++named;
            if (id == &idA) return std::string("0002aaaa");
            if (id == &idB) return std::string("0002bbbb");
            return std::string();
        },
        [&](const std::string& member, const PlayerPlace& place) { fed[member] = place; },
        [&](const std::string& member) { forgotten.push_back(member); });
    Check(places.Observe(PlayerUser(local.bytes), {1, 2, 3}, {}, 0.0), "local player fed");
    Check(places.Observe(PlayerUser(remote.bytes), {100, 0, 0}, {0, 0, -6}, 0.0), "remote player fed");
    Check(fed.size() == 2 && fed["0002aaaa"].position.y == 2.0f && fed["0002bbbb"].position.x == 100.0f,
          "each member gets its own player's place");
    Check(fed["0002bbbb"].facing.z == -1.0f, "facing follows the motion");
    Check(!places.Observe(PlayerUser(ghost.bytes), {}, {}, 0.0), "an object without a user feeds nothing");
    Check(!places.Observe(nameless.bytes, {}, {}, 0.0) && !fed.count(""), "a user without a readable id feeds nothing");
    Check(!places.Observe(userA.bytes, {5, 0, 0}, {}, 50.0) && fed["0002aaaa"].position.x == 1.0f,
          "at most every 100 ms per member");
    Check(places.Observe(userA.bytes, {5, 0, 0}, {}, 100.0) && fed["0002aaaa"].position.x == 5.0f, "then the new place");
    Check(fed["0002bbbb"].facing.z == -1.0f, "a standing player keeps its facing");
    const int namedBefore = named;
    places.Observe(userA.bytes, {6, 0, 0}, {}, 300.0);
    Check(named == namedBefore, "the id is read once per user");
    places.Observe(userA.bytes, {6, 0, 0}, {}, 2200.0);
    Check(places.Expire(2200.0) == 2 && forgotten.size() == 1 && forgotten[0] == "0002bbbb",
          "a member not seen for 2 s is forgotten (the nameless user too, silently)");

    // Fed into the gate: two guests 20 m and 600 m from the observer, a third nobody knows, a budget for about half.
    std::map<std::string, MemberPlace> where;
    MemberPlaces feed(
        [](const void* id) {
            return id == &idA ? std::string("observer") : id == &idB ? std::string("near") : std::string("far");
        },
        [&](const std::string& member, const PlayerPlace& p) {
            MemberPlace m;
            m.position = {p.position.x, p.position.y, p.position.z};
            where[member] = m;
        },
        nullptr);
    static const int idFar = 4;
    FakeUser userFar;
    userFar.SetId(&idFar);
    feed.Observe(userA.bytes, {0, 0, 0}, {}, 0.0);
    feed.Observe(userB.bytes, {0, 0, 20}, {}, 0.0);
    feed.Observe(userFar.bytes, {0, 0, 600}, {}, 0.0);
    InterestGate gate([](const std::string&) { return 3000u; },
                      [&](const std::string& m) -> std::optional<MemberPlace> {
                          auto it = where.find(m);
                          if (it == where.end()) return std::nullopt;
                          return it->second;
                      });
    std::map<std::string, int> sent;
    const std::vector<std::string> subjects = {"near", "far", "unknown"};
    for (std::uint64_t t = 0; t < 6000; t += 10)
        for (std::size_t i = 0; i < subjects.size(); ++i)
            if ((t / 10 + i * 3) % 9 == 0 && gate.Allow("observer", subjects[i], 120, 0, t)) ++sent[subjects[i]];
    std::printf("INFO: gate sends near=%d far=%d unknown=%d in 6 s\n", sent["near"], sent["far"], sent["unknown"]);
    Check(sent["near"] > sent["far"], "the near guest goes more often than the far one");
    Check(sent["unknown"] >= sent["far"], "a member without a place is not ranked below the far one (medium default)");
    Check(sent["far"] >= 5, "the far guest still goes at least about once a second");
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
        for (int frame = 0; frame * 1000.0 / 60.0 < p.staleMs; ++frame) {
            const double now = frame * 1000.0 / 60.0;
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

void TestRoomTrackCapacity() {
    // Receive a whole room's samples before its frame update, including rooms larger than the EOS lobby.
    // The old 64-entry table evicted players before UpdateHook could use their samples and convergence history.
    for (const int count : {65, kMaxPlayers}) {
        PlayerTracks<RemoteTrack> tracks;
        std::vector<int> objects(static_cast<std::size_t>(count) + 1);
        NetPlayerParams params;
        for (int frame = 1; frame <= 3; ++frame) {
            const double now = frame * 33.0;
            for (int i = 0; i < count; ++i) {
                auto* track = tracks.Find(&objects[i], now, true);
                if (frame > 1) Check(track->stepped, "whole-room samples preserve prior convergence state");
                PlayerSample sample;
                sample.seq = static_cast<std::uint16_t>(frame);
                sample.senderMs = static_cast<std::uint32_t>(now);
                sample.position = {static_cast<float>(i), 0, 0};
                Check(AcceptSample(*track, sample, now, params), "whole-room sample accepted");
            }
            for (int i = 0; i < count; ++i) {
                auto* track = tracks.Find(&objects[i], now + 1, false);
                Check(track && track->last.seq == frame && track->last.position.x == static_cast<float>(i),
                      "every supported player remains available to the frame update");
                if (track) {
                    StepRemote(*track, track->last.position, now + 1, params);
                    Check(track->stepped, "each player's convergence runs");
                }
            }
        }
        if (count == kMaxPlayers) {
            // The first player departs; keep the others fresh, then admit its replacement.
            for (int i = 1; i < count; ++i) tracks.Find(&objects[i], 200.0, false);
            auto* newcomer = tracks.Find(&objects[count], 201.0, true);
            Check(!newcomer->have && !newcomer->stepped, "replacement starts with fresh sync state");
            Check(!tracks.Find(&objects[0], 202.0, false), "oldest departed player yields its slot");
            for (int i = 1; i < count; ++i)
                Check(tracks.Find(&objects[i], 202.0, false) != nullptr, "replacement retains other players");
        }
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
    TestMemberPlaces();
    TestRoomTrackCapacity();
    TestFeature();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
