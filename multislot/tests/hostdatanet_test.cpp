// Host data between machines (hostdatanet.h) without EOS: what a lobby says, the menu's words, and two or more
// HostDataLinks wired together through a queue that can lose, hold back or forge packets.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <cstdio>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "../src/hostdatanet.h"

using namespace multislot;
using namespace multislot::hostdata;

namespace {

int failures = 0;

void Check(bool condition, const char* what) {
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", what);
    }
}

DataFile File(const char* path, std::size_t size, std::uint8_t fill) {
    std::vector<std::uint8_t> bytes(size, fill);
    bytes[0] = 'S';
    bytes[1] = 'G';
    bytes[2] = 'O';
    bytes[3] = 0;
    return DataFile{path, bytes};
}

Bundle Make(std::uint8_t fill) {
    std::vector<DataFile> files{File("WEAPON/AWEAPON346.SGO", 4000, fill), File("OBJECT/V501_TANK.SGO", 5000, fill),
                                File("WEAPON/BWEAPON001.SGO", 300, fill)};
    return *MakeBundle(std::move(files), nullptr);
}

// Machines by id, and what is on the wire between them.
struct Net {
    struct Sent {
        std::string from, to;
        std::vector<std::uint8_t> packet;
    };
    std::deque<Sent> wire;
    std::map<std::string, HostDataLink*> links;
    std::map<std::string, bool> refuses;  // sends from this machine fail (EOS not learnt yet)
    int sends = 0;
    bool lose = false;

    HostDataLink::Send SenderFor(const std::string& from) {
        return [this, from](const std::string& to, const std::vector<std::uint8_t>& packet) {
            if (refuses[from]) return false;
            ++sends;
            if (!lose) wire.push_back({from, to, packet});
            return true;
        };
    }
    void Deliver(std::uint64_t now) {
        while (!wire.empty()) {
            Sent sent = std::move(wire.front());
            wire.pop_front();
            const auto packet = DecodePacket(sent.packet.data(), sent.packet.size());
            if (packet && links.count(sent.to)) links[sent.to]->Received(sent.from, *packet, now);
        }
    }
    void Run(std::uint64_t& now, int ticks) {
        for (int i = 0; i < ticks; ++i) {
            now += 16;
            for (auto& [id, link] : links) link->Tick(now);
            Deliver(now);
        }
    }
};

void Rooms() {
    const Bundle bundle = Make(1);
    LobbyView view;
    view.lobbyId = "lobby";
    view.self = "guest";
    view.owner = "host";
    view.members = {{"host", {{kHostDataKey, kHostDataFormat}, {kHostDigestKey, DigestHex(bundle.digest)}}},
                    {"guest", {{kHostDataKey, kHostDataFormat}}}};
    HostDataRoom room = ReadHostDataRoom(view);
    Check(room.inRoom && !room.hosting && room.hostTakesPart && room.hostId == "host", "a guest sees the host take part");
    Check(room.hostDigest && *room.hostDigest == bundle.digest, "and the digest it shares");
    view.self = "host";
    Check(ReadHostDataRoom(view).hosting, "the owner is hosting");
    view.self = "guest";
    view.members[0].texts[kHostDataKey] = "2";
    room = ReadHostDataRoom(view);
    Check(!room.hostTakesPart && !room.hostDigest, "another packet format is not taken part in, and its digest is ignored");
    view.members[0].texts = {{kHostDataKey, kHostDataFormat}, {kHostDigestKey, "not hex"}};
    room = ReadHostDataRoom(view);
    Check(room.hostTakesPart && !room.hostDigest, "a digest that is not one is no digest");
    view.owner.clear();
    Check(!ReadHostDataRoom(view).hostTakesPart && !ReadHostDataRoom(view).hosting, "no owner known: nothing to take");
    Check(!ReadHostDataRoom(LobbyView{}).inRoom, "no lobby: no room");
}

