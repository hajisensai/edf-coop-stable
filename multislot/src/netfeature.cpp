#include "netfeature.h"

#include <algorithm>
#include <map>
#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "log.h"
#include "netcompress.h"

namespace multislot {
namespace {

struct Settings {
    std::uint32_t caps = 0;  // the features on in this machine's INI
    bool rejectMismatched = true;
    std::int64_t protocol = kNetProtocol;  // [Test] NetProtocol plays another version
};
Settings settings;
std::atomic<bool> started{false};  // the lobby glue is in: the room can be read

std::int64_t ParseInt(const std::string& text, bool& ok) {
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    ok = !text.empty() && end && *end == '\0';
    return value;
}

const NetRoom::Member* Find(const std::vector<NetRoom::Member>& members, const std::string& id) {
    for (const auto& m : members)
        if (m.id == id) return &m;
    return nullptr;
}

// The lobby beat: what everyone publishes. The owner refuses a member of another netcode protocol.
void Observe(const LobbyView& seen) {
    // A room host also counts the members beyond Epic's lobby, by what their direct-link hellos said.
    LobbyView view = seen;
    if (!view.lobbyId.empty() && !view.owner.empty() && view.owner == view.self) {
        for (const auto& [id, netcode] : dn::directMemberNetcode()) {
            bool listed = false;
            for (const auto& m : view.members) listed = listed || m.id == id;
            if (listed) continue;
            LobbyView::Member m;
            m.id = id;
            if (netcode.first) {
                m.texts[kNetSeenKey] = "hello";  // it said its protocol to us itself: the host refuses at the hello
                m.texts[kNetProtocolKey] = std::to_string(netcode.first);
                char caps[16];
                std::snprintf(caps, sizeof(caps), "%X", netcode.second);
                m.texts[kNetCapsKey] = caps;
            }
            view.members.push_back(std::move(m));
        }
    }
    const std::vector<NetRoom::Member> mismatched = NetGate().Observe(view);
    if (view.lobbyId.empty()) return;
    const bool owner = !view.owner.empty() && view.owner == view.self;
    if (owner) {  // what the whole room runs, for everyone else's gate (kNetRoomCapsKey); never empty: "0" says none
        char caps[16];
        std::snprintf(caps, sizeof(caps), "%X", NetGate().ActiveMask());
        PublishLobbyText(kNetRoomCapsKey, caps);
    }
    // Having read the host's protocol (and logged above what it means), say so: only then may a host of another
    // protocol refuse us (kNetSeenKey).
    struct SeenAfter {
        std::string value;
        ~SeenAfter() {
            if (!value.empty()) PublishMemberText(kNetSeenKey, value);
        }
    } seenAfter;
    if (!owner)
        for (const auto& member : view.members)
            if (member.id == view.owner)
                if (const auto proto = member.texts.find(kNetProtocolKey); proto != member.texts.end())
                    seenAfter.value = proto->second;
    for (const NetRoom::Member& m : mismatched) {
        if (m.id == view.owner) {
            Log("NETCODE the room's host %s runs netcode protocol %lld, this machine %lld (another EDF6Coop "
                "version): every netcode feature is off in this room%s",
                m.id.c_str(), static_cast<long long>(m.protocol), static_cast<long long>(settings.protocol),
                settings.rejectMismatched ? "; a host of our version would refuse us" : "");
            continue;
        }
        if (!owner) {
            Log("NETCODE %s runs netcode protocol %lld, this machine %lld: every netcode feature is off in this room "
                "until it leaves",
                m.id.c_str(), static_cast<long long>(m.protocol), static_cast<long long>(settings.protocol));
            continue;
        }
        if (settings.rejectMismatched && KickLobbyMember(m.id)) {
            Log("NETCODE REFUSED %s: it runs netcode protocol %lld, this room %lld (another EDF6Coop version). It "
                "is removed from the room so that nobody plays by other rules ([Netcode] RejectMismatched=0 keeps "
                "it and turns every netcode feature off instead)",
                m.id.c_str(), static_cast<long long>(m.protocol), static_cast<long long>(settings.protocol));
        } else {
            Log("NETCODE %s runs netcode protocol %lld, this room %lld: every netcode feature is off in this room "
                "while it is in it%s",
                m.id.c_str(), static_cast<long long>(m.protocol), static_cast<long long>(settings.protocol),
                settings.rejectMismatched ? " (it could not be removed)" : " (RejectMismatched=0)");
        }
    }
    // What packing did (netcompress.h), every StatsSeconds while it does anything.
    static ULONGLONG lastPackLog = 0;
    const ULONGLONG tick = GetTickCount64();
    if (tick - lastPackLog >= dn::netcodeOptions().statsIntervalMs) {
        lastPackLog = tick;
        std::uint64_t packed = 0, saved = 0, unpacked = 0;
        TakePackStats(packed, saved, unpacked);
        if (packed || unpacked)
            Log("NETCODE XPRESS: %llu datagrams packed (%llu bytes saved), %llu unpacked", static_cast<unsigned long long>(packed),
                static_cast<unsigned long long>(saved), static_cast<unsigned long long>(unpacked));
    }
    // Once per change of the room's answer, what runs.
    static std::string said;
    const std::string why = NetGate().WhyOff(settings.caps);
    const std::string now = why.empty() ? "on: " + FormatCaps(settings.caps) : "off: " + why;
    if (now != said) {
        said = now;
        Log("NETCODE features in room %s %s", view.lobbyId.c_str(), now.c_str());
    }
}

bool RoomCap(std::uint32_t cap) { return NetFeatureActive(static_cast<NetFeature>(cap)); }

// Bulk messages by tag (SetBulkHandlerForTag); a tag nobody handles is logged with [Test] BulkEcho=1.
SRWLOCK bulkLock = SRWLOCK_INIT;
std::map<std::uint16_t, dn::BulkHandler> bulkHandlers;
bool bulkEcho = false;

// [Test] BulkEcho=1: what arrives in bulk is logged (GameNet_bulk).
void LogBulk(const std::string& src, std::uint16_t tag, const std::uint8_t* data, std::size_t size) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 1099511628211ull;
    Log("NETCODE bulk message from %s: tag %u, %zu bytes, fnv %016llx", src.c_str(), tag, size,
        static_cast<unsigned long long>(hash));
}

}  // namespace

