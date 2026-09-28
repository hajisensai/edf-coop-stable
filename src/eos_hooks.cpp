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

struct Api {
    PFN_EOS_Platform_GetP2PInterface getP2P = nullptr;  // original targets (IAT)
    PFN_EOS_P2P_SendPacket send = nullptr;
    PFN_EOS_P2P_ReceivePacket receive = nullptr;
    PFN_EOS_P2P_AddNotifyEstablished addEstablished = nullptr;  // resolved from the SDK
    PFN_EOS_P2P_AddNotifyInterrupted addInterrupted = nullptr;
    PFN_EOS_P2P_AddNotifyClosed addClosed = nullptr;
    PFN_EOS_P2P_AddNotifyQueueFull addQueueFull = nullptr;
    PFN_EOS_P2P_QueryNATType queryNat = nullptr;
    PFN_EOS_P2P_SetRelayControl setRelay = nullptr;
    PFN_EOS_P2P_SetPortRange setPortRange = nullptr;
    PFN_EOS_ProductUserId_ToString idToString = nullptr;
    PFN_EOS_ProductUserId_FromString idFromString = nullptr;
    PFN_EOS_EResult_ToString resultToString = nullptr;
    PFN_EOS_P2P_AcceptConnection accept = nullptr;
    // Original targets of the game's own imports we wrap (may point at another mod's hook).
    PFN_EOS_P2P_AddNotifyClosed gameAddClosed = nullptr;
    PFN_EOS_P2P_CloseConnection gameClose = nullptr;
    PFN_EOS_P2P_CloseConnections gameCloseAll = nullptr;
    PFN_EOS_Lobby_AddNotifyMemberStatus gameAddMemberStatus = nullptr;
};

// The game's connection-closed handler, which we sit in front of.
struct ClosedHandler {
    EOS_P2P_OnRemoteConnectionClosedCallback callback;
    void* clientData;
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
    std::vector<std::unique_ptr<ClosedHandler>> closedHandlers;  // stable addresses: used as ClientData
    std::vector<std::unique_ptr<MemberStatusHandler>> memberHandlers;
    EOS_HP2P p2p = nullptr;
    std::atomic<EOS_HP2P> configuredHandle{nullptr};
    std::atomic<EOS_ProductUserId> localUser{nullptr};
    std::mutex idMutex;
    std::unordered_map<std::string, EOS_ProductUserId> idCache;
    std::atomic<uint64_t> directOut{0}, directIn{0}, eosOut{0}, eosIn{0}, eosSendFail{0};
    ULONGLONG lastStatsMs = 0;
};

State g;

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

std::string peerLabel(EOS_ProductUserId id) {
    std::string s = idString(id);
    bool direct = g.net && !s.empty() && g.net->canRoute(s);
    return shortId(s) + (direct ? " (direct link)" : "");
}

std::string socketName(const EOS_P2P_SocketId* sock) {
    if (!sock) return {};
    return std::string(sock->SocketName, strnlen(sock->SocketName, EOS_P2P_SOCKETID_SOCKETNAME_SIZE));
}

void onEstablished(const EOS_P2P_OnPeerConnectionEstablishedInfo* i) {
    static const char* nets[] = {"no-connection", "DIRECT", "RELAYED via Epic"};
    int32_t n = i->NetworkType;
    logf("EOS connection %s with %s: %s", i->ConnectionType == 1 ? "RE-established" : "established",
         peerLabel(i->RemoteUserId).c_str(), n >= 0 && n < 3 ? nets[n] : "?");
    if (g.hold && g.hold->onEstablished(idString(i->RemoteUserId), GetTickCount64()) > 0)
        logf("RESILIENCE %s RECOVERED within the grace period; the game never saw the disconnect",
             shortId(idString(i->RemoteUserId)).c_str());
}

void onInterrupted(const EOS_P2P_OnPeerConnectionInterruptedInfo* i) {
    logf("EOS connection INTERRUPTED with %s (EOS is trying to recover it)", peerLabel(i->RemoteUserId).c_str());
}

void onClosed(const EOS_P2P_OnRemoteConnectionClosedInfo* i) {
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
    h->info.SocketId = h->hasSocket ? &h->socket : nullptr;
    h->handler->callback(&h->info);
}

void reacceptPeer(const std::shared_ptr<HeldClose>& h) {
    if (!g.api.accept || !g.p2p) return;
    EOS_P2P_PeerConnectionOptions o{1, h->info.LocalUserId, h->info.RemoteUserId,
                                    h->hasSocket ? &h->socket : nullptr};
    EOS_EResult r = g.api.accept(g.p2p, &o);
    logRateLimited("reaccept", 5000, "RESILIENCE asking EOS to reconnect %s: %s",
                   shortId(idString(h->info.RemoteUserId)).c_str(), resultName(r));
}

