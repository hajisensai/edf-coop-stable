#include "eos_hooks.h"

#include <atomic>
#include <algorithm>
#include <deque>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "auth.h"
#include "eos_min.h"
#include "fake_lobby.h"
#include "hold.h"
#include "iat.h"
#include "lobby_marker.h"
#include "log.h"
#include "netclass.h"
#include "netcode.h"
#include "congestion.h"
#include "fragment.h"
#include "room_view.h"
#include "traffic.h"
#include "updater.h"

namespace dn {
// Defined with the netcode API at the end of the file, used by the hooks before it.
void meshTick(uint64_t now);
void bulkTick(uint64_t now);
void ackBulk(const std::string& member, uint64_t id);
DirectOptions withNetcode(DirectOptions o);
namespace {
void sendIdentityProof(EOS_ProductUserId local, const std::string& host);
bool takeIdentityProof(const std::string& src, uint8_t channel, const uint8_t* data, uint32_t size);
void identityProofTick();

constexpr const char* kEosDll = "EOSSDK-Win64-Shipping.dll";
constexpr uint64_t kMinQueueBytes = 64ull * 1024 * 1024;

using PFN_EOS_Platform_Tick = void (*)(EOS_HPlatform);
using PFN_EOS_P2P_RemoveNotify = void (*)(EOS_HP2P, EOS_NotificationId);
using PFN_EOS_Lobby_RemoveNotify = void (*)(EOS_HLobby, EOS_NotificationId);
using PFN_EOS_EResult_IsOperationComplete = int32_t (*)(EOS_EResult);

struct Api {
    PFN_EOS_Platform_GetP2PInterface getP2P = nullptr;  // original targets (IAT)
    PFN_EOS_P2P_SendPacket send = nullptr;
    PFN_EOS_P2P_ReceivePacket receive = nullptr;
    PFN_EOS_Platform_Tick tick = nullptr;
    PFN_EOS_P2P_AddNotifyEstablished addEstablished = nullptr;  // resolved from the SDK
    PFN_EOS_P2P_AddNotifyInterrupted addInterrupted = nullptr;
    PFN_EOS_P2P_AddNotifyClosed addClosed = nullptr;
    PFN_EOS_P2P_AddNotifyQueueFull addQueueFull = nullptr;
    PFN_EOS_P2P_QueryNATType queryNat = nullptr;
    PFN_EOS_P2P_SetRelayControl setRelay = nullptr;
    PFN_EOS_P2P_SetPortRange setPortRange = nullptr;
    PFN_EOS_P2P_SetPacketQueueSize setQueueSize = nullptr;
    PFN_EOS_P2P_GetPacketQueueInfo getQueueInfo = nullptr;
    PFN_EOS_ProductUserId_ToString idToString = nullptr;
    PFN_EOS_ProductUserId_FromString idFromString = nullptr;
    PFN_EOS_EResult_ToString resultToString = nullptr;
    PFN_EOS_EResult_IsOperationComplete isComplete = nullptr;
    PFN_EOS_P2P_AcceptConnection accept = nullptr;
    // Original targets of the game's own imports we wrap (may point at another mod's hook).
    PFN_EOS_P2P_AddNotifyClosed gameAddClosed = nullptr;
    PFN_EOS_P2P_RemoveNotify gameRemoveClosed = nullptr;
    PFN_EOS_P2P_CloseConnection gameClose = nullptr;
    PFN_EOS_P2P_CloseConnections gameCloseAll = nullptr;
    PFN_EOS_Lobby_AddNotifyMemberStatus gameAddMemberStatus = nullptr;
    PFN_EOS_Lobby_RemoveNotify gameRemoveMemberStatus = nullptr;
    PFN_EOS_Lobby_AddNotifyMemberUpdate gameAddMemberUpdate = nullptr;
    PFN_EOS_Lobby_CreateLobby gameCreateLobby = nullptr;
    PFN_EOS_Lobby_JoinLobby gameJoinLobby = nullptr;
    PFN_EOS_Lobby_LeaveOrDestroy gameLeaveLobby = nullptr;
    PFN_EOS_Lobby_LeaveOrDestroy gameDestroyLobby = nullptr;
    PFN_EOS_Lobby_KickMember gameKick = nullptr;
    PFN_EOS_Lobby_UpdateLobby gameUpdateLobby = nullptr;
    PFN_EOS_LobbyDetails_GetLobbyOwner gameGetOwner = nullptr;
    PFN_EOS_Lobby_PromoteMember promote = nullptr;  // resolved from the SDK: hands a usurped lobby back
    // Resolved from the SDK: what a room looks like, kept for coming back into it (LastRoom).
    PFN_EOS_Lobby_CopyLobbyDetailsHandle copyDetails = nullptr;
    PFN_EOS_LobbyDetails_GetAttributeCount attributeCount = nullptr;
    PFN_EOS_LobbyDetails_CopyAttributeByIndex copyAttribute = nullptr;
    PFN_EOS_LobbyDetails_CopyInfo copyInfo = nullptr;
    PFN_EOS_LobbyDetails_Info_Release releaseInfo = nullptr;
    PFN_EOS_Lobby_Attribute_Release releaseAttribute = nullptr;
    PFN_EOS_LobbyDetails_Release releaseDetails = nullptr;
};

// The game's imports the virtual room sits in front of (installVirtualRoomHooks): outermost, after every
// other wrapper of the plugin, so a LobbyDetails handle of ours never reaches one of them or EOS.
struct Outer {
    PFN_EOS_LobbySearch_Find find = nullptr;
    PFN_EOS_LobbySearch_GetSearchResultCount resultCount = nullptr;
    PFN_EOS_LobbySearch_CopySearchResultByIndex copyResult = nullptr;
    PFN_EOS_LobbySearch_SetLobbyId setLobbyId = nullptr;
    PFN_EOS_LobbySearch_Release releaseSearch = nullptr;
    PFN_EOS_Lobby_JoinLobby join = nullptr;
    PFN_EOS_Lobby_LeaveOrDestroy leave = nullptr;
    PFN_EOS_Lobby_LeaveOrDestroy destroy = nullptr;
    PFN_EOS_Lobby_CopyLobbyDetailsHandle copyDetails = nullptr;
    PFN_EOS_LobbyDetails_GetAttributeCount attributeCount = nullptr;
    PFN_EOS_LobbyDetails_CopyAttributeByIndex copyAttribute = nullptr;
    PFN_EOS_LobbyDetails_GetMemberCount memberCount = nullptr;
    PFN_EOS_LobbyDetails_GetMemberByIndex memberByIndex = nullptr;
    PFN_EOS_LobbyDetails_GetLobbyOwner owner = nullptr;
    PFN_EOS_LobbyDetails_CopyInfo copyInfo = nullptr;
    PFN_EOS_LobbyDetails_Release releaseDetails = nullptr;
    PFN_EOS_LobbyDetails_Info_Release releaseInfo = nullptr;
    PFN_EOS_Lobby_Attribute_Release releaseAttribute = nullptr;
};

// The game's connection-closed handler, which we sit in front of. Never freed: its address is the
// ClientData EOS hands back to us, and a held event may outlive the game's registration.
struct ClosedHandler {
    EOS_P2P_OnRemoteConnectionClosedCallback callback;
    void* clientData;
    EOS_NotificationId id = 0;
    std::atomic<bool> removed{false};  // the game unregistered it; its clientData may be gone
};

struct MemberStatusHandler {
    EOS_Lobby_OnLobbyMemberStatusReceivedCallback callback;
    void* clientData;
    EOS_NotificationId id = 0;
    std::atomic<bool> removed{false};  // the game unregistered it; a status we still hold must not reach it
};

struct MemberUpdateHandler {
    EOS_Lobby_OnLobbyMemberUpdateReceivedCallback callback;
    void* clientData;
};

// A pending CreateLobby / JoinLobby call; EOS runs its completion callback exactly once.
struct LobbyCall {
    EOS_Lobby_OnLobbyIdCallback callback;
    void* clientData;
    EOS_HLobby lobby;
    EOS_ProductUserId localUser;
    bool owner;  // CreateLobby: we own the lobby we enter
};

// Direct link to a room host found through the lobby (AutoJoin). Touched on the EOS tick and in the
// lobby hooks, all under `mu`.
struct AutoJoin {
    std::mutex mu;
    std::string advertised;               // the host's advertised address list we are working on
    std::vector<std::string> candidates;  // that list, best first
    size_t next = 0;                      // next candidate to try
    std::string hostPuid;
    std::shared_ptr<DirectNet> net;  // other threads may still hold a copy; see retire()
    uint64_t attemptMs = 0;
    bool connected = false;  // the current attempt reached the host; DirectNet reconnects by itself
    uint64_t retryAtMs = 0;  // every candidate failed: try the list again from here on
    uint64_t noAddressSinceMs = 0;  // in someone's room but its host advertises nothing (yet)
    bool noAddressLogged = false;
    uint64_t lastCheckMs = 0;
};

// The room the game is in without Epic's lobby: the one it was last in, entered again over the direct link
// to its host (startVirtualJoin). Under State::virtualMutex.
struct VirtualRoom {
    std::string roomId;
    // The game holds this room through us: from its JoinLobby until it left the room (LeaveLobby /
    // DestroyLobby, completed here), its join failed, or it entered a room through Epic. Its leave is ours
    // to complete only while this is set: the same room joined through Epic later is Epic's again.
    bool owned = false;
    bool joining = false;
    bool in = false;
    LastRoom room;
    std::vector<std::string> candidates;  // the host's addresses, best first
    size_t next = 0;
    std::shared_ptr<DirectNet> net;
    EOS_ProductUserId user = nullptr;
    uint64_t startMs = 0, attemptMs = 0, hostSeenMs = 0;
    bool proveEos = false;  // Epic's lobby was full: our EOS id goes to the host over EOS (kIdentityProofSocket)
    uint64_t provedMs = 0;
    EOS_Lobby_OnLobbyIdCallback callback = nullptr;  // the game's JoinLobby, until it completes
    void* clientData = nullptr;
};

// A room search of the game's, and whether its results got the remembered room added (searchFound).
struct SearchState {
    std::string lobbyId;  // SetLobbyId: the search looks for this room only
    bool added = false;
};

struct State {
    Api api;
    Config config;
    // Everything below that the hooks read is atomic: the hooks go live one by one while
    // installEosHooks still fills this in, and the game may already be calling them.
    // The current direct transport. Hooks copy it for the duration of a call, so an instance AutoJoin
    // swaps out is freed only once nobody uses it any more.
    std::atomic<std::shared_ptr<DirectNet>> net;
    // Mode=host/join transport (owned by plugin.cpp, never freed); AutoJoin swaps in and back out.
    std::atomic<std::shared_ptr<DirectNet>> baseNet;
    std::atomic<bool> autoJoinOn{false};  // AutoJoin active (not with Mode=join: its target is fixed)
    AutoJoin autoJoin;
    std::atomic<EOS_ProductUserId> lobbyUser{nullptr};
    std::unique_ptr<DisconnectHold> hold;
    std::unique_ptr<LobbyStatusHold> lobbyHold;
    LobbyOwnerPin ownerPin;
    std::mutex roomMutex;
    EOS_HLobby roomLobby = nullptr;  // the lobby we are in, for handing it back to its pinned owner
    std::string roomId;
    LobbyMarker marker;
    std::atomic<bool> markerReady{false};
    std::atomic<bool> lobbyTracked{false};  // we see entering and leaving rooms: direct links follow the room
    std::atomic<bool> ticking{false};  // EOS_Platform_Tick is ours: what we answer for the game completes there
    std::mutex markedMutex;
    std::unordered_set<std::string> markedPeers;  // logged once as plugin users
    std::string loggedHostAddress;
    std::mutex handlerMutex;
    std::vector<ClosedHandler*> closedHandlers;
    std::vector<MemberStatusHandler*> statusHandlers;
    std::atomic<EOS_HP2P> p2p{nullptr};  // written on the game's send/receive thread, read on the tick
    std::atomic<EOS_HP2P> configuredHandle{nullptr};
    std::atomic<EOS_ProductUserId> localUser{nullptr};
    std::mutex notifyMutex;
    std::unordered_set<std::string> notifiedUsers;  // local users whose connection events we log
    std::mutex idMutex;
    std::unordered_map<std::string, EOS_ProductUserId> idCache;
    std::atomic<uint64_t> directOut{0}, directIn{0}, eosOut{0}, eosIn{0}, eosSendFail{0}, eosUpgraded{0};
    TrafficMeter gameOut;  // everything the game sends, whichever way it goes
    // What the game's datagrams hold, from their plaintext (netclass.h): per record type for the log, and which
    // record types behave as state.
    RecordTypeMeter recordTypes;
    StateLearner stateTypes;
    // The transport side of the netcode rewrite (netcode.h).
    std::mutex netcodeMutex;
    NetcodeOptions netcode;
    std::atomic<RoomCapQuery> roomCaps{nullptr};
    std::atomic<StateSendFilter> stateFilter{nullptr};
    std::atomic<BulkHandler> bulkHandler{nullptr};
    std::atomic<RoomCapacitySource> roomCapacity{nullptr};
    std::atomic<GameSlotsSource> gameSlots{nullptr};
    // A join through Epic into a room whose host hosts a direct link: the game's completion waits for the host's
    // member slots (parkedEntryTick), so its game takes every member in the host's slots from the start.
    struct ParkedEntry {
        bool active = false;
        EOS_Lobby_OnLobbyIdCallback callback = nullptr;
        EOS_Lobby_LobbyIdCallbackInfo info{};
        std::string lobbyId;
        uint64_t sinceMs = 0;
        uint64_t noHostSinceMs = 0;  // since when the room's owner advertises no direct link (0: it does)
    };
    std::mutex parkedMutex;
    ParkedEntry parked;
    std::atomic<bool> parkedActive{false};  // parked.active, read without parkedMutex
    std::atomic<bool> testLoopbackHosts{false};
    std::atomic<uint32_t> testBulkFragments{0};  // [Test] BulkFragmentsSent
    std::atomic<uint32_t> netProtocol{0}, netCaps{0};
    std::atomic<bool> refuseOtherProtocols{false};
    // A game packet that came twice (over two paths, or the direct link and EOS) reaches the game once.
    DuplicateFilter duplicates;
    // Fragments (fragment.h): ours going out get ids from here, theirs come together here (receive thread only).
    std::atomic<uint32_t> fragmentIds{0};
    // This process's fragment id epoch (fragment.h): random, so ids of a restarted sender never repeat its last run's.
    const uint64_t fragmentEpoch = [] {
        uint32_t e = 0;
        randomBytes(reinterpret_cast<uint8_t*>(&e), sizeof(e));
        return static_cast<uint64_t>(e) << 32;
    }();
    std::mutex fragmentMutex;
    Reassembler reassembler;
    struct ReadyPacket {
        std::string src;
        std::string socket;
        uint8_t channel = 0;
        std::vector<uint8_t> data;
    };
    std::deque<ReadyPacket> ready;  // reassembled game datagrams, waiting for the game's receive (fragmentMutex)
    // What the game's last send used, for the plugin's own sends (sendBulk) on its behalf.
    std::mutex lastSendMutex;
    EOS_ProductUserId lastLocal = nullptr;
    std::string lastSocket;
    std::atomic<uint64_t> eosCopies{0}, aoiSkipped{0}, fragmentsOut{0}, bulkIn{0}, dupBefore{0}, trailsSent{0};
    // The newest state datagram to each member (noteTrail): sent once more, reliably, when no newer one follows.
    struct Trail {
        std::string member;
        EOS_HP2P p2p = nullptr;
        EOS_ProductUserId local = nullptr, remote = nullptr;
        std::string socket;
        uint8_t channel = 0;
        std::vector<uint8_t> data;
        uint64_t sentMs = 0;
    };
    std::mutex trailMutex;
    std::map<std::string, Trail> trails;
    // Bulk messages sent and not yet acknowledged (sendBulk): resent until their receiver says it has them.
    struct PendingBulk {
        std::string member;
        EOS_HP2P p2p = nullptr;
        EOS_ProductUserId local = nullptr, remote = nullptr;
        std::string socket;
        uint64_t id = 0;
        uint16_t tag = 0;
        std::vector<uint8_t> data;
        uint64_t sentMs = 0, waitMs = 0;
        uint32_t tries = 1;
    };
    std::mutex bulkMutex;
    std::vector<PendingBulk> bulks;
    std::atomic<uint64_t> bulkResent{0}, bulkLost{0};
    ULONGLONG lastStatsMs = 0;
    HMODULE eos = nullptr;
    Outer outer;
    // What the game has in its room, and the room host's say over it (room_view.h). The lock is never held
    // while the game is called.
    std::mutex viewMutex;
    RoomView view;
    std::string viewRoom;  // the lobby the view is of
    std::string endedRoom;  // the last room the game was told it is out of (removed, or the room closed)
    uint32_t viewCapacity = 0;  // the room's size, as last read from Epic's copy of it (0: not read yet)
    std::weak_ptr<DirectNet> followedNet;  // a member: the link whose host list was followed last...
    uint64_t followedVersion = 0;          // ...and that list's version
    std::map<std::string, std::string> roomIds;  // a host: every member identity seen in this room
    // Becoming the room's host: the joins we held back, with the slot the old host's game had each in (-1 none), for
    // our game to take before we publish our slots (RoomView::promoted, hostRoomTick); since when.
    std::vector<std::pair<std::string, int>> inherited;
    uint64_t inheritedSinceMs = 0;
    // Calls of the game answered by us, completed on the next tick: never inside the call itself.
    std::mutex deferredMutex;
    std::vector<std::function<void()>> deferred;
    // The room we were last in as a member, for coming back into it while Epic cannot list it.
    std::mutex lastMutex;
    LastRoom lastRoom;
    uint64_t lastRoomLeftMs = 0;  // 0 while we are in it
    std::mutex virtualMutex;
    VirtualRoom virtualRoom;
    FakeLobbies fakes;
    std::mutex searchMutex;
    std::unordered_map<EOS_HLobbySearch, SearchState> searches;
};

// Heap-allocated and never destroyed: EDF.dll may still call EOS through our hooks while the
// process tears down, after this DLL's static destructors would have run.
State& g = *new State();

// Set at process exit: every hook then forwards straight to the original function, touching no
// locks (a killed worker thread may still own one) and writing no log.
std::atomic<bool> g_shutdown{false};

const char* resultName(EOS_EResult r) { return g.api.resultToString ? g.api.resultToString(r) : "?"; }

const char* closedReasonName(int32_t r) {
    static const char* names[] = {"Unknown",         "ClosedByLocalUser", "ClosedByPeer",     "TimedOut",
                                  "TooManyConnections", "InvalidMessage", "InvalidData",     "ConnectionFailed",
                                  "ConnectionClosed", "NegotiationFailed", "UnexpectedError"};
    return r >= 0 && r < static_cast<int32_t>(std::size(names)) ? names[r] : "?";
}

const char* natName(int32_t t) {
    static const char* names[] = {"Unknown", "Open", "Moderate", "Strict"};
    return t >= 0 && t < 4 ? names[t] : "?";
}

std::string idString(EOS_ProductUserId id) {
    if (!id || !g.api.idToString) return {};
    char buf[EOS_PRODUCTUSERID_MAX_LENGTH + 1] = {};
    int32_t len = sizeof(buf);
    return g.api.idToString(id, buf, &len) == EOS_Success ? std::string(buf) : std::string();
}

// Direct packets only come from room members whose identity a host checked, so few ids ever get
// here; the cap still bounds the cache whatever arrives. EOS keeps the handles themselves, so
// forgetting them costs only a lookup.
constexpr size_t kMaxIdCache = 256;
EOS_ProductUserId idHandle(const std::string& s) {
    std::lock_guard<std::mutex> lock(g.idMutex);
    auto it = g.idCache.find(s);
    if (it != g.idCache.end()) return it->second;
    EOS_ProductUserId id = g.api.idFromString ? g.api.idFromString(s.c_str()) : nullptr;
    if (!id) return id;
    if (g.idCache.size() >= kMaxIdCache) g.idCache.clear();
    g.idCache.emplace(s, id);
    return id;
}

// A held peer stays hidden while its direct link answers: the game's traffic to it no longer flows over
// EOS, so what EOS says about it does not matter. The link pings every second, also in menus where
// the game itself sends next to nothing. A peer whose game stopped does not linger: leaving the room
// reaches us as a lobby "left" (which releases it), and its plugin closes the link with it.
constexpr uint64_t kDirectDataFreshMs = 10000;  // diagnostics: game data counts as recent
constexpr uint64_t kLinkAliveMs = 5000;
bool directAlive(const std::string& remote) {
    std::shared_ptr<DirectNet> net = g.net.load();
    return net && !remote.empty() && net->canRoute(remote) && net->linkAlive(remote, kLinkAliveMs);
}

// Whether the direct link says `remote` still plays (see directAlive). For ourselves: any of our
// direct links answers.
bool reachableDirectly(const std::string& remote) {
    std::shared_ptr<DirectNet> net = g.net.load();
    if (!net || remote.empty()) return false;
    return remote == idString(g.lobbyUser.load()) ? net->anyLinkAlive(kLinkAliveMs) : net->linkAlive(remote, kLinkAliveMs);
}

std::string peerLabel(EOS_ProductUserId id) {
    std::string s = idString(id);
    return shortId(s) + (directAlive(s) ? " (direct link)" : "");
}

std::string socketName(const EOS_P2P_SocketId* sock) {
    if (!sock) return {};
    return std::string(sock->SocketName, strnlen(sock->SocketName, EOS_P2P_SOCKETID_SOCKETNAME_SIZE));
}

void onEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo* i) {
    if (g_shutdown) return;
    static const char* nets[] = {"no-connection", "DIRECT", "RELAYED via Epic"};
    int32_t n = i->NetworkType;
    logf("EOS connection %s with %s: %s", i->ConnectionType == 1 ? "RE-established" : "established",
         peerLabel(i->RemoteUserId).c_str(), n >= 0 && n < 3 ? nets[n] : "?");
    if (g.hold && g.hold->onEstablished(idString(i->RemoteUserId)) > 0)
        logf("RESILIENCE %s RECOVERED; the game never saw the disconnect", shortId(idString(i->RemoteUserId)).c_str());
}

void onInterrupted(const EOS_P2P_OnPeerConnectionInterruptedInfo* i) {
    if (g_shutdown) return;
    logf("EOS connection INTERRUPTED with %s (EOS is trying to recover it)", peerLabel(i->RemoteUserId).c_str());
}

void onClosed(const EOS_P2P_OnRemoteConnectionClosedInfo* i) {
    if (g_shutdown) return;
    logf("EOS connection CLOSED with %s, reason=%s(%d), socket=%s", peerLabel(i->RemoteUserId).c_str(),
         closedReasonName(i->Reason), i->Reason, socketName(i->SocketId).c_str());
}

// A close event copied out of the EOS callback so it can be handed to the game later.
struct HeldClose {
    ClosedHandler* handler;
    EOS_P2P_OnRemoteConnectionClosedInfo info;
    EOS_P2P_SocketId socket;
    bool hasSocket;
};

void forwardClose(const std::shared_ptr<HeldClose>& h) {
    if (h->handler->removed) return;  // the game unregistered; its clientData may be freed
    h->info.SocketId = h->hasSocket ? &h->socket : nullptr;
    h->handler->callback(&h->info);
}

void reacceptPeer(const std::shared_ptr<HeldClose>& h) {
    EOS_HP2P p2p = g.p2p;
    if (!g.api.accept || !p2p || g_shutdown) return;
    EOS_P2P_PeerConnectionOptions o{1, h->info.LocalUserId, h->info.RemoteUserId,
                                    h->hasSocket ? &h->socket : nullptr};
    EOS_EResult r = g.api.accept(p2p, &o);
    logRateLimited("reaccept", 10000, "RESILIENCE asking EOS to reconnect %s: %s",
                   shortId(idString(h->info.RemoteUserId)).c_str(), resultName(r));
}

// Sits in front of the game's connection-closed handler.
void closedWrapper(const EOS_P2P_OnRemoteConnectionClosedInfo* i) {
    auto* handler = static_cast<ClosedHandler*>(i->ClientData);
    auto held = std::make_shared<HeldClose>();
    held->handler = handler;
    held->info = *i;
    held->info.ClientData = handler->clientData;
    held->hasSocket = i->SocketId != nullptr;
    if (held->hasSocket) held->socket = *i->SocketId;
    held->info.SocketId = nullptr;  // re-pointed at the copy when forwarded

    std::string remote = idString(i->RemoteUserId);
    if (!g_shutdown && g.hold && !remote.empty()) {
        bool direct = directAlive(remote);
        bool plugin = g.markerReady && g.marker.hasMarker(i->RemoteUserId);
        if (g.hold->offer(remote, i->Reason, direct, plugin, GetTickCount64(), [held] { forwardClose(held); },
                          [held] { reacceptPeer(held); })) {
            if (direct)
                logf("RESILIENCE EOS link to %s closed (%s) but the direct link is up: hidden from the game",
                     shortId(remote).c_str(), closedReasonName(i->Reason));
            else
                logf("RESILIENCE holding back disconnect of %s (%s) for up to %u s while reconnecting",
                     shortId(remote).c_str(), closedReasonName(i->Reason), g.config.graceMs / 1000);
            return;
        }
    }
    forwardClose(held);
}

EOS_NotificationId hookAddNotifyClosed(EOS_HP2P h, const EOS_P2P_AddNotifyOptions* o, void* clientData,
                                       EOS_P2P_OnRemoteConnectionClosedCallback cb) {
    if (!cb || g_shutdown) return g.api.gameAddClosed(h, o, clientData, cb);
    auto* handler = new ClosedHandler{cb, clientData};
    EOS_NotificationId id = g.api.gameAddClosed(h, o, handler, closedWrapper);
    handler->id = id;
    std::lock_guard<std::mutex> lock(g.handlerMutex);
    g.closedHandlers.push_back(handler);
    return id;
}

void hookRemoveNotifyClosed(EOS_HP2P h, EOS_NotificationId id) {
    if (!g_shutdown) {
        std::lock_guard<std::mutex> lock(g.handlerMutex);
        for (ClosedHandler* handler : g.closedHandlers)
            if (handler->id == id) handler->removed = true;
    }
    g.api.gameRemoveClosed(h, id);
}

EOS_EResult hookCloseConnection(EOS_HP2P h, const EOS_P2P_PeerConnectionOptions* o) {
    if (o && !g_shutdown) {
        std::string remote = idString(o->RemoteUserId);
        size_t dropped = g.hold ? g.hold->onGameClosed(remote) : 0;
        logf("GAME closed its connection to %s%s", shortId(remote).c_str(),
             dropped ? " (it was being held for reconnect; the game gave up on its own)" : "");
    }
    return g.api.gameClose(h, o);
}

EOS_EResult hookCloseConnections(EOS_HP2P h, const EOS_P2P_CloseConnectionsOptions* o) {
    if (!g_shutdown) {
        size_t dropped = g.hold ? g.hold->onGameClosedAll() : 0;
        logf("GAME closed all connections (left the room)%s", dropped ? ", dropping held disconnects" : "");
    }
    return g.api.gameCloseAll(h, o);
}

void leftLobby(const char* why);
void trailTick(uint64_t now);
bool roomCap(uint32_t cap);
void forgetLastRoom(const char* why);

// What a lobby status means for us, applied when the game gets to see it.
void applyLobbyStatus(const std::string& target, bool self, int32_t s) {
    bool gone = s == kLeft || s == kDisconnected || s == kKicked;
    // The lobby is authoritative about who is still in the room. A held disconnect of someone who
    // left must reach the game now; a live direct link would otherwise keep it held forever.
    // Delivered before the lobby event, the same order the game sees without the plugin.
    size_t released = 0;
    if (g.hold && gone) released = g.hold->release(target);
    if (g.hold && s == kClosed) released = g.hold->releaseAll();
    if (g.lobbyHold && s == kClosed) g.lobbyHold->releaseAll();  // the members' statuses go before the room's
    if (gone && !self) g.marker.memberGone(target);
    if (self && s == kPromoted) g.marker.promoted();
    if (!self && s == kJoined) g.marker.memberJoined();
    // A room that closed, or that removed us, is nothing to come back into.
    if (s == kClosed || (self && s == kKicked)) forgetLastRoom(s == kClosed ? "the room was closed" : "we were kicked");
    if (s == kClosed || (self && gone))
        leftLobby(s == kClosed ? "the room was closed" : s == kDisconnected ? "we lost the lobby service" : "we left the room");
    if (released) logf("RESILIENCE handed %zu held disconnect(s) to the game because of the lobby change", released);
}

// A lobby status copied out of the EOS callback so it can reach the game later.
struct LobbyStatusEvent {
    MemberStatusHandler* handler;
    EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo info;
    std::string lobbyId;
    std::string target;
    bool self;
};

const char* statusName(int32_t s) {
    static const char* names[] = {"JOINED", "LEFT", "DISCONNECTED", "KICKED", "PROMOTED", "CLOSED"};
    return s >= 0 && s < 6 ? names[s] : "?";
}

// A status that takes the room away from us: we were removed from it, or it closed.
bool endsRoomForUs(bool self, int32_t s) {
    return s == kClosed || (self && (s == kLeft || s == kDisconnected || s == kKicked));
}

// How long a member that followed the old host's slots holds joins back for the new host's (room_view.h awaitingHost),
// and how long any join is held back at most: then it reaches our game in the order it came (RoomView::releaseHeld).
constexpr uint64_t kNewHostSlotsMs = 20000;
constexpr uint64_t kHeldJoinCapMs = 20000;

// Why the JOINED of `target` cannot reach our game yet, or nullptr. Under viewMutex. In a room whose host's slots we
// follow, a member joins our game once the host's game has it (then we know its slot) and that slot is empty in our
// game: a member taking the slot of one whose departure has not reached our game yet waits for it, it never takes
// another slot (eos::Users::Add would give it the first empty one, and nothing renumbers it later). The host's say
// (followHost) brings every held join again. After a change of host, joins wait for the new host's slots.
const char* joinHeldBack(const std::string& target) {
    if (g.view.consumeRelease(target)) return nullptr;  // held back long enough (releaseHeld)
    if (g.view.awaitingHost()) return "the room's new host says its member slots";
    if (!g.view.slotted()) return nullptr;
    const int slot = g.view.hostSlot(target);
    if (slot < 0) return "the room's host's game has it (its slot is not known yet)";
    if (GameSlotsSource source = g.gameSlots.load()) {
        const std::vector<std::string> ours = source();
        if (static_cast<size_t>(slot) < ours.size() && !ours[slot].empty() && ours[slot] != target)
            return "its slot is empty in our game (the member that had it has not left our game yet)";
    }
    return nullptr;
}

// Whether a status reaches the game: one it already has that way is not repeated, as Epic and the room's
// host may both report it (room_view.h). That the room ended for us, too, is told once: the host's say
// may come before Epic's, and by then the view is gone. EDF.dll registers one member-status handler (its
// lobby manager, 012B3380), so this is decided once per status.
bool admitStatus(const std::string& lobbyId, const std::string& target, bool self, int32_t status) {
    std::lock_guard<std::mutex> lock(g.viewMutex);
    const bool ends = endsRoomForUs(self, status);
    if (ends && !lobbyId.empty() && lobbyId == g.endedRoom) return false;
    if (lobbyId != g.viewRoom) return true;
    const uint64_t now = GetTickCount64();
    // A new host: the old one's slots are not the room's any more (room_view.h promoted). Its list is followed from
    // scratch, the one that may have come before this PROMOTED included.
    if (status == kPromoted) {
        auto placements = g.view.promoted(target, now);
        g.followedNet.reset();
        g.followedVersion = 0;
        if (self) {
            g.inherited = std::move(placements);
            g.inheritedSinceMs = now;
        }
    }
    if (status == kJoined && !self) {
        const char* why = joinHeldBack(target);
        if (why) {
            // Remembered: the host's say or releaseHeld brings it, it is never lost. One line per member and reason.
            g.view.holdJoin(target, now);
            const std::string key = "room-held:" + std::string(why) + target;
            logRateLimited(key.c_str(), 5000, "ROOM %s JOINED: held back until %s", shortId(target).c_str(), why);
            return false;
        }
    }
    if (!g.view.admit(target, status)) return false;
    if (ends) g.endedRoom = lobbyId;
    return true;
}

// Whether the game got it.
bool deliverLobbyStatus(const std::shared_ptr<LobbyStatusEvent>& e) {
    // A repeat changes nothing: what it means for us was applied with the first.
    if (!admitStatus(e->lobbyId, e->target, e->self, e->info.CurrentStatus)) {
        logRateLimited("room-repeat", 10000, "ROOM %s %s: the game has it that way already, not told again",
                       shortId(e->target).c_str(), statusName(e->info.CurrentStatus));
        return false;
    }
    applyLobbyStatus(e->target, e->self, e->info.CurrentStatus);
    if (e->handler->removed) return true;  // the game unregistered; its clientData may be freed
    e->info.LobbyId = e->lobbyId.c_str();
    e->handler->callback(&e->info);
    return true;
}

// Tells the game a member status Epic did not send: the room host's say, or ours as the host. It goes the
// way Epic's statuses go, view and all. On the EOS tick.
void tellGame(const std::string& lobbyId, const std::string& target, int32_t status, const char* why) {
    EOS_ProductUserId id = idHandle(target);
    if (!id || lobbyId.empty()) return;
    std::vector<MemberStatusHandler*> handlers;
    {
        std::lock_guard<std::mutex> lock(g.handlerMutex);
        for (MemberStatusHandler* handler : g.statusHandlers)
            if (!handler->removed) handlers.push_back(handler);
    }
    const bool self = target == idString(g.lobbyUser.load());
    bool told = false;
    for (MemberStatusHandler* handler : handlers) {
        auto e = std::make_shared<LobbyStatusEvent>();
        e->handler = handler;
        e->info = {handler->clientData, nullptr, id, status};
        e->lobbyId = lobbyId;
        e->target = target;
        e->self = self;
        told = deliverLobbyStatus(e) || told;
    }
    // Only what reached the game: a join held back is said again every few seconds (settle) and has its own line.
    if (told) logf("ROOM %s -> %s for the game: %s", shortId(target).c_str(), statusName(status), why);
}

// Runs `fn` on the next tick: a call of the game we answer ourselves completes after it returned, as EOS's do.
void defer(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(g.deferredMutex);
    g.deferred.push_back(std::move(fn));
}

void runDeferred() {
    std::vector<std::function<void()>> due;
    {
        std::lock_guard<std::mutex> lock(g.deferredMutex);
        due.swap(g.deferred);
    }
    for (auto& fn : due) fn();
}

// Completes one of the game's lobby calls (their callback infos share EOS_Lobby_LobbyIdCallbackInfo's layout).
void completeLobbyCall(void* callback, void* clientData, EOS_EResult result, const std::string& lobbyId) {
    if (!callback) return;
    EOS_Lobby_LobbyIdCallbackInfo info{result, clientData, lobbyId.c_str()};
    reinterpret_cast<EOS_Lobby_OnLobbyIdCallback>(callback)(&info);
}

// The game is in room `lobbyId` with `members` (EOS ids; it is listed among them).
void enterView(const std::string& lobbyId, const std::string& self, const std::vector<std::string>& members) {
    std::lock_guard<std::mutex> lock(g.viewMutex);
    g.view.reset(self, members);
    g.inherited.clear();
    g.viewRoom = lobbyId;
    g.endedRoom.clear();
    g.viewCapacity = 0;
    g.followedNet.reset();
    g.followedVersion = 0;
    g.roomIds.clear();
}

void leaveView() {
    std::lock_guard<std::mutex> lock(g.viewMutex);
    g.view.clear();
    g.inherited.clear();
    g.viewRoom.clear();
    g.followedNet.reset();
    g.followedVersion = 0;
    g.roomIds.clear();
}

// Hands the lobby back to its pinned owner: Epic made us owner while the pinned one still hosts the
// game over the direct link. EOS calls: run on the EOS tick only.
void promoteBack(const char* why) {
    const std::string pinned = g.ownerPin.pinned();
    EOS_ProductUserId me = g.lobbyUser.load();
    EOS_ProductUserId target = pinned.empty() ? nullptr : idHandle(pinned);
    EOS_HLobby lobby = nullptr;
    std::string room;
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        lobby = g.roomLobby;
        room = g.roomId;
    }
    if (!g.api.promote || !lobby || room.empty() || !me || !target) return;
    logf("RESILIENCE Epic made us the room owner: handing it back to %s (%s)", shortId(pinned).c_str(), why);
    EOS_Lobby_PromoteMemberOptions o{1, room.c_str(), me, target};
    g.api.promote(lobby, &o, nullptr, [](const EOS_Lobby_LobbyIdCallbackInfo* i) {
        if (g.api.isComplete && !g.api.isComplete(i->ResultCode)) return;
        if (i->ResultCode != EOS_Success)
            logf("RESILIENCE handing the room back failed (%s): done again when the owner rejoins the lobby",
                 resultName(i->ResultCode));
    });
}