std::vector<NetRoom::Member> NetRoom::Observe(const LobbyView& view) {
    std::vector<Member> fresh;
    AcquireSRWLockExclusive(&lock_);
    if (view.lobbyId != lobby_) reported_.clear();
    lobby_ = view.lobbyId;
    self_ = view.self;
    owner_ = view.owner;
    const auto roomCaps = view.lobbyTexts.find(kNetRoomCapsKey);
    hostCaps_ = roomCaps == view.lobbyTexts.end() ? 0u : ParseCaps(roomCaps->second);
    members_.clear();
    for (const LobbyView::Member& seen : view.members) {
        Member m;
        m.id = seen.id;
        const auto proto = seen.texts.find(kNetProtocolKey);
        bool ok = false;
        if (proto != seen.texts.end()) m.protocol = ParseInt(proto->second, ok);
        m.published = ok;
        if (const auto caps = seen.texts.find(kNetCapsKey); caps != seen.texts.end()) m.caps = ParseCaps(caps->second);
        m.seenHost = seen.texts.count(kNetSeenKey) != 0;
        // The owner acts on a member only once it has read the owner's protocol (kNetSeenKey).
        const bool due = self_ != owner_ || m.id == owner_ || m.seenHost;
        if (m.published && m.protocol != settings.protocol && m.id != self_ && due &&
            std::find(reported_.begin(), reported_.end(), m.id) == reported_.end()) {
            reported_.push_back(m.id);
            fresh.push_back(m);
        }
        members_.push_back(std::move(m));
    }
    UpdateActiveLocked();
    ReleaseSRWLockExclusive(&lock_);
    return fresh;
}

