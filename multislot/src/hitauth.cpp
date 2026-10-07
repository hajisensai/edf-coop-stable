#include "hitauth.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace multislot {
namespace {

template <typename T>
T Load(const std::uint8_t* at, std::size_t offset) {
    T value{};
    std::memcpy(&value, at + offset, sizeof(value));
    return value;
}
template <typename T>
void Store(std::uint8_t* at, std::size_t offset, T value) {
    std::memcpy(at + offset, &value, sizeof(value));
}

// Little-endian writer and reader over a byte range (x64 is little-endian, so these are plain copies).
struct Writer {
    std::uint8_t* at;
    template <typename T>
    void Put(T value) {
        std::memcpy(at, &value, sizeof(value));
        at += sizeof(value);
    }
    void Put3(const float* v) {
        for (int i = 0; i < 3; ++i) Put(v[i]);
    }
};
struct Reader {
    const std::uint8_t* at;
    template <typename T>
    T Get() {
        T value{};
        std::memcpy(&value, at, sizeof(value));
        at += sizeof(value);
        return value;
    }
    void Get3(float* v) {
        for (int i = 0; i < 3; ++i) v[i] = Get<float>();
    }
};

bool Finite3(const float* v) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

float Distance(const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
}

constexpr int kWindow = 64;

}  // namespace

NetOwner OwnerOf(std::uint32_t networkFlags) {
    if (networkFlags & 1) return NetOwner::Remote;  // the bit the game itself reads (547E7B, 5A360A, 6347C0)
    if (networkFlags & 2) return NetOwner::Local;
    return NetOwner::Unregistered;
}

NetOwner VehicleShooter(bool noRegisteredDriver, bool host, int runner) {
    // The last driver's machine is not an authority after the current driver leaves.
    if (noRegisteredDriver) return host ? NetOwner::Local : NetOwner::Remote;
    return runner == 1 ? NetOwner::Local : NetOwner::Remote;
}

HitVerdict DecideHit(const HitInput& input) {
    if (input.replaying) return HitVerdict::Deal;
    // The game's kill message, dealt by everyone after the machine that decided it: as the game does.
    if (input.fromNetwork) return HitVerdict::Vanilla;
    // Heals (and nothing) keep the game's rule: they are not hits.
    if (!(input.damage > 0.0f)) return HitVerdict::Vanilla;
    // Without a network identity on either side there is no owner to send to, and every machine has the object as
    // its own: the game's rule (the plugins' objects, docs/net-re/damage.md section 6).
    if (input.attacker == NetOwner::Unregistered || input.target == NetOwner::Unregistered) return HitVerdict::Vanilla;
    // Someone else's shot: its machine forwards it (HitRuleClock: dropping implies every sender forwards). Not yet
    // dropping: the game's rule (a remote shot at our player or vehicle is dealt here, at anything else not at all).
    if (input.attacker == NetOwner::Remote) return input.dropping ? HitVerdict::Drop : HitVerdict::Vanilla;
    if (input.target == NetOwner::Local) return HitVerdict::Deal;  // the game takes our shot at our object anyway
    return input.forwarding ? HitVerdict::Forward : HitVerdict::Vanilla;
}

void HitRuleClock::Observe(bool gateOn, std::uint64_t nowMs) {
    const bool run = any_ && on_ && nowMs - lastMs_ <= maxGapMs_;
    if (gateOn) {
        if (!run) onSinceMs_ = nowMs;
        lastOnMs_ = nowMs;
        everOn_ = true;
    }
    any_ = true;
    on_ = gateOn;
    lastMs_ = nowMs;
}

bool HitRuleClock::Forwarding(std::uint64_t nowMs) const {
    return everOn_ && nowMs - lastOnMs_ < graceMs_;
}

bool HitRuleClock::Dropping(std::uint64_t nowMs) const {
    return any_ && on_ && nowMs - lastMs_ <= maxGapMs_ && nowMs - onSinceMs_ >= settleMs_;
}

