#include "eos_hooks.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "auth.h"
#include "eos_min.h"
#include "hold.h"
#include "iat.h"
#include "lobby_marker.h"
#include "log.h"
#include "traffic.h"

namespace dn {
namespace {

constexpr const char* kEosDll = "EOSSDK-Win64-Shipping.dll";
constexpr ULONGLONG kStatsIntervalMs = 60000;
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
    LobbyMarker marker;
    std::atomic<bool> markerReady{false};
    std::atomic<bool> lobbyTracked{false};  // we see entering and leaving rooms: direct links follow the room
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
    ULONGLONG lastStatsMs = 0;
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

enum : int32_t { kJoined = 0, kLeft = 1, kDisconnected = 2, kKicked = 3, kPromoted = 4, kClosed = 5 };

// What a lobby status means for us, applied when the game gets to see it.
void applyLobbyStatus(const std::string& target, bool self, int32_t s) {
    bool gone = s == kLeft || s == kDisconnected || s == kKicked;
    // The lobby is authoritative about who is still in the room. A held disconnect of someone who
    // left must reach the game now; a live direct link would otherwise keep it held forever.
    // Delivered before the lobby event, the same order the game sees without the plugin.
    size_t released = 0;
    if (g.hold && gone) released = g.hold->release(target);
    if (g.hold && s == kClosed) released = g.hold->releaseAll();
    if (gone && !self) g.marker.memberGone(target);
    if (self && s == kPromoted) g.marker.promoted();
    if (!self && s == kJoined) g.marker.memberJoined();
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

void deliverLobbyStatus(const std::shared_ptr<LobbyStatusEvent>& e) {
    applyLobbyStatus(e->target, e->self, e->info.CurrentStatus);
    if (e->handler->removed) return;  // the game unregistered; its clientData may be freed
    e->info.LobbyId = e->lobbyId.c_str();
    e->handler->callback(&e->info);
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

    // Epic's lobby service dropping someone says nothing about the game when its traffic runs over
    // our direct link: while that link shows the member (or us) still playing, the game is not told.
    if (s == kDisconnected && g.lobbyHold && !e->target.empty() &&
        g.lobbyHold->offer(e->target, reachableDirectly(e->target), GetTickCount64(), [e] { deliverLobbyStatus(e); })) {
        logf("RESILIENCE %s lost Epic's lobby service but the direct link is up: hidden from the game",
             e->self ? "we" : shortId(e->target).c_str());
        return;
    }
    if (g.lobbyHold && !e->target.empty() && g.lobbyHold->onStatus(e->target, s)) {
        logf("RESILIENCE %s back in Epic's lobby service; the game never saw the drop",
             e->self ? "we are" : (shortId(e->target) + " is").c_str());
        g.marker.memberJoined();  // a member coming back needs our attributes (and ours may be gone)
        return;
    }
    if (s == kClosed && g.lobbyHold) g.lobbyHold->releaseAll();
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

// Completion of the game's CreateLobby / JoinLobby: once we are in, publish our plugin marker.
void lobbyEnteredWrapper(const EOS_Lobby_LobbyIdCallbackInfo* i) {
    auto* call = static_cast<LobbyCall*>(i->ClientData);
    if (!g_shutdown && i->ResultCode == EOS_Success) {
        g.lobbyUser = call->localUser;
        g.marker.entered(call->lobby, i->LobbyId, call->localUser, call->owner);
        if (std::shared_ptr<DirectNet> base = g.baseNet.load()) base->setActive(true);
    }
    EOS_Lobby_LobbyIdCallbackInfo copy = *i;
    copy.ClientData = call->clientData;
    EOS_Lobby_OnLobbyIdCallback cb = call->callback;
    // EOS runs the callback again after a non-final result (EOS_OperationWillRetry): keep the call
    // until the final one, or the next run reads freed memory.
    if (!g.api.isComplete || g.api.isComplete(i->ResultCode)) delete call;
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
    g.marker.left();
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
void hookKickMember(EOS_HLobby h, const EOS_Lobby_KickMemberOptions* o, void* clientData, void* cb) {
    if (o && !g_shutdown) {
        std::string target = idString(o->TargetUserId);
        std::shared_ptr<DirectNet> net = g.net.load();
        logf("GAME kicks %s from the room (direct link %s, game data from it %s, P2P disconnect %s, lobby status %s)",
             shortId(target).c_str(), reachableDirectly(target) ? "up" : "down",
             net && net->heardFromRecently(target, kDirectDataFreshMs) ? "recent" : "none for 10 s",
             g.hold && g.hold->isHeld(target) ? "held by us" : "not held",
             g.lobbyHold && g.lobbyHold->isHeld(target) ? "hidden by us" : "normal");
        // The game gave up on this member: a disconnect we are hiding must reach it now, or the member
        // would stay in the game (EOS already dropped it, so the kick itself may produce no status).
        if (g.lobbyHold && g.lobbyHold->abandon(target))
            logf("RESILIENCE the game kicks %s: its hidden lobby disconnect goes to the game", shortId(target).c_str());
    }
    g.api.gameKick(h, o, clientData, cb);
}

// Diagnostics: when the game rewrites the room info, for correlating with members' attributes
// vanishing from other players' copies of the lobby.
void hookUpdateLobby(EOS_HLobby h, const EOS_Lobby_UpdateLobbyOptions* o, void* clientData, EOS_Lobby_OnLobbyIdCallback cb) {
    if (!g_shutdown) logRateLimited("game-update-lobby", 30000, "GAME updated the room info");
    g.api.gameUpdateLobby(h, o, clientData, cb);
}

void hookLeaveLobby(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!g_shutdown) leftLobby("left the room");
    g.api.gameLeaveLobby(h, o, clientData, cb);
}

void hookDestroyLobby(EOS_HLobby h, const void* o, void* clientData, void* cb) {
    if (!g_shutdown) leftLobby("closed the room");
    g.api.gameDestroyLobby(h, o, clientData, cb);
}

void startAutoJoinAttemptLocked(uint64_t now) {
    AutoJoin& a = g.autoJoin;
    DirectOptions o = g.config.direct;
    o.mode = Mode::Join;
    o.listenPort = 0;
    o.hostAddress = a.candidates[a.next++];
    o.advertisedHost = true;  // the room host chose it, not this player
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
    if (now - g.lastStatsMs < kStatsIntervalMs) return;
    bool first = g.lastStatsMs == 0;
    g.lastStatsMs = now;
    if (first) return;
    uint64_t dOut = g.directOut.exchange(0), dIn = g.directIn.exchange(0);
    uint64_t eOut = g.eosOut.exchange(0), eIn = g.eosIn.exchange(0), fail = g.eosSendFail.exchange(0);
    uint64_t upg = g.eosUpgraded.exchange(0);
    TrafficSummary t = g.gameOut.take();
    std::shared_ptr<DirectNet> net = g.net.load();
    WireTraffic w = net ? net->takeWireTraffic() : WireTraffic{};
    if (!(dOut | dIn | eOut | eIn | fail)) return;
    // kbps = bytes * 8 / 1000 / seconds
    auto kbps = [](uint64_t bytes, double seconds) { return static_cast<double>(bytes) * 8.0 / 1000.0 / seconds; };
    logf("TRAFFIC last 60s: game sends %.0f kbps avg, busiest second %.0f kbps, to %zu players, %.0f B/packet avg "
         "(largest %u), %.0f%% copies of the same data to another player | direct link up %.0f kbps down %.0f kbps, "
         "relayed for others %.0f kbps",
         kbps(t.bytes, 60.0), kbps(t.busiestSecondBytes, 1.0), t.peers,
         t.packets ? static_cast<double>(t.bytes) / static_cast<double>(t.packets) : 0.0, t.largestPacket,
         t.bytes ? 100.0 * static_cast<double>(t.copyBytes) / static_cast<double>(t.bytes) : 0.0, kbps(w.out, 60.0),
         kbps(w.in, 60.0), kbps(w.relayed, 60.0));
    logf("STATS last 60s: direct out=%llu in=%llu | EOS out=%llu (sent reliably %llu) in=%llu send-failures=%llu%s%s",
         static_cast<unsigned long long>(dOut), static_cast<unsigned long long>(dIn),
         static_cast<unsigned long long>(eOut), static_cast<unsigned long long>(upg), static_cast<unsigned long long>(eIn),
         static_cast<unsigned long long>(fail), net ? " | " : "", net ? net->statusLine().c_str() : "");
}

// A direct-link host lets a player in only as the room member whose published identity it proves
// (DirectNet::setMemberIdentities). The member list lives in EOS, which we only call from the tick.
constexpr uint64_t kIdentityRefreshMs = 500;
void refreshMemberIdentities(uint64_t now) {
    static uint64_t lastMs = 0;
    if (now - lastMs < kIdentityRefreshMs) return;
    lastMs = now;
    std::shared_ptr<DirectNet> base = g.baseNet.load();
    if (base && g.markerReady && g.config.direct.mode == Mode::Host)
        base->setMemberIdentities(g.marker.memberIdentities());
}

// Runs after every EOS_Platform_Tick, i.e. where EOS itself would deliver callbacks to the game.
void hookPlatformTick(EOS_HPlatform platform) {
    g.api.tick(platform);
    if (g_shutdown) return;
    static bool loggedThread = false;
    if (!loggedThread) {
        loggedThread = true;
        logf("EOS tick runs on thread %lu", GetCurrentThreadId());
    }
    maybeLogStats();
    g.marker.tick();
    refreshMemberIdentities(GetTickCount64());
    if (g.autoJoinOn) autoJoinTick(GetTickCount64());
    if (g.hold && g.hold->heldCount()) {
        for (const auto& remote : g.hold->poll(GetTickCount64(), directAlive))
            logf("RESILIENCE %s did not come back within %u s; disconnect handed to the game",
                 shortId(remote).c_str(), g.config.graceMs / 1000);
    }
    if (g.lobbyHold && g.lobbyHold->heldCount()) {
        for (const auto& remote : g.lobbyHold->poll(GetTickCount64(), reachableDirectly))
            logf("RESILIENCE %s: lobby disconnect handed to the game (direct link silent for %u s, or kicked)",
                 shortId(remote).c_str(), g.config.graceMs / 1000);
    }
}

EOS_HP2P hookGetP2PInterface(EOS_HPlatform platform) {
    EOS_HP2P h = g.api.getP2P(platform);
    if (!g_shutdown) configureHandle(h);
    return h;
}

EOS_EResult hookSendPacket(EOS_HP2P h, const EOS_P2P_SendPacketOptions* o) {
    if (g_shutdown) return g.api.send(h, o);
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    std::string remote = o ? idString(o->RemoteUserId) : std::string();
    if (o && o->Data)
        g.gameOut.record(remote, o->DataLengthBytes, TrafficMeter::hash(o->Data, o->DataLengthBytes), GetTickCount64());
    std::shared_ptr<DirectNet> net = g.net.load();
    if (net && o && o->Data && o->DataLengthBytes <= EOS_P2P_MAX_PACKET_SIZE && !remote.empty() &&
        net->send(remote, socketName(o->SocketId), o->Channel, static_cast<uint8_t>(o->Reliability),
                    static_cast<const uint8_t*>(o->Data), o->DataLengthBytes)) {
        ++g.directOut;
        return EOS_Success;
    }
    EOS_EResult r;
    bool held = g.hold && o && o->ApiVersion >= 3 && g.hold->isHeld(remote);
    bool upgrade = g.config.reliableGameTraffic && o && o->ApiVersion >= 3 && o->Reliability == EOS_PR_UnreliableUnordered;
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
        std::string key = "send-fail-" + std::to_string(r);
        logRateLimited(key.c_str(), 10000, "EOS SendPacket to %s failed: %s", peerLabel(o ? o->RemoteUserId : nullptr).c_str(),
                       resultName(r));
    }
    return r;
}

EOS_EResult hookReceivePacket(EOS_HP2P h, const EOS_P2P_ReceivePacketOptions* o, EOS_ProductUserId* outPeer,
                              EOS_P2P_SocketId* outSocket, uint8_t* outChannel, void* outData, uint32_t* outBytes) {
    if (g_shutdown) return g.api.receive(h, o, outPeer, outSocket, outChannel, outData, outBytes);
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    std::shared_ptr<DirectNet> net = g.net.load();
    if (net && o && outPeer && outData && outBytes) {
        const uint8_t* channel = o->ApiVersion >= 2 ? o->RequestedChannel : nullptr;
        Delivered d;
        if (net->pop(channel, o->MaxDataSizeBytes, d)) {
            EOS_ProductUserId peer = idHandle(d.src);
            if (peer) {
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
        }
    }
    EOS_EResult r = g.api.receive(h, o, outPeer, outSocket, outChannel, outData, outBytes);
    if (r == EOS_Success) ++g.eosIn;
    return r;
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

}  // namespace dn