void NetRoom::UpdateActiveLocked() {
    std::uint32_t mask = lobby_.empty() || !Find(members_, self_) ? 0u : ~0u;
    for (const Member& m : members_) mask &= m.published && m.protocol == settings.protocol ? m.caps : 0u;
    // Not the host: only what the host says the whole room runs (members beyond the lobby included), and nothing until
    // it said it.
    if (self_ != owner_) mask &= hostCaps_;
    active_ = mask;
}

std::string NetRoom::WhyOff(std::uint32_t caps) const {
    AcquireSRWLockShared(&lock_);
    std::string why;
    if (lobby_.empty()) why = "not in a room";
    else if (members_.empty()) why = "the room's members are not known yet";
    for (const Member& m : why.empty() ? members_ : std::vector<Member>()) {
        if (!m.published) {
            why = m.id + " publishes no netcode protocol (the game as it ships, an older EDF6Coop, or its lobby entry has "
                         "not reached us yet)";
        } else if (m.protocol != settings.protocol) {
            why = m.id + " runs netcode protocol " + std::to_string(m.protocol);
        } else if ((m.caps & caps) != caps) {
            why = m.id + " has features " + FormatCaps(m.caps) + " on, not all of " + FormatCaps(caps);
        }
        if (!why.empty()) break;
    }
    if (why.empty() && !Find(members_, self_)) why = "our own lobby entry is not listed yet";
    if (why.empty() && self_ != owner_ && (hostCaps_ & caps) != caps)
        why = "the room's host has not said that the whole room runs them (members beyond Epic's lobby without them, or its "
              "word has not reached us yet)";
    ReleaseSRWLockShared(&lock_);
    return why;
}

bool NetRoom::Active(std::uint32_t caps) const { return (active_.load() & caps) == caps; }

bool NetRoom::InRoom() const {
    AcquireSRWLockShared(&lock_);
    const bool in = !lobby_.empty();
    ReleaseSRWLockShared(&lock_);
    return in;
}

bool NetRoom::Owner() const {
    AcquireSRWLockShared(&lock_);
    const bool owner = !owner_.empty() && owner_ == self_;
    ReleaseSRWLockShared(&lock_);
    return owner;
}

std::vector<NetRoom::Member> NetRoom::Members() const {
    AcquireSRWLockShared(&lock_);
    std::vector<Member> members = members_;
    ReleaseSRWLockShared(&lock_);
    return members;
}

NetRoom& NetGate() {
    static NetRoom room;
    return room;
}

std::uint32_t ParseCaps(const std::string& hex) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(hex.c_str(), &end, 16);
    return !hex.empty() && end && *end == '\0' ? static_cast<std::uint32_t>(value) : 0;
}

std::string FormatCaps(std::uint32_t caps) {
    static const struct {
        NetFeature feature;
        const char* name;
    } names[] = {{NetFeature::TrafficClasses, "TrafficClasses"}, {NetFeature::Mesh, "Mesh"},
                 {NetFeature::Fragments, "Fragments"},           {NetFeature::Compression, "Compression"},
                 {NetFeature::PlayerSync, "PlayerSync"},
                 {NetFeature::HitAuthority, "HitAuthority"},     {NetFeature::WorldAuthority, "WorldAuthority"},
                 {NetFeature::PluginObjects, "PluginObjects"}};
    std::string out;
    std::uint32_t named = 0;
    for (const auto& n : names) {
        const auto bit = static_cast<std::uint32_t>(n.feature);
        named |= bit;
        if (caps & bit) out += (out.empty() ? "" : ",") + std::string(n.name);
    }
    if (caps & ~named) {
        char rest[16];
        std::snprintf(rest, sizeof(rest), "0x%X", caps & ~named);
        out += (out.empty() ? "" : ",") + std::string(rest);
    }
    return out.empty() ? "none" : out;
}