// Holds a lobby status the game must not see while the direct link shows the room still playing.
// Returns true when held (or swallowed). Each status is checked against whose link decides it:
//   DISCONNECTED  the member lost Epic's lobby service: its own link, for the configured grace;
//   LEFT          someone else left Epic's lobby: its own link (a member that quits closes it);
//   KICKED        by an owner Epic made, not the pinned one: the kicked member's link (ours: any);
//   CLOSED        Epic closed the room under us: the pinned owner's link (we own it: any link);
//   PROMOTED      Epic moved the room away from the pinned owner: the pinned owner's link.
// Whatever is held reaches the game once that link is down.
bool holdLobbyStatus(const std::shared_ptr<LobbyStatusEvent>& e) {
    const int32_t s = e->info.CurrentStatus;
    const uint64_t now = GetTickCount64();
    const std::string self = idString(g.lobbyUser.load());
    auto deliver = [e] { deliverLobbyStatus(e); };
    auto offer = [&](const std::string& key, const std::string& probe, uint32_t graceMs, bool replace,
                     std::function<void()> fn) {
        return !probe.empty() &&
               g.lobbyHold->offer(key, probe, reachableDirectly(probe), graceMs, replace, now, std::move(fn));
    };
    // A member's own LEFT or KICKED while its link is up replaces a disconnect hidden for it: the game is
    // told the newer status, without the disconnect's grace. With the link down both reach the game now.
    auto offerFinal = [&]() {
        return reachableDirectly(e->target) && offer(e->target, e->target, 0, true, deliver);
    };
    const char* who = e->self ? "we" : nullptr;
    const std::string label = who ? who : shortId(e->target);
    switch (s) {
    case kDisconnected:
        if (!offer(e->target, e->target, g.config.graceMs, false, deliver)) return false;
        logf("RESILIENCE %s lost Epic's lobby service but the direct link is up: hidden from the game", label.c_str());
        return true;
    case kLeft:
        if (e->self || !offerFinal()) return false;
        logf("RESILIENCE %s left Epic's lobby but still plays over the direct link: hidden from the game",
             label.c_str());
        return true;
    case kKicked: {
        // The pinned owner gone from the direct link too: whoever Epic made owner rules the room now.
        const std::string pinned = g.ownerPin.pinned();
        if (g.ownerPin.kickAuthorized() || pinned.empty() || !reachableDirectly(pinned) || !offerFinal()) return false;
        logf("RESILIENCE %s kicked by %s, whom Epic made the room owner, while the direct link is up: hidden "
             "from the game",
             label.c_str(), shortId(g.ownerPin.usurper()).c_str());
        return true;
    }
    case kClosed: {
        if (!g.marker.inLobby()) return false;
        const std::string pinned = g.ownerPin.pinned();
        if (!offer("#room", pinned, 0, false, deliver)) return false;
        logf("RESILIENCE Epic closed the room but %s: hidden from the game",
             pinned == self ? "our direct links are up" : "the host's direct link is up");
        return true;
    }
    case kPromoted: {
        const std::string pinned = g.ownerPin.pinned();
        const LobbyOwnerPin::Promotion p =
            g.ownerPin.onPromoted(e->target, self, !pinned.empty() && reachableDirectly(pinned));
        if (!p.hide) {
            g.lobbyHold->discard("#owner");  // superseded: delivering it later would roll the owner back
            return false;
        }
        g.lobbyHold->onStatus(e->target, s);  // a promotion ends a hidden disconnect, as before
        if (e->target == pinned) {
            g.lobbyHold->discard("#owner");  // Epic gave it back; the game never saw it go
            logf("RESILIENCE the room is %s's again in Epic's lobby", pinned == self ? "ours" : shortId(pinned).c_str());
            return true;
        }
        const std::string target = e->target;
        // Delivered once the pinned owner's link is down: the room follows Epic's newest owner then.
        if (!offer("#owner", pinned, 0, true, [e, target] {
                g.ownerPin.follow(target);
                deliverLobbyStatus(e);
            })) {
            g.ownerPin.follow(target);
            g.lobbyHold->discard("#owner");
            return false;
        }
        logf("RESILIENCE Epic made %s the room owner while %s still hosts over the direct link: hidden from the game",
             label.c_str(), pinned == self ? "this machine" : shortId(pinned).c_str());
        if (p.promoteBack) promoteBack("the pinned owner's direct link is up");
        return true;
    }
    default:
        return false;
    }
}

void memberStatusWrapper(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* i) {
    auto* handler = static_cast<MemberStatusHandler*>(i->ClientData);
    auto e = std::make_shared<LobbyStatusEvent>();
    e->handler = handler;
    e->info = *i;
    e->info.ClientData = handler->clientData;
    if (g_shutdown) {
        handler->callback(&e->info);
        return;
    }
    static const char* names[] = {"JOINED", "LEFT", "DISCONNECTED", "KICKED", "PROMOTED", "CLOSED"};
    int32_t s = i->CurrentStatus;
    e->lobbyId = i->LobbyId ? i->LobbyId : "";
    e->target = idString(i->TargetUserId);
    e->self = !e->target.empty() && e->target == idString(g.lobbyUser.load());
    logf("LOBBY member %s -> %s%s", shortId(e->target).c_str(), s >= 0 && s < 6 ? names[s] : "?",
         s == kDisconnected ? " (lost its connection to Epic's lobby service, not the P2P link)" : "");

    if (!g.lobbyHold || e->target.empty()) {
        deliverLobbyStatus(e);
        return;
    }
    if (holdLobbyStatus(e)) return;
    if (s == kJoined && e->target == g.ownerPin.pinned() && g.ownerPin.onPinnedJoined(idString(g.lobbyUser.load())))
        promoteBack("the pinned owner is back in the lobby");
    if (g.lobbyHold->onStatus(e->target, s)) {
        logf("RESILIENCE %s back in Epic's lobby service; the game never saw the drop",
             e->self ? "we are" : (shortId(e->target) + " is").c_str());
        g.marker.memberJoined();  // a member coming back needs our attributes (and ours may be gone)
        return;
    }
    deliverLobbyStatus(e);
}

