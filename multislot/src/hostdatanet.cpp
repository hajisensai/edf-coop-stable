#include "hostdatanet.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <set>

#include "hostdataopen.h"
#include "hostdataprompt.h"
#include "identity.h"
#include "log.h"

namespace multislot {

using hostdata::Digest;

namespace {

bool TakesPart(const LobbyView::Member& member) {
    const auto found = member.texts.find(kHostDataKey);
    return found != member.texts.end() && found->second == kHostDataFormat;
}

std::optional<Digest> TextDigest(const LobbyView::Member& member, const char* key) {
    if (!TakesPart(member)) return std::nullopt;
    const auto found = member.texts.find(key);
    return found == member.texts.end() ? std::nullopt : hostdata::DigestFromHex(found->second);
}

std::wstring RoomPart(const WeaponsView& view) {
    const std::wstring key = view.accept == HostAccept::Ask && view.acceptKey[0] ? std::wstring(view.acceptKey) + L" " : L"";
    const std::wstring count = L" (" + std::to_wstring(view.remote) + L")";
    if (view.accept == HostAccept::Never) return L"ROOM WEAPONS differ" + count;
    if (!view.wanted) return key + L"ROOM WEAPONS :OFF" + count;
    if (view.percent >= 0) return key + L"ROOM WEAPONS " + std::to_wstring(std::clamp(view.percent, 0, 99)) + L"%";
    std::wstring text = key + L"ROOM WEAPONS :ON" + count;
    if (view.failed) text += L" " + std::to_wstring(view.failed) + L" failed";
    if (view.lost) text += L" -" + std::to_wstring(view.lost) + L" of yours";
    return text;
}

}  // namespace

std::vector<RoomSource> PlanRoomSources(const LobbyView& view, const std::optional<Digest>& selfMods,
                                        const std::optional<Digest>& selfPage) {
    std::vector<RoomSource> sources;
    if (view.lobbyId.empty()) return sources;
    const auto page = [&](const LobbyView::Member& member) {
        return member.id == view.self ? selfPage : TextDigest(member, kPageDigestKey);
    };
    const auto owner = std::find_if(view.members.begin(), view.members.end(),
                                    [&view](const LobbyView::Member& member) { return member.id == view.owner; });
    if (owner != view.members.end()) {
        const auto mods = owner->id == view.self ? selfMods : TextDigest(*owner, kHostDigestKey);
        if (mods) sources.push_back({owner->id, *mods, true});
        if (const auto ownerPage = page(*owner)) sources.push_back({owner->id, *ownerPage, false});
    }
    for (const LobbyView::Member& member : view.members) {
        if (member.id == view.owner) continue;
        if (const auto memberPage = page(member)) sources.push_back({member.id, *memberPage, false});
    }
    return sources;
}

RoomOverlay MergeSources(const std::vector<const SourceFiles*>& sources) {
    RoomOverlay merged;
    merged.lost.assign(sources.size(), 0);
    std::map<std::string, const std::wstring*, std::less<>> claimed;  // path -> the folder of the first source with it
    for (std::size_t i = 0; i < sources.size(); ++i) {
        if (!sources[i]) continue;
        for (const std::string& path : sources[i]->paths)
            if (!claimed.try_emplace(path, &sources[i]->folder).second) ++merged.lost[i];
    }
    for (const auto& [path, folder] : claimed)
        if (!folder->empty()) merged.overlay.entries.push_back({path, *folder});
    return merged;
}

std::wstring WeaponsNotice(const WeaponsView& view) {
    const std::wstring room = view.inRoom && view.remote ? RoomPart(view) : L"";
    std::wstring page;
    if (view.pages)
        page = std::wstring(view.pageKey) + (view.pageKey[0] ? L" " : L"") + L"Page:" + (view.page.empty() ? L"off" : view.page);
    if (room.empty() || page.empty()) return room + page;
    return room + L"   " + page;
}

// --- HostDataLink ---

void HostDataLink::Share(std::vector<std::shared_ptr<const hostdata::Bundle>> bundles) {
    std::erase(bundles, nullptr);
    shared_ = std::move(bundles);
    served_.clear();
}

void HostDataLink::Share(std::shared_ptr<const hostdata::Bundle> bundle) {
    Share(std::vector<std::shared_ptr<const hostdata::Bundle>>{std::move(bundle)});
}

void HostDataLink::Fetch(const std::string& member, const Digest& digest, std::uint64_t now) {
    member_ = member;
    digest_ = digest;
    assembler_ = std::make_unique<hostdata::Assembler>(digest);
    fetched_.clear();
    failure_.clear();
    state_ = Fetching::Running;
    asks_ = 0;
    Ask(now);
}

void HostDataLink::Cancel() {
    state_ = Fetching::Idle;
    assembler_.reset();
    fetched_.clear();
}

void HostDataLink::Fail(const std::string& why) {
    state_ = Fetching::Failed;
    failure_ = why;
    assembler_.reset();
}

void HostDataLink::Ask(std::uint64_t now) {
    ++asks_;
    lastProgress_ = now;
    asked_ = send_(member_, hostdata::EncodeGet(digest_));
}

int HostDataLink::Percent() const {
    if (!assembler_ || !assembler_->Parts()) return 0;
    return static_cast<int>(assembler_->Received() * 100 / assembler_->Parts());
}

std::vector<std::uint8_t> HostDataLink::TakeBundle() {
    if (state_ == Fetching::Done) state_ = Fetching::Idle;
    return std::move(fetched_);
}

void HostDataLink::Received(const std::string& peer, const hostdata::Packet& packet, std::uint64_t now) {
    using enum hostdata::PacketType;
    if (packet.type == Get) {
        const auto bundle = std::find_if(shared_.begin(), shared_.end(),
                                         [&packet](const auto& shared) { return shared->digest == packet.digest; });
        if (bundle == shared_.end()) {
            send_(peer, hostdata::EncodeNone(packet.digest));
            return;
        }
        // Asking again starts over (what got lost is not known); a new question waits for a free place.
        auto served = std::find_if(served_.begin(), served_.end(), [&](const Served& s) {
            return s.peer == peer && s.bundle->digest == packet.digest;
        });
        if (served != served_.end())
            served->next = 0;
        else if (served_.size() < kMaxServed)
            served_.push_back({peer, *bundle, 0});
        return;
    }
    if (state_ != Fetching::Running || peer != member_ || packet.digest != digest_) return;
    if (packet.type == None) {
        Fail("the member no longer shares these files");
        return;
    }
    if (!assembler_->Add(packet)) return;
    lastProgress_ = now;
    if (!assembler_->Complete()) return;
    std::string why;
    auto bytes = assembler_->Take(&why);
    if (!bytes) {
        Fail(why);
        return;
    }
    fetched_ = std::move(*bytes);
    assembler_.reset();
    state_ = Fetching::Done;
}

void HostDataLink::Tick(std::uint64_t now) {
    // A question that could not go out yet goes now, and the wait for an answer starts when it did. One that cannot
    // go out for as long as all asks may take (the member never reached over P2P) gives up as well.
    if (state_ == Fetching::Running && !asked_) {
        asked_ = send_(member_, hostdata::EncodeGet(digest_));
        if (asked_)
            lastProgress_ = now;
        else if (now - lastProgress_ >= kStallMs * kMaxAsks)
            Fail("the member could not be reached over P2P");
    } else if (state_ == Fetching::Running && now - lastProgress_ >= kStallMs) {
        if (asks_ >= kMaxAsks)
            Fail("the member did not send its files (asked " + std::to_string(asks_) + " times)");
        else
            Ask(now);
    }
    // Round robin over what is being sent, one part each. A peer whose send is refused waits until the
    // next tick; the other peers must still get their files. Retry each refused peer at most once per tick.
    const auto done = [](const Served& s) { return s.next >= hostdata::PartCount(s.bundle->bytes.size()); };
    std::set<std::string> blocked;
    std::size_t budget = kPartsPerTick;
    while (budget && !served_.empty()) {
        bool sent = false;
        for (Served& served : served_) {
            if (!budget || done(served) || blocked.count(served.peer)) continue;
            if (!send_(served.peer, hostdata::EncodePart(*served.bundle, served.next))) {
                blocked.insert(served.peer);
                continue;
            }
            ++served.next;
            --budget;
            sent = true;
        }
        std::erase_if(served_, done);
        if (!sent) break;
    }
}

// --- the game's side ---
namespace {

constexpr EosResult kEosNotFound = 18;  // EOS_NotFound: what ReceivePacket returns when no packet is waiting
constexpr std::size_t kMaxPages = 16;

struct EosReceiveOptions {  // EOS_P2P_ReceivePacketOptions (packetfit.cpp)
    std::int32_t ApiVersion;
    const void* LocalUserId;
};
struct EosSocketId {  // EOS_P2P_SocketId
    std::int32_t ApiVersion;
    char SocketName[33];
};
static_assert(sizeof(EosSocketId) == 40, "EOS_P2P_SocketId");
constexpr std::int32_t kReliableOrdered = 2;  // EOS_PR_ReliableOrdered

std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }

// Deletes `path` (a folder of ours under the store, at most two levels deep) and everything in it.
void DeleteTree(const std::wstring& path) {
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = found.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring child = path + L"\\" + name;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                DeleteTree(child);
            else
                DeleteFileW(child.c_str());
        } while (FindNextFileW(find, &found));
        FindClose(find);
    }
    RemoveDirectoryW(path.c_str());
}