// Sits in front of the game's connection-closed handler (EOS calls this on the game thread).
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
    if (g.hold && !remote.empty() &&
        g.hold->offer(remote, i->Reason, GetTickCount64(), [held] { forwardClose(held); },
                      [held] { reacceptPeer(held); })) {
        logf("RESILIENCE holding back disconnect of %s (%s) for up to %u s while reconnecting",
             shortId(remote).c_str(), closedReasonName(i->Reason), g.config.graceMs / 1000);
        return;
    }
    forwardClose(held);
}

EOS_NotificationId hookAddNotifyClosed(EOS_HP2P h, const EOS_P2P_AddNotifyOptions* o, void* clientData,
                                       EOS_P2P_OnRemoteConnectionClosedCallback cb) {
    if (!cb) return g.api.gameAddClosed(h, o, clientData, cb);
    g.closedHandlers.push_back(std::make_unique<ClosedHandler>(ClosedHandler{cb, clientData}));
    return g.api.gameAddClosed(h, o, g.closedHandlers.back().get(), closedWrapper);
}

EOS_EResult hookCloseConnection(EOS_HP2P h, const EOS_P2P_PeerConnectionOptions* o) {
    if (o) {
        std::string remote = idString(o->RemoteUserId);
        size_t dropped = g.hold ? g.hold->onGameClosed(remote) : 0;
        logf("GAME closed its connection to %s%s", shortId(remote).c_str(),
             dropped ? " (was being held for reconnect; the game gave up on its own)" : "");
    }
    return g.api.gameClose(h, o);
}

EOS_EResult hookCloseConnections(EOS_HP2P h, const EOS_P2P_CloseConnectionsOptions* o) {
    size_t dropped = g.hold ? g.hold->onGameClosedAll() : 0;
    logf("GAME closed all connections (left the room)%s", dropped ? ", dropping held disconnects" : "");
    return g.api.gameCloseAll(h, o);
}

void memberStatusWrapper(const EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo* i) {
    static const char* names[] = {"JOINED", "LEFT", "DISCONNECTED", "KICKED", "PROMOTED", "CLOSED"};
    int32_t s = i->CurrentStatus;
    logf("LOBBY member %s -> %s%s", shortId(idString(i->TargetUserId)).c_str(), s >= 0 && s < 6 ? names[s] : "?",
         s == 2 ? " (lost connection to Epic's lobby service, not the P2P link)" : "");
    auto* handler = static_cast<MemberStatusHandler*>(i->ClientData);
    EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo copy = *i;
    copy.ClientData = handler->clientData;
    handler->callback(&copy);
}

EOS_NotificationId hookAddNotifyMemberStatus(EOS_HLobby h, const EOS_Lobby_AddNotifyLobbyMemberStatusReceivedOptions* o,
                                             void* clientData, EOS_Lobby_OnLobbyMemberStatusReceivedCallback cb) {
    if (!cb) return g.api.gameAddMemberStatus(h, o, clientData, cb);
    g.memberHandlers.push_back(std::make_unique<MemberStatusHandler>(MemberStatusHandler{cb, clientData}));
    return g.api.gameAddMemberStatus(h, o, g.memberHandlers.back().get(), memberStatusWrapper);
}

void onQueueFull(const EOS_P2P_OnIncomingPacketQueueFullInfo* i) {
    logRateLimited("queue-full", 5000,
                   "EOS incoming packet queue FULL (%llu/%llu bytes, channel %u): EOS drops packets now",
                   static_cast<unsigned long long>(i->PacketQueueCurrentSizeBytes),
                   static_cast<unsigned long long>(i->PacketQueueMaxSizeBytes), i->OverflowPacketChannel);
}

void onNatType(const EOS_P2P_OnQueryNATTypeCompleteInfo* i) {
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
    logf("WHOAMI local EOS ProductUserId %s", s.c_str());
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
    if (!(dOut | dIn | eOut | eIn | fail)) return;
    logf("STATS last 60s: direct out=%llu in=%llu | EOS out=%llu in=%llu send-failures=%llu%s%s",
         static_cast<unsigned long long>(dOut), static_cast<unsigned long long>(dIn),
         static_cast<unsigned long long>(eOut), static_cast<unsigned long long>(eIn),
         static_cast<unsigned long long>(fail), g.net ? " | " : "", g.net ? g.net->statusLine().c_str() : "");
}

EOS_HP2P hookGetP2PInterface(EOS_HPlatform platform) {
    EOS_HP2P h = g.api.getP2P(platform);
    configureHandle(h);
    return h;
}