EOS_NotificationId hookAddNotifyMemberStatus(EOS_HLobby h, const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions* o,
                                             void* clientData, EOS_Lobby_OnLobbyMemberStatusReceivedCallback cb) {
    if (!cb || g_shutdown) return g.api.gameAddMemberStatus(h, o, clientData, cb);
    auto* handler = new MemberStatusHandler{cb, clientData};  // never freed, same reason as ClosedHandler
    EOS_NotificationId id = g.api.gameAddMemberStatus(h, o, handler, memberStatusWrapper);
    handler->id = id;
    std::lock_guard<std::mutex> lock(g.handlerMutex);
    g.statusHandlers.push_back(handler);
    return id;
}

void hookRemoveNotifyMemberStatus(EOS_HLobby h, EOS_NotificationId id) {
    if (!g_shutdown) {
        std::lock_guard<std::mutex> lock(g.handlerMutex);
        for (MemberStatusHandler* handler : g.statusHandlers)
            if (handler->id == id) handler->removed = true;
    }
    g.api.gameRemoveMemberStatus(h, id);
}

// Diagnostics for a lobby member whose attributes changed: plugin users (once each) and the
// address the room host advertises (once per value).
void logMemberUpdate(EOS_ProductUserId member) {
    std::string remote = idString(member);
    bool self = !remote.empty() && remote == idString(g.lobbyUser.load());
    bool announce = false;
    if (!self && g.marker.hasMarker(member)) {
        std::lock_guard<std::mutex> lock(g.markedMutex);
        announce = g.markedPeers.insert(remote).second;
    }
    if (announce) logf("LOBBY member %s runs EDF6DirectNet: its disconnects can be held", shortId(remote).c_str());
    EOS_ProductUserId owner = nullptr;
    std::string address = g.marker.ownerAddress(&owner);
    if (owner != member || address.empty()) return;
    std::lock_guard<std::mutex> lock(g.markedMutex);
    if (address == g.loggedHostAddress) return;
    g.loggedHostAddress = address;
    logf("LOBBY room host %s%s advertises direct-link address %s", shortId(remote).c_str(), self ? " (us)" : "",
         address.c_str());
}

void memberUpdateWrapper(const EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo* i) {
    if (!g_shutdown && g.markerReady) logMemberUpdate(i->TargetUserId);
    auto* handler = static_cast<MemberUpdateHandler*>(i->ClientData);
    EOS_Lobby_LobbyMemberUpdateReceivedCallbackInfo copy = *i;
    copy.ClientData = handler->clientData;
    handler->callback(&copy);
}

EOS_NotificationId hookAddNotifyMemberUpdate(EOS_HLobby h, const EOS_Lobby_AddNotifyLobbyMemberUpdateReceivedOptions* o,
                                             void* clientData, EOS_Lobby_OnLobbyMemberUpdateReceivedCallback cb) {
    if (!cb || g_shutdown) return g.api.gameAddMemberUpdate(h, o, clientData, cb);
    auto* handler = new MemberUpdateHandler{cb, clientData};  // never freed, same reason as ClosedHandler
    return g.api.gameAddMemberUpdate(h, o, handler, memberUpdateWrapper);
}

void endVirtualRoom(const char* why);
void releaseVirtualRoom();

bool entryParked() { return g.parkedActive.load(); }

// Whether lobby `lobbyId` says its host hosts a direct link (kHostAddressKey, a lobby attribute every searcher reads):
// then its member slots come over that link (room_view.h).
bool lobbyAdvertisesHost(EOS_HLobby lobby, const char* lobbyId, EOS_ProductUserId user) {
    const Api& a = g.api;
    if (!lobby || !lobbyId || !user || !a.copyDetails || !a.attributeCount || !a.copyAttribute || !a.releaseAttribute ||
        !a.releaseDetails)
        return false;
    EOS_Lobby_CopyLobbyDetailsHandleOptions co{};
    co.ApiVersion = 1;
    co.LobbyId = lobbyId;
    co.LocalUserId = user;
    EOS_HLobbyDetails details = nullptr;
    if (a.copyDetails(lobby, &co, &details) != EOS_Success || !details) return false;
    bool advertised = false;
    EOS_LobbyDetails_GetAttributeCountOptions count{1};
    const uint32_t n = a.attributeCount(details, &count);
    for (uint32_t i = 0; i < n && !advertised; ++i) {
        EOS_LobbyDetails_CopyAttributeByIndexOptions ai{1, i};
        EOS_Lobby_Attribute* attribute = nullptr;
        if (a.copyAttribute(details, &ai, &attribute) == EOS_Success && attribute && attribute->Data && attribute->Data->Key)
            advertised = std::strcmp(attribute->Data->Key, kHostAddressKey) == 0;
        if (attribute) a.releaseAttribute(attribute);
    }
    a.releaseDetails(details);
    return advertised;
}

// Completion of the game's CreateLobby / JoinLobby: once we are in, publish our plugin marker.
void lobbyEnteredWrapper(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    auto* call = static_cast<LobbyCall*>(i->ClientData);
    if (!g_shutdown && i->ResultCode == EOS_Success) {
        g.lobbyUser = call->localUser;
        g.marker.entered(call->lobby, i->LobbyId, call->localUser, call->owner);
        {
            std::lock_guard<std::mutex> lock(g.roomMutex);
            g.roomLobby = call->lobby;
            g.roomId = i->LobbyId ? i->LobbyId : "";
        }
        // The room keeps this owner for as long as its direct link lives (see LobbyOwnerPin).
        EOS_ProductUserId owner = nullptr;
        if (!call->owner) g.marker.ownerAddress(&owner);
        g.ownerPin.entered(idString(call->owner ? call->localUser : owner));
        // What the game reads from the lobby when its callback below runs: who is in the room.
        const std::string room = i->LobbyId ? i->LobbyId : "";
        bool known = false;
        std::vector<std::string> members = g.marker.members(&known);
        endVirtualRoom("we entered a room through Epic");
        releaseVirtualRoom();
        enterView(room, idString(call->localUser), members);
        {
            std::lock_guard<std::mutex> lock(g.lastMutex);
            if (!g.lastRoom.roomId.empty() && g.lastRoom.roomId != room) {
                logf("REJOIN forgetting room %s: we are in another one", g.lastRoom.roomId.c_str());
                g.lastRoom = {};
            }
        }
        if (std::shared_ptr<DirectNet> base = g.baseNet.load()) base->setActive(true);
    }
    EOS_Lobby_LobbyIdCallbackInfo copy = *i;
    copy.ClientData = call->clientData;
    EOS_Lobby_OnLobbyIdCallback cb = call->callback;
    const bool final = !g.api.isComplete || g.api.isComplete(i->ResultCode);
    // A join into a room whose host hosts a direct link: the game enters once the host's member slots are here.
    if (!g_shutdown && final && i->ResultCode == EOS_Success && !call->owner && g.autoJoinOn &&
        lobbyAdvertisesHost(call->lobby, i->LobbyId, call->localUser)) {
        std::lock_guard<std::mutex> lock(g.parkedMutex);
        g.parked.active = true;
        g.parked.callback = cb;
        g.parked.info = copy;
        g.parked.lobbyId = i->LobbyId ? i->LobbyId : "";
        g.parked.sinceMs = GetTickCount64();
        g.parkedActive = true;
        logf("ROOM entering room %s once its host's member slots are here (its game numbers the members by them)",
             g.parked.lobbyId.c_str());
        delete call;
        return;
    }
    // EOS runs the callback again after a non-final result (EOS_OperationWillRetry): keep the call
    // until the final one, or the next run reads freed memory.
    if (final) delete call;
    cb(&copy);
}

void hookCreateLobby(EOS_HLobby h, const EOS_Lobby_CreateLobbyOptionsHead* o, void* clientData,
                     EOS_Lobby_OnLobbyIdCallback cb) {
    if (!cb || !o || g_shutdown) return g.api.gameCreateLobby(h, o, clientData, cb);
    g.api.gameCreateLobby(h, o, new LobbyCall{cb, clientData, h, o->LocalUserId, true}, lobbyEnteredWrapper);
}

void hookJoinLobby(EOS_HLobby h, const EOS_Lobby_JoinLobbyOptionsHead* o, void* clientData, EOS_Lobby_OnLobbyIdCallback cb) {
    if (!cb || !o || g_shutdown) return g.api.gameJoinLobby(h, o, clientData, cb);
    g.api.gameJoinLobby(h, o, new LobbyCall{cb, clientData, h, o->LocalUserId, false}, lobbyEnteredWrapper);
}

// Stops a replaced AutoJoin instance off the game thread: its worker may sit in a blocking DNS lookup
// of the host's name, and joining it here would freeze the game. A hook that copied the pointer just
// before the swap finishes its call on the stopped instance (it fails, the game falls back to EOS);
// the last copy to go frees it.
void retire(std::shared_ptr<DirectNet> net) {
    std::thread([net = std::move(net)] { net->stop(); }).detach();
}

// Stops an AutoJoin link: we left the room, or its host cannot be reached directly.
void stopAutoJoinLocked(const char* why) {
    AutoJoin& a = g.autoJoin;
    a.noAddressSinceMs = 0;
    a.noAddressLogged = false;
    a.advertised.clear();
    a.candidates.clear();
    if (!a.net) return;
    std::shared_ptr<DirectNet> net = std::move(a.net);
    std::shared_ptr<DirectNet> expected = net;  // compare_exchange overwrites its first argument on failure
    g.net.compare_exchange_strong(expected, g.baseNet.load());  // a host's own listener takes over again
    retire(std::move(net));
    logf("DIRECT auto-connect stopped (%s)", why);
}

void leftLobby(const char* why) {
    if (g.marker.inLobby()) logf("LOBBY %s", why);
    g.stateTypes.forget();  // the next room's players and missions teach their own
    // Datagrams put together, copies remembered and fragments waiting belong to the room just left.
    {
        std::lock_guard<std::mutex> lock(g.fragmentMutex);
        g.ready.clear();
        g.reassembler.clear();
    }
    g.duplicates.clear();
    {
        std::lock_guard<std::mutex> lock(g.trailMutex);
        g.trails.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g.bulkMutex);  // unacknowledged bulk messages to the room just left
        g.bulks.clear();
    }
    endVirtualRoom(why);
    leaveView();
    {
        std::lock_guard<std::mutex> lock(g.lastMutex);
        if (!g.lastRoom.roomId.empty() && !g.lastRoomLeftMs) g.lastRoomLeftMs = GetTickCount64();
    }
    g.marker.left();
    g.ownerPin.left();
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        g.roomLobby = nullptr;
        g.roomId.clear();
    }
    if (g.lobbyHold) g.lobbyHold->clear();  // the game left too; other members' statuses are moot
    if (std::shared_ptr<DirectNet> base = g.baseNet.load(); base && g.lobbyTracked) base->setActive(false);
    {
        std::lock_guard<std::mutex> lock(g.markedMutex);
        g.loggedHostAddress.clear();
    }
    std::lock_guard<std::mutex> lock(g.autoJoin.mu);
    stopAutoJoinLocked(why);
}

// Diagnostics: the game removes a player from the room by itself (the local player kicking someone
// goes through here too). Tells whether the direct link still showed that player playing.
// The game calls KickMember for the same member over and over (thousands of times in half a second)
// until the KICKED status comes back: the first call per member in kKickLogWindowMs is logged in
// full, the rest are counted and summed up with the next STATS line.
constexpr uint64_t kKickLogWindowMs = 10000;
struct KickLog {
    uint64_t firstMs = 0;
    uint64_t repeats = 0;
};
std::mutex g_kickMu;
std::unordered_map<std::string, KickLog> g_kicks;

// True when this kick is the first for `target` in the window (log it); counts it otherwise.
bool firstKickInWindow(const std::string& target, uint64_t now) {
    std::lock_guard<std::mutex> lock(g_kickMu);
    KickLog& k = g_kicks[target];
    if (k.firstMs != 0 && now - k.firstMs <= kKickLogWindowMs) {
        ++k.repeats;
        return false;
    }
    if (k.repeats) logf("GAME kicked %s %llu more times", shortId(target).c_str(), static_cast<unsigned long long>(k.repeats));
    k = {now, 0};
    return true;
}

void logKickRepeats() {
    std::lock_guard<std::mutex> lock(g_kickMu);
    for (auto it = g_kicks.begin(); it != g_kicks.end();) {
        if (it->second.repeats)
            logf("GAME kicked %s %llu more times", shortId(it->first).c_str(),
                 static_cast<unsigned long long>(it->second.repeats));
        it = g_kicks.erase(it);
    }
}

bool isVirtualRoom(const std::string& lobbyId);

// A kick EOS ran for the game: one that failed (Epic's lobby service is down) never brings the KICKED the
// game waits for, and it would go on kicking. The game decided, so it is told.
struct KickCall {
    void* callback;
    void* clientData;
    std::string lobbyId, target;
};

void kickDone(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    auto* call = static_cast<KickCall*>(i->ClientData);
    const bool final = !g.api.isComplete || g.api.isComplete(i->ResultCode);
    if (final && i->ResultCode != EOS_Success && !g_shutdown) {
        const std::string lobbyId = call->lobbyId, target = call->target;
        logf("RESILIENCE EOS could not kick %s (%s): the game is told it is gone", shortId(target).c_str(),
             resultName(i->ResultCode));
        defer([lobbyId, target] { tellGame(lobbyId, target, kKicked, "our game kicked it"); });
    }
    EOS_Lobby_LobbyIdCallbackInfo copy = *i;
    copy.ClientData = call->clientData;
    void* cb = call->callback;
    if (final) delete call;
    if (cb) reinterpret_cast<EOS_Lobby_OnLobbyIdCallback>(cb)(&copy);
}

enum class Kick { Pass, Eos, Answered };

// The room host's game removes a member that is in its room: kicked for this room, it is not let back in by
// its direct link (RoomView::kick). A member that plays in the room only over its direct link is not in
// Epic's lobby, so EOS cannot kick it: answered here (Answered), the game told once that it is gone. The
// first kick of one Epic has goes to EOS, watched for a failure (Eos). Pass: the game's repeats of a kick
// of an Epic member (it kicks over and over until KICKED comes), a member no longer in the room (the game
// tidies up after members that left, too), and everything without our tick to complete answers on: EOS gets
// the call as it is.
Kick hostKick(const EOS_Lobby_KickMemberOptions* o, void* clientData, void* cb) {
    if (!g.ticking) return Kick::Pass;
    const std::string target = idString(o->TargetUserId);
    const std::string lobbyId = o->LobbyId ? o->LobbyId : "";
    bool first = false;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (lobbyId.empty() || lobbyId != g.viewRoom || !g.view.has(target)) return Kick::Pass;
        first = g.view.kick(target);
    }
    bool known = false;
    const std::vector<std::string> epic = g.marker.members(&known);
    if (!known || std::find(epic.begin(), epic.end(), target) != epic.end()) return first ? Kick::Eos : Kick::Pass;
    defer([lobbyId, target, clientData, cb, first] {
        if (first) tellGame(lobbyId, target, kKicked, "our game kicked it; it was in the room over its direct link only");
        completeLobbyCall(cb, clientData, EOS_Success, lobbyId);
    });
    return Kick::Answered;
}

void hookKickMember(EOS_HLobby h, const EOS_Lobby_KickMemberOptions* o, void* clientData, void* cb) {
    if (o && !g_shutdown) {
        std::string target = idString(o->TargetUserId);
        std::shared_ptr<DirectNet> net = g.net.load();
        if (firstKickInWindow(target, GetTickCount64()))
            logf("GAME kicks %s from the room (direct link %s, direct game packets to us from it %s, P2P disconnect %s, "
                 "lobby status %s)",
                 shortId(target).c_str(), reachableDirectly(target) ? "up" : "down",
                 net && net->heardFromRecently(target, kDirectDataFreshMs) ? "recent" : "none for 10 s",
                 g.hold && g.hold->isHeld(target) ? "held by us" : "not held",
                 g.lobbyHold && g.lobbyHold->isHeld(target) ? "hidden by us" : "normal");
        // The game gave up on this member: a disconnect we are hiding must reach it now, or the member
        // would stay in the game (EOS already dropped it, so the kick itself may produce no status).
        if (g.lobbyHold && g.lobbyHold->abandon(target))
            logf("RESILIENCE the game kicks %s: its hidden lobby disconnect goes to the game", shortId(target).c_str());
        switch (hostKick(o, clientData, cb)) {
        case Kick::Answered:
            return;
        case Kick::Eos:
            g.api.gameKick(h, o, new KickCall{cb, clientData, o->LobbyId ? o->LobbyId : "", target},
                           reinterpret_cast<void*>(&kickDone));
            return;
        case Kick::Pass:
            break;
        }
    }
    g.api.gameKick(h, o, clientData, cb);
}

// Diagnostics: when the game rewrites the room info, for correlating with members' attributes
// vanishing from other players' copies of the lobby.
void hookUpdateLobby(EOS_HLobby h, const EOS_Lobby_UpdateLobbyOptions* o, void* clientData, EOS_Lobby_OnLobbyIdCallback cb) {
    if (!g_shutdown) logRateLimited("game-update-lobby", 30000, "GAME updated the room info");
    g.api.gameUpdateLobby(h, o, clientData, cb);
}

// The owner the game reads from a lobby's details: Epic's, except that an owner Epic made over the
// pinned one reads as the pinned owner while its direct link is up (the game would make the usurper the
// host otherwise). Only Epic's current owner of our room is rewritten: anyone else's lobby is untouched.
EOS_ProductUserId hookGetLobbyOwner(EOS_HLobbyDetails h, const EOS_LobbyDetails_GetLobbyOwnerOptions* o) {
    EOS_ProductUserId owner = g.api.gameGetOwner(h, o);
    if (!owner || g_shutdown) return owner;
    const std::string usurper = g.ownerPin.usurper();
    if (usurper.empty() || idString(owner) != usurper) return owner;
    const std::string pinned = g.ownerPin.pinned();
    if (pinned.empty() || !reachableDirectly(pinned)) return owner;
    EOS_ProductUserId id = idHandle(pinned);
    return id ? id : owner;
}

void hookLeaveLobby(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!g_shutdown) leftLobby("left the room");
    g.api.gameLeaveLobby(h, o, clientData, cb);
}

void hookDestroyLobby(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!g_shutdown) leftLobby("closed the room");
    g.api.gameDestroyLobby(h, o, clientData, cb);
}

// The room owner's EOS id and the direct-link identity commitment it published ("" when not known).
// EOS calls: run on the EOS tick only.
std::pair<std::string, std::string> roomOwnerIdentity() {
    {
        // A room entered over the direct link: its host is the one we came back to.
        std::lock_guard<std::mutex> lock(g.virtualMutex);
        const VirtualRoom& v = g.virtualRoom;
        if (v.joining || v.in) return {v.room.host, v.room.hostIdentity};
    }
    EOS_ProductUserId owner = nullptr;
    g.marker.ownerAddress(&owner);
    std::string id = idString(owner);
    std::map<std::string, std::string> ids = g.marker.memberIdentities();
    auto it = ids.find(id);
    return {id, it == ids.end() ? std::string() : it->second};
}

void startAutoJoinAttemptLocked(uint64_t now) {
    AutoJoin& a = g.autoJoin;
    DirectOptions o = withNetcode(g.config.direct);
    o.mode = Mode::Join;
    o.listenPort = 0;
    o.hostAddress = a.candidates[a.next++];
    o.advertisedHost = !g.testLoopbackHosts;  // the room host chose it, not this player
    std::tie(o.roomOwner, o.roomOwnerIdentity) = roomOwnerIdentity();  // who may answer on it
    auto net = std::make_shared<DirectNet>();
    if (!net->start(o)) {
        logf("DIRECT auto-connect could not open a UDP socket; game traffic stays on EOS");
        return;
    }
    EOS_ProductUserId me = g.localUser.load();
    if (!me) me = g.lobbyUser.load();
    std::string local = idString(me);
    if (!local.empty()) net->setLocalUser(local);
    std::shared_ptr<DirectNet> old = std::move(a.net);
    a.net = net;
    g.net.store(net);
    if (old) retire(std::move(old));
    a.attemptMs = now;
    a.connected = false;
    logf("DIRECT auto-connecting to the room host %s at %s", shortId(a.hostPuid).c_str(), o.hostAddress.c_str());
}