bool WriteWhole(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
                    written == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    return ok;
}

std::vector<std::string> PathsOf(const std::vector<hostdata::DataFile>& files) {
    std::vector<std::string> paths;
    for (const hostdata::DataFile& file : files) paths.push_back(file.path);
    return paths;
}

// A weapon page of this machine: Mods\Variants\<name>.
struct Page {
    std::wstring name;
    std::shared_ptr<const hostdata::Bundle> bundle;
    SourceFiles files;  // its folder as the game takes it: ./Mods/Variants/<name>/
};

struct Runtime {
    std::mutex lock;
    HostDataSettings settings;
    std::wstring store;  // <game>\Mods\Plugins\EDF6Coop.hostdata
    std::shared_ptr<const hostdata::Bundle> mods;  // this machine's Mods, shareable or not
    SourceFiles own;     // and as a source: files the game reads anyway
    Digest empty{};      // of a bundle without files
    std::vector<Page> pages;
    int page = -1;       // the page picked, -1 none
    LobbyView view;      // the lobby as last read; no lobby outside a room
    std::string lobby;   // the lobby `approved`, `declined` and `failed` are about
    std::set<Digest> approved;           // other members' bundles the game may read (Ask: the player said yes)
    std::set<Digest> declined;           // and those it must not (the player said no, or gave them back)
    bool fetching = false;               // a bundle not declined is still on its way: the question waits for it
    bool asking = false;                 // the question is on screen
    // What the player answered, for the next menu frame: the window runs on a thread of its own, and acting on the
    // answer may start a fetch, which calls EOS - only ever from the game's thread.
    struct Answer {
        std::string lobby;
        std::vector<Digest> digests;
        bool use = false;
    };
    std::vector<Answer> answers;
    std::map<Digest, SourceFiles> have;  // other members' bundles here and checked
    std::set<Digest> failed;             // those that could not be fetched in this lobby
    std::vector<RoomSource> sources;     // the room's sources as last worked out
    std::size_t remote = 0;              // of them, not this machine's own
    bool waiting = false;                // one of them is still being fetched: `next` waits for it
    std::shared_ptr<const hostdata::Overlay> next;  // what the game reads from the next menu frame
    std::size_t lost = 0;                // files of this machine's page another source has first, in `next`
    bool loggedServing = false;
    bool storing = false;  // a fetched bundle is being written (outside the lock)
    // EOS, learnt from what the game receives.
    EosReceiveFn receive = nullptr;
    EosSendFn send = nullptr;
    void* p2p = nullptr;
    const void* localUser = nullptr;
    EosSocketId socket{};
    std::map<const void*, std::string, std::less<>> peerText;
    std::map<std::string, const void*, std::less<>> peerHandle;
    HostDataLink link{[](const std::string& peer, const std::vector<std::uint8_t>& packet) {
        return SendLocked(peer, packet);
    }};

