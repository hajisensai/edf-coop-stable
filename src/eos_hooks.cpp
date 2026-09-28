#include "eos_hooks.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "eos_min.h"
#include "hold.h"
#include "iat.h"
#include "log.h"

namespace dn {
namespace {

constexpr const char* kEosDll = "EOSSDK-Win64-Shipping.dll";
constexpr ULONGLONG kStatsIntervalMs = 60000;
constexpr uint64_t kMinQueueBytes = 64ull * 1024 * 1024;

using PFN_EOS_Platform_Tick = void (*)(EOS_HPlatform);
using PFN_EOS_P2P_RemoveNotify = void (*)(EOS_HP2P, EOS_NotificationId);

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
    PFN_EOS_P2P_AcceptConnection accept = nullptr;
    // Original targets of the game's own imports we wrap (may point at another mod's hook).
    PFN_EOS_P2P_AddNotifyClosed gameAddClosed = nullptr;
    PFN_EOS_P2P_RemoveNotify gameRemoveClosed = nullptr;
    PFN_EOS_P2P_CloseConnection gameClose = nullptr;
    PFN_EOS_P2P_CloseConnections gameCloseAll = nullptr;
    PFN_EOS_Lobby_AddNotifyMemberStatus gameAddMemberStatus = nullptr;
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
};

struct State {
    Api api;
    Config config;
    DirectNet* net = nullptr;
    std::unique_ptr<DisconnectHold> hold;
    std::mutex handlerMutex;
    std::vector<ClosedHandler*> closedHandlers;
    EOS_HP2P p2p = nullptr;
    std::atomic<EOS_HP2P> configuredHandle{nullptr};
    std::atomic<EOS_ProductUserId> localUser{nullptr};
    std::mutex idMutex;
    std::unordered_map<std::string, EOS_ProductUserId> idCache;
    std::atomic<uint64_t> directOut{0}, directIn{0}, eosOut{0}, eosIn{0}, eosSendFail{0}, eosUpgraded{0};
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

EOS_ProductUserId idHandle(const std::string& s) {
    std::lock_guard<std::mutex> lock(g.idMutex);
    auto it = g.idCache.find(s);
    if (it != g.idCache.end()) return it->second;
    EOS_ProductUserId id = g.api.idFromString ? g.api.idFromString(s.c_str()) : nullptr;
    if (id) g.idCache.emplace(s, id);
    return id;
}

// A held peer stays hidden only while it demonstrably still plays: the direct link is up AND game
// data from it arrived recently. A peer whose plugin keeps pinging after its game stopped (left,
// crashed to menu) must not stay held, or everyone would wait for it at the next sync point.
constexpr uint64_t kDirectDataFreshMs = 10000;
bool directAlive(const std::string& remote) {
    return g.net && !remote.empty() && g.net->canRoute(remote) && g.net->heardFromRecently(remote, kDirectDataFreshMs);
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
    if (!g.api.accept || !g.p2p || g_shutdown) return;
    EOS_P2P_PeerConnectionOptions o{1, h->info.LocalUserId, h->info.RemoteUserId,
                                    h->hasSocket ? &h->socket : nullptr};
    EOS_EResult r = g.api.accept(g.p2p, &o);
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
        if (g.hold->offer(remote, i->Reason, direct, GetTickCount64(), [held] { forwardClose(held); },
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

void memberStatusWrapper(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* i) {
    if (!g_shutdown) {
        static const char* names[] = {"JOINED", "LEFT", "DISCONNECTED", "KICKED", "PROMOTED", "CLOSED"};
        int32_t s = i->CurrentStatus;
        logf("LOBBY member %s -> %s%s", shortId(idString(i->TargetUserId)).c_str(),
             s >= 0 && s < 6 ? names[s] : "?",
             s == 2 ? " (lost its connection to Epic's lobby service, not the P2P link)" : "");
        // The lobby is authoritative about who is still in the room. A held disconnect of someone who
        // left must reach the game now; a live direct link would otherwise keep it held forever.
        // Delivered before the lobby event, the same order the game sees without the plugin.
        size_t released = 0;
        if (g.hold && (s == 1 || s == 2 || s == 3)) released = g.hold->release(idString(i->TargetUserId));
        if (g.hold && s == 5) released = g.hold->releaseAll();
        if (released)
            logf("RESILIENCE handed %zu held disconnect(s) to the game because of the lobby change", released);
    }
    auto* handler = static_cast<MemberStatusHandler*>(i->ClientData);
    EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo copy = *i;
    copy.ClientData = handler->clientData;
    handler->callback(&copy);
}

EOS_NotificationId hookAddNotifyMemberStatus(EOS_HLobby h, const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions* o,
                                             void* clientData, EOS_Lobby_OnLobbyMemberStatusReceivedCallback cb) {
    if (!cb || g_shutdown) return g.api.gameAddMemberStatus(h, o, clientData, cb);
    auto* handler = new MemberStatusHandler{cb, clientData};  // never freed, same reason as ClosedHandler
    return g.api.gameAddMemberStatus(h, o, handler, memberStatusWrapper);
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
    g.p2p = h;
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
    if (g.net && !s.empty()) g.net->setLocalUser(s);
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
    if (!(dOut | dIn | eOut | eIn | fail)) return;
    logf("STATS last 60s: direct out=%llu in=%llu | EOS out=%llu (sent reliably %llu) in=%llu send-failures=%llu%s%s",
         static_cast<unsigned long long>(dOut), static_cast<unsigned long long>(dIn),
         static_cast<unsigned long long>(eOut), static_cast<unsigned long long>(upg), static_cast<unsigned long long>(eIn),
         static_cast<unsigned long long>(fail), g.net ? " | " : "", g.net ? g.net->statusLine().c_str() : "");
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
    if (g.hold && g.hold->heldCount()) {
        for (const auto& remote : g.hold->poll(GetTickCount64(), directAlive))
            logf("RESILIENCE %s did not come back within %u s; disconnect handed to the game",
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
    if (g.net && o && o->Data && o->DataLengthBytes <= EOS_P2P_MAX_PACKET_SIZE && !remote.empty() &&
        g.net->send(remote, socketName(o->SocketId), o->Channel, static_cast<uint8_t>(o->Reliability),
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
    if (g.net && o && outPeer && outData && outBytes) {
        const uint8_t* channel = o->ApiVersion >= 2 ? o->RequestedChannel : nullptr;
        Delivered d;
        if (g.net->pop(channel, o->MaxDataSizeBytes, d)) {
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

bool installEosHooks(HMODULE game, HMODULE eos, const Config& config, DirectNet* net) {
    g.config = config;
    g.net = nullptr;  // enabled below only when every transport hook is in place
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
    resolve(eos, "EOS_P2P_AcceptConnection", g.api.accept);

    // Diagnostics and resilience are optional: failing here must not disable the rest.
    hook(game, "EOS_P2P_CloseConnection", hookCloseConnection, g.api.gameClose);
    hook(game, "EOS_P2P_CloseConnections", hookCloseConnections, g.api.gameCloseAll);
    bool lobby = hook(game, "EOS_Lobby_AddNotifyLobbyMemberStatusReceived", hookAddNotifyMemberStatus,
                      g.api.gameAddMemberStatus);
    bool tick = hook(game, "EOS_Platform_Tick", hookPlatformTick, g.api.tick);
    if (config.hold != Config::Hold::Off) {
        // Holding needs all of: our closed wrapper, its unregister hook, the tick to expire events,
        // AcceptConnection to reconnect, and lobby status to release players who really left.
        bool held = tick && lobby && g.api.accept && g.api.gameClose &&
                    hook(game, "EOS_P2P_RemoveNotifyPeerConnectionClosed", hookRemoveNotifyClosed,
                         g.api.gameRemoveClosed) &&
                    hook(game, "EOS_P2P_AddNotifyPeerConnectionClosed", hookAddNotifyClosed, g.api.gameAddClosed);
        if (held)
            g.hold = std::make_unique<DisconnectHold>(
                DisconnectHold::Options{config.graceMs, 2000, config.hold == Config::Hold::All});
        logf("RESILIENCE disconnect hold %s (mode %s, grace %u s)", held ? "enabled" : "UNAVAILABLE",
             config.hold == Config::Hold::All ? "all" : "auto (direct-link members only)", config.graceMs / 1000);
    }

    bool ok = hook(game, "EOS_Platform_GetP2PInterface", hookGetP2PInterface, g.api.getP2P) &&
              hook(game, "EOS_P2P_SendPacket", hookSendPacket, g.api.send) &&
              hook(game, "EOS_P2P_ReceivePacket", hookReceivePacket, g.api.receive);
    // A half-hooked transport would send direct packets that the game can never receive.
    g.net = ok ? net : nullptr;
    logf("EOS hooks %s", ok ? "installed" : "FAILED (EDF.dll import table not as expected), direct link disabled");
    return ok;
}

}  // namespace dn