// AutoJoin: once in someone else's room, connect directly to its host if it advertises an address.
constexpr uint64_t kAutoCheckMs = 2000;
constexpr uint64_t kAutoAttemptMs = 10000;  // per candidate address before trying the next one
constexpr uint64_t kAutoRetryMs = 60000;    // all candidates failed: try them again after this
void autoJoinTick(uint64_t now) {
    AutoJoin& a = g.autoJoin;
    std::lock_guard<std::mutex> lock(a.mu);
    // Made the room's owner (its host left): our link to the old host is no link to the room any more, and the game's
    // packets must come and go through our own listener, which the members now connect to. At once, not at the next
    // check: every packet of a member that came in meanwhile would be read from the dead link.
    if (g.marker.inLobby() && g.marker.isOwner() && a.net) stopAutoJoinLocked("we host the room now");
    if (now - a.lastCheckMs < kAutoCheckMs) return;
    a.lastCheckMs = now;
    if (!g.marker.inLobby() || g.marker.isOwner()) return;
    EOS_ProductUserId owner = nullptr;
    std::string advertised = g.marker.ownerAddress(&owner);
    // Our own address (we were promoted to room owner) is nothing to connect to.
    if (!owner || idString(owner) == idString(g.lobbyUser.load())) return;
    if (advertised.empty()) {
        // Normal for a vanilla host or one without Mode=host. Logged once per room so a host that
        // does advertise but whose attributes never reach us shows up in the log.
        if (!a.noAddressSinceMs) a.noAddressSinceMs = now;
        if (!a.noAddressLogged && now - a.noAddressSinceMs >= 6000) {
            a.noAddressLogged = true;
            logf("DIRECT room host %s advertises no direct-link address (%s)", shortId(idString(owner)).c_str(),
                 g.marker.describeOwner().c_str());
        }
        return;
    }
    a.noAddressSinceMs = 0;
    if (advertised != a.advertised) {  // new room, or the host's address list changed
        a.advertised = advertised;
        a.candidates = orderHostCandidates(advertised);
        if (g.testLoopbackHosts && a.candidates.empty())
            a.candidates.push_back(advertised.substr(0, advertised.find(' ')));  // tests: a loopback host
        a.next = 0;
        a.retryAtMs = 0;
        a.hostPuid = idString(owner);
        logf("DIRECT room host %s advertises %s", shortId(a.hostPuid).c_str(), advertised.c_str());
        if (!a.candidates.empty()) startAutoJoinAttemptLocked(now);
        return;
    }
    if (a.net) {
        // Once connected, a drop is DirectNet's to recover (it keeps reconnecting the same address).
        if (a.connected || a.net->canRoute(a.hostPuid)) {
            a.connected = true;
            return;
        }
        if (now - a.attemptMs < kAutoAttemptMs) return;
        if (a.next < a.candidates.size()) {
            startAutoJoinAttemptLocked(now);
            return;
        }
        std::string keep = a.advertised;
        std::vector<std::string> candidates = a.candidates;
        stopAutoJoinLocked("the room host did not answer on any advertised address; retrying in 60 s");
        a.advertised = keep;
        a.candidates = candidates;
        a.retryAtMs = now + kAutoRetryMs;
        return;
    }
    if (a.retryAtMs && now >= a.retryAtMs && !a.candidates.empty()) {
        a.next = 0;
        a.retryAtMs = 0;
        startAutoJoinAttemptLocked(now);
    }
}

void onQueueFull(const EOS_P2P_OnIncomingPacketQueueFullInfo* i) {
    if (g_shutdown) return;
    logRateLimited("queue-full", 5000,
                   "EOS incoming packet queue FULL (%llu/%llu bytes, channel %u): EOS drops packets now",
                   static_cast<unsigned long long>(i->PacketQueueCurrentSizeBytes),
                   static_cast<unsigned long long>(i->PacketQueueMaxSizeBytes), i->OverflowPacketChannel);
}

void onNatType(const EOS_P2P_OnQueryNATTypeCompleteInfo* i) {
    if (g_shutdown) return;
    logf("EOS NAT type: %s (query result %s)", natName(i->NATType), resultName(i->ResultCode));
}

// Applies EOS tuning once per P2P handle. Runs on the game thread.
void configureHandle(EOS_HP2P h) {
    if (!h || g.configuredHandle.exchange(h) == h) return;
    g.p2p = h;  // atomic store
    if (g.config.eosFixedPort && g.api.setPortRange) {
        EOS_P2P_SetPortRangeOptions o{1, g.config.eosFixedPort, 7};
        EOS_EResult r = g.api.setPortRange(h, &o);
        logf("EOS fixed UDP port range %u-%u: %s", o.Port, o.Port + 7, resultName(r));
    }
    if (g.config.eosRelay >= 0 && g.api.setRelay) {
        EOS_P2P_SetRelayControlOptions o{1, g.config.eosRelay};
        EOS_EResult r = g.api.setRelay(h, &o);
        logf("EOS relay control %s: %s", relayName(g.config.eosRelay), resultName(r));
    }
    if (g.api.getQueueInfo && g.api.setQueueSize) {
        // A full queue makes EOS silently drop packets, which desyncs the game. Only ever grow it.
        EOS_P2P_GetPacketQueueInfoOptions qo{1};
        EOS_P2P_PacketQueueInfo info{};
        if (g.api.getQueueInfo(h, &qo, &info) == EOS_Success) {
            auto grow = [](uint64_t cur) { return cur == 0 ? 0 : (cur < kMinQueueBytes ? kMinQueueBytes : cur); };
            EOS_P2P_SetPacketQueueSizeOptions so{1, grow(info.IncomingPacketQueueMaxSizeBytes),
                                                grow(info.OutgoingPacketQueueMaxSizeBytes)};
            EOS_EResult r = g.api.setQueueSize(h, &so);
            logf("EOS packet queues in %llu -> %llu bytes, out %llu -> %llu bytes (0 = unlimited): %s",
                 static_cast<unsigned long long>(info.IncomingPacketQueueMaxSizeBytes),
                 static_cast<unsigned long long>(so.IncomingPacketQueueMaxSizeBytes),
                 static_cast<unsigned long long>(info.OutgoingPacketQueueMaxSizeBytes),
                 static_cast<unsigned long long>(so.OutgoingPacketQueueMaxSizeBytes), resultName(r));
        }
    }
    if (g.api.addQueueFull) {
        EOS_P2P_AddNotifyIncomingPacketQueueFullOptions o{1};
        g.api.addQueueFull(h, &o, nullptr, onQueueFull);
    }
    if (g.api.queryNat) {
        EOS_P2P_QueryNATTypeOptions o{1};
        g.api.queryNat(h, &o, nullptr, onNatType);
    }
}

// Registers connection diagnostics and tells the direct transport who we are.
void noteLocalUser(EOS_HP2P h, EOS_ProductUserId id) {
    if (!id || g.localUser.exchange(id) == id) return;
    std::string s = idString(id);
    logf("WHOAMI local EOS ProductUserId %s (thread %lu)", s.c_str(), GetCurrentThreadId());
    if (!s.empty()) {
        // The Mode=host/join transport as well as the current one: AutoJoin may have swapped in its
        // own instance, and the host listener without our id would ignore every hello once it is back.
        std::shared_ptr<DirectNet> base = g.baseNet.load();
        if (base) base->setLocalUser(s);
        if (std::shared_ptr<DirectNet> net = g.net.load(); net && net != base) net->setLocalUser(s);
    }
    {
        // Notifications are per local user and stay registered: switching back and forth between
        // accounts must not register them twice (every event would be logged and counted twice).
        std::lock_guard<std::mutex> lock(g.notifyMutex);
        if (s.empty() || !g.notifiedUsers.insert(s).second) return;
    }
    EOS_P2P_AddNotifyOptions o{1, id, nullptr};
    if (g.api.addEstablished) g.api.addEstablished(h, &o, nullptr, onEstablished);
    if (g.api.addInterrupted) g.api.addInterrupted(h, &o, nullptr, onInterrupted);
    if (g.api.addClosed) g.api.addClosed(h, &o, nullptr, onClosed);
}

void maybeLogStats() {
    ULONGLONG now = GetTickCount64();
    const ULONGLONG interval = netcodeOptions().statsIntervalMs;
    if (now - g.lastStatsMs < interval) return;
    bool first = g.lastStatsMs == 0;
    g.lastStatsMs = now;
    if (first) return;
    uint64_t dOut = g.directOut.exchange(0), dIn = g.directIn.exchange(0);
    uint64_t eOut = g.eosOut.exchange(0), eIn = g.eosIn.exchange(0), fail = g.eosSendFail.exchange(0);
    uint64_t upg = g.eosUpgraded.exchange(0);
    TrafficSummary t = g.gameOut.take();
    std::shared_ptr<DirectNet> net = g.net.load();
    WireTraffic w = net ? net->takeWireTraffic() : WireTraffic{};
    // Nothing sent or received by the game (menus, idle links only pinging): nothing to report.
    if (!(dOut | dIn | eOut | eIn | fail | t.packets)) return;
    // kbps = bytes * 8 / 1000 / seconds
    auto kbps = [](uint64_t bytes, double seconds) { return static_cast<double>(bytes) * 8.0 / 1000.0 / seconds; };
    logKickRepeats();
    // One line: what the game sent, what that cost on our socket (resends, acks and pings included, so
    // "wire up" far above "game sends" is the transport's own overhead), and each link's resend state.
    const double seconds = static_cast<double>(interval) / 1000.0;
    logf("STATS last %.0fs: game sends %.0f kbps avg, busiest second %.0f kbps, to %zu players, %.0f B/packet avg "
         "(largest %u), %.0f%% copies of the same data to another player, %.1f%% of packets repeat one sent to the "
         "same player within 5 s | wire up %.0f kbps down %.0f kbps, relayed for others %.0f kbps | direct packets "
         "out=%llu in=%llu | EOS out=%llu (sent reliably %llu) in=%llu send-failures=%llu%s%s",
         seconds, kbps(t.bytes, seconds), kbps(t.busiestSecondBytes, 1.0), t.peers,
         t.packets ? static_cast<double>(t.bytes) / static_cast<double>(t.packets) : 0.0, t.largestPacket,
         t.bytes ? 100.0 * static_cast<double>(t.copyBytes) / static_cast<double>(t.bytes) : 0.0,
         t.packets ? 100.0 * static_cast<double>(t.repeatPackets) / static_cast<double>(t.packets) : 0.0, kbps(w.out, seconds),
         kbps(w.in, seconds), kbps(w.relayed, seconds), static_cast<unsigned long long>(dOut),
         static_cast<unsigned long long>(dIn), static_cast<unsigned long long>(eOut), static_cast<unsigned long long>(upg),
         static_cast<unsigned long long>(eIn), static_cast<unsigned long long>(fail), net ? " | " : "",
         net ? net->statusLine().c_str() : "");
    // The netcode rewrite's paths: what went where, and what was dropped on purpose.
    const uint64_t dups = g.duplicates.duplicates();
    logf("PATHS last %.0fs: direct %.0f kbps, through the host %.0f kbps, relayed for others %.0f kbps (%llu state "
         "datagrams over the relay budget), %llu state datagrams over a path's budget, %llu event copies over EOS, "
         "%llu state datagrams held back by interest management, %llu last state datagrams sent again reliably, %llu "
         "fragments out, %llu bulk messages in, %llu copies dropped on arrival",
         seconds, kbps(w.direct, seconds), kbps(w.viaRelay, seconds), kbps(w.relayed, seconds),
         static_cast<unsigned long long>(w.relayShed), static_cast<unsigned long long>(w.stateShed),
         static_cast<unsigned long long>(g.eosCopies.exchange(0)), static_cast<unsigned long long>(g.aoiSkipped.exchange(0)),
         static_cast<unsigned long long>(g.trailsSent.exchange(0)),
         static_cast<unsigned long long>(g.fragmentsOut.exchange(0)), static_cast<unsigned long long>(g.bulkIn.exchange(0)),
         static_cast<unsigned long long>(dups - g.dupBefore.exchange(dups)));
    // What the game's datagrams held (P0 of the netcode rewrite): per class, then per record type.
    for (const std::string& line : g.recordTypes.take(seconds)) logf("%s", line.c_str());
}

// A direct-link host lets a player in only as the room member whose published identity it proves
// (DirectNet::setMemberIdentities), and a joiner accepts a host only as the room owner, proven the
// same way (DirectNet::setRoomOwner). The member list lives in EOS, which we only call from the tick.
constexpr uint64_t kIdentityRefreshMs = 500;

// A host: whom it lets in over a direct link. Everyone who was in this room with a published identity (a
// member's newest one), not only who Epic lists now: a member Epic's lobby lost, or that comes back while
// Epic is down, still proves who it is the same way. A member our game kicked is not let back in.
std::map<std::string, std::string> roomIdentities() {
    std::map<std::string, std::string> now = g.marker.memberIdentities();
    std::lock_guard<std::mutex> lock(g.viewMutex);
    for (auto& [id, commitment] : now) g.roomIds[id] = commitment;
    std::map<std::string, std::string> out;
    for (const auto& [id, commitment] : g.roomIds)
        if (!g.view.banned(id)) out.emplace(id, commitment);
    return out;
}

void refreshMemberIdentities(uint64_t now) {
    static uint64_t lastMs = 0;
    if (now - lastMs < kIdentityRefreshMs || !g.markerReady) return;
    lastMs = now;
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    // Joiners too: two joiners linking directly (mesh) prove to each other the identities they published.
    const std::map<std::string, std::string> ids = roomIdentities();
    if (base) base->setMemberIdentities(ids);
    auto [owner, commitment] = roomOwnerIdentity();
    if (base && g.config.direct.mode == Mode::Join) base->setRoomOwner(owner, commitment);
    if (std::shared_ptr<DirectNet> net = g.net.load(); net && net != base) {
        net->setRoomOwner(owner, commitment);
        net->setMemberIdentities(ids);
    }
}

// --- The host's say over who is in the room (P2) ---

constexpr uint64_t kRoomTickMs = 250;
// How long a status the plugin would tell the game waits for Epic to report it first (RoomView::settle):
// Epic's events come within a second or two of the change while its lobby service works.
constexpr uint64_t kEpicFirstMs = 5000;

// The room's size as Epic's copy of our lobby says; the size last read while there is no copy. 0: never
// read. EOS calls: on the EOS tick.
uint32_t roomCapacity() {
    const Api& a = g.api;
    EOS_HLobby lobby = nullptr;
    std::string roomId;
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        lobby = g.roomLobby;
        roomId = g.roomId;
    }
    uint32_t capacity = 0;
    EOS_HLobbyDetails details = nullptr;
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{1, roomId.c_str(), g.lobbyUser.load()};
    if (lobby && a.copyDetails && a.copyInfo && a.releaseInfo && a.releaseDetails &&
        a.copyDetails(lobby, &o, &details) == EOS_Success && details) {
        EOS_LobbyDetails_CopyInfoOptions io{1};
        EOS_LobbyDetails_Info* info = nullptr;
        if (a.copyInfo(details, &io, &info) == EOS_Success && info) {
            capacity = info->MaxMembers;
            a.releaseInfo(info);
        }
        a.releaseDetails(details);
    }
    // A MultiSlot room's real size (up to 1024) is the room part's: Epic's lobby holds at most 64.
    const uint32_t epic = capacity;
    if (RoomCapacitySource source = g.roomCapacity.load()) {
        if (const uint32_t real = source()) capacity = real;
    }
    // Members beyond Epic's lobby come in over the direct link only, and only while that lobby is full (see
    // DirectNet::setUnlistedPolicy): never as someone Epic lists, never after this room removed them.
    if (std::shared_ptr<DirectNet> base = g.baseNet.load(); base && g.config.direct.mode == Mode::Host) {
        bool known = false;
        const std::vector<std::string> listed = g.marker.members(&known);
        std::set<std::string> banned;
        {
            std::lock_guard<std::mutex> lock(g.viewMutex);
            banned = g.view.bannedMembers();
        }
        const bool full = known && epic && capacity > epic && listed.size() >= epic;
        base->setUnlistedPolicy(full, std::set<std::string>(listed.begin(), listed.end()), std::move(banned));
    }
    std::lock_guard<std::mutex> lock(g.viewMutex);
    if (capacity) g.viewCapacity = capacity;
    return g.viewCapacity;
}

// The joins we held back when we became the room's host (RoomView::promoted): each goes to our game when the slot the
// old host's game had it in is the one our game gives next (eos::Users::Add takes the first empty slot), so that it
// keeps the slot every other game has it in; the ones without such a slot after them. A slot our game cannot give
// it (another member took it, or a lower one stays empty) is waited for until kHeldJoinCapMs, then the join goes as
// it came.
void placeInherited(const std::string& room, uint64_t now) {
    std::vector<std::pair<std::string, const char*>> tell;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (g.inherited.empty() || g.viewRoom != room) return;
        std::vector<std::string> ours;
        if (GameSlotsSource source = g.gameSlots.load()) ours = source();
        const bool late = now - g.inheritedSinceMs >= kHeldJoinCapMs;
        while (!g.inherited.empty()) {
            const auto [member, slot] = g.inherited.front();
            if (g.view.has(member)) {  // Epic or a direct link brought it meanwhile
                g.inherited.erase(g.inherited.begin());
                continue;
            }
            const InheritedPlacement step = decideInheritedPlacement(slot, ours, late);
            if (!step.tell) break;  // a lower slot is still empty: wait
            const bool fits = step.fits;
            tell.push_back({member, fits ? "it was held back when we became the room's host; the old host's game had it "
                                           "in the slot our game gives next"
                                         : "it was held back when we became the room's host; the slot the old host's "
                                           "game had it in cannot be given, so it goes as it came"});
            g.inherited.erase(g.inherited.begin());
            if (fits && slot >= 0 && !ours.empty()) {  // it takes that slot: the next one waits for the slot after
                if (static_cast<size_t>(slot) >= ours.size()) ours.resize(static_cast<size_t>(slot) + 1);
                ours[static_cast<size_t>(slot)] = member;
            }
        }
    }
    for (const auto& [member, why] : tell) tellGame(room, member, kJoined, why);
}

// Every join held back too long - the room's new host never said its slots, or the host's game never took the member
// in a slot our game has free - reaches our game now, in the order they came (RoomView::releaseHeld), with one line.
void releaseHeldJoins(uint64_t now) {
    std::string room;
    std::vector<std::string> released;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active() || !g.view.heldCount()) return;
        const bool awaiting = g.view.awaitingHost();
        released = g.view.releaseHeld(now, kNewHostSlotsMs, kHeldJoinCapMs);
        if (released.empty()) return;
        room = g.viewRoom;
        logf("ROOM %zu held join(s) go to the game in the order they came: %s", released.size(),
             awaiting && !g.view.awaitingHost() ? "the room's new host did not say its member slots in time"
                                                : "the room's host's game did not take them in a free slot in time");
    }
    for (const std::string& m : released) tellGame(room, m, kJoined, "held back too long");
}

// A host: its game is told who plays in the room by the direct links, and every member is told who the
// game has (DirectNet::setRoomMembers). A member Epic does not list whose direct link is up joins (it dials
// us only while its game is in this room); one with only its link to be in the room by leaves once the
// link stayed down for the grace a disconnect gets. Members Epic lists are Epic's to report, as without the
// plugin, and nothing is told before Epic had its chance (kEpicFirstMs).
void hostRoomTick(const std::shared_ptr<DirectNet>& base, uint64_t now) {
    bool known = false;
    const std::vector<std::string> epic = g.marker.members(&known);
    const std::set<std::string> inEpic(epic.begin(), epic.end());
    std::vector<Linked> linked;
    for (const std::string& m : base->directMembers())
        if (base->linkAlive(m, kLinkAliveMs)) linked.push_back({m, base->linkId(m), inEpic.count(m) != 0});
    const size_t capacity = roomCapacity();
    std::string room;
    std::vector<StatusChange> due;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active()) return;
        room = g.viewRoom;
        std::vector<StatusChange> wanted = g.view.hostJoins(linked, capacity);
        // Without Epic's copy of the lobby, only a member that came in by its direct link is known to have
        // nothing else to be in the room by.
        std::vector<StatusChange> leaves = g.view.hostLeaves(
            [&](const std::string& m) { return known ? inEpic.count(m) != 0 : !g.view.direct(m); },
            [&](const std::string& m) { return base->linkAlive(m, g.config.graceMs); });
        wanted.insert(wanted.end(), leaves.begin(), leaves.end());
        due = g.view.settle(wanted, now, kEpicFirstMs);
    }
    for (const StatusChange& c : due) {
        tellGame(room, c.target, c.status,
                 c.status == kJoined ? "its direct link to us is up, so it plays in this room"
                                     : "it was in the room by its direct link only, and that stayed down");
        if (c.status != kJoined) continue;
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (g.viewRoom == room) g.view.markDirect(c.target);
    }
    placeInherited(room, now);
    std::vector<std::string> members;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active()) return;
        // Our game's slots (room_view.h): every member's game takes each member in the same one. Until the game has
        // its slots, the order they would take them in.
        if (GameSlotsSource slots = g.gameSlots.load()) members = slots();
        if (members.empty()) members = known ? roomOrder(epic, g.view.members()) : g.view.members();
        // Whom the room removed goes with it: whoever hosts the room next keeps them out.
        members = roomMessage(members, g.view.bannedMembers());
    }
    base->setRoomMembers(std::move(members));
}