    static bool SendLocked(const std::string& peer, const std::vector<std::uint8_t>& packet);
};

Runtime& Rt() {
    static Runtime runtime;
    return runtime;
}

// Caller holds Rt().lock (the link sends from inside its calls).
bool Runtime::SendLocked(const std::string& peer, const std::vector<std::uint8_t>& packet) {
    Runtime& rt = Rt();
    const auto handle = rt.peerHandle.find(peer);
    if (!rt.send || !rt.p2p || !rt.localUser || !rt.socket.SocketName[0] || handle == rt.peerHandle.end()) return false;
    const EosSendOptions options{2, rt.localUser, handle->second, &rt.socket, kHostDataChannel,
                                 static_cast<std::uint32_t>(packet.size()), packet.data(), 1, kReliableOrdered, 0};
    return rt.send(rt.p2p, &options) == 0;
}

std::wstring OverlayFolder(const Digest& digest) {
    return L"./Mods/Plugins/EDF6Coop.hostdata/" + Wide(hostdata::DigestHex(digest)) + L"/";
}

// A bundle fetched before and kept in the store, checked again. Caller holds the lock.
std::optional<SourceFiles> CachedLocked(const Digest& digest) {
    const std::wstring folder = Rt().store + L"\\" + Wide(hostdata::DigestHex(digest));
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return std::nullopt;
    std::vector<hostdata::DataFile> files = hostdata::ScanMods(folder, nullptr);
    const auto bundle = hostdata::MakeBundle(files, nullptr);
    if (bundle && bundle->digest == digest) return SourceFiles{PathsOf(files), OverlayFolder(digest)};
    Log("Host data: the kept copy of %s no longer matches; fetching it again", hostdata::DigestHex(digest).c_str());
    DeleteTree(folder);
    return std::nullopt;
}