void Notices() {
    using enum HostDataStage;
    Check(HostDataNotice(None, HostAccept::Ask, L"F1", 0, 0).empty(), "nothing to say outside a room");
    Check(HostDataNotice(Same, HostAccept::Ask, L"F1", 0, 0).empty(), "nor with the host's files");
    Check(HostDataNotice(Differs, HostAccept::Ask, L"F1", 0, 0) == L"F1 host weapons :OFF", "different files, the key");
    Check(HostDataNotice(Differs, HostAccept::Never, L"F1", 0, 0) == L"weapons differ from host", "Never: only said");
    Check(HostDataNotice(Fetching, HostAccept::Ask, L"F1", 42, 0) == L"F1 host weapons 42%", "a fetch says how far");
    Check(HostDataNotice(Fetching, HostAccept::Ask, L"F1", 100, 0) == L"F1 host weapons 99%", "and never 100% before done");
    Check(HostDataNotice(Using, HostAccept::Always, L"F1", 0, 2) == L"F1 host weapons :ON +2 own", "own extras are named");
    Check(HostDataNotice(Using, HostAccept::Ask, L"F1", 0, 0) == L"F1 host weapons :ON", "and left out when none");
    Check(HostDataNotice(Failed, HostAccept::Ask, L"", 0, 0) == L"host weapons failed (log)", "no key name, no key");
    Check(HostDataNotice(HostHasNone, HostAccept::Ask, L"F1", 0, 0) == L"host has no weapon mods", "host has none");
}

void Transfer() {
    Net net;
    HostDataLink host(net.SenderFor("host")), guest(net.SenderFor("guest"));
    net.links = {{"host", &host}, {"guest", &guest}};
    const auto bundle = std::make_shared<const Bundle>(Make(3));
    host.Share(bundle);
    std::uint64_t now = 1000;
    guest.Fetch("host", bundle->digest, now);
    Check(guest.State() == HostDataLink::Fetching::Running && net.wire.size() == 1, "a fetch asks the host at once");
    net.Deliver(now);
    Check(host.Serving() == 1, "the host serves who asked");
    net.Run(now, 1);
    Check(guest.State() == HostDataLink::Fetching::Running && guest.Percent() > 0 && guest.Percent() < 100,
          "a tick sends a few parts, and the guest knows how far it is");
    net.Run(now, 20);
    Check(guest.State() == HostDataLink::Fetching::Done && host.Serving() == 0, "the rest follows, then the host is done");
    Check(guest.TakeBundle() == bundle->bytes && guest.State() == HostDataLink::Fetching::Idle, "the bundle as shared");
    const int sendsDone = net.sends;
    net.Run(now, 5);
    Check(net.sends == sendsDone, "nothing is sent once done");
}

void Refusals() {
    Net net;
    HostDataLink host(net.SenderFor("host")), guest(net.SenderFor("guest"));
    net.links = {{"host", &host}, {"guest", &guest}};
    const auto bundle = std::make_shared<const Bundle>(Make(4));
    std::uint64_t now = 0;
    // The host shares nothing (Share=0, or files changed): the guest is told and stops.
    guest.Fetch("host", bundle->digest, now);
    net.Run(now, 1);
    Check(guest.State() == HostDataLink::Fetching::Failed && !guest.Failure().empty(), "a host without it says so");
    host.Share(std::make_shared<const Bundle>(Make(5)));
    guest.Fetch("host", bundle->digest, now);
    net.Run(now, 1);
    Check(guest.State() == HostDataLink::Fetching::Failed, "and so does one sharing other files");
    // Parts from anyone but the host are not taken, even with the right digest and bytes.
    host.Share(nullptr);
    HostDataLink stranger(net.SenderFor("stranger"));
    stranger.Share(bundle);
    net.links["stranger"] = &stranger;
    guest.Fetch("host", bundle->digest, now);
    stranger.Received("guest", Packet{PacketType::Get, bundle->digest, 0, 0, {}}, now);
    net.wire.clear();  // the host's None to the guest's question; the stranger serves anyway
    for (int i = 0; i < 4; ++i) {
        stranger.Tick(now);
        net.Deliver(now);
    }
    Check(guest.State() == HostDataLink::Fetching::Running && guest.Percent() == 0, "a stranger's parts are ignored");
    guest.Cancel();
}