bool inVirtualRoom();
bool entryParked();

// A member: its game follows who the host's game has in the room (RoomView::followHost), once Epic had its
// chance to say the same. In a room Epic does not know we are in, the host is all there is to hear.
void followHostTick(const std::shared_ptr<DirectNet>& net, uint64_t now) {
    uint64_t version = 0;
    std::string from;
    const std::vector<std::string> host = net->hostRoom(&version, &from);
    const uint64_t delay = inVirtualRoom() ? 0 : kEpicFirstMs;
    std::string room;
    std::vector<StatusChange> due;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active()) return;
        // An empty list: a host of protocol 5 (it never says), or one whose game is out of the room (its
        // room ends for us the way it would without the plugin).
        if (!host.empty() && (g.followedNet.lock() != net || g.followedVersion != version)) {
            g.followedNet = net;
            g.followedVersion = version;
            g.view.heardHost(host, from);
        }
        room = g.viewRoom;
        if (entryParked()) return;  // the game is not in the room yet: it enters with the host's members
        due = g.view.settle(g.view.followHost(), now, delay);
    }
    // One round that frees a slot and fills it: the departures go first (followHost), so the newcomer finds its
    // slot empty. Logged so a test can tell such a round happened.
    const size_t joins = static_cast<size_t>(
        std::count_if(due.begin(), due.end(), [](const StatusChange& c) { return c.status == kJoined; }));
    if (joins && joins < due.size()) {
        std::string order;
        for (const StatusChange& c : due) order += std::string(order.empty() ? "" : ", ") + statusName(c.status) + " " + shortId(c.target);
        logf("ROOM following the host: %zu departure(s) and %zu join(s) in one round (%s)", due.size() - joins, joins,
             order.c_str());
    }
    for (const StatusChange& c : due)
        tellGame(room, c.target, c.status,
                 c.status == kKicked ? "the room's host no longer has us in its room" : "the room's host says so");
}

// A parked entry given up: the game was told its join failed, so it will not leave Epic's lobby itself.
void leaveParkedLobby(const std::string& lobbyId) {
    struct LeaveOptions {  // EOS_Lobby_LeaveLobbyOptions, ApiVersion 1
        int32_t ApiVersion;
        EOS_ProductUserId LocalUserId;
        const char* LobbyId;
    };
    EOS_HLobby lobby = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        lobby = g.roomLobby;
    }
    const PFN_EOS_Lobby_LeaveOrDestroy leave = g.outer.leave ? g.outer.leave : g.api.gameLeaveLobby;
    EOS_ProductUserId user = g.lobbyUser.load();
    leftLobby("the join was given up: its host's member slots did not come");
    // A room we could not enter is nothing to come back into either (it would show up as REJOIN).
    forgetLastRoom("the join was given up");
    if (!leave || !lobby || !user) return;
    const LeaveOptions options{1, user, lobbyId.c_str()};
    // The id is copied by EOS; the completion only logs.
    leave(lobby, &options, nullptr, reinterpret_cast<void*>(static_cast<EOS_Lobby_OnLobbyIdCallback>(
                                        [](const EOS_Lobby_LobbyIdCallbackInfo* i) {
                                            logf("ROOM left lobby %s after the given-up join: %s",
                                                 i && i->LobbyId ? i->LobbyId : "?", i ? resultName(i->ResultCode) : "?");
                                        })));
}

// Whether our direct link to the room's host is getting anywhere: up (its member slots are on their way) or an attempt
// at one of its addresses still running.
bool hostLinkProgressing(uint64_t now) {
    AutoJoin& a = g.autoJoin;
    std::lock_guard<std::mutex> lock(a.mu);
    if (!a.net) return false;
    if (a.connected || a.net->canRoute(a.hostPuid)) return true;
    return now - a.attemptMs < kAutoAttemptMs;
}

// The parked entry (lobbyEnteredWrapper) completes once we follow the host's slots (or the room is gone). Entering
// without them would number the members by Epic's order, and nothing renumbers a member once our game added it: the
// first one the host's game has elsewhere and every later one after it would be off for the rest of the room. So the
// wait is not cut short at a fixed time while the link to the host gets somewhere, and when it cannot (the link is
// going nowhere after kParkedEntryMs, or kParkedEntryCapMs passed) the join fails as a join that could not connect -
// with a log line saying why - and we leave Epic's lobby again.
void parkedEntryTick(uint64_t now) {
    EOS_Lobby_OnLobbyIdCallback callback = nullptr;
    EOS_Lobby_LobbyIdCallbackInfo info{};
    std::string lobbyId;
    ParkedEntryOutcome outcome = ParkedEntryOutcome::Wait;
    uint64_t waited = 0;
    const bool progressing = hostLinkProgressing(now);
    EOS_ProductUserId owner = nullptr;
    const bool hostLink = !g.marker.ownerAddress(&owner).empty() || !owner;  // an owner not known yet: not "none"
    {
        std::lock_guard<std::mutex> lock(g.parkedMutex);
        if (!g.parked.active) return;
        waited = now - g.parked.sinceMs;
        if (hostLink)
            g.parked.noHostSinceMs = 0;
        else if (!g.parked.noHostSinceMs)
            g.parked.noHostSinceMs = now;
        const uint64_t noHost = g.parked.noHostSinceMs ? now - g.parked.noHostSinceMs : 0;
        {
            std::lock_guard<std::mutex> view(g.viewMutex);
            const bool slotted = g.viewRoom == g.parked.lobbyId && g.view.slotted();
            const bool gone = g.viewRoom != g.parked.lobbyId;
            outcome = decideParkedEntry(waited, slotted, gone, progressing, noHost);
            if (outcome == ParkedEntryOutcome::Wait) return;
            if (outcome == ParkedEntryOutcome::Slotted) g.view.adoptHost();
        }
        callback = g.parked.callback;
        info = g.parked.info;
        lobbyId = g.parked.lobbyId;
        g.parked = {};
        g.parkedActive = false;
    }
    info.LobbyId = lobbyId.c_str();
    if (outcome == ParkedEntryOutcome::GiveUp) {
        logf("ROOM NOT entering room %s: its host's member slots did not come (%llu s, the direct link to its host %s); "
             "entering without them would put members in other slots than the host's game has them. The join fails "
             "and we leave the lobby",
             lobbyId.c_str(), static_cast<unsigned long long>(waited / 1000),
             progressing ? "was still connecting" : "got nowhere");
        leaveParkedLobby(lobbyId);
        info.ResultCode = EOS_NoConnection;
        callback(&info);
        return;
    }
    logf("ROOM entering room %s %s", lobbyId.c_str(),
         outcome == ParkedEntryOutcome::Slotted ? "with its host's member slots"
         : outcome == ParkedEntryOutcome::NoHost
             ? "without member slots: its host (the room's owner changed) hosts no direct link, so nobody says slots in "
               "this room and the game goes as it would without the plugin"
             : "(the room is gone)");
    callback(&info);
}

void roomTick(uint64_t now) {
    static uint64_t lastMs = 0;
    if (now - lastMs < kRoomTickMs) return;
    lastMs = now;
    parkedEntryTick(now);
    releaseHeldJoins(now);
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    const bool hosting = g.marker.inLobby() && g.marker.isOwner();
    if (hosting) {
        if (base && g.config.direct.mode == Mode::Host) hostRoomTick(base, now);
        return;
    }
    if (std::shared_ptr<DirectNet> net = g.net.load()) followHostTick(net, now);
}

// --- Coming back into the room we were last in, without Epic (P2) ---

constexpr uint64_t kRememberMs = 2000;
// How long after leaving a room it is offered in the room list. A room is still around for a while when
// its member dropped out (Epic down, the game gave up on the room); after this it most likely is not.
constexpr uint64_t kRejoinWindowMs = 30 * 60 * 1000;
constexpr uint64_t kVirtualAttemptMs = 5000;  // per host address
constexpr uint64_t kVirtualJoinMs = 30000;    // the whole join

LobbyAttribute ownAttribute(const EOS_Lobby_Attribute& a) {
    LobbyAttribute out;
    out.visibility = a.Visibility;
    const EOS_Lobby_AttributeData& d = *a.Data;
    out.key = d.Key ? d.Key : "";
    out.type = d.ValueType;
    switch (d.ValueType) {
    case 0: out.integer = d.Value.AsBool ? 1 : 0; break;
    case 1: out.integer = d.Value.AsInt64; break;
    case 2: out.number = d.Value.AsDouble; break;
    default: out.text = d.Value.AsUtf8 ? d.Value.AsUtf8 : ""; break;
    }
    return out;
}

// What the room list shows of our room, copied out of our copy of the lobby. EOS calls: on the EOS tick.
bool snapshotRoom(LastRoom* r) {
    const Api& a = g.api;
    if (!a.copyDetails || !a.attributeCount || !a.copyAttribute || !a.releaseAttribute || !a.copyInfo ||
        !a.releaseInfo || !a.releaseDetails)
        return false;
    EOS_HLobby lobby = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        lobby = g.roomLobby;
    }
    EOS_HLobbyDetails details = nullptr;
    EOS_Lobby_CopyLobbyDetailsHandleOptions o{1, r->roomId.c_str(), g.lobbyUser.load()};
    if (!lobby || a.copyDetails(lobby, &o, &details) != EOS_Success || !details) return false;
    EOS_LobbyDetails_GetAttributeCountOptions co{1};
    const uint32_t count = a.attributeCount(details, &co);
    for (uint32_t i = 0; i < count; ++i) {
        EOS_LobbyDetails_CopyAttributeByIndexOptions io{1, i};
        EOS_Lobby_Attribute* attribute = nullptr;
        if (a.copyAttribute(details, &io, &attribute) == EOS_Success && attribute && attribute->Data)
            r->attributes.push_back(ownAttribute(*attribute));
        if (attribute) a.releaseAttribute(attribute);
    }
    EOS_LobbyDetails_CopyInfoOptions info{1};
    EOS_LobbyDetails_Info* copy = nullptr;
    if (a.copyInfo(details, &info, &copy) == EOS_Success && copy) {
        r->maxMembers = copy->MaxMembers;
        a.releaseInfo(copy);
    }
    a.releaseDetails(details);
    return r->maxMembers != 0 && !r->attributes.empty();
}

void forgetLastRoomIf(const std::string& roomId, const char* why) {
    std::lock_guard<std::mutex> lock(g.lastMutex);
    if (roomId.empty() || g.lastRoom.roomId != roomId) return;
    logf("REJOIN forgetting room %s: %s", roomId.c_str(), why);
    g.lastRoom = {};
}

// A member of someone's room keeps what coming back into it takes (LastRoom).
void rememberRoomTick(uint64_t now) {
    static uint64_t lastMs = 0;
    if (now - lastMs < kRememberMs || !g.markerReady) return;
    lastMs = now;
    if (!g.marker.inLobby()) return;
    LastRoom r;
    {
        std::lock_guard<std::mutex> lock(g.roomMutex);
        r.roomId = g.roomId;
    }
    if (g.marker.isOwner()) {
        forgetLastRoomIf(r.roomId, "we host it now");
        return;
    }
    EOS_ProductUserId owner = nullptr;
    r.hostAddress = g.marker.ownerAddress(&owner);
    std::tie(r.host, r.hostIdentity) = roomOwnerIdentity();
    const std::string self = idString(g.lobbyUser.load());
    if (!r.usable() || r.host == self || !snapshotRoom(&r)) {
        // The noted host is no longer the room's: coming back to it would reach nobody who can let us in.
        std::string noted;
        {
            std::lock_guard<std::mutex> lock(g.lastMutex);
            if (g.lastRoom.roomId == r.roomId) noted = g.lastRoom.host;
        }
        if (!noted.empty() && !r.host.empty() && r.host != noted)
            forgetLastRoomIf(r.roomId, "its host changed to one that cannot be reached directly");
        return;
    }
    bool known = false;
    for (const std::string& m : g.marker.members(&known))
        if (m != self) r.members.push_back(m);
    std::lock_guard<std::mutex> lock(g.lastMutex);
    if (g.lastRoom.roomId != r.roomId)
        logf("REJOIN remembering room %s of %s: while Epic cannot list it, it is offered in the room list and "
             "entered over the direct link", r.roomId.c_str(), shortId(r.host).c_str());
    g.lastRoom = std::move(r);
    g.lastRoomLeftMs = 0;
}

void forgetLastRoom(const char* why) {
    std::lock_guard<std::mutex> lock(g.lastMutex);
    if (g.lastRoom.roomId.empty()) return;
    logf("REJOIN forgetting room %s: %s", g.lastRoom.roomId.c_str(), why);
    g.lastRoom = {};
}

// The room to offer in the room list, or none (roomId empty).
LastRoom rememberedRoom() {
    std::lock_guard<std::mutex> lock(g.lastMutex);
    if (g.lastRoomLeftMs && GetTickCount64() - g.lastRoomLeftMs > kRejoinWindowMs) g.lastRoom = {};
    return g.lastRoom.usable() ? g.lastRoom : LastRoom{};
}

bool isVirtualRoom(const std::string& lobbyId) {
    std::lock_guard<std::mutex> lock(g.virtualMutex);
    return !lobbyId.empty() && g.virtualRoom.owned && lobbyId == g.virtualRoom.roomId;
}

bool inVirtualRoom() {
    std::lock_guard<std::mutex> lock(g.virtualMutex);
    return g.virtualRoom.in;
}

// The game is done with the virtual room: it left it, or it is in a room through Epic now.
void releaseVirtualRoom() {
    std::lock_guard<std::mutex> lock(g.virtualMutex);
    g.virtualRoom.owned = false;
}

// Takes the virtual room's link out of service (the base transport takes over again). Caller holds
// virtualMutex.
void dropVirtualNetLocked() {
    VirtualRoom& v = g.virtualRoom;
    if (!v.net) return;
    std::shared_ptr<DirectNet> net = std::move(v.net);
    std::shared_ptr<DirectNet> expected = net;
    g.net.compare_exchange_strong(expected, g.baseNet.load());
    retire(std::move(net));
}

// The game left the virtual room (or it went away): its link goes. A join still pending fails.
void endVirtualRoom(const char* why) {
    void* callback = nullptr;
    void* clientData = nullptr;
    std::string roomId;
    {
        std::lock_guard<std::mutex> lock(g.virtualMutex);
        VirtualRoom& v = g.virtualRoom;
        if (!v.joining && !v.in) return;
        logf("REJOIN left room %s, which we were in over the direct link (%s)", v.roomId.c_str(), why);
        if (v.joining) {
            callback = reinterpret_cast<void*>(v.callback);
            clientData = v.clientData;
            roomId = v.roomId;
            v.owned = false;  // its join fails: the game never was in it
        }
        v.joining = v.in = false;
        v.callback = nullptr;
        dropVirtualNetLocked();
    }
    if (callback)
        defer([callback, clientData, roomId] { completeLobbyCall(callback, clientData, EOS_NoConnection, roomId); });
}

// Dials the next of the host's addresses. Caller holds virtualMutex.
bool startVirtualAttemptLocked(uint64_t now) {
    VirtualRoom& v = g.virtualRoom;
    DirectOptions o = withNetcode(g.config.direct);
    o.mode = Mode::Join;
    o.listenPort = 0;
    o.hostAddress = v.candidates[v.next++ % v.candidates.size()];
    o.advertisedHost = !g.testLoopbackHosts;  // the room's host advertised it, not this player
    o.roomOwner = v.room.host;
    o.roomOwnerIdentity = v.room.hostIdentity;  // only that host may answer on it
    auto net = std::make_shared<DirectNet>();
    if (!net->start(o)) {
        logf("REJOIN could not open a UDP socket");
        return false;
    }
    net->setLocalUser(idString(v.user));
    dropVirtualNetLocked();
    v.net = net;
    g.net.store(net);
    v.attemptMs = now;
    logf("REJOIN dialling the room's host %s at %s", shortId(v.room.host).c_str(), o.hostAddress.c_str());
    return true;
}

// The game joins the room it was last in, from the entry the room list got for it: over the direct link to
// its host, whose game lets us in (hostRoomTick). The join completes once the host lists us in its room.
void startVirtualJoinRoom(const LastRoom& room, EOS_ProductUserId user, const std::string& roomId, void* clientData,
                          EOS_Lobby_OnLobbyIdCallback callback, bool proveEos);

// Our EOS id to `host` over EOS P2P (see kIdentityProofSocket): EOS vouches for the sender, the packet names the key
// our direct-link hellos are signed with. Delayed delivery: it waits until the host accepts the connection.
void sendIdentityProof(EOS_ProductUserId local, const std::string& host) {
    std::shared_ptr<const Identity> identity = processIdentity();
    EOS_HP2P p2p = g.p2p;
    EOS_ProductUserId remote = idHandle(host);
    if (!identity || !p2p || !local || !remote) return;
    const std::string commitment = identity->commitment();
    std::vector<uint8_t> proof = {'E', 'D', 'I', 'D'};
    proof.insert(proof.end(), commitment.begin(), commitment.end());
    EOS_P2P_SocketId socket{1, {}};
    strncpy_s(socket.SocketName, kIdentityProofSocket, _TRUNCATE);
    EOS_P2P_SendPacketOptions o{};
    o.ApiVersion = 3;
    o.LocalUserId = local;
    o.RemoteUserId = remote;
    o.SocketId = &socket;
    o.Channel = kIdentityProofChannel;
    o.DataLengthBytes = static_cast<uint32_t>(proof.size());
    o.Data = proof.data();
    o.Reliability = EOS_PR_ReliableOrdered;
    o.bAllowDelayedDelivery = 1;
    g.api.send(p2p, &o);
}

// The host: an identity proof that arrived over EOS (never over the direct link: only EOS vouches for its sender).
bool takeIdentityProof(const std::string& src, uint8_t channel, const uint8_t* data, uint32_t size) {
    if (channel != kIdentityProofChannel || size < 4 || memcmp(data, "EDID", 4) != 0) return false;
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    const std::string commitment(reinterpret_cast<const char*>(data) + 4, size - 4);
    if (base && g.config.direct.mode == Mode::Host && !src.empty() && commitment.size() <= 64) {
        base->proveEosIdentity(src, commitment);
        logRateLimited(("proof-" + src).c_str(), 10000, "DIRECT %s proved its EOS id over EOS", shortId(src).c_str());
    }
    return true;  // never the game's
}

// The host lets the proofs in: it accepts the EOS connection on the proof socket from each EOS id that said hello
// without one. On the EOS tick.
void identityProofTick() {
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    EOS_HP2P p2p = g.p2p;
    EOS_ProductUserId local = g.lobbyUser.load();
    if (!base || g.config.direct.mode != Mode::Host || !p2p || !local || !g.api.accept) return;
    for (const std::string& puid : base->takeProofRequests()) {
        EOS_ProductUserId remote = idHandle(puid);
        if (!remote) continue;
        EOS_P2P_SocketId socket{1, {}};
        strncpy_s(socket.SocketName, kIdentityProofSocket, _TRUNCATE);
        EOS_P2P_PeerConnectionOptions o{1, local, remote, &socket};
        g.api.accept(p2p, &o);
    }
}

void startVirtualJoin(EOS_ProductUserId user, const std::string& roomId, void* clientData,
                      EOS_Lobby_OnLobbyIdCallback callback) {
    startVirtualJoinRoom(rememberedRoom(), user, roomId, clientData, callback, false);
}

void startVirtualJoinRoom(const LastRoom& room, EOS_ProductUserId user, const std::string& roomId, void* clientData,
                          EOS_Lobby_OnLobbyIdCallback callback, bool proveEos) {
    const uint64_t now = GetTickCount64();
    std::lock_guard<std::mutex> lock(g.virtualMutex);
    VirtualRoom& v = g.virtualRoom;
    const bool busy = v.joining || v.in;
    if (busy || room.roomId != roomId || !user) {
        logf("REJOIN cannot join room %s over the direct link (%s)", roomId.c_str(),
             busy ? "already in or joining a room that way" : "it is no longer remembered");
        defer([callback, clientData, roomId] {
            completeLobbyCall(reinterpret_cast<void*>(callback), clientData, EOS_NotFound, roomId);
        });
        return;
    }
    v = {};
    v.roomId = roomId;
    v.owned = true;
    v.joining = true;
    v.room = room;
    v.candidates = orderHostCandidates(room.hostAddress);
    if (g.testLoopbackHosts && v.candidates.empty() && !room.hostAddress.empty())
        v.candidates.push_back(room.hostAddress.substr(0, room.hostAddress.find(' ')));  // tests: a loopback host
    v.user = user;
    v.startMs = now;
    v.proveEos = proveEos;
    v.callback = callback;
    v.clientData = clientData;
    logf("REJOIN joining room %s of %s over the direct link: Epic's lobby is not asked", roomId.c_str(),
         shortId(room.host).c_str());
    if (v.candidates.empty() || !startVirtualAttemptLocked(now)) v.startMs = now - kVirtualJoinMs;  // fails next tick
}