// Writes a fetched bundle to `store`: into <digest>.part, then renamed. Its files, or nullopt. Without the lock (up
// to 4 MB of disk writes must not hold up the game's receive and menu).
std::optional<std::vector<hostdata::DataFile>> WriteStore(const std::wstring& store, const Digest& digest,
                                                          const std::vector<std::uint8_t>& bytes) {
    std::string why;
    auto files = hostdata::ParseBundle(bytes, &why);
    if (!files) {
        Log("Host data: the files fetched were refused: %s", why.c_str());
        return std::nullopt;
    }
    const std::wstring name = store + L"\\" + Wide(hostdata::DigestHex(digest));
    const std::wstring part = name + L".part";
    DeleteTree(part);
    CreateDirectoryW(store.c_str(), nullptr);
    bool ok = CreateDirectoryW(part.c_str(), nullptr) && CreateDirectoryW((part + L"\\WEAPON").c_str(), nullptr) &&
              CreateDirectoryW((part + L"\\OBJECT").c_str(), nullptr);
    for (const hostdata::DataFile& file : *files) {
        std::wstring path = part + L"\\" + Wide(file.path);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        ok = ok && WriteWhole(path, file.bytes);
    }
    // Only what was just written and checked becomes <digest>: a folder that is somehow there already is not used.
    ok = ok && MoveFileExW(part.c_str(), name.c_str(), 0);
    if (!ok) {
        Log("Host data: the files fetched could not be written to %ls (error %lu)", part.c_str(), GetLastError());
        DeleteTree(part);
        return std::nullopt;
    }
    Log("Host data: %zu file(s) from the room kept in %ls", files->size(), name.c_str());
    return files;
}

// A digest that stands for files this machine has of its own: no files, its Mods, or one of its pages.
const SourceFiles* OwnFilesLocked(const Digest& digest) {
    static const SourceFiles none;
    Runtime& rt = Rt();
    if (digest == rt.empty) return &none;
    if (rt.mods && digest == rt.mods->digest) return &rt.own;
    const auto page = std::find_if(rt.pages.begin(), rt.pages.end(), [&digest](const Page& p) { return p.bundle->digest == digest; });
    return page == rt.pages.end() ? nullptr : &page->files;
}

std::optional<Digest> PageDigestLocked() {
    const Runtime& rt = Rt();
    return rt.page >= 0 ? std::optional<Digest>(rt.pages[static_cast<std::size_t>(rt.page)].bundle->digest) : std::nullopt;
}

// The room's sources; outside a room this machine's page alone.
std::vector<RoomSource> SourcesLocked() {
    Runtime& rt = Rt();
    const std::optional<Digest> page = PageDigestLocked();
    if (rt.view.lobbyId.empty()) return page ? std::vector<RoomSource>{{std::string(), *page, false}} : std::vector<RoomSource>{};
    const std::optional<Digest> mods = rt.settings.share && rt.mods ? std::optional<Digest>(rt.mods->digest) : std::nullopt;
    return PlanRoomSources(rt.view, mods, page);
}

// Another member's bundle as this machine has it (fetched in this run, or kept from an earlier one), or null.
// Caller holds the lock.
const SourceFiles* HaveLocked(const Digest& digest) {
    Runtime& rt = Rt();
    if (!rt.have.contains(digest))
        if (auto kept = CachedLocked(digest)) rt.have.emplace(digest, std::move(*kept));
    const auto have = rt.have.find(digest);
    return have == rt.have.end() ? nullptr : &have->second;
}

// Starts or keeps the fetch of `missing`, or stops one nothing needs. Caller holds the lock.
void FetchLocked(const std::optional<RoomSource>& missing) {
    Runtime& rt = Rt();
    const bool running = rt.link.State() == HostDataLink::Fetching::Running;
    if (!missing) {
        if (running) rt.link.Cancel();
        return;
    }
    if (rt.storing || (running && rt.link.FetchDigest() == missing->digest && rt.link.FetchMember() == missing->member))
        return;
    rt.link.Fetch(missing->member, missing->digest, GetTickCount64());
    Log("Host data: fetching %s %s from %.8s", missing->mods ? "the host's Mods" : "a weapon page",
        hostdata::DigestHex(missing->digest).c_str(), missing->member.c_str());
}