bool NetFeatureEnabledLocally(NetFeature feature) {
    return (settings.caps & static_cast<std::uint32_t>(feature)) != 0;
}

std::atomic<std::uint32_t> forcedMask{0}, forcedOn{0};  // SetNetFeatureForTest

bool NetFeatureActive(NetFeature feature) {
    const auto bit = static_cast<std::uint32_t>(feature);
    if (forcedMask & bit) return (forcedOn & bit) != 0;
    return started && NetFeatureEnabledLocally(feature) && NetGate().Active(bit);
}

void SetNetFeatureForTest(NetFeature feature, bool active) {
    const auto bit = static_cast<std::uint32_t>(feature);
    forcedMask |= bit;
    if (active)
        forcedOn |= bit;
    else
        forcedOn &= ~bit;
}

void ClearNetFeatureForTest() {
    forcedMask = 0;
    forcedOn = 0;
}

std::uint32_t LinkBudgetBytesPerSec(const std::string& peer) { return dn::linkBudgetBytesPerSec(peer); }
void SetStateSendFilter(dn::StateSendFilter filter) { dn::setStateSendFilter(filter); }
bool SendBulk(const std::string& remote, std::uint16_t tag, const void* data, std::size_t size) {
    return dn::sendBulk(remote, tag, static_cast<const std::uint8_t*>(data), size);
}
void SetBulkHandler(dn::BulkHandler handler) { dn::setBulkHandler(handler); }

namespace {
void DispatchBulk(const std::string& src, std::uint16_t tag, const std::uint8_t* data, std::size_t size) {
    AcquireSRWLockShared(&bulkLock);
    const auto it = bulkHandlers.find(tag);
    const dn::BulkHandler handler = it == bulkHandlers.end() ? nullptr : it->second;
    ReleaseSRWLockShared(&bulkLock);
    if (handler) handler(src, tag, data, size);
    else if (bulkEcho) LogBulk(src, tag, data, size);
}
}  // namespace

void SetBulkHandlerForTag(std::uint16_t tag, dn::BulkHandler handler) {
    AcquireSRWLockExclusive(&bulkLock);
    bulkHandlers[tag] = handler;
    ReleaseSRWLockExclusive(&bulkLock);
    dn::setBulkHandler(&DispatchBulk);
}