// Drives a virtual join to its end, and keeps the room going: its host's link down for the grace a
// disconnect gets closes it for the game, as Epic closing a room would.
void virtualRoomTick(uint64_t now) {
    std::string self, roomId;
    std::vector<std::string> list;
    uint64_t version = 0;
    EOS_ProductUserId user = nullptr;
    void* callback = nullptr;
    void* clientData = nullptr;
    EOS_EResult result = EOS_Success;
    bool closed = false;
    std::shared_ptr<DirectNet> net;
    {
        std::lock_guard<std::mutex> lock(g.virtualMutex);
        VirtualRoom& v = g.virtualRoom;
        if (!v.joining && !v.in) return;
        if (v.joining && v.proveEos && now - v.provedMs >= 500) {
            v.provedMs = now;
            sendIdentityProof(v.user, v.room.host);
        }
        net = v.net;
        user = v.user;
        self = idString(user);
        roomId = v.roomId;
        list = net ? net->hostRoom(&version) : std::vector<std::string>{};
        std::vector<std::string> slots;
        parseRoomMessage(list, &slots, nullptr);
        if (v.joining) {
            if (std::find(slots.begin(), slots.end(), self) != slots.end()) {
                v.joining = false;
                v.in = true;
                v.hostSeenMs = now;
                callback = reinterpret_cast<void*>(v.callback);
                clientData = v.clientData;
                v.callback = nullptr;
            } else if (now - v.startMs >= kVirtualJoinMs) {
                logf("REJOIN room %s: its host did not let us in within %llu s", roomId.c_str(),
                     static_cast<unsigned long long>(kVirtualJoinMs / 1000));
                callback = reinterpret_cast<void*>(v.callback);
                clientData = v.clientData;
                result = EOS_NoConnection;
                v.joining = false;
                v.owned = false;
                v.callback = nullptr;
                dropVirtualNetLocked();
            } else if (net && !net->canRoute(v.room.host) && now - v.attemptMs >= kVirtualAttemptMs) {
                startVirtualAttemptLocked(now);
            }
        } else if (net && net->linkAlive(v.room.host, kLinkAliveMs)) {
            v.hostSeenMs = now;
        } else if (now - v.hostSeenMs >= g.config.graceMs) {
            // Over whatever the game makes of the CLOSED below (it may have no handler to hear it).
            closed = true;
            v.in = false;
            dropVirtualNetLocked();
        }
    }
    if (callback && result == EOS_Success) {
        logf("REJOIN in room %s again, over the direct link to its host", roomId.c_str());
        g.lobbyUser = user;
        std::vector<std::string> slots;
        parseRoomMessage(list, &slots, nullptr);
        enterView(roomId, self, slots);
        {
            std::lock_guard<std::mutex> lock(g.viewMutex);
            g.view.heardHost(list);  // the list we entered with
            g.followedNet = net;
            g.followedVersion = version;
        }
        std::lock_guard<std::mutex> lock(g.lastMutex);
        g.lastRoomLeftMs = 0;
    }
    if (callback) {
        defer([callback, clientData, result, roomId] { completeLobbyCall(callback, clientData, result, roomId); });
        return;
    }
    if (!closed) return;
    const char* why = "the room's host stayed out of reach over the direct link";
    logf("REJOIN room %s closed: %s", roomId.c_str(), why);
    tellGame(roomId, self, kClosed, why);
    // What the CLOSED means for us, also when no handler of the game's was there to deliver it to.
    forgetLastRoom(why);
    leftLobby(why);
}

// Runs after every EOS_Platform_Tick, i.e. where EOS itself would deliver callbacks to the game.
void hookPlatformTick(EOS_HPlatform platform) {
    g.api.tick(platform);
    if (g_shutdown) return;
    static bool loggedThread = false;
    if (!loggedThread) {
        loggedThread = true;
        logf("EOS tick runs on thread %lu", GetCurrentThreadId());
        noteGameRunning();  // an update on trial starts its health clock here
    }
    maybeLogStats();
    trailTick(GetTickCount64());
    meshTick(GetTickCount64());
    bulkTick(GetTickCount64());
    identityProofTick();
    g.marker.tick();
    refreshMemberIdentities(GetTickCount64());
    if (g.autoJoinOn) autoJoinTick(GetTickCount64());
    runDeferred();
    roomTick(GetTickCount64());
    rememberRoomTick(GetTickCount64());
    virtualRoomTick(GetTickCount64());
    if (g.hold && g.hold->heldCount()) {
        for (const auto& remote : g.hold->poll(GetTickCount64(), directAlive))
            logf("RESILIENCE %s did not come back within %u s; disconnect handed to the game",
                 shortId(remote).c_str(), g.config.graceMs / 1000);
    }
    if (g.lobbyHold && g.lobbyHold->heldCount()) {
        for (const auto& remote : g.lobbyHold->poll(GetTickCount64(), reachableDirectly))
            logf("RESILIENCE %s: held lobby status handed to the game (its direct link is down, or the game kicked)",
                 remote[0] == '#' ? remote.c_str() : shortId(remote).c_str());
    }
}

EOS_HP2P hookGetP2PInterface(EOS_HPlatform platform) {
    EOS_HP2P h = g.api.getP2P(platform);
    if (!g_shutdown) configureHandle(h);
    return h;
}

// The class of a packet the game sends (netclass.h), from the records its packet controller flushed into it on
// this thread just before. Only the game's own datagrams (sent UnreliableUnordered) have them; anything else - the
// plugin's packets, sent with a reliability of their own - is Unknown and goes as it asks.
TrafficClass classifyGameSend(const std::string& remote, const EOS_P2P_SendPacketOptions& o) {
    std::optional<PlainDatagram> plain = takePendingDatagram(o.DataLengthBytes);
    if (o.Reliability != EOS_PR_UnreliableUnordered) return TrafficClass::Unknown;
    if (!plain) {
        g.recordTypes.recordUnmatched();
        return TrafficClass::Unknown;
    }
    const uint64_t now = GetTickCount64();
    for (const RecordInfo& r : plain->records)
        if (!r.reliable && !isControllerRecord(r.type) && g.stateTypes.observe(remote, r.type, now))
            logf("NETCLASS record type 0x%05X behaves as state (sent to %s %u times in a row): its datagrams go unreliably "
                 "from now on", r.type, shortId(remote).c_str(), StateLearner::kSamples);
    g.recordTypes.record(remote, plain->records, now);
    const TrafficClass cls = classify(plain->records, plain->parsed, g.stateTypes);
    g.recordTypes.recordDatagram(cls, o.DataLengthBytes);
    return cls;
}

bool roomCap(uint32_t cap) {
    RoomCapQuery query = g.roomCaps.load();
    return query && query(cap);
}

// A state datagram is carried unreliably because the next one replaces it. The last one before a pause has nothing
// after it (a player stopped, a state that changes no more): kTrailMs after the newest state datagram to a member,
// that datagram goes once more, reliably. The game drops it as a copy when the first one arrived (its datagram
// number and CRC, 12CE1F3), and so does DuplicateFilter.
constexpr uint64_t kTrailMs = 300;
void noteTrail(EOS_HP2P h, const EOS_P2P_SendPacketOptions& o, const std::string& remote) {
    if (!o.Data || remote.empty()) return;
    std::lock_guard<std::mutex> lock(g.trailMutex);
    State::Trail& t = g.trails[remote];
    t.p2p = h;
    t.local = o.LocalUserId;
    t.remote = o.RemoteUserId;
    t.socket = socketName(o.SocketId);
    t.channel = o.Channel;
    t.data.assign(static_cast<const uint8_t*>(o.Data), static_cast<const uint8_t*>(o.Data) + o.DataLengthBytes);
    t.sentMs = GetTickCount64();
}

void trailTick(uint64_t now) {
    std::vector<State::Trail> due;
    {
        std::lock_guard<std::mutex> lock(g.trailMutex);
        for (auto it = g.trails.begin(); it != g.trails.end();) {
            if (now - it->second.sentMs < kTrailMs) {
                ++it;
                continue;
            }
            due.push_back(std::move(it->second));
            due.back().member = it->first;
            it = g.trails.erase(it);
        }
    }
    for (const State::Trail& t : due) {
        std::shared_ptr<DirectNet> net = g.net.load();
        if (net && net->send(t.member, t.socket, t.channel, EOS_PR_ReliableUnordered, t.data.data(), t.data.size())) {
            ++g.trailsSent;
            continue;
        }
        EOS_P2P_SocketId socket{1, {}};
        strncpy_s(socket.SocketName, t.socket.c_str(), _TRUNCATE);
        EOS_P2P_SendPacketOptions copy{};
        copy.ApiVersion = 3;
        copy.LocalUserId = t.local;
        copy.RemoteUserId = t.remote;
        copy.SocketId = &socket;
        copy.Channel = t.channel;
        copy.DataLengthBytes = static_cast<uint32_t>(t.data.size());
        copy.Data = t.data.data();
        copy.Reliability = EOS_PR_ReliableUnordered;
        if (g.api.send(t.p2p, &copy) == EOS_Success) ++g.trailsSent;
    }
}

// One packet over EOS as the game would send it, except for channel, data and reliability.
EOS_EResult sendOverEos(EOS_HP2P h, const EOS_P2P_SendPacketOptions& o, uint8_t channel, const uint8_t* data,
                        uint32_t size, EOS_EPacketReliability reliability) {
    EOS_P2P_SendPacketOptions copy = o;
    copy.Channel = channel;
    copy.Data = data;
    copy.DataLengthBytes = size;
    copy.Reliability = reliability;
    return g.api.send(h, &copy);
}

// A message above EOS's packet size, in fragments (fragment.h): reliably over the direct link when it reaches
// `remote`, otherwise reliably over EOS. EOS_Success once every fragment left.
EOS_EResult sendFragments(EOS_HP2P h, const EOS_P2P_SendPacketOptions& o, const std::string& remote, uint8_t flags,
                          uint16_t tag, const uint8_t* data, size_t size, uint64_t id = 0) {
    const auto parts = splitIntoFragments(id ? id : g.fragmentEpoch | ++g.fragmentIds, flags, tag, data, size);
    if (parts.empty()) return EOS_LimitExceeded;
    std::shared_ptr<DirectNet> net = g.net.load();
    const bool direct = net && net->canRoute(remote);
    const uint32_t cut = (flags & kFragmentBulk) ? g.testBulkFragments.load() : 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto& part = parts[i];
        if (cut && i >= cut) break;  // [Test]: the rest is lost on the way, every time
        bool sent = direct && net->send(remote, socketName(o.SocketId), kFragmentChannel, EOS_PR_ReliableOrdered,
                                        part.data(), part.size());
        if (!sent) {
            EOS_EResult r = sendOverEos(h, o, kFragmentChannel, part.data(), static_cast<uint32_t>(part.size()),
                                        EOS_PR_ReliableOrdered);
            if (r != EOS_Success) return r;
        }
        ++g.fragmentsOut;
    }
    return EOS_Success;
}

EOS_EResult hookSendPacket(EOS_HP2P h, const EOS_P2P_SendPacketOptions* o) {
    if (g_shutdown) return g.api.send(h, o);
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    std::string remote = o ? idString(o->RemoteUserId) : std::string();
    if (o && o->Data)
        g.gameOut.record(remote, o->DataLengthBytes, TrafficMeter::hash(o->Data, o->DataLengthBytes), GetTickCount64());
    const NetcodeOptions netcode = netcodeOptions();
    TrafficClass cls = o ? classifyGameSend(remote, *o) : TrafficClass::Unknown;
    // Measured always (the log), acted on only while every member runs the classes: a member of the game as it
    // ships, or of an older EDF6Coop, keeps getting everything as before (reliable, unfiltered).
    if (!netcode.trafficClasses || !roomCap(kCapTrafficClasses)) cls = TrafficClass::Unknown;
    if (cls == TrafficClass::State) noteTrail(h, *o, remote);
    if (o && o->ApiVersion >= 3) {
        std::lock_guard<std::mutex> lock(g.lastSendMutex);
        g.lastLocal = o->LocalUserId;
        g.lastSocket = socketName(o->SocketId);
    }
    // Interest management (W6) may hold a state datagram back: the next one replaces it.
    if (cls == TrafficClass::State) {
        StateSendFilter filter = g.stateFilter.load();
        if (filter && !filter(remote, idString(o->LocalUserId), o->DataLengthBytes, linkBudgetBytesPerSec(remote),
                              GetTickCount64())) {
            ++g.aoiSkipped;
            return EOS_Success;
        }
    }
    // EOS refuses anything above its packet limit, and so does the direct link (its receivers' games read
    // with that limit too). The game then retries for about 26 s and disbands the room - unless the whole room
    // reads fragments.
    if (o && o->Data && o->DataLengthBytes > EOS_P2P_MAX_PACKET_SIZE && !remote.empty() && netcode.fragments &&
        roomCap(kCapFragments))
        return sendFragments(h, *o, remote, 0, o->Channel, static_cast<const uint8_t*>(o->Data), o->DataLengthBytes);
    if (o && o->DataLengthBytes > EOS_P2P_MAX_PACKET_SIZE)
        logRateLimited("oversize", 10000,
                       "GAME sends a %u-byte packet (channel %u) to %s: over the %u-byte packet limit, so it cannot be "
                       "delivered; the game retries for about 26 s and then disbands the room",
                       o->DataLengthBytes, static_cast<unsigned>(o->Channel), peerLabel(o->RemoteUserId).c_str(),
                       static_cast<unsigned>(EOS_P2P_MAX_PACKET_SIZE));
    std::shared_ptr<DirectNet> net = g.net.load();
    if (net && o && o->Data && o->DataLengthBytes <= EOS_P2P_MAX_PACKET_SIZE && !remote.empty()) {
        const SendReport sent =
            net->sendClassified(remote, socketName(o->SocketId), o->Channel, static_cast<uint8_t>(o->Reliability),
                                static_cast<const uint8_t*>(o->Data), o->DataLengthBytes, static_cast<uint8_t>(cls));
        if (sent.sent) {
            ++g.directOut;
            // An event goes twice only where two different direct paths lead (the direct link and the relay,
            // DirectNet::routeFor). Over one path (a joiner and its host) a copy over EOS too would double the
            // host's uplink for every acknowledgement; the game resends what was lost.
            return EOS_Success;
        }
    }
    EOS_EResult r;
    bool held = g.hold && o && o->ApiVersion >= 3 && g.hold->isHeld(remote);
    // State is replaced by the next datagram: carried unreliably, as the game sent it. Everything else the game
    // sent unreliably keeps EOS's repair (an event's loss would otherwise wait for the game's resend).
    bool upgrade = g.config.reliableGameTraffic && o && o->ApiVersion >= 3 &&
                   o->Reliability == EOS_PR_UnreliableUnordered && cls != TrafficClass::State;
    if (held || upgrade) {
        EOS_P2P_SendPacketOptions copy = *o;
        // The connection is being rebuilt: let EOS queue the packet instead of discarding it.
        if (held) copy.bAllowDelayedDelivery = 1;
        // EDF6 sends all game data UnreliableUnordered; lost packets are a desync source. Reliable-
        // unordered delivers every packet exactly once, possibly reordered, which the unreliable
        // transport could also do, so the game handles it. Only this sender needs the plugin: EOS on
        // the receiving side acknowledges reliable packets by itself.
        if (upgrade) copy.Reliability = EOS_PR_ReliableUnordered;
        r = g.api.send(h, &copy);
        if (upgrade) ++g.eosUpgraded;
    } else {
        r = g.api.send(h, o);
    }
    ++g.eosOut;
    if (r != EOS_Success) {
        ++g.eosSendFail;
        std::string key = "send-fail-" + std::to_string(r) + "-" + remote;  // per target: one hides no others
        logRateLimited(key.c_str(), 10000, "EOS SendPacket to %s failed: %s", peerLabel(o ? o->RemoteUserId : nullptr).c_str(),
                       resultName(r));
    }
    return r;
}

// A packet that arrived (over the direct link or EOS): a fragment goes into its message, a copy of one already
// delivered is dropped. True when the game gets it as it is.
bool takeArrival(const std::string& src, const std::string& socket, uint8_t channel, const uint8_t* data, uint32_t size,
                 bool mayBeCopy) {
    uint64_t acked = 0;
    if (channel == kFragmentChannel && parseFragmentAck(data, size, acked)) {
        std::lock_guard<std::mutex> lock(g.bulkMutex);
        g.bulks.erase(std::remove_if(g.bulks.begin(), g.bulks.end(),
                                     [&](const State::PendingBulk& b) { return b.member == src && b.id == acked; }),
                      g.bulks.end());
        return false;
    }
    if (channel == kFragmentChannel && isFragment(data, size)) {
        std::optional<FragmentMessage> done;
        bool again = false;
        {
            std::lock_guard<std::mutex> lock(g.fragmentMutex);
            done = g.reassembler.add(src, data, size, GetTickCount64(), &again);
        }
        const bool bulk = done ? (done->flags & kFragmentBulk) != 0 : again && (data[5] & kFragmentBulk) != 0;
        if (bulk) ackBulk(src, Reassembler::idOf(data, size));  // its sender stops resending
        if (!done) return false;
        if (done->flags & kFragmentBulk) {
            ++g.bulkIn;
            if (BulkHandler handler = g.bulkHandler.load()) handler(src, done->tag, done->bytes.data(), done->bytes.size());
            else logRateLimited("bulk-unhandled", 10000, "NETCODE a bulk message (tag %u, %zu bytes) from %s has no handler",
                                done->tag, done->bytes.size(), shortId(src).c_str());
            return false;
        }
        std::lock_guard<std::mutex> lock(g.fragmentMutex);
        g.ready.push_back({src, socket, static_cast<uint8_t>(done->tag), std::move(done->bytes)});
        return false;
    }
    // Only what the sender may have sent over two paths (a classified game datagram over the direct link and the
    // relay) is checked for a copy: anything else - the plugin's channels, a room of the game as it ships, a
    // datagram that came once by design - reaches the game as it came, also when its bytes repeat.
    return !mayBeCopy || channel != 0 || g.duplicates.first(src, data, size, GetTickCount64());
}

// A reassembled datagram the game asked for (channel and size), into its receive buffers.
bool deliverReady(const EOS_P2P_ReceivePacketOptions* o, EOS_ProductUserId* outPeer, EOS_P2P_SocketId* outSocket,
                  uint8_t* outChannel, void* outData, uint32_t* outBytes) {
    const uint8_t* channel = o->ApiVersion >= 2 ? o->RequestedChannel : nullptr;
    std::lock_guard<std::mutex> lock(g.fragmentMutex);
    for (auto it = g.ready.begin(); it != g.ready.end(); ++it) {
        if (channel && it->channel != *channel) continue;
        EOS_ProductUserId peer = idHandle(it->src);
        if (!peer || it->data.size() > o->MaxDataSizeBytes) {
            logRateLimited("ready-drop", 10000, "NETCODE a reassembled %zu-byte packet does not fit the game's %u-byte buffer",
                           it->data.size(), o->MaxDataSizeBytes);
            g.ready.erase(it);
            return false;
        }
        *outPeer = peer;
        if (outSocket) {
            outSocket->ApiVersion = 1;
            strncpy_s(outSocket->SocketName, it->socket.c_str(), _TRUNCATE);
        }
        if (outChannel) *outChannel = it->channel;
        memcpy(outData, it->data.data(), it->data.size());
        *outBytes = static_cast<uint32_t>(it->data.size());
        g.ready.erase(it);
        return true;
    }
    return false;
}

EOS_EResult hookReceivePacket(EOS_HP2P h, const EOS_P2P_ReceivePacketOptions* o, EOS_ProductUserId* outPeer,
                              EOS_P2P_SocketId* outSocket, uint8_t* outChannel, void* outData, uint32_t* outBytes) {
    if (g_shutdown) return g.api.receive(h, o, outPeer, outSocket, outChannel, outData, outBytes);
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    if (!o || !outPeer || !outData || !outBytes) return g.api.receive(h, o, outPeer, outSocket, outChannel, outData, outBytes);
    std::shared_ptr<DirectNet> net = g.net.load();
    const uint8_t* channel = o->ApiVersion >= 2 ? o->RequestedChannel : nullptr;
    // Fragments travel on a channel of their own; a receive that asks for one channel only must not leave them
    // waiting. A bounded number per call: the game's frame goes on meanwhile.
    if (channel && *channel != kFragmentChannel) {
        for (int i = 0; i < 64; ++i) {
            Delivered d;
            if (!net || !net->pop(&kFragmentChannel, kFragmentPacket, d)) break;
            takeArrival(d.src, d.socketName, d.channel, d.data.data(), static_cast<uint32_t>(d.data.size()), false);
        }
        EOS_P2P_ReceivePacketOptions fragments = *o;
        fragments.RequestedChannel = &kFragmentChannel;
        std::vector<uint8_t> buf(kFragmentPacket);
        fragments.MaxDataSizeBytes = static_cast<uint32_t>(buf.size());
        for (int i = 0; i < 64; ++i) {
            EOS_ProductUserId peer = nullptr;
            EOS_P2P_SocketId socket{};
            uint8_t ch = 0;
            uint32_t n = 0;
            if (g.api.receive(h, &fragments, &peer, &socket, &ch, buf.data(), &n) != EOS_Success) break;
            takeArrival(idString(peer), std::string(socket.SocketName, strnlen(socket.SocketName, sizeof(socket.SocketName))),
                        ch, buf.data(), n, false);
        }
    }
    for (;;) {
        if (deliverReady(o, outPeer, outSocket, outChannel, outData, outBytes)) return EOS_Success;
        Delivered d;
        if (net && net->pop(channel, o->MaxDataSizeBytes, d)) {
            if (!takeArrival(d.src, d.socketName, d.channel, d.data.data(), static_cast<uint32_t>(d.data.size()), d.cls != 0))
                continue;
            EOS_ProductUserId peer = idHandle(d.src);
            if (!peer) continue;
            *outPeer = peer;
            if (outSocket) {
                outSocket->ApiVersion = 1;
                strncpy_s(outSocket->SocketName, d.socketName.c_str(), _TRUNCATE);
            }
            if (outChannel) *outChannel = d.channel;
            memcpy(outData, d.data.data(), d.data.size());
            *outBytes = static_cast<uint32_t>(d.data.size());
            ++g.directIn;
            return EOS_Success;
        }
        EOS_EResult r = g.api.receive(h, o, outPeer, outSocket, outChannel, outData, outBytes);
        if (r != EOS_Success) return r;
        ++g.eosIn;
        const std::string src = idString(*outPeer);
        const std::string socket =
            outSocket ? std::string(outSocket->SocketName, strnlen(outSocket->SocketName, sizeof(outSocket->SocketName)))
                      : std::string();
        if (takeIdentityProof(src, outChannel ? *outChannel : 0, static_cast<const uint8_t*>(outData), *outBytes)) continue;
        if (takeArrival(src, socket, outChannel ? *outChannel : 0, static_cast<const uint8_t*>(outData), *outBytes, false))
            return EOS_Success;
    }
}