// What the lobby, the page and what has arrived say now: the files the game reads from the next menu frame, and
// what is still fetched. Caller holds the lock.
void DecideLocked() {
    Runtime& rt = Rt();
    if (rt.view.lobbyId != rt.lobby) {  // another lobby, or none: what was answered or failed was about the last one
        rt.lobby = rt.view.lobbyId;
        rt.approved.clear();
        rt.declined.clear();
        rt.failed.clear();
    }
    const std::vector<RoomSource> sources = SourcesLocked();
    if (sources != rt.sources && !rt.view.lobbyId.empty())
        Log("Host data: the room's weapon files come from %zu source(s) (its host's Mods and the pages its members use)",
            sources.size());
    rt.sources = sources;
    rt.remote = 0;
    std::vector<const SourceFiles*> files;
    std::optional<RoomSource> missing;
    bool waiting = false;  // a bundle the game is to read has not arrived
    for (const RoomSource& source : rt.sources) {
        const SourceFiles* here = OwnFilesLocked(source.digest);
        if (here) {
            files.push_back(here);
            continue;
        }
        ++rt.remote;
        // Fetched before anyone is asked, so the question can name the files; read only once approved.
        const bool skip = rt.settings.accept == HostAccept::Never || rt.failed.contains(source.digest) ||
                          rt.declined.contains(source.digest);
        if (!skip && rt.settings.accept == HostAccept::Auto) rt.approved.insert(source.digest);
        const SourceFiles* have = skip ? nullptr : HaveLocked(source.digest);
        if (!skip && !have && !missing) missing = source;
        const bool used = !skip && rt.approved.contains(source.digest);
        waiting = waiting || (used && !have);
        files.push_back(used ? have : nullptr);
    }
    FetchLocked(missing);
    rt.fetching = missing.has_value();
    rt.waiting = waiting;
    if (rt.waiting) return;
    RoomOverlay merged = MergeSources(files);
    rt.lost = 0;
    for (std::size_t i = 0; i < rt.sources.size(); ++i)
        if (!rt.sources[i].mods && (rt.sources[i].member.empty() || rt.sources[i].member == rt.view.self))
            rt.lost = merged.lost[i];
    rt.next = std::make_shared<const hostdata::Overlay>(std::move(merged.overlay));
}

// Points the game at `next` if it is not already: on a menu frame, where no mission is reading its files.
void ApplyLocked() {
    Runtime& rt = Rt();
    if (rt.waiting || !rt.next) return;
    const auto current = HostOverlay();
    const std::size_t files = rt.next->entries.size();
    if ((current ? current->entries : std::vector<hostdata::Overlay::Entry>{}) == rt.next->entries) return;
    SetHostOverlay(files ? rt.next : nullptr);
    if (files)
        Log("Host data: the game now reads %zu weapon/vehicle file(s) in place of its own (%zu from other members)",
            files, static_cast<std::size_t>(std::count_if(rt.next->entries.begin(), rt.next->entries.end(),
                                                          [](const auto& e) { return e.folder.starts_with(L"./Mods/Plugins/"); })));
    else
        Log("Host data: back to this machine's own weapon files");
}

void ShareLocked() {
    Runtime& rt = Rt();
    if (!rt.settings.share) return;
    rt.link.Share({rt.mods, rt.page >= 0 ? rt.pages[static_cast<std::size_t>(rt.page)].bundle : nullptr});
    const std::optional<Digest> page = PageDigestLocked();
    PublishMemberText(kPageDigestKey, page ? hostdata::DigestHex(*page) : std::string(kPageNone));
}

void NextPageLocked() {
    Runtime& rt = Rt();
    if (rt.pages.empty()) return;
    rt.page = rt.page + 1 >= static_cast<int>(rt.pages.size()) ? -1 : rt.page + 1;
    const std::wstring name = rt.page >= 0 ? rt.pages[static_cast<std::size_t>(rt.page)].name : std::wstring();
    if (!rt.settings.iniPath.empty())
        WritePrivateProfileStringW(L"HostData", L"Page", name.c_str(), rt.settings.iniPath.c_str());
    Log("Host data: %ls - weapon page %ls (from the next mission)", rt.settings.pageKeyName,
        name.empty() ? L"off" : name.c_str());
    ShareLocked();
    DecideLocked();
}

void Observe(const LobbyView& view) {
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    rt.view = view;
    DecideLocked();
}

// After every EOS tick: the link's sends, and what a fetch came to.
bool TickLocked();

void AfterTick(void*) {
    Runtime& rt = Rt();
    Digest fetched{};
    std::vector<std::uint8_t> bytes;
    std::wstring store;
    {
        std::scoped_lock lock(rt.lock);
        if (!TickLocked()) return;
        fetched = rt.link.FetchDigest();
        bytes = rt.link.TakeBundle();
        store = rt.store;
        rt.storing = true;
    }
    const auto files = WriteStore(store, fetched, bytes);
    std::scoped_lock lock(rt.lock);
    rt.storing = false;
    if (files)
        rt.have[fetched] = SourceFiles{PathsOf(*files), OverlayFolder(fetched)};
    else
        rt.failed.insert(fetched);
    DecideLocked();
}