std::size_t WriteDamageEvent(const DamageEvent& event, std::uint8_t* out, std::size_t capacity) {
    if (!out || capacity < kDamageEventBytes) return 0;
    Writer w{out};
    w.Put(kDamageEventMagic0);
    w.Put(kDamageEventMagic1);
    w.Put(kDamageEventVersion);
    w.Put(std::uint8_t{0});
    w.Put(event.seq);
    w.Put(event.sender);
    w.Put(event.attackerRef);
    w.Put(event.targetRef);
    w.Put(event.kind);
    w.Put(event.team);
    w.Put3(event.point);
    w.Put3(event.impulse);
    w.Put(event.damage);
    w.Put(event.size);
    w.Put(event.radius);
    w.Put(event.extra5C);
    w.Put(static_cast<std::uint16_t>(event.flags & ~kGdiFromNetwork));
    w.Put(event.extra64);
    w.Put(event.extra68);
    w.Put3(event.attackerPos);
    return static_cast<std::size_t>(w.at - out);
}

bool ReadDamageEvent(const std::uint8_t* data, std::size_t size, DamageEvent& out) {
    if (!data || size != kDamageEventBytes) return false;
    Reader r{data};
    if (r.Get<std::uint8_t>() != kDamageEventMagic0 || r.Get<std::uint8_t>() != kDamageEventMagic1 ||
        r.Get<std::uint8_t>() != kDamageEventVersion)
        return false;
    r.Get<std::uint8_t>();
    DamageEvent e;
    e.seq = r.Get<std::uint32_t>();
    e.sender = r.Get<std::uint64_t>();
    e.attackerRef = r.Get<std::int32_t>();
    e.targetRef = r.Get<std::int32_t>();
    e.kind = r.Get<std::uint32_t>();
    e.team = r.Get<std::int32_t>();
    r.Get3(e.point);
    r.Get3(e.impulse);
    e.damage = r.Get<float>();
    e.size = r.Get<float>();
    e.radius = r.Get<float>();
    e.extra5C = r.Get<float>();
    e.flags = static_cast<std::uint16_t>(r.Get<std::uint16_t>() & ~kGdiFromNetwork);
    e.extra64 = r.Get<float>();
    e.extra68 = r.Get<float>();
    r.Get3(e.attackerPos);
    out = e;
    return true;
}

void GdiToEvent(const std::uint8_t* gdi, DamageEvent& event) {
    event.kind = Load<std::uint32_t>(gdi, kGdiKind);
    event.team = Load<std::int32_t>(gdi, kGdiTeam);
    for (int i = 0; i < 3; ++i) {
        event.point[i] = Load<float>(gdi, kGdiPoint + 4 * i);
        event.impulse[i] = Load<float>(gdi, kGdiImpulse + 4 * i);
    }
    event.damage = Load<float>(gdi, kGdiDamage);
    event.size = Load<float>(gdi, kGdiSize);
    event.radius = Load<float>(gdi, kGdiRadius);
    event.extra5C = Load<float>(gdi, kGdiExtra5C);
    event.flags = static_cast<std::uint16_t>(Load<std::uint16_t>(gdi, kGdiFlags) & ~kGdiFromNetwork);
    event.extra64 = Load<float>(gdi, kGdiExtra64);
    event.extra68 = Load<float>(gdi, kGdiExtra68);
}

void EventToGdi(const DamageEvent& event, std::uint8_t* gdi) {
    gdi[kGdiOverride] = 1;
    gdi[kGdiOverrideRemote] = 0;
    gdi[kGdiResult] = 0;
    Store<std::uint32_t>(gdi, kGdiKind, event.kind);
    Store<std::int32_t>(gdi, kGdiTeam, event.team);
    for (int i = 0; i < 3; ++i) {
        Store<float>(gdi, kGdiPoint + 4 * i, event.point[i]);
        Store<float>(gdi, kGdiImpulse + 4 * i, event.impulse[i]);
    }
    Store<float>(gdi, kGdiPoint + 12, 1.0f);
    Store<float>(gdi, kGdiImpulse + 12, 0.0f);
    Store<float>(gdi, kGdiDamage, event.damage);
    Store<float>(gdi, kGdiSize, event.size);
    Store<float>(gdi, kGdiRadius, event.radius);
    Store<float>(gdi, kGdiExtra5C, event.extra5C);
    Store<std::uint16_t>(gdi, kGdiFlags, static_cast<std::uint16_t>(event.flags & ~kGdiFromNetwork));
    Store<float>(gdi, kGdiExtra64, event.extra64);
    Store<float>(gdi, kGdiExtra68, event.extra68);
}