// --- The room list and the virtual room, in front of every other wrapper (installVirtualRoomHooks) ---

// The room list's entry for the remembered room: the room as we last saw it, without us (the game skips
// JoinLobby for a room that lists it already).
EOS_HLobbyDetails rememberedRoomDetails(const LastRoom& r) {
    FakeDetails d;
    d.roomId = r.roomId;
    d.owner = r.host;
    d.members = r.members;
    d.maxMembers = r.maxMembers;
    d.attributes = r.attributes;
    return g.fakes.make(std::move(d));
}

// A finished room search: when Epic could not search (its lobby service down), it succeeds with the
// remembered room as its one result. A search Epic answered is the game's as it is: a room it leaves out is
// full, filtered out or gone. Not while we are in a room, and not for a search after another room (invites).
bool addRememberedRoom(EOS_HLobbySearch search, EOS_EResult result) {
    if (result == EOS_Success) return false;
    const LastRoom r = rememberedRoom();
    if (r.roomId.empty() || g.marker.inLobby()) return false;
    {
        std::lock_guard<std::mutex> lock(g.virtualMutex);
        if (g.virtualRoom.joining || g.virtualRoom.in) return false;
    }
    {
        std::lock_guard<std::mutex> lock(g.searchMutex);
        const std::string& wanted = g.searches[search].lobbyId;
        if (!wanted.empty() && wanted != r.roomId) return false;
    }
    std::lock_guard<std::mutex> lock(g.searchMutex);
    g.searches[search].added = true;
    logf("REJOIN the room list gets room %s of %s: Epic could not search (%s)", r.roomId.c_str(),
         shortId(r.host).c_str(), resultName(result));
    return true;
}

struct SearchCall {
    EOS_LobbySearch_OnFindCallback callback;
    void* clientData;
    EOS_HLobbySearch search;
};

void searchFound(const EOS_LobbySearch_FindCallbackInfo* i) {
    auto* call = static_cast<SearchCall*>(i->ClientData);
    EOS_LobbySearch_FindCallbackInfo copy = *i;
    copy.ClientData = call->clientData;
    const bool final = !g.api.isComplete || g.api.isComplete(i->ResultCode);
    if (final && !g_shutdown && addRememberedRoom(call->search, i->ResultCode)) copy.ResultCode = EOS_Success;
    EOS_LobbySearch_OnFindCallback cb = call->callback;
    if (final) delete call;
    cb(&copy);
}

void hookSearchFind(EOS_HLobbySearch search, const EOS_LobbySearch_FindOptions* o, void* clientData,
                    EOS_LobbySearch_OnFindCallback cb) {
    if (!cb || g_shutdown) return g.outer.find(search, o, clientData, cb);
    {
        std::lock_guard<std::mutex> lock(g.searchMutex);
        g.searches[search].added = false;  // a search run again starts over
    }
    g.outer.find(search, o, new SearchCall{cb, clientData, search}, searchFound);
}

bool searchHasRemembered(EOS_HLobbySearch search) {
    std::lock_guard<std::mutex> lock(g.searchMutex);
    auto it = g.searches.find(search);
    return it != g.searches.end() && it->second.added;
}

uint32_t hookSearchResultCount(EOS_HLobbySearch search, const EOS_LobbySearch_GetSearchResultCountOptions* o) {
    const uint32_t count = g.outer.resultCount(search, o);
    return !g_shutdown && searchHasRemembered(search) ? count + 1 : count;
}

EOS_EResult hookSearchCopyResult(EOS_HLobbySearch search, const EOS_LobbySearch_CopySearchResultByIndexOptions* o,
                                 EOS_HLobbyDetails* out) {
    if (g_shutdown || !o || !out || !searchHasRemembered(search)) return g.outer.copyResult(search, o, out);
    EOS_LobbySearch_GetSearchResultCountOptions co{1};
    if (o->LobbyIndex != g.outer.resultCount(search, &co)) return g.outer.copyResult(search, o, out);
    const LastRoom r = rememberedRoom();
    if (r.roomId.empty()) return EOS_NotFound;
    *out = rememberedRoomDetails(r);
    return EOS_Success;
}

EOS_EResult hookSearchSetLobbyId(EOS_HLobbySearch search, const EOS_LobbySearch_SetLobbyIdOptions* o) {
    if (!g_shutdown) {
        std::lock_guard<std::mutex> lock(g.searchMutex);
        g.searches[search].lobbyId = o && o->LobbyId ? o->LobbyId : "";
    }
    return g.outer.setLobbyId(search, o);
}

void hookSearchRelease(EOS_HLobbySearch search) {
    if (!g_shutdown) {
        std::lock_guard<std::mutex> lock(g.searchMutex);
        g.searches.erase(search);
    }
    g.outer.releaseSearch(search);
}

// A room Epic lists, read for coming in over the direct link when its lobby is full (rooms above 64): its owner,
// the address and identity the owner put on the lobby, its members, size and attributes. Not usable (roomId empty
// or no address) for any other room.
LastRoom fullRoomFrom(EOS_HLobbyDetails details) {
    LastRoom r;
    const Outer& o = g.outer;
    if (!details || !o.owner || !o.attributeCount || !o.copyAttribute || !o.releaseAttribute || !o.copyInfo ||
        !o.releaseInfo)
        return r;
    EOS_LobbyDetails_CopyInfoOptions io{1};
    EOS_LobbyDetails_Info* info = nullptr;
    if (o.copyInfo(details, &io, &info) != EOS_Success || !info) return r;
    r.roomId = info->LobbyId ? info->LobbyId : "";
    r.maxMembers = info->MaxMembers;
    o.releaseInfo(info);
    EOS_LobbyDetails_GetLobbyOwnerOptions oo{1};
    r.host = idString(o.owner(details, &oo));
    EOS_LobbyDetails_GetAttributeCountOptions co{1};
    const uint32_t count = o.attributeCount(details, &co);
    for (uint32_t i = 0; i < count; ++i) {
        EOS_LobbyDetails_CopyAttributeByIndexOptions ai{1, i};
        EOS_Lobby_Attribute* attribute = nullptr;
        if (o.copyAttribute(details, &ai, &attribute) == EOS_Success && attribute && attribute->Data) {
            LobbyAttribute a = ownAttribute(*attribute);
            if (a.key == kHostAddressKey) r.hostAddress = a.text;
            if (a.key == kHostIdentityKey) r.hostIdentity = a.text;
            r.attributes.push_back(std::move(a));
        }
        if (attribute) o.releaseAttribute(attribute);
    }
    return r;
}

// The game's JoinLobby of a room Epic lists. Epic turns a player away from a lobby holding its 64: when the room's
// owner put its direct-link address and identity on the lobby, the join goes on over the direct link to it instead
// (the same way back into a room as REJOIN), and the room's host lets the player in by its own member list.
struct EpicJoin {
    EOS_Lobby_OnLobbyIdCallback callback;
    void* clientData;
    EOS_ProductUserId user;
    LastRoom room;
    bool full;  // the lobby listed as many members as it holds
};

void epicJoined(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    auto* call = static_cast<EpicJoin*>(i->ClientData);
    const bool final = !g.api.isComplete || g.api.isComplete(i->ResultCode);
    // Epic's code for a full lobby is not one this plugin can rely on: a lobby that listed its 64 when the game found
    // it, or EOS_LimitExceeded, is taken as full.
    const bool full = i->ResultCode != EOS_Success && (call->full || i->ResultCode == EOS_LimitExceeded);
    if (final && full && !g_shutdown && call->room.usable()) {
        logf("REJOIN Epic's lobby of room %s is full (%s): coming in over the direct link to its host %s",
             call->room.roomId.c_str(), resultName(i->ResultCode), shortId(call->room.host).c_str());
        startVirtualJoinRoom(call->room, call->user, call->room.roomId, call->clientData, call->callback, true);
        delete call;
        return;
    }
    EOS_Lobby_LobbyIdCallbackInfo copy = *i;
    copy.ClientData = call->clientData;
    EOS_Lobby_OnLobbyIdCallback cb = call->callback;
    if (final) delete call;
    cb(&copy);
}

void hookVirtualJoin(EOS_HLobby h, const EOS_Lobby_JoinLobbyOptionsHead* o, void* clientData,
                     EOS_Lobby_OnLobbyIdCallback cb) {
    FakeDetails d;
    if (g_shutdown || !o || !cb) return g.outer.join(h, o, clientData, cb);
    if (g.fakes.lookup(o->LobbyDetailsHandle, &d)) return startVirtualJoin(o->LocalUserId, d.roomId, clientData, cb);
    LastRoom room = fullRoomFrom(o->LobbyDetailsHandle);
    if (!room.usable()) return g.outer.join(h, o, clientData, cb);
    bool full = false;
    if (g.outer.memberCount) {
        EOS_LobbyDetails_GetMemberCountOptions mc{1};
        full = room.maxMembers && g.outer.memberCount(o->LobbyDetailsHandle, &mc) >= room.maxMembers;
    }
    g.outer.join(h, o, new EpicJoin{cb, clientData, o->LocalUserId, std::move(room), full}, epicJoined);
}

struct LeaveOptionsHead {  // EOS_Lobby_LeaveLobbyOptions and EOS_Lobby_DestroyLobbyOptions, ApiVersion 1
    int32_t ApiVersion;
    EOS_ProductUserId LocalUserId;
    const char* LobbyId;
};

// The game leaves (or, as its owner, closes) the virtual room: nothing for Epic, it completes here.
bool leaveVirtualRoom(const void* options, void* clientData, void* cb) {
    const auto* o = static_cast<const LeaveOptionsHead*>(options);
    const std::string lobbyId = o && o->LobbyId ? o->LobbyId : "";
    if (g_shutdown || !isVirtualRoom(lobbyId)) return false;
    leftLobby("left the room we were in over the direct link");
    releaseVirtualRoom();
    defer([cb, clientData, lobbyId] { completeLobbyCall(cb, clientData, EOS_Success, lobbyId); });
    return true;
}

void hookVirtualLeave(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!leaveVirtualRoom(o, clientData, cb)) g.outer.leave(h, o, clientData, cb);
}

void hookVirtualDestroy(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!leaveVirtualRoom(o, clientData, cb)) g.outer.destroy(h, o, clientData, cb);
}

// The virtual room as the game reads it on entering (Room::Initialize): its host owns it, and its members
// are who the game has in it.
EOS_EResult hookCopyDetails(EOS_HLobby h, const EOS_Lobby_CopyLobbyDetailsHandleOptions* o, EOS_HLobbyDetails* out) {
    if (g_shutdown || !o || !o->LobbyId || !out) return g.outer.copyDetails(h, o, out);
    FakeDetails d;
    {
        std::lock_guard<std::mutex> lock(g.virtualMutex);
        const VirtualRoom& v = g.virtualRoom;
        if (!v.in || v.roomId != o->LobbyId) return g.outer.copyDetails(h, o, out);
        d.roomId = v.roomId;
        d.owner = v.room.host;
        d.maxMembers = v.room.maxMembers;
        d.attributes = v.room.attributes;
    }
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        d.members = g.view.slotted() ? g.view.hostMembers() : g.view.members();
    }
    *out = g.fakes.make(std::move(d));
    return EOS_Success;
}

uint32_t hookDetailsAttributeCount(EOS_HLobbyDetails h, const EOS_LobbyDetails_GetAttributeCountOptions* o) {
    FakeDetails d;
    return g.fakes.lookup(h, &d) ? static_cast<uint32_t>(d.attributes.size()) : g.outer.attributeCount(h, o);
}

EOS_EResult hookDetailsCopyAttribute(EOS_HLobbyDetails h, const EOS_LobbyDetails_CopyAttributeByIndexOptions* o,
                                     EOS_Lobby_Attribute** out) {
    FakeDetails d;
    if (!g.fakes.lookup(h, &d)) return g.outer.copyAttribute(h, o, out);
    if (!o || !out || o->AttrIndex >= d.attributes.size()) return EOS_InvalidParameters;
    *out = g.fakes.copyAttribute(d.attributes[o->AttrIndex]);
    return EOS_Success;
}

// Whom the game has in our room that Epic's copy `h` of it does not list (it lists `epicCount`): members
// in the room by their direct link only. The game reads the room's members from such a copy when it is told
// that one joined, so they follow Epic's. Empty for every other lobby, and while Epic lists everyone.
std::vector<std::string> extraMembers(EOS_HLobbyDetails h, uint32_t epicCount) {
    std::string room;
    std::vector<std::string> members;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active()) return {};
        room = g.viewRoom;
        members = g.view.members();
    }
    const Api& a = g.api;
    EOS_LobbyDetails_CopyInfoOptions io{1};
    EOS_LobbyDetails_Info* info = nullptr;
    if (!a.copyInfo || !a.releaseInfo || a.copyInfo(h, &io, &info) != EOS_Success || !info) return {};
    const bool ours = info->LobbyId && room == info->LobbyId;
    a.releaseInfo(info);
    if (!ours) return {};
    std::set<std::string> listed;
    for (uint32_t i = 0; i < epicCount; ++i) {
        EOS_LobbyDetails_GetMemberByIndexOptions mo{1, i};
        listed.insert(idString(g.outer.memberByIndex(h, &mo)));
    }
    std::vector<std::string> extra;
    for (const std::string& m : members)
        if (!listed.count(m)) extra.push_back(m);
    return extra;
}

// Epic's copy `h` of the room we are in while we follow its host's slots (or wait for a new host's): the members its host's game has that our
// game was told of, in slot order - the members the game may take now (room_view.h; one Epic lists that the host's
// game does not have yet comes once it has, and is told then). False for every other lobby, and while we do not
// follow the host's slots.
bool slottedMembers(EOS_HLobbyDetails h, std::vector<std::string>* out) {
    std::string room;
    {
        std::lock_guard<std::mutex> lock(g.viewMutex);
        if (!g.view.active()) return false;
        const bool awaiting = g.view.awaitingHost();
        if (!g.view.slotted() && !awaiting) return false;
        room = g.viewRoom;
        out->clear();
        if (awaiting) {
            // A new host whose slots are not here yet: the members our game was told of, no one Epic lists besides
            // (the game reads its members whenever any status reaches it, and would take that one in the first empty
            // slot).
            *out = g.view.members();
        } else {
            for (const std::string& m : g.view.hostMembers())
                if (g.view.has(m)) out->push_back(m);
            // One our game took before we followed the host's slots, which the host's game does not have (yet): it stays.
            for (const std::string& m : g.view.members())
                if (g.view.hostSlot(m) < 0) out->push_back(m);
        }
    }
    const Api& a = g.api;
    EOS_LobbyDetails_CopyInfoOptions io{1};
    EOS_LobbyDetails_Info* info = nullptr;
    if (!a.copyInfo || !a.releaseInfo || a.copyInfo(h, &io, &info) != EOS_Success || !info) return false;
    const bool ours = info->LobbyId && room == info->LobbyId;
    a.releaseInfo(info);
    return ours;
}

uint32_t hookDetailsMemberCount(EOS_HLobbyDetails h, const EOS_LobbyDetails_GetMemberCountOptions* o) {
    FakeDetails d;
    if (g.fakes.lookup(h, &d)) return static_cast<uint32_t>(d.members.size());
    std::vector<std::string> slotted;
    if (!g_shutdown && slottedMembers(h, &slotted)) return static_cast<uint32_t>(slotted.size());
    const uint32_t count = g.outer.memberCount(h, o);
    return g_shutdown ? count : count + static_cast<uint32_t>(extraMembers(h, count).size());
}

EOS_ProductUserId hookDetailsMemberByIndex(EOS_HLobbyDetails h, const EOS_LobbyDetails_GetMemberByIndexOptions* o) {
    FakeDetails d;
    if (g.fakes.lookup(h, &d))
        return o && o->MemberIndex < d.members.size() ? idHandle(d.members[o->MemberIndex]) : nullptr;
    if (g_shutdown || !o) return g.outer.memberByIndex(h, o);
    std::vector<std::string> slotted;
    if (slottedMembers(h, &slotted)) return o->MemberIndex < slotted.size() ? idHandle(slotted[o->MemberIndex]) : nullptr;
    EOS_LobbyDetails_GetMemberCountOptions co{1};
    const uint32_t count = g.outer.memberCount(h, &co);
    if (o->MemberIndex < count) return g.outer.memberByIndex(h, o);
    const std::vector<std::string> extra = extraMembers(h, count);
    const uint32_t i = o->MemberIndex - count;
    return i < extra.size() ? idHandle(extra[i]) : g.outer.memberByIndex(h, o);
}

EOS_ProductUserId hookDetailsOwner(EOS_HLobbyDetails h, const EOS_LobbyDetails_GetLobbyOwnerOptions* o) {
    FakeDetails d;
    return g.fakes.lookup(h, &d) ? idHandle(d.owner) : g.outer.owner(h, o);
}

EOS_EResult copyInfoOf(PFN_EOS_LobbyDetails_CopyInfo next, EOS_HLobbyDetails h, const EOS_LobbyDetails_CopyInfoOptions* o,
                       EOS_LobbyDetails_Info** out) {
    FakeDetails d;
    if (!g.fakes.lookup(h, &d)) return next ? next(h, o, out) : EOS_NotFound;
    if (!out) return EOS_InvalidParameters;
    *out = g.fakes.copyInfo(d, idHandle(d.owner));
    return EOS_Success;
}

EOS_EResult hookDetailsCopyInfo(EOS_HLobbyDetails h, const EOS_LobbyDetails_CopyInfoOptions* o,
                                EOS_LobbyDetails_Info** out) {
    return copyInfoOf(g.outer.copyInfo, h, o, out);
}

void hookDetailsRelease(EOS_HLobbyDetails h) {
    if (!g.fakes.release(h)) g.outer.releaseDetails(h);
}

void hookInfoRelease(EOS_LobbyDetails_Info* info) {
    if (!g.fakes.releaseInfo(info)) g.outer.releaseInfo(info);
}

void hookAttributeRelease(EOS_Lobby_Attribute* attribute) {
    if (!g.fakes.releaseAttribute(attribute)) g.outer.releaseAttribute(attribute);
}

template <typename T>
void resolve(HMODULE eos, const char* name, T& out) {
    out = reinterpret_cast<T>(GetProcAddress(eos, name));
    if (!out) logf("EOS export %s not found; the related feature is disabled", name);
}

template <typename T>
bool hook(HMODULE game, const char* name, T replacement, T& original) {
    bool ok = patchImport(game, kEosDll, name, reinterpret_cast<void*>(replacement), reinterpret_cast<void**>(&original));
    if (!ok) logf("EOS hook %s not installed (not imported by EDF.dll)", name);
    return ok;
}

}  // namespace

void eosHooksShutdown() { g_shutdown = true; }