// The tick's part under the lock: true when a fetched bundle is ready to be written.
bool TickLocked() {
    Runtime& rt = Rt();
    rt.link.Tick(GetTickCount64());
    if (rt.link.Serving() && !rt.loggedServing) Log("Host data: sending this machine's files to %zu player(s)", rt.link.Serving());
    rt.loggedServing = rt.link.Serving() != 0;
    if (rt.link.State() == HostDataLink::Fetching::Failed) {
        Log("Host data: fetching %s failed: %s", hostdata::DigestHex(rt.link.FetchDigest()).c_str(), rt.link.Failure().c_str());
        rt.failed.insert(rt.link.FetchDigest());
        rt.link.Cancel();
        DecideLocked();
    }
    return rt.link.State() == HostDataLink::Fetching::Done;
}

// Who `peer` is, as text; learnt once per handle (EOS keeps a user's handle for as long as it runs).
// Caller holds the lock.
const std::string& PeerTextLocked(const void* peer) {
    Runtime& rt = Rt();
    auto known = rt.peerText.find(peer);
    if (known != rt.peerText.end()) return known->second;
    char id[40]{};
    const std::string text = ProductUserIdText(peer, id, sizeof(id));
    rt.peerHandle[text] = peer;
    return rt.peerText.emplace(peer, text).first->second;
}

EosResult HostDataReceive(void* handle, const void* options, void** peer, void* socket, std::uint8_t* channel, void* data,
                          std::uint32_t* size) {
    Runtime& rt = Rt();
    // RedirectGameImport stores the original before it swaps the import, so the game cannot reach this wrapper
    // without one; should it ever, the game is told no packet came rather than calling nothing.
    const EosReceiveFn receive = rt.receive;
    if (!receive) return kEosNotFound;
    for (;;) {
        const EosResult result = receive(handle, options, peer, socket, channel, data, size);
        if (result != 0 || !data || !size || !channel || !peer || !*peer) return result;
        std::scoped_lock lock(rt.lock);
        rt.p2p = handle;
        if (options) rt.localUser = static_cast<const EosReceiveOptions*>(options)->LocalUserId;
        const std::string& from = PeerTextLocked(*peer);
        if (*channel != kHostDataChannel) {
            if (socket) std::memcpy(&rt.socket, socket, sizeof(rt.socket));  // the game's socket, as it is now
            return result;
        }
        // Ours, whatever it holds: the game never sends on this channel and must not see it.
        if (const auto packet = hostdata::DecodePacket(static_cast<const std::uint8_t*>(data), *size))
            rt.link.Received(from, *packet, GetTickCount64());
    }
}

// Removes what is left of an interrupted write, and all but the newest kept bundles.
void Tidy(const std::wstring& store) {
    constexpr std::size_t kKept = 16;
    struct Kept {
        std::wstring name;
        FILETIME written;
    };
    std::vector<Kept> kept;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((store + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = found.cFileName;
        if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || name == L"." || name == L"..") continue;
        std::string narrow;
        for (const wchar_t c : name) narrow.push_back(c < 0x80 ? static_cast<char>(c) : '?');
        if (hostdata::DigestFromHex(narrow))
            kept.push_back({name, found.ftLastWriteTime});
        else if (name.ends_with(L".part"))
            DeleteTree(store + L"\\" + name);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) { return CompareFileTime(&a.written, &b.written) > 0; });
    for (std::size_t i = kKept; i < kept.size(); ++i) DeleteTree(store + L"\\" + kept[i].name);
}

// The folders under Mods\Variants, by name.
std::vector<std::wstring> PageNames(const std::wstring& variants) {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((variants + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        const std::wstring name = found.cFileName;
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && name != L"." && name != L"..") names.push_back(name);
    } while (FindNextFileW(find, &found));
    FindClose(find);
    std::sort(names.begin(), names.end());
    return names;
}

// This machine's weapon pages, each checked as shared files are. Caller holds the lock.
void LoadPagesLocked() {
    Runtime& rt = Rt();
    const std::wstring variants = rt.settings.gameFolder + L"\\Mods\\Variants";
    for (const std::wstring& name : PageNames(variants)) {
        if (rt.pages.size() == kMaxPages) {
            Log("Host data: more than %zu weapon pages under Mods\\Variants; %ls and after are not used", kMaxPages, name.c_str());
            break;
        }
        std::vector<std::string> skipped;
        std::vector<hostdata::DataFile> files = hostdata::ScanMods(variants + L"\\" + name, &skipped);
        for (const std::string& line : skipped) Log("Host data: weapon page %ls: %s; not used", name.c_str(), line.c_str());
        std::vector<std::string> paths = PathsOf(files);
        std::string why;
        auto bundle = hostdata::MakeBundle(std::move(files), &why);
        if (!bundle || paths.empty()) {
            Log("Host data: weapon page %ls is not used (%s)", name.c_str(), bundle ? "no weapon or vehicle files" : why.c_str());
            continue;
        }
        Log("Host data: weapon page %ls: %zu file(s), %zu bytes, SHA-256 %s", name.c_str(), paths.size(),
            bundle->bytes.size(), hostdata::DigestHex(bundle->digest).c_str());
        Page page;
        page.name = name;
        page.bundle = std::make_shared<const hostdata::Bundle>(std::move(*bundle));
        page.files.paths = std::move(paths);
        page.files.folder = L"./Mods/Variants/" + name + L"/";
        rt.pages.push_back(std::move(page));
    }
    const auto picked = std::find_if(rt.pages.begin(), rt.pages.end(),
                                     [&rt](const Page& p) { return _wcsicmp(p.name.c_str(), rt.settings.page.c_str()) == 0; });
    if (picked != rt.pages.end())
        rt.page = static_cast<int>(picked - rt.pages.begin());
    else if (!rt.settings.page.empty())
        Log("Host data: [HostData] Page=%ls is not a weapon page here; no page", rt.settings.page.c_str());
}

