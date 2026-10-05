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
#include "../src/hostdataprompt.h"

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
    std::map<std::string, bool> refusesTo;  // one peer is unreachable while the others still work
    int sends = 0;
    bool lose = false;

    HostDataLink::Send SenderFor(const std::string& from) {
        return [this, from](const std::string& to, const std::vector<std::uint8_t>& packet) {
            if (refuses[from] || refusesTo[to]) return false;
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

// Who brings what in a room, in the order that decides a file two of them have.
void Plan() {
    const Bundle mods = Make(1), hostPage = Make(2), guestPage = Make(3), thirdPage = Make(4), oldPage = Make(5);
    LobbyView view;
    view.lobbyId = "lobby";
    view.self = "guest";
    view.owner = "host";
    view.members = {{"third", {{kHostDataKey, kHostDataFormat}, {kPageDigestKey, DigestHex(thirdPage.digest)}}},
                    {"host",
                     {{kHostDataKey, kHostDataFormat},
                      {kHostDigestKey, DigestHex(mods.digest)},
                      {kPageDigestKey, DigestHex(hostPage.digest)}}},
                    {"guest", {{kHostDataKey, kHostDataFormat}, {kPageDigestKey, "not yet"}}},
                    {"old", {{kPageDigestKey, DigestHex(oldPage.digest)}}},
                    {"none", {{kHostDataKey, kHostDataFormat}, {kPageDigestKey, kPageNone}}},
                    {"earlier none", {{kHostDataKey, kHostDataFormat}, {kPageDigestKey, ""}}}};
    std::vector<RoomSource> plan = PlanRoomSources(view, std::nullopt, guestPage.digest);
    Check(plan == std::vector<RoomSource>{{"host", mods.digest, true},
                                          {"host", hostPage.digest, false},
                                          {"third", thirdPage.digest, false},
                                          {"guest", guestPage.digest, false}},
          "the host's Mods, its page, then the others' pages in the lobby's order; our own page as we have it");
    Check(PlanRoomSources(view, std::nullopt, std::nullopt).size() == 3, "no page of ours: none of ours");
    // Owning the lobby: our Mods as we have them, whatever the lobby shows yet.
    view.self = "host";
    plan = PlanRoomSources(view, guestPage.digest, std::nullopt);
    Check(plan.size() == 2 && plan[0] == RoomSource{"host", guestPage.digest, true} && plan[1].member == "third",
          "the owner's own Mods and page come from this machine");
    plan = PlanRoomSources(view, std::nullopt, std::nullopt);
    Check(plan.size() == 1 && plan[0].member == "third", "an owner sharing nothing brings nothing");
    // Another packet format takes no part.
    view.self = "guest";
    view.members[1].texts[kHostDataKey] = "2";
    plan = PlanRoomSources(view, std::nullopt, std::nullopt);
    Check(plan.size() == 1 && plan[0].member == "third", "an owner in another format brings nothing");
    view.owner = "nobody";
    Check(PlanRoomSources(view, std::nullopt, std::nullopt).size() == 1, "no owner in the lobby: only the pages");
    Check(PlanRoomSources(LobbyView{}, mods.digest, guestPage.digest).empty(), "no lobby: no room sources");
}

// The room's files: each path the first source's, and what the later ones lose.
void Merge() {
    const SourceFiles native{{"WEAPON/A.SGO", "WEAPON/B.SGO"}, L""};
    const SourceFiles first{{"WEAPON/B.SGO", "WEAPON/C.SGO"}, L"./b/"};
    const SourceFiles second{{"OBJECT/V401.SGO", "WEAPON/C.SGO", "WEAPON/D.SGO"}, L"./c/"};
    const RoomOverlay merged = MergeSources({&native, nullptr, &first, &second});
    const std::vector<Overlay::Entry> expected{
        {"OBJECT/V401.SGO", L"./c/"}, {"WEAPON/C.SGO", L"./b/"}, {"WEAPON/D.SGO", L"./c/"}};
    Check(merged.overlay.entries == expected,
          "a path goes to the first source with it; files the game reads anyway are not pointed anywhere");
    Check(merged.lost == std::vector<std::size_t>{0, 0, 1, 1}, "each later source loses the paths an earlier one has");
    Check(MergeSources({}).overlay.entries.empty(), "no sources, nothing to read elsewhere");
}

void Notices() {
    WeaponsView view;
    view.pageKey = L"F6";
    view.acceptKey = L"F1";
    Check(WeaponsNotice(view).empty(), "no pages and no room: nothing to say");
    view.pages = 2;
    Check(WeaponsNotice(view) == L"F6 Page:off", "pages but none picked");
    view.page = L"Laser";
    Check(WeaponsNotice(view) == L"F6 Page:Laser", "the page picked");
    view.inRoom = true;
    view.accept = HostAccept::Auto;
    view.wanted = true;
    Check(WeaponsNotice(view) == L"F6 Page:Laser", "a room whose files are all ours says nothing of it");
    view.remote = 2;
    Check(WeaponsNotice(view) == L"ROOM WEAPONS :ON (2)   F6 Page:Laser", "the room's files in use, first");
    view.percent = 42;
    Check(WeaponsNotice(view) == L"ROOM WEAPONS 42%   F6 Page:Laser", "while they come");
    view.percent = 100;
    Check(WeaponsNotice(view) == L"ROOM WEAPONS 99%   F6 Page:Laser", "never 100% before they are in use");
    view.percent = -1;
    view.failed = 1;
    view.lost = 3;
    Check(WeaponsNotice(view) == L"ROOM WEAPONS :ON (2) 1 failed -3 of yours   F6 Page:Laser",
          "what failed and what of our page gave way");
    view.failed = view.lost = 0;
    view.accept = HostAccept::Ask;
    view.wanted = false;
    Check(WeaponsNotice(view) == L"F1 ROOM WEAPONS :OFF (2)   F6 Page:Laser", "Ask: the key takes them");
    view.wanted = true;
    Check(WeaponsNotice(view) == L"F1 ROOM WEAPONS :ON (2)   F6 Page:Laser", "and gives them back");
    view.accept = HostAccept::Never;
    view.wanted = false;
    Check(WeaponsNotice(view) == L"ROOM WEAPONS differ (2)   F6 Page:Laser", "Never: only said");
    view.pages = 0;
    view.inRoom = false;
    Check(WeaponsNotice(view).empty(), "outside a room the room is not mentioned");
}

bool Has(const std::wstring& text, const std::wstring& part) { return text.find(part) != std::wstring::npos; }

// The question before another member's files are used: who brings what, every file, and both answers' risks.
void Prompt() {
    const std::vector<PromptSource> sources = {
        {true, "0002de167055479fbaeb19199b5d75f0", {"OBJECT/VEHICLE404_BIGTANK_AI.SGO", "WEAPON/AWEAPON346.SGO"}},
        {false, "0002cf0177ee4e98ac2b8a3c1ecb00da", {"WEAPON/AWEAPON346.SGO", "WEAPON/EWEAPON076.SGO"}}};
    const std::vector<std::string> yours = {"WEAPON/AWEAPON346.SGO"};
    const std::wstring en = PromptText(sources, yours, L"F1", PromptLanguage::English);
    Check(Has(en, L"the room host's Mods: 2 file(s)") && Has(en, L"the weapon page of member 0002cf01: 2 file(s)"),
          "each source, by who brings it, with its file count");
    Check(Has(en, L"  OBJECT/VEHICLE404_BIGTANK_AI.SGO\n") && Has(en, L"  WEAPON/EWEAPON076.SGO\n"),
          "every file is named");
    Check(Has(en, L"  WEAPON/AWEAPON346.SGO  (replaces the one in your Mods)\n"),
          "a file of the player's own Mods says it takes its place");
    Check(en.find(L"WEAPON/AWEAPON346.SGO") == en.rfind(L"WEAPON/AWEAPON346.SGO"), "a file two bring is listed once");
    Check(Has(en, L"Yes:") && Has(en, L"Risks:") && Has(en, L"No:") && Has(en, L"crash") && Has(en, L"SHA-256"),
          "both answers and their risks");
    Check(Has(en, L"with F1 on a menu screen"), "and the key that changes it later");
    const std::wstring zh = PromptText(sources, yours, L"F1", PromptLanguage::Chinese);
    Check(Has(zh, L"房主的 Mods: 2 个文件") && Has(zh, L"风险") && Has(zh, L"按 F1 切换"), "in Chinese");
    const std::wstring ja = PromptText(sources, yours, L"F1", PromptLanguage::Japanese);
    Check(Has(ja, L"リスク") && Has(ja, L"F1 を押して"), "in Japanese");
    Check(PromptLanguageFor(MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL)) == PromptLanguage::Chinese &&
              PromptLanguageFor(MAKELANGID(LANG_JAPANESE, SUBLANG_DEFAULT)) == PromptLanguage::Japanese &&
              PromptLanguageFor(MAKELANGID(LANG_KOREAN, SUBLANG_DEFAULT)) == PromptLanguage::English,
          "the language follows Windows', English otherwise");
    Check(!Has(PromptText(sources, yours, L"", PromptLanguage::English), L"on a menu screen"), "no key, no key line");

    std::vector<std::string> many;
    for (int i = 0; i < 25; ++i) many.push_back("WEAPON/W" + std::to_string(100 + i) + ".SGO");
    const std::wstring longer = PromptText({{true, "host", many}}, {}, L"F1", PromptLanguage::English);
    Check(Has(longer, L"  WEAPON/W119.SGO\n") && !Has(longer, L"W120.SGO") && Has(longer, L"... and 5 more"),
          "a long list stops at kPromptMaxPaths and counts the rest");
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

// One machine serves its Mods and its page; two others fetch one each.
void ManyBundles() {
    Net net;
    HostDataLink host(net.SenderFor("host")), first(net.SenderFor("first")), second(net.SenderFor("second"));
    net.links = {{"host", &host}, {"first", &first}, {"second", &second}};
    const auto mods = std::make_shared<const Bundle>(Make(10));
    const auto page = std::make_shared<const Bundle>(Make(11));
    host.Share({mods, page, nullptr});
    std::uint64_t now = 0;
    first.Fetch("host", mods->digest, now);
    second.Fetch("host", page->digest, now);
    net.Run(now, 40);
    Check(first.State() == HostDataLink::Fetching::Done && first.TakeBundle() == mods->bytes, "one gets the Mods");
    Check(second.State() == HostDataLink::Fetching::Done && second.TakeBundle() == page->bytes, "the other the page");
    Check(host.Serving() == 0, "and the host is done with both");
    host.Share(std::vector<std::shared_ptr<const Bundle>>{});
    first.Fetch("host", page->digest, now);
    net.Run(now, 2);
    Check(first.State() == HostDataLink::Fetching::Failed, "sharing nothing: a question is told so");
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
    // Refused sends retain their place: the same parts go next time.
    net.refuses["host"] = true;
    const int before = net.sends;
    host.Tick(16);
    Check(net.sends == before && host.Serving() == HostDataLink::kMaxServed, "nothing is lost to a refused send");
}

void UnreachableAskerDoesNotBlockOthers() {
    Net net;
    HostDataLink host(net.SenderFor("host")), absent(net.SenderFor("absent")), guest(net.SenderFor("guest"));
    net.links = {{"host", &host}, {"absent", &absent}, {"guest", &guest}};
    const auto bundle = std::make_shared<const Bundle>(Make(10));
    host.Share(bundle);
    std::uint64_t now = 0;
    absent.Fetch("host", bundle->digest, now);  // the first asker becomes unreachable
    guest.Fetch("host", bundle->digest, now);
    net.Deliver(now);
    net.refusesTo["absent"] = true;
    net.Run(now, 20);
    Check(guest.State() == HostDataLink::Fetching::Done && guest.TakeBundle() == bundle->bytes,
          "an unreachable first asker cannot block another member's weapon files");
    Check(host.Serving() == 1, "only the unreachable asker's transfer remains queued");
    net.refusesTo["absent"] = false;
    net.Run(now, 20);
    Check(absent.State() == HostDataLink::Fetching::Done && absent.TakeBundle() == bundle->bytes,
          "the refused transfer resumes from its unsent part when the peer becomes reachable");
    Check(host.Serving() == 0, "both transfers finish without a re-ask");
}

}  // namespace

int main() {
    Plan();
    Merge();
    Notices();
    Prompt();
    Transfer();
    ManyBundles();
    Refusals();
    Forged();
    Stalls();
    ManyAskers();
    UnreachableAskerDoesNotBlockOthers();
    if (failures) {
        std::printf("%d host data link check(s) failed\n", failures);
        return 1;
    }
    std::printf("host data link: all checks passed\n");
    return 0;
}
