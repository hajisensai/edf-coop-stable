#include "hostdatanet.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include "hostdataopen.h"
#include "identity.h"
#include "log.h"

namespace multislot {

using hostdata::Digest;

HostDataRoom ReadHostDataRoom(const LobbyView& view) {
    HostDataRoom room;
    room.inRoom = !view.lobbyId.empty();
    room.hosting = room.inRoom && !view.owner.empty() && view.owner == view.self;
    room.hostId = view.owner;
    for (const LobbyView::Member& member : view.members) {
        if (member.id != view.owner) continue;
        const auto takesPart = member.texts.find(kHostDataKey);
        room.hostTakesPart = takesPart != member.texts.end() && takesPart->second == kHostDataFormat;
        const auto digest = member.texts.find(kHostDigestKey);
        if (room.hostTakesPart && digest != member.texts.end()) room.hostDigest = hostdata::DigestFromHex(digest->second);
    }
    return room;
}

std::wstring HostDataNotice(HostDataStage stage, HostAccept accept, const wchar_t* keyName, int percent, std::size_t extra) {
    using enum HostDataStage;
    const std::wstring key = keyName && keyName[0] && accept != HostAccept::Never ? std::wstring(keyName) + L" " : L"";
    switch (stage) {
        case HostHasNone: return L"host has no weapon mods";
        case Differs: return accept == HostAccept::Never ? L"weapons differ from host" : key + L"host weapons :OFF";
        case Fetching: return key + L"host weapons " + std::to_wstring(std::clamp(percent, 0, 99)) + L"%";
        case Ready: return key + L"host weapons :ON";
        case Using: return key + L"host weapons :ON" + (extra ? L" +" + std::to_wstring(extra) + L" own" : L"");
        case Failed: return key + L"host weapons failed (log)";
        default: return L"";
    }
}

// --- HostDataLink ---

void HostDataLink::Share(std::shared_ptr<const hostdata::Bundle> bundle) {
    shared_ = std::move(bundle);
    served_.clear();
}

void HostDataLink::Fetch(const std::string& host, const Digest& digest, std::uint64_t now) {
    host_ = host;
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
    asked_ = send_(host_, hostdata::EncodeGet(digest_));
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
        if (!shared_ || packet.digest != shared_->digest) {
            send_(peer, hostdata::EncodeNone(packet.digest));
            return;
        }
        // Asking again starts over (what got lost is not known); a new asker waits for a free place.
        auto served = std::find_if(served_.begin(), served_.end(), [&peer](const Served& s) { return s.peer == peer; });
        if (served != served_.end())
            served->next = 0;
        else if (served_.size() < kMaxServed)
            served_.push_back({peer, 0});
        return;
    }
    if (state_ != Fetching::Running || peer != host_ || packet.digest != digest_) return;
    if (packet.type == None) {
        Fail("the host no longer shares these files");
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
    // A question that could not go out yet goes now, and the wait for an answer starts when it did.
    if (state_ == Fetching::Running && !asked_) {
        asked_ = send_(host_, hostdata::EncodeGet(digest_));
        if (asked_) lastProgress_ = now;
    } else if (state_ == Fetching::Running && now - lastProgress_ >= kStallMs) {
        if (asks_ >= kMaxAsks)
            Fail("the host did not send its files (asked " + std::to_string(asks_) + " times)");
        else
            Ask(now);
    }
    // Round robin over whoever is being served, one part each, until the budget is spent or a send is refused.
    std::size_t budget = kPartsPerTick;
    while (budget && !served_.empty() && shared_) {
        bool sent = false;
        for (Served& served : served_) {
            if (!budget || served.next >= hostdata::PartCount(shared_->bytes.size())) continue;
            if (!send_(served.peer, hostdata::EncodePart(*shared_, served.next))) return;
            ++served.next;
            --budget;
            sent = true;
        }
        std::erase_if(served_, [this](const Served& s) { return s.next >= hostdata::PartCount(shared_->bytes.size()); });
        if (!sent) break;
    }
}

// --- the game's side ---
namespace {

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

struct Runtime {
    std::mutex lock;
    HostDataSettings settings;
    std::wstring store;  // <game>\Mods\Plugins\EDF6Coop.hostdata
    Digest own{};        // the digest of our own files, shared or not
    Digest empty{};      // of a bundle without files
    std::vector<std::string> ownPaths;
    HostDataRoom room;
    HostDataStage stage = HostDataStage::None;
    bool wanted = false;  // the player takes the host's files in this room
    std::optional<Digest> target;  // the host digest `wanted` and `ready` are about
    std::shared_ptr<const hostdata::Overlay> ready;
    std::size_t extra = 0;
    bool loggedServing = false;
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

// The host's files `files` (checked) for the overlay, and how many of ours they leave alone. Caller holds the lock.
std::shared_ptr<const hostdata::Overlay> MakeOverlayLocked(const Digest& digest, const std::vector<hostdata::DataFile>& files) {
    auto overlay = std::make_shared<hostdata::Overlay>();
    for (const hostdata::DataFile& file : files) overlay->paths.push_back(file.path);
    overlay->folder = OverlayFolder(digest);
    Runtime& rt = Rt();
    rt.extra = static_cast<std::size_t>(std::count_if(rt.ownPaths.begin(), rt.ownPaths.end(), [&overlay](const std::string& p) {
        return !std::binary_search(overlay->paths.begin(), overlay->paths.end(), p);
    }));
    return overlay;
}

// A bundle fetched before and kept in the store, checked again. Caller holds the lock.
std::shared_ptr<const hostdata::Overlay> CachedLocked(const Digest& digest) {
    const std::wstring folder = Rt().store + L"\\" + Wide(hostdata::DigestHex(digest));
    if (GetFileAttributesW(folder.c_str()) == INVALID_FILE_ATTRIBUTES) return nullptr;
    std::vector<hostdata::DataFile> files = hostdata::ScanMods(folder, nullptr);
    const auto bundle = hostdata::MakeBundle(files, nullptr);
    if (bundle && bundle->digest == digest) return MakeOverlayLocked(digest, files);
    Log("Host data: the kept copy of %s no longer matches; fetching it again", hostdata::DigestHex(digest).c_str());
    DeleteTree(folder);
    return nullptr;
}

// Writes a fetched bundle to the store: into <digest>.part, then renamed. Caller holds the lock.
std::shared_ptr<const hostdata::Overlay> StoreLocked(const Digest& digest, const std::vector<std::uint8_t>& bytes) {
    std::string why;
    const auto files = hostdata::ParseBundle(bytes, &why);
    if (!files) {
        Log("Host data: the host's files were refused: %s", why.c_str());
        return nullptr;
    }
    const std::wstring name = Rt().store + L"\\" + Wide(hostdata::DigestHex(digest));
    const std::wstring part = name + L".part";
    DeleteTree(part);
    CreateDirectoryW(Rt().store.c_str(), nullptr);
    bool ok = CreateDirectoryW(part.c_str(), nullptr) && CreateDirectoryW((part + L"\\WEAPON").c_str(), nullptr) &&
              CreateDirectoryW((part + L"\\OBJECT").c_str(), nullptr);
    for (const hostdata::DataFile& file : *files) {
        std::wstring path = part + L"\\" + Wide(file.path);
        std::replace(path.begin(), path.end(), L'/', L'\\');
        ok = ok && WriteWhole(path, file.bytes);
    }
    ok = ok && (MoveFileExW(part.c_str(), name.c_str(), 0) || GetFileAttributesW(name.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (!ok) {
        Log("Host data: the host's files could not be written to %ls (error %lu)", part.c_str(), GetLastError());
        DeleteTree(part);
        return nullptr;
    }
    Log("Host data: %zu file(s) of the host kept in %ls", files->size(), name.c_str());
    return MakeOverlayLocked(digest, *files);
}

// Stops using the host's files (they stay in the store). Caller holds the lock.
void DropLocked(const char* why) {
    Runtime& rt = Rt();
    rt.link.Cancel();
    rt.ready.reset();
    if (HostOverlay()) {
        SetHostOverlay(nullptr);
        Log("Host data: back to this machine's own weapon files (%s)", why);
    }
}

// Gets the host's files: kept from before, or fetched. Caller holds the lock.
void TakeLocked() {
    Runtime& rt = Rt();
    if (!rt.target || rt.ready || rt.link.State() == HostDataLink::Fetching::Running) return;
    rt.ready = CachedLocked(*rt.target);
    if (rt.ready) {
        rt.stage = HostDataStage::Ready;
        return;
    }
    rt.link.Fetch(rt.room.hostId, *rt.target, GetTickCount64());
    rt.stage = HostDataStage::Fetching;
    Log("Host data: fetching the host's files %s", hostdata::DigestHex(*rt.target).c_str());
}

// What the lobby says now decides the stage. Caller holds the lock.
void DecideLocked() {
    using enum HostDataStage;
    Runtime& rt = Rt();
    const HostDataRoom& room = rt.room;
    const bool guest = room.inRoom && !room.hosting && room.hostTakesPart && room.hostDigest;
    const std::optional<Digest> target = guest && *room.hostDigest != rt.own && *room.hostDigest != rt.empty
                                             ? room.hostDigest
                                             : std::nullopt;
    if (target != rt.target) {
        DropLocked(room.inRoom ? "the host's files changed" : "left the room");
        rt.target = target;
        rt.wanted = target && rt.settings.accept == HostAccept::Always;
        if (target)
            Log("Host data: the host's weapon files (%s) differ from this machine's (%s)",
                hostdata::DigestHex(*target).c_str(), hostdata::DigestHex(rt.own).c_str());
    }
    if (!guest) {
        rt.stage = None;
    } else if (!target) {
        rt.stage = *room.hostDigest == rt.own ? Same : (rt.ownPaths.empty() ? Same : HostHasNone);
    } else if (rt.stage == None || rt.stage == Same || rt.stage == HostHasNone) {
        rt.stage = Differs;
    }
    if (rt.target && rt.wanted && rt.stage == Differs) TakeLocked();
}

void Observe(const LobbyView& view) {
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    rt.room = ReadHostDataRoom(view);
    DecideLocked();
}

// After every EOS tick: the link's sends, and what a fetch came to.
void AfterTick(void*) {
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    rt.link.Tick(GetTickCount64());
    if (rt.link.Serving() && !rt.loggedServing) Log("Host data: sending this machine's files to %zu player(s)", rt.link.Serving());
    rt.loggedServing = rt.link.Serving() != 0;
    using enum HostDataLink::Fetching;
    if (rt.link.State() == Running) rt.stage = HostDataStage::Fetching;
    if (rt.link.State() == Failed) {
        Log("Host data: fetching the host's files failed: %s", rt.link.Failure().c_str());
        rt.link.Cancel();
        rt.stage = HostDataStage::Failed;
        rt.wanted = false;
    }
    if (rt.link.State() == Done && rt.target) {
        rt.ready = StoreLocked(*rt.target, rt.link.TakeBundle());
        rt.stage = rt.ready ? HostDataStage::Ready : HostDataStage::Failed;
        if (!rt.ready) rt.wanted = false;
    }
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
    for (;;) {
        const EosResult result = rt.receive(handle, options, peer, socket, channel, data, size);
        if (result != 0 || !data || !size || !channel || !peer || !*peer) return result;
        std::scoped_lock lock(rt.lock);
        rt.p2p = handle;
        if (options) rt.localUser = static_cast<const EosReceiveOptions*>(options)->LocalUserId;
        const std::string& from = PeerTextLocked(*peer);
        if (*channel != kHostDataChannel) {
            if (socket && !rt.socket.SocketName[0]) std::memcpy(&rt.socket, socket, sizeof(rt.socket));
            return result;
        }
        // Ours, whatever it holds: the game never sends on this channel and must not see it.
        if (const auto packet = hostdata::DecodePacket(static_cast<const std::uint8_t*>(data), *size))
            rt.link.Received(from, *packet, GetTickCount64());
    }
}

// Removes what is left of an interrupted write, and all but the newest kept bundles.
void Tidy(const std::wstring& store) {
    constexpr std::size_t kKept = 4;
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

}  // namespace

bool StartHostData(HMODULE game, ImportRedirect redirect, const HostDataSettings& settings) {
    Runtime& rt = Rt();
    const HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    {
        std::scoped_lock lock(rt.lock);
        rt.settings = settings;
        rt.store = settings.gameFolder + L"\\Mods\\Plugins\\EDF6Coop.hostdata";
        Tidy(rt.store);
        std::vector<std::string> skipped;
        std::vector<hostdata::DataFile> files = hostdata::ScanMods(settings.gameFolder + L"\\Mods", &skipped);
        for (const std::string& line : skipped) Log("Host data: %s; not shared", line.c_str());
        for (const hostdata::DataFile& file : files) rt.ownPaths.push_back(file.path);
        std::string why;
        auto bundle = hostdata::MakeBundle(std::move(files), &why);
        if (const auto empty = hostdata::MakeBundle({}, nullptr)) rt.empty = empty->digest;
        if (!bundle) {
            Log("Host data: this machine's weapon files cannot be shared (%s); nothing is offered", why.c_str());
        } else {
            rt.own = bundle->digest;
            Log("Host data: %zu weapon/vehicle file(s) of this machine, %zu bytes, SHA-256 %s", bundle->files,
                bundle->bytes.size(), hostdata::DigestHex(bundle->digest).c_str());
        }
        PublishMemberText(kHostDataKey, kHostDataFormat);
        if (bundle && settings.share) {
            PublishMemberText(kHostDigestKey, hostdata::DigestHex(bundle->digest));
            rt.link.Share(std::make_shared<const hostdata::Bundle>(std::move(*bundle)));
        }
        rt.send = eos ? reinterpret_cast<EosSendFn>(reinterpret_cast<void*>(GetProcAddress(eos, "EOS_P2P_SendPacket")))
                      : nullptr;
    }
    WatchMemberTexts({kHostDataKey, kHostDigestKey}, &Observe);
    ListenToTicks(&AfterTick);
    const bool received = rt.send && redirect(game, "EOSSDK-Win64-Shipping.dll", "EOS_P2P_ReceivePacket",
                                              reinterpret_cast<void*>(&HostDataReceive),
                                              reinterpret_cast<void**>(&rt.receive));
    if (!received) Log("Host data: EOS P2P could not be reached; the host's files cannot be fetched");
    return received;
}

std::wstring HostDataMenuFrame(bool inRoom, bool pressed) {
    using enum HostDataStage;
    Runtime& rt = Rt();
    std::scoped_lock lock(rt.lock);
    if (!inRoom) {
        if (rt.target || HostOverlay()) {
            rt.room = HostDataRoom{};
            DecideLocked();
        }
        return L"";
    }
    if (pressed && rt.target && rt.settings.accept != HostAccept::Never) {
        rt.wanted = !rt.wanted;
        if (rt.wanted) {
            Log("Host data: %ls - taking the host's weapon files", rt.settings.keyName);
            rt.stage = Differs;
            TakeLocked();
        } else {
            DropLocked("turned off in the menu");
            rt.stage = Differs;
        }
    }
    // Between missions: this is the menu, so the game is not in the middle of reading a mission's files.
    if (rt.wanted && rt.ready && rt.stage == Ready) {
        SetHostOverlay(rt.ready);
        rt.stage = Using;
        Log("Host data: the game now reads the host's %zu weapon/vehicle file(s) (%zu of this machine's own stay)",
            rt.ready->paths.size(), rt.extra);
    }
    return HostDataNotice(rt.stage, rt.settings.accept, rt.settings.keyName, rt.link.Percent(), rt.extra);
}

}  // namespace multislot