// This machine's Mods, scanned and checked. Caller holds the lock.
void LoadModsLocked() {
    Runtime& rt = Rt();
    std::vector<std::string> skipped;
    std::vector<hostdata::DataFile> files = hostdata::ScanMods(rt.settings.gameFolder + L"\\Mods", &skipped);
    for (const std::string& line : skipped) Log("Host data: %s; not shared", line.c_str());
    rt.own.paths = PathsOf(files);
    std::string why;
    auto bundle = hostdata::MakeBundle(std::move(files), &why);
    if (const auto empty = hostdata::MakeBundle({}, nullptr)) rt.empty = empty->digest;
    if (!bundle) {
        Log("Host data: this machine's weapon files cannot be shared (%s); nothing is offered", why.c_str());
        return;
    }
    Log("Host data: %zu weapon/vehicle file(s) of this machine, %zu bytes, SHA-256 %s", bundle->files,
        bundle->bytes.size(), hostdata::DigestHex(bundle->digest).c_str());
    rt.mods = std::make_shared<const hostdata::Bundle>(std::move(*bundle));
}

// Other members' bundles in the room, in the room's order.
std::vector<RoomSource> RemoteLocked() {
    Runtime& rt = Rt();
    std::vector<RoomSource> remote;
    for (const RoomSource& source : rt.sources)
        if (!OwnFilesLocked(source.digest)) remote.push_back(source);
    return remote;
}

// The game reads (or is about to read) files of another member. Caller holds the lock.
bool TakenLocked() {
    const Runtime& rt = Rt();
    const std::vector<RoomSource> remote = RemoteLocked();
    return std::any_of(remote.begin(), remote.end(), [&rt](const RoomSource& source) {
        return rt.approved.contains(source.digest) && !rt.declined.contains(source.digest) &&
               !rt.failed.contains(source.digest);
    });
}

// `use` (or not) the bundles `digests`, for as long as this machine stays in the lobby. Caller holds the lock.
void AnswerLocked(const std::vector<Digest>& digests, bool use) {
    Runtime& rt = Rt();
    for (const Digest& digest : digests) {
        (use ? rt.approved : rt.declined).insert(digest);
        (use ? rt.declined : rt.approved).erase(digest);
    }
    DecideLocked();
}

// On the window's thread.
void Answered(const std::string& lobby, const std::vector<Digest>& digests, bool use) {
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    rt.asking = false;
    rt.answers.push_back({lobby, digests, use});
}

// The answers given since the last menu frame. Caller holds the lock.
void TakeAnswersLocked() {
    Runtime& rt = Rt();
    for (const Runtime::Answer& answer : rt.answers) {
        if (answer.lobby != rt.lobby) {
            Log("Host data: the question was answered after leaving that room; nothing changes");
            continue;
        }
        Log("Host data: the player %s the room's files (%zu bundle(s)); %ls switches", answer.use ? "takes" : "declines",
            answer.digests.size(), rt.settings.keyName);
        AnswerLocked(answer.digests, answer.use);
    }
    rt.answers.clear();
}

// On a menu frame in a room (Accept=Ask): asks about the bundles nobody answered for yet, once every bundle that is
// coming has arrived, so one question names them all. Caller holds the lock.
void AskLocked() {
    Runtime& rt = Rt();
    if (rt.settings.accept != HostAccept::Ask || rt.asking || !rt.answers.empty() || rt.fetching || rt.storing) return;
    std::vector<PromptSource> prompt;
    std::vector<Digest> digests;
    std::size_t files = 0;
    for (const RoomSource& source : RemoteLocked()) {
        const auto have = rt.have.find(source.digest);
        if (have == rt.have.end() || rt.approved.contains(source.digest) || rt.declined.contains(source.digest) ||
            rt.failed.contains(source.digest))
            continue;
        prompt.push_back({source.mods, source.member, have->second.paths});
        digests.push_back(source.digest);
        files += have->second.paths.size();
    }
    if (digests.empty()) return;
    std::vector<std::string> yours = rt.own.paths;
    std::sort(yours.begin(), yours.end());
    const PromptLanguage language = PromptLanguageFor(GetUserDefaultUILanguage());
    rt.asking = AskInWindow(PromptTitle(language), PromptText(prompt, yours, rt.settings.keyName, language),
                            [lobby = rt.lobby, digests](bool use) { Answered(lobby, digests, use); });
    if (rt.asking) {
        Log("Host data: asking the player about %zu bundle(s) of other members, %zu file(s)", digests.size(), files);
        return;
    }
    Log("Host data: keeping this machine's own files; %ls takes the room's", rt.settings.keyName);
    AnswerLocked(digests, false);
}