void InitNetFeature(const wchar_t* iniPath) {
    const auto flag = [&](const wchar_t* key, int fallback) {
        return GetPrivateProfileIntW(L"Netcode", key, fallback, iniPath) != 0;
    };
    std::uint32_t caps = 0;
    if (flag(L"TrafficClasses", 1)) caps |= static_cast<std::uint32_t>(NetFeature::TrafficClasses);
    if (flag(L"Mesh", 1)) caps |= static_cast<std::uint32_t>(NetFeature::Mesh);
    if (flag(L"Fragments", 1)) caps |= static_cast<std::uint32_t>(NetFeature::Fragments);
    if (flag(L"Compression", 1)) caps |= static_cast<std::uint32_t>(NetFeature::Compression);
    // The later workstreams' parts (W2-W4), read here so that every part of the netcode has its key in [Netcode].
    if (flag(L"PlayerSync", 1)) caps |= static_cast<std::uint32_t>(NetFeature::PlayerSync);
    if (flag(L"HitAuthority", 1)) caps |= static_cast<std::uint32_t>(NetFeature::HitAuthority);
    if (flag(L"WorldAuthority", 1)) caps |= static_cast<std::uint32_t>(NetFeature::WorldAuthority);
    settings.caps = caps;
    settings.rejectMismatched = flag(L"RejectMismatched", 1);
    if (const int test = static_cast<int>(GetPrivateProfileIntW(L"Test", L"NetProtocol", 0, iniPath)); test > 0) {
        settings.protocol = test;
        Log("TEST NetProtocol=%d: this machine publishes and expects netcode protocol %d (another version's)", test, test);
    }
    dn::NetcodeOptions options;
    options.trafficClasses = (caps & dn::kCapTrafficClasses) != 0;
    options.mesh = (caps & dn::kCapMesh) != 0;
    options.fragments = (caps & dn::kCapFragments) != 0;
    options.shedState = flag(L"ShedState", 1);
    options.statsIntervalMs = 1000u * GetPrivateProfileIntW(L"Netcode", L"StatsSeconds", 60, iniPath);
    dn::setNetcodeOptions(options);
    bulkEcho = GetPrivateProfileIntW(L"Test", L"BulkEcho", 0, iniPath) != 0;
    if (GetPrivateProfileIntW(L"Test", L"LoopbackHosts", 0, iniPath)) {
        dn::setTestLoopbackHosts(true);
        Log("TEST LoopbackHosts=1: a room host may advertise a loopback address");
    }
    dn::setBulkHandler(&DispatchBulk);
    const UINT blockAfter = GetPrivateProfileIntW(L"Test", L"PeerBlockAfterMs", UINT_MAX, iniPath);
    if (blockAfter != UINT_MAX) {
        const UINT blockFor = GetPrivateProfileIntW(L"Test", L"PeerBlockForMs", UINT_MAX, iniPath);
        dn::setTestPeerBlock(blockAfter, blockFor);
        Log("TEST PeerBlockAfterMs=%u PeerBlockForMs=%u: direct links to other joiners lose everything then", blockAfter,
            blockFor);
    }
    const UINT bulkCut = GetPrivateProfileIntW(L"Test", L"BulkFragmentsSent", 0, iniPath);
    if (bulkCut) {
        dn::setTestBulkFragmentsSent(bulkCut);
        Log("TEST BulkFragmentsSent=%u: of every bulk message (and every resend of it) only that many fragments go out",
            bulkCut);
    }
    Log("NETCODE protocol %lld, features on in the INI: %s; RejectMismatched=%d, ShedState=%d", static_cast<long long>(settings.protocol),
        FormatCaps(caps).c_str(), settings.rejectMismatched ? 1 : 0, options.shedState ? 1 : 0);
}

bool StartNetFeature(bool lobbyGlue) {
    if (!lobbyGlue) {
        Log("NETCODE UNAVAILABLE: the lobby glue is not in, so what the room runs cannot be read; every netcode feature "
            "stays off");
        return false;
    }
    PublishMemberText(kNetProtocolKey, std::to_string(settings.protocol));
    char caps[16];
    std::snprintf(caps, sizeof(caps), "%X", settings.caps);
    PublishMemberText(kNetCapsKey, caps);
    WatchMemberTexts({kNetProtocolKey, kNetCapsKey, kNetSeenKey}, &Observe);
    WatchLobbyTexts({kNetRoomCapsKey});
    // The direct link says the same in every hello; a host refuses another protocol there too (members beyond
    // Epic's lobby, whose lobby entry nobody can read).
    dn::setNetcodeIdentity(static_cast<std::uint32_t>(settings.protocol), settings.caps, settings.rejectMismatched);
    // Rooms above Epic's 64: the host puts its direct-link address and identity on the lobby, where a player Epic
    // turns away finds them (src/netcode.h kHostAddressKey).
    ListenToTicks([](void*) {
        std::string address, identity;
        if (dn::hostAdvertisement(address, identity)) {
            PublishLobbyText(dn::kHostAddressKey, address);
            PublishLobbyText(dn::kHostIdentityKey, identity);
        }
    });
    dn::setRoomCapQuery(&RoomCap);
    started = true;
    return true;
}

}  // namespace multislot