void Forged() {
    Net net;
    HostDataLink host(net.SenderFor("host")), guest(net.SenderFor("guest"));
    net.links = {{"host", &host}, {"guest", &guest}};
    const Bundle real = Make(6);
    // The host's machine sends other bytes under the digest it published: they do not hash to it.
    Bundle forged = Make(7);
    forged.digest = real.digest;
    host.Share(std::make_shared<const Bundle>(forged));
    std::uint64_t now = 0;
    guest.Fetch("host", real.digest, now);
    net.Run(now, 20);
    Check(guest.State() == HostDataLink::Fetching::Failed && guest.TakeBundle().empty(),
          "bytes that do not hash to the digest are refused");
}

void Stalls() {
    Net net;
    HostDataLink guest(net.SenderFor("guest"));
    net.links = {{"guest", &guest}};
    const Bundle bundle = Make(8);
    std::uint64_t now = 0;
    // Before EOS is learnt nothing goes out; the question goes once it can, and only then does the wait start.
    net.refuses["guest"] = true;
    guest.Fetch("host", bundle.digest, now);
    now += HostDataLink::kStallMs * 2;
    guest.Tick(now);
    Check(guest.State() == HostDataLink::Fetching::Running && net.sends == 0, "a refused question is no ask");
    net.refuses["guest"] = false;
    guest.Tick(now);
    Check(net.sends == 1, "it goes once sends work");
    guest.Tick(now + HostDataLink::kStallMs - 1);
    Check(net.sends == 1, "no answer yet is no stall");
    for (int ask = 2; ask <= HostDataLink::kMaxAsks; ++ask) {
        now += HostDataLink::kStallMs;
        guest.Tick(now);
    }
    Check(net.sends == HostDataLink::kMaxAsks && guest.State() == HostDataLink::Fetching::Running,
          "a stall asks again, up to the limit");
    now += HostDataLink::kStallMs;
    guest.Tick(now);
    Check(guest.State() == HostDataLink::Fetching::Failed && net.sends == HostDataLink::kMaxAsks,
          "then the fetch gives up");
    // A host never reached over P2P: the question never goes out, and the fetch still ends.
    net.refuses["guest"] = true;
    guest.Fetch("host", bundle.digest, now);
    guest.Tick(now + HostDataLink::kStallMs * HostDataLink::kMaxAsks - 1);
    Check(guest.State() == HostDataLink::Fetching::Running, "a question that cannot go out is tried for a while");
    guest.Tick(now + HostDataLink::kStallMs * HostDataLink::kMaxAsks);
    Check(guest.State() == HostDataLink::Fetching::Failed, "and then the fetch gives up too");
}

void ManyAskers() {
    Net net;
    HostDataLink host(net.SenderFor("host"));
    const auto bundle = std::make_shared<const Bundle>(Make(9));
    host.Share(bundle);
    const Packet get{PacketType::Get, bundle->digest, 0, 0, {}};
    for (std::size_t i = 0; i <= HostDataLink::kMaxServed; ++i) host.Received("p" + std::to_string(i), get, 0);
    Check(host.Serving() == HostDataLink::kMaxServed, "at most kMaxServed are served at once");
    host.Received("p0", get, 0);
    Check(host.Serving() == HostDataLink::kMaxServed, "asking again does not take a second place");
    net.lose = true;
    host.Tick(0);
    Check(net.sends == static_cast<int>(HostDataLink::kPartsPerTick), "one tick sends kPartsPerTick parts in all");
    // A refused send stops the tick: the same part goes next time.
    net.refuses["host"] = true;
    const int before = net.sends;
    host.Tick(16);
    Check(net.sends == before && host.Serving() == HostDataLink::kMaxServed, "nothing is lost to a refused send");
}

}  // namespace

int main() {
    Rooms();
    Notices();
    Transfer();
    Refusals();
    Forged();
    Stalls();
    ManyAskers();
    if (failures) {
        std::printf("%d host data link check(s) failed\n", failures);
        return 1;
    }
    std::printf("host data link: all checks passed\n");
    return 0;
}