bool installVirtualRoomHooks(HMODULE game) {
    const Api& a = g.api;
    // Needs the direct link (a Mode=host/join transport, or AutoJoin: the virtual room dials a link of its own), the
    // lobby tracking and the tick that completes our answers (installEosHooks), and what a room snapshot reads.
    // Players on the defaults (Mode=off, AutoJoin=1) have no base transport: they come back into a room, or into one
    // whose Epic lobby is full, the same way.
    if (!(g.baseNet.load() || g.autoJoinOn) || !g.lobbyTracked || !g.ticking || !a.copyDetails || !a.attributeCount ||
        !a.copyAttribute || !a.copyInfo || !a.releaseInfo || !a.releaseAttribute || !a.releaseDetails) {
        logf("REJOIN unavailable: needs the direct link, the EOS tick and EOS's lobby functions");
        return false;
    }
    Outer& o = g.outer;
    // The handle functions first: by the time a search can hand out one of our handles, every function the
    // game reads or releases it with must already know it.
    bool ok = hook(game, "EOS_LobbyDetails_Release", hookDetailsRelease, o.releaseDetails) &&
              hook(game, "EOS_LobbyDetails_Info_Release", hookInfoRelease, o.releaseInfo) &&
              hook(game, "EOS_Lobby_Attribute_Release", hookAttributeRelease, o.releaseAttribute) &&
              hook(game, "EOS_LobbyDetails_GetAttributeCount", hookDetailsAttributeCount, o.attributeCount) &&
              hook(game, "EOS_LobbyDetails_CopyAttributeByIndex", hookDetailsCopyAttribute, o.copyAttribute) &&
              hook(game, "EOS_LobbyDetails_GetMemberCount", hookDetailsMemberCount, o.memberCount) &&
              hook(game, "EOS_LobbyDetails_GetMemberByIndex", hookDetailsMemberByIndex, o.memberByIndex) &&
              hook(game, "EOS_LobbyDetails_GetLobbyOwner", hookDetailsOwner, o.owner) &&
              hook(game, "EOS_LobbyDetails_CopyInfo", hookDetailsCopyInfo, o.copyInfo) &&
              hook(game, "EOS_Lobby_CopyLobbyDetailsHandle", hookCopyDetails, o.copyDetails) &&
              hook(game, "EOS_Lobby_LeaveLobby", hookVirtualLeave, o.leave) &&
              hook(game, "EOS_Lobby_DestroyLobby", hookVirtualDestroy, o.destroy) &&
              hook(game, "EOS_Lobby_JoinLobby", hookVirtualJoin, o.join);
    // Only with all of the above may a search hand out our handle.
    ok = ok && hook(game, "EOS_LobbySearch_Release", hookSearchRelease, o.releaseSearch) &&
         hook(game, "EOS_LobbySearch_SetLobbyId", hookSearchSetLobbyId, o.setLobbyId) &&
         hook(game, "EOS_LobbySearch_GetSearchResultCount", hookSearchResultCount, o.resultCount) &&
         hook(game, "EOS_LobbySearch_CopySearchResultByIndex", hookSearchCopyResult, o.copyResult) &&
         hook(game, "EOS_LobbySearch_Find", hookSearchFind, o.find);
    logf("REJOIN %s", ok ? "on: the room we were last in is offered in the room list while Epic cannot list it, "
                           "and entered over the direct link to its host"
                         : "UNAVAILABLE (EDF.dll import table not as expected)");
    return ok;
}

EOS_EResult lobbyDetailsCopyInfo(EOS_HLobbyDetails details, const EOS_LobbyDetails_CopyInfoOptions* options,
                                 EOS_LobbyDetails_Info** out) {
    return copyInfoOf(g.api.copyInfo, details, options, out);
}

void lobbyDetailsInfoRelease(EOS_LobbyDetails_Info* info) {
    if (!g.fakes.releaseInfo(info) && g.api.releaseInfo) g.api.releaseInfo(info);
}

bool directMemberReadsSplitSync(const void* remote) {
    std::shared_ptr<DirectNet> net = g.net.load();
    if (!net || !remote || g_shutdown) return false;
    const std::string id = idString(static_cast<EOS_ProductUserId>(const_cast<void*>(remote)));
    return !id.empty() && net->canRoute(id);
}

void setAdvertisedAddress(const std::string& address) { g.marker.setAddress(address); }

bool installEosHooks(HMODULE game, HMODULE eos, const Config& config, DirectNet* net) {
    g.config = config;
    g.net.store(nullptr);  // enabled below only when every transport hook is in place
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionEstablished", g.api.addEstablished);
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionInterrupted", g.api.addInterrupted);
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionClosed", g.api.addClosed);
    resolve(eos, "EOS_P2P_AddNotifyIncomingPacketQueueFull", g.api.addQueueFull);
    resolve(eos, "EOS_P2P_QueryNATType", g.api.queryNat);
    resolve(eos, "EOS_P2P_SetRelayControl", g.api.setRelay);
    resolve(eos, "EOS_P2P_SetPortRange", g.api.setPortRange);
    resolve(eos, "EOS_P2P_SetPacketQueueSize", g.api.setQueueSize);
    resolve(eos, "EOS_P2P_GetPacketQueueInfo", g.api.getQueueInfo);
    resolve(eos, "EOS_ProductUserId_ToString", g.api.idToString);
    resolve(eos, "EOS_ProductUserId_FromString", g.api.idFromString);
    resolve(eos, "EOS_EResult_ToString", g.api.resultToString);
    resolve(eos, "EOS_EResult_IsOperationComplete", g.api.isComplete);
    resolve(eos, "EOS_P2P_AcceptConnection", g.api.accept);
    resolve(eos, "EOS_Lobby_PromoteMember", g.api.promote);
    resolve(eos, "EOS_Lobby_CopyLobbyDetailsHandle", g.api.copyDetails);
    resolve(eos, "EOS_LobbyDetails_GetAttributeCount", g.api.attributeCount);
    resolve(eos, "EOS_LobbyDetails_CopyAttributeByIndex", g.api.copyAttribute);
    resolve(eos, "EOS_LobbyDetails_CopyInfo", g.api.copyInfo);
    resolve(eos, "EOS_LobbyDetails_Info_Release", g.api.releaseInfo);
    resolve(eos, "EOS_Lobby_Attribute_Release", g.api.releaseAttribute);
    resolve(eos, "EOS_LobbyDetails_Release", g.api.releaseDetails);
    g.eos = eos;

    // Every player publishes its direct-link identity with its plugin marker; hosts check hellos against it.
    if (auto identity = processIdentity())
        g.marker.setIdentity(identity->commitment());
    else
        logf("DIRECT cannot create our direct-link identity (Windows crypto failed): direct links need it, "
             "the game stays on EOS");

    // Created before the hooks that read it go live (the game may already be ticking). If the hooks it
    // needs cannot be installed it just stays empty: only the connection-closed wrapper ever holds.
    if (config.hold != Config::Hold::Off) {
        g.hold = std::make_unique<DisconnectHold>(
            DisconnectHold::Options{config.graceMs, 2000, config.hold == Config::Hold::All});
        g.lobbyHold = std::make_unique<LobbyStatusHold>(config.graceMs);
    }

    // Diagnostics and resilience are optional: failing here must not disable the rest.
    hook(game, "EOS_P2P_CloseConnection", hookCloseConnection, g.api.gameClose);
    hook(game, "EOS_P2P_CloseConnections", hookCloseConnections, g.api.gameCloseAll);
    bool lobby = hook(game, "EOS_Lobby_AddNotifyLobbyMemberStatusReceived", hookAddNotifyMemberStatus,
                      g.api.gameAddMemberStatus);
    bool tick = hook(game, "EOS_Platform_Tick", hookPlatformTick, g.api.tick);
    g.ticking = tick;
    if (!tick) noteGameRunning();  // an update on trial cannot wait for a tick it will never see
    // Plugin detection: publish our marker on entering a lobby, read the other members' markers.
    g.markerReady = g.marker.init(eos) && hook(game, "EOS_Lobby_CreateLobby", hookCreateLobby, g.api.gameCreateLobby) &&
                    hook(game, "EOS_Lobby_JoinLobby", hookJoinLobby, g.api.gameJoinLobby);
    hook(game, "EOS_Lobby_AddNotifyLobbyMemberUpdateReceived", hookAddNotifyMemberUpdate, g.api.gameAddMemberUpdate);
    bool leave = hook(game, "EOS_Lobby_LeaveLobby", hookLeaveLobby, g.api.gameLeaveLobby);
    bool destroy = hook(game, "EOS_Lobby_DestroyLobby", hookDestroyLobby, g.api.gameDestroyLobby);
    hook(game, "EOS_Lobby_KickMember", hookKickMember, g.api.gameKick);
    hook(game, "EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived", hookRemoveNotifyMemberStatus,
         g.api.gameRemoveMemberStatus);
    hook(game, "EOS_Lobby_UpdateLobby", hookUpdateLobby, g.api.gameUpdateLobby);
    hook(game, "EOS_LobbyDetails_GetLobbyOwner", hookGetLobbyOwner, g.api.gameGetOwner);
    logf("LOBBY plugin detection %s", g.markerReady ? "enabled" : "UNAVAILABLE (only direct-link players can be held)");
    if (!g.markerReady && config.direct.mode == Mode::Host)
        logf("DIRECT without the lobby functions we cannot check who connects: nobody can connect to us directly");
    if (config.hold != Config::Hold::Off) {
        // Holding needs all of: our closed wrapper, its unregister hook, the tick to expire events,
        // AcceptConnection to reconnect, and lobby status to release players who really left.
        bool held = tick && lobby && g.api.accept && g.api.gameClose &&
                    hook(game, "EOS_P2P_RemoveNotifyPeerConnectionClosed", hookRemoveNotifyClosed,
                         g.api.gameRemoveClosed) &&
                    hook(game, "EOS_P2P_AddNotifyPeerConnectionClosed", hookAddNotifyClosed, g.api.gameAddClosed);
        logf("RESILIENCE disconnect hold %s (mode %s, grace %u s)", held ? "enabled" : "UNAVAILABLE",
             config.hold == Config::Hold::All ? "all" : "auto (players running the plugin)", config.graceMs / 1000);
    }

    bool ok = hook(game, "EOS_Platform_GetP2PInterface", hookGetP2PInterface, g.api.getP2P) &&
              hook(game, "EOS_P2P_SendPacket", hookSendPacket, g.api.send) &&
              hook(game, "EOS_P2P_ReceivePacket", hookReceivePacket, g.api.receive);
    // A half-hooked transport would send direct packets that the game can never receive. plugin.cpp
    // owns `net` and never frees it once the hooks are in: the shared pointers do not own it.
    std::shared_ptr<DirectNet> base = ok && net ? std::shared_ptr<DirectNet>(net, [](DirectNet*) {}) : nullptr;
    g.lobbyTracked = g.markerReady && lobby && leave && destroy;
    if (base && g.lobbyTracked) base->setActive(false);  // opened on entering a room
    g.baseNet.store(base);
    g.net.store(base);
    // The lobby hooks are already in: a room entered meanwhile found no transport to open.
    if (base && g.lobbyTracked && g.marker.inLobby()) base->setActive(true);
    g.autoJoinOn = ok && g.markerReady && config.autoJoin && config.direct.mode != Mode::Join;
    if (ok && config.autoJoin && config.direct.mode != Mode::Join)
        logf("DIRECT auto-connect %s", g.autoJoinOn ? "on: in other players' rooms we connect directly to a host "
                                                    "that advertises its address"
                                                  : "UNAVAILABLE (lobby functions missing)");
    logf("EOS hooks %s", ok ? "installed" : "FAILED (EDF.dll import table not as expected), direct link disabled");
    return ok;
}


// --- netcode.h ---

void setNetcodeOptions(const NetcodeOptions& options) {
    {
        std::lock_guard<std::mutex> lock(g.netcodeMutex);
        g.netcode = options;
        if (g.netcode.statsIntervalMs < 1000) g.netcode.statsIntervalMs = 1000;
    }
    for (std::shared_ptr<DirectNet> net : {g.baseNet.load(), g.net.load()})
        if (net) net->setShedState(options.shedState);
}

// A direct transport made later (AutoJoin, the virtual room) starts with the netcode options; mesh comes on once the
// whole room runs it (meshTick).
DirectOptions withNetcode(DirectOptions o) {
    const NetcodeOptions n = netcodeOptions();
    o.netProtocol = g.netProtocol;
    o.netCaps = g.netCaps;
    o.refuseOtherProtocols = g.refuseOtherProtocols;
    o.shedState = n.shedState;
    o.mesh = false;
    return o;
}

// Joiners link to each other only while every member runs the mesh ([Netcode] Mesh and NetFeature::Mesh).
void meshTick(uint64_t now) {
    static uint64_t lastMs = 0;
    if (now - lastMs < 500) return;
    lastMs = now;
    const bool on = netcodeOptions().mesh && roomCap(kCapMesh);
    for (std::shared_ptr<DirectNet> net : {g.baseNet.load(), g.net.load()})
        if (net && net->mesh() != on) net->setMesh(on);
}

NetcodeOptions netcodeOptions() {
    std::lock_guard<std::mutex> lock(g.netcodeMutex);
    return g.netcode;
}

void setRoomCapQuery(RoomCapQuery query) { g.roomCaps = query; }
// The host relays joiners' state too: the same filter decides there (observer = receiver, subject = sender).
bool relayFilter(const std::string& observer, const std::string& subject, uint32_t bytes, uint32_t budget, uint64_t nowMs) {
    StateSendFilter filter = g.stateFilter.load();
    return !filter || filter(observer, subject, bytes, budget, nowMs);
}

void setStateSendFilter(StateSendFilter filter) {
    g.stateFilter = filter;
    for (std::shared_ptr<DirectNet> net : {g.baseNet.load(), g.net.load()})
        if (net) net->setRelayStateFilter(filter ? &relayFilter : nullptr);
}

void setRoomCapacitySource(RoomCapacitySource source) { g.roomCapacity = source; }

void setGameSlotsSource(GameSlotsSource source) { g.gameSlots = source; }

int hostSlotOf(const std::string& member) {
    std::lock_guard<std::mutex> lock(g.viewMutex);
    return g.view.active() && g.view.slotted() ? g.view.hostSlot(member) : -1;
}

uint64_t bulkUndelivered() { return g.bulkLost.load(); }

uint64_t bulkIncoming(const std::string& src, uint16_t tag, size_t* total) {
    std::lock_guard<std::mutex> lock(g.fragmentMutex);
    return g.reassembler.pending(src, kFragmentBulk, tag, GetTickCount64(), total);
}

bool hostAdvertisement(std::string& address, std::string& identity) {
    if (g.config.direct.mode != Mode::Host) return false;
    address = g.marker.ownAddress();
    identity = g.marker.ownIdentity();
    return !address.empty() && !identity.empty();
}

void setNetcodeIdentity(uint32_t protocol, uint32_t caps, bool refuseOthers) {
    g.netProtocol = protocol;
    g.netCaps = caps;
    g.refuseOtherProtocols = refuseOthers;
    for (std::shared_ptr<DirectNet> net : {g.baseNet.load(), g.net.load()})
        if (net) net->setNetcode(protocol, caps, refuseOthers);
}

std::map<std::string, std::pair<uint32_t, uint32_t>> directMemberNetcode() {
    std::map<std::string, std::pair<uint32_t, uint32_t>> out;
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    if (!base || g.config.direct.mode != Mode::Host) return out;
    for (const auto& [id, n] : base->clientNetcode()) out[id] = {n.protocol, n.caps};
    return out;
}

void setTestLoopbackHosts(bool on) { g.testLoopbackHosts = on; }
void setTestBulkFragmentsSent(uint32_t count) { g.testBulkFragments = count; }
void setBulkHandler(BulkHandler handler) { g.bulkHandler = handler; }

void setTestPeerBlock(uint32_t afterMs, uint32_t forMs) {
    const uint64_t length = forMs == UINT32_MAX ? UINT64_MAX : forMs;
    if (std::shared_ptr<DirectNet> base = g.baseNet.load()) base->setTestBlockPeers(afterMs, length);
    if (std::shared_ptr<DirectNet> net = g.net.load()) net->setTestBlockPeers(afterMs, length);
}


uint32_t linkBudgetBytesPerSec(const std::string& peer) {
    if (peer.empty()) return 0;
    std::shared_ptr<DirectNet> net = g.net.load();
    if (net && net->canRoute(peer)) {
        if (uint32_t budget = net->linkBudget(peer)) return budget;
    }
    // EOS only: nobody measures Epic's path. The game's own budget for its routine state (about 320 kbps).
    return RateController::kMinRate;
}

// Tells `member` its bulk message `id` arrived: over the direct link when it reaches it, else EOS (reliably: a lost
// acknowledgement only costs a resend, but a resend of 140 KiB is worth avoiding).
void ackBulk(const std::string& member, uint64_t id) {
    const std::vector<uint8_t> ack = fragmentAck(id);
    std::shared_ptr<DirectNet> net = g.net.load();
    if (net && net->send(member, "", kFragmentChannel, EOS_PR_ReliableOrdered, ack.data(), ack.size())) return;
    std::string socketName;
    EOS_ProductUserId local = nullptr;
    {
        std::lock_guard<std::mutex> lock(g.lastSendMutex);
        socketName = g.lastSocket;
        local = g.lastLocal;
    }
    EOS_HP2P p2p = g.p2p;
    EOS_ProductUserId remote = idHandle(member);
    if (!p2p || !local || !remote) return;
    EOS_P2P_SocketId socket{1, {}};
    strncpy_s(socket.SocketName, socketName.c_str(), _TRUNCATE);
    EOS_P2P_SendPacketOptions o{};
    o.ApiVersion = 3;
    o.LocalUserId = local;
    o.RemoteUserId = remote;
    o.SocketId = &socket;
    o.Channel = kFragmentChannel;
    o.DataLengthBytes = static_cast<uint32_t>(ack.size());
    o.Data = ack.data();
    o.Reliability = EOS_PR_ReliableOrdered;
    o.bAllowDelayedDelivery = 1;
    g.api.send(p2p, &o);
}

// Bulk messages not acknowledged in time go again (the same id: the receiver takes it once), with twice the wait;
// after kBulkTries the loss is logged and counted (sendBulk's caller learns it from bulkUndelivered).
constexpr uint32_t kBulkTries = 5;
void bulkTick(uint64_t now) {
    std::vector<State::PendingBulk> due;
    {
        std::lock_guard<std::mutex> lock(g.bulkMutex);
        for (auto& b : g.bulks)
            if (now - b.sentMs >= b.waitMs) due.push_back(b);
    }
    for (const State::PendingBulk& b : due) {
        bool lost = false;
        {
            std::lock_guard<std::mutex> lock(g.bulkMutex);
            for (auto it = g.bulks.begin(); it != g.bulks.end(); ++it) {
                if (it->member != b.member || it->id != b.id) continue;
                if (it->tries >= kBulkTries) {
                    g.bulks.erase(it);
                    lost = true;
                } else {
                    ++it->tries;
                    it->sentMs = now;
                    it->waitMs *= 2;
                }
                break;
            }
        }
        if (lost) {
            ++g.bulkLost;
            logf("NETCODE a bulk message (tag %u, %zu bytes) to %s was not acknowledged after %u tries: NOT delivered", b.tag,
                 b.data.size(), shortId(b.member).c_str(), kBulkTries);
            continue;
        }
        EOS_P2P_SocketId socket{1, {}};
        strncpy_s(socket.SocketName, b.socket.c_str(), _TRUNCATE);
        EOS_P2P_SendPacketOptions o{};
        o.ApiVersion = 3;
        o.LocalUserId = b.local;
        o.RemoteUserId = b.remote;
        o.SocketId = &socket;
        o.bAllowDelayedDelivery = 1;
        ++g.bulkResent;
        logRateLimited("bulk-resend", 5000, "NETCODE resending a bulk message (tag %u, %zu bytes) to %s: not acknowledged yet",
                       b.tag, b.data.size(), shortId(b.member).c_str());
        sendFragments(b.p2p, o, b.member, kFragmentBulk, b.tag, b.data.data(), b.data.size(), b.id);
    }
}

bool sendBulk(const std::string& remote, uint16_t tag, const uint8_t* data, size_t size) {
    if (g_shutdown || remote.empty() || !data || !size || size > kMaxBulkBytes || !netcodeOptions().fragments ||
        !roomCap(kCapFragments))
        return false;
    EOS_P2P_SendPacketOptions o{};
    EOS_P2P_SocketId socket{1, {}};
    {
        std::lock_guard<std::mutex> lock(g.lastSendMutex);
        if (!g.lastLocal || g.lastSocket.empty()) return false;  // the game has not opened its socket yet
        o.LocalUserId = g.lastLocal;
        strncpy_s(socket.SocketName, g.lastSocket.c_str(), _TRUNCATE);
    }
    EOS_HP2P p2p = g.p2p;
    o.ApiVersion = 3;
    o.RemoteUserId = idHandle(remote);
    o.SocketId = &socket;
    o.bAllowDelayedDelivery = 1;
    if (!p2p || !o.RemoteUserId) return false;
    const uint64_t id = g.fragmentEpoch | ++g.fragmentIds;
    if (sendFragments(p2p, o, remote, kFragmentBulk, tag, data, size, id) != EOS_Success) return false;
    State::PendingBulk pending;
    pending.member = remote;
    pending.p2p = p2p;
    pending.local = o.LocalUserId;
    pending.remote = o.RemoteUserId;
    pending.socket = socket.SocketName;
    pending.id = id;
    pending.tag = tag;
    pending.data.assign(data, data + size);
    pending.sentMs = GetTickCount64();
    // Long enough for the whole message at the game's own budget (40 KB/s), a second at least.
    pending.waitMs = 1000 + size * 1000 / RateController::kMinRate;
    std::lock_guard<std::mutex> lock(g.bulkMutex);
    g.bulks.push_back(std::move(pending));
    return true;
}
}  // namespace dn