// AcceptKey: gives back what is taken, or takes every bundle of the room. Caller holds the lock.
void SwitchLocked() {
    std::vector<Digest> digests;
    for (const RoomSource& source : RemoteLocked()) digests.push_back(source.digest);
    const bool take = !TakenLocked();
    Log("Host data: %ls - %s the room's weapon files", Rt().settings.keyName, take ? "taking" : "giving back");
    AnswerLocked(digests, take);
}

}  // namespace

bool StartHostData(HMODULE game, ImportRedirect redirect, const HostDataSettings& settings) {
    Runtime& rt = Rt();
    const HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    {
        std::scoped_lock lock(rt.lock);
        rt.settings = settings;
        rt.store = settings.gameFolder + L"\\Mods\\Plugins\\EDF6Coop.hostdata";
        Tidy(rt.store);
        LoadModsLocked();
        LoadPagesLocked();
        Log("Host data: %zu weapon page(s) under Mods\\Variants; using %ls (%ls switches)", rt.pages.size(),
            rt.page >= 0 ? rt.pages[static_cast<std::size_t>(rt.page)].name.c_str() : L"none", settings.pageKeyName);
        rt.send = eos ? reinterpret_cast<EosSendFn>(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_P2P_SendPacket")))
                      : nullptr;
        DecideLocked();  // outside a room: the page, from the first menu frame
    }
    // Only once our channel is taken out before the game reads it does this machine say it takes part: a member
    // sends our packets only to one that said so, and a machine without the wrapper would read them as game data.
    const bool received = rt.send && redirect(game, "EOSSDK-Win64-Shipping.dll", "EOS_P2P_ReceivePacket",
                                              reinterpret_cast<void*>(&HostDataReceive),
                                              reinterpret_cast<void**>(&rt.receive));
    if (!received) {
        Log("Host data: EOS P2P could not be reached; nothing is offered or fetched (pages still work offline)");
        return true;
    }
    PublishMemberText(kHostDataKey, kHostDataFormat);
    {
        std::scoped_lock lock(rt.lock);
        if (settings.share && rt.mods) PublishMemberText(kHostDigestKey, hostdata::DigestHex(rt.mods->digest));
        ShareLocked();
    }
    WatchMemberTexts({kHostDataKey, kHostDigestKey, kPageDigestKey}, &Observe);
    ListenToTicks(&AfterTick);
    return true;
}

std::wstring HostDataMenuFrame(bool inRoom, bool acceptPressed, bool pagePressed) {
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    if (pagePressed) NextPageLocked();
    TakeAnswersLocked();
    if (acceptPressed && inRoom && rt.remote && rt.settings.accept == HostAccept::Ask) SwitchLocked();
    if (inRoom && !rt.view.lobbyId.empty()) AskLocked();
    // Between missions: this is the menu, so the game is not in the middle of reading a mission's files.
    ApplyLocked();
    WeaponsView view;
    view.pageKey = rt.settings.pageKeyName;
    view.acceptKey = rt.settings.keyName;
    view.pages = rt.pages.size();
    view.page = rt.page >= 0 ? rt.pages[static_cast<std::size_t>(rt.page)].name : std::wstring();
    // Only what is shown follows the game's room session; what is taken follows the lobby (Observe). While joining,
    // the lobby is there before the game's session is.
    view.inRoom = inRoom && !rt.view.lobbyId.empty();
    view.remote = rt.remote;
    view.accept = rt.settings.accept;
    view.wanted = TakenLocked();
    // While a fetched bundle is written the link holds nothing any more (its bytes were taken): all of it came.
    if (rt.waiting) view.percent = rt.storing ? 100 : rt.link.Percent();
    view.failed = static_cast<std::size_t>(std::count_if(
        rt.sources.begin(), rt.sources.end(), [&rt](const RoomSource& source) { return rt.failed.contains(source.digest); }));
    view.using_ = !rt.waiting;
    view.lost = rt.lost;
    return WeaponsNotice(view);
}

}  // namespace multislot