EOS_EResult hookSendPacket(EOS_HP2P h, const EOS_P2P_SendPacketOptions* o) {
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    if (g.net && o && o->RemoteUserId && o->Data && o->DataLengthBytes <= EOS_P2P_MAX_PACKET_SIZE) {
        std::string remote = idString(o->RemoteUserId);
        if (!remote.empty() && g.net->send(remote, socketName(o->SocketId), o->Channel,
                                           static_cast<uint8_t>(o->Reliability),
                                           static_cast<const uint8_t*>(o->Data), o->DataLengthBytes)) {
            ++g.directOut;
            return EOS_Success;
        }
    }
    EOS_EResult r;
    if (g.hold && o && g.hold->isHeld(idString(o->RemoteUserId))) {
        // Connection is being rebuilt: let EOS queue the packet instead of discarding it.
        EOS_P2P_SendPacketOptions queued = *o;
        queued.bAllowDelayedDelivery = 1;
        r = g.api.send(h, &queued);
    } else {
        r = g.api.send(h, o);
    }
    ++g.eosOut;
    if (r != EOS_Success) {
        ++g.eosSendFail;
        std::string key = "send-fail-" + std::to_string(r);
        logRateLimited(key.c_str(), 10000, "EOS SendPacket to %s failed: %s",
                       o ? peerLabel(o->RemoteUserId).c_str() : "?", resultName(r));
    }
    return r;
}

EOS_EResult hookReceivePacket(EOS_HP2P h, const EOS_P2P_ReceivePacketOptions* o, EOS_ProductUserId* outPeer,
                              EOS_P2P_SocketId* outSocket, uint8_t* outChannel, void* outData, uint32_t* outBytes) {
    configureHandle(h);
    if (o) noteLocalUser(h, o->LocalUserId);
    maybeLogStats();
    if (g.hold && g.hold->heldCount()) {
        for (const auto& remote : g.hold->poll(GetTickCount64()))
            logf("RESILIENCE %s did not come back within %u s; disconnect handed to the game", shortId(remote).c_str(),
                 g.config.graceMs / 1000);
    }
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

}  // namespace

bool installEosHooks(HMODULE game, HMODULE eos, const Config& config, DirectNet* net) {
    g.config = config;
    g.net = nullptr;  // enabled below only when every hook is in place
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionEstablished", g.api.addEstablished);
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionInterrupted", g.api.addInterrupted);
    resolve(eos, "EOS_P2P_AddNotifyPeerConnectionClosed", g.api.addClosed);
    resolve(eos, "EOS_P2P_AddNotifyIncomingPacketQueueFull", g.api.addQueueFull);
    resolve(eos, "EOS_P2P_QueryNATType", g.api.queryNat);
    resolve(eos, "EOS_P2P_SetRelayControl", g.api.setRelay);
    resolve(eos, "EOS_P2P_SetPortRange", g.api.setPortRange);
    resolve(eos, "EOS_ProductUserId_ToString", g.api.idToString);
    resolve(eos, "EOS_ProductUserId_FromString", g.api.idFromString);
    resolve(eos, "EOS_EResult_ToString", g.api.resultToString);
    resolve(eos, "EOS_P2P_AcceptConnection", g.api.accept);

    // Resilience and lobby diagnostics are optional: failing here must not disable the rest.
    if (config.holdDisconnects && g.api.accept) {
        bool held = patchImport(game, kEosDll, "EOS_P2P_AddNotifyPeerConnectionClosed",
                                reinterpret_cast<void*>(hookAddNotifyClosed),
                                reinterpret_cast<void**>(&g.api.gameAddClosed));
        if (held) g.hold = std::make_unique<DisconnectHold>(DisconnectHold::Options{true, config.graceMs, 2000});
        logf("RESILIENCE disconnect hold %s (grace %u s)", held ? "enabled" : "UNAVAILABLE", config.graceMs / 1000);
    }
    patchImport(game, kEosDll, "EOS_P2P_CloseConnection", reinterpret_cast<void*>(hookCloseConnection),
                reinterpret_cast<void**>(&g.api.gameClose));
    patchImport(game, kEosDll, "EOS_P2P_CloseConnections", reinterpret_cast<void*>(hookCloseConnections),
                reinterpret_cast<void**>(&g.api.gameCloseAll));
    patchImport(game, kEosDll, "EOS_Lobby_AddNotifyLobbyMemberStatusReceived",
                reinterpret_cast<void*>(hookAddNotifyMemberStatus),
                reinterpret_cast<void**>(&g.api.gameAddMemberStatus));

    bool ok = patchImport(game, kEosDll, "EOS_Platform_GetP2PInterface", reinterpret_cast<void*>(hookGetP2PInterface),
                          reinterpret_cast<void**>(&g.api.getP2P)) &&
              patchImport(game, kEosDll, "EOS_P2P_SendPacket", reinterpret_cast<void*>(hookSendPacket),
                          reinterpret_cast<void**>(&g.api.send)) &&
              patchImport(game, kEosDll, "EOS_P2P_ReceivePacket", reinterpret_cast<void*>(hookReceivePacket),
                          reinterpret_cast<void**>(&g.api.receive));
    // A half-hooked transport would send direct packets that the game can never receive.
    g.net = ok ? net : nullptr;
    logf("EOS hooks %s", ok ? "installed" : "FAILED (EDF.dll import table not as expected), direct link disabled");
    return ok;
}

}  // namespace dn