const char* HitRejectName(HitReject reject) {
    switch (reject) {
        case HitReject::None: return "none";
        case HitReject::Malformed: return "malformed";
        case HitReject::NotOwner: return "not the owner";
        case HitReject::TargetDead: return "target dead";
        case HitReject::Damage: return "damage out of range";
        case HitReject::Offset: return "hit point too far from the target";
        case HitReject::Range: return "attacker too far from the hit point";
        case HitReject::Duplicate: return "duplicate";
        case HitReject::Rate: return "attacker over the event rate";
    }
    return "?";
}

HitReject CheckEvent(const DamageEvent& event, const HitView& view, const HitLimits& limits) {
    if (!Finite3(event.point) || !Finite3(event.impulse) || !Finite3(event.attackerPos) || !std::isfinite(event.size) ||
        !std::isfinite(event.radius) || !std::isfinite(event.extra5C) || !std::isfinite(event.extra64) ||
        !std::isfinite(event.extra68) || !std::isfinite(event.damage))
        return HitReject::Malformed;
    if (!view.targetLocal) return HitReject::NotOwner;
    if (!view.targetAlive) return HitReject::TargetDead;
    if (!(event.damage > 0.0f) || event.damage > limits.maxDamage) return HitReject::Damage;
    const float radius = std::max(0.0f, event.radius);
    if (!Finite3(view.targetPos) || Distance(event.point, view.targetPos) > limits.maxHitOffset + radius)
        return HitReject::Offset;
    // The attacker as this machine has it when it knows it; otherwise as the sender said.
    const float* from = view.attackerKnown && Finite3(view.attackerPos) ? view.attackerPos : event.attackerPos;
    if (Distance(from, event.point) > limits.maxRange + radius) return HitReject::Range;
    return HitReject::None;
}

void HitGate::Forget(std::uint64_t nowMs) {
    if (nowMs - lastSweepMs_ < limits_.forgetMs) return;
    lastSweepMs_ = nowMs;
    for (auto it = attackers_.begin(); it != attackers_.end();)
        it = nowMs - it->second.lastMs > limits_.forgetMs ? attackers_.erase(it) : std::next(it);
}

HitReject HitGate::Admit(const DamageEvent& event, const HitView& view, std::uint64_t nowMs) {
    if (const HitReject geometry = CheckEvent(event, view, limits_); geometry != HitReject::None) return geometry;
    Forget(nowMs);
    Attacker& a = attackers_[Key{event.sender, event.attackerRef}];
    if (a.any && nowMs - a.lastMs > limits_.forgetMs) a = Attacker{};
    // Sequence: newer than the newest moves the window; within the window, once each.
    int ahead = 0;
    if (a.any) {
        const auto diff = static_cast<std::int32_t>(event.seq - a.newest);
        if (diff <= -kWindow) return HitReject::Duplicate;  // older than anything still remembered
        if (diff <= 0 && (a.seen >> static_cast<unsigned>(-diff) & 1)) return HitReject::Duplicate;
        ahead = diff;
    }
    // Rate: a token bucket, full at the start.
    const double elapsed = a.any ? static_cast<double>(nowMs - a.lastMs) / 1000.0 : 0.0;
    const double tokens = a.any ? std::min(limits_.burst, a.tokens + elapsed * limits_.eventsPerSecond) : limits_.burst;
    if (tokens < 1.0) {
        a.tokens = tokens;
        a.lastMs = nowMs;
        a.any = true;
        return HitReject::Rate;
    }
    a.tokens = tokens - 1.0;
    a.lastMs = nowMs;
    if (!a.any) {
        a.any = true;
        a.newest = event.seq;
        a.seen = 1;
    } else if (ahead > 0) {
        a.seen = ahead >= kWindow ? 1 : (a.seen << static_cast<unsigned>(ahead)) | 1;
        a.newest = event.seq;
    } else {
        a.seen |= std::uint64_t{1} << static_cast<unsigned>(-ahead);
    }
    return HitReject::None;
}

}  // namespace multislot
