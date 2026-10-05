// A stand-in EOSSDK-Win64-Shipping.dll for the game-code tests (GameNetTests): the real EDF.dll loads it in place of
// Epic's, and every machine of a test reaches the others through it. The room is one EOS lobby and the P2P packets
// go through the network section of net_shared.h. It behaves as EOS does where the game and EDF6Coop can tell:
//  - a packet above 1170 bytes is refused with EOS_LimitExceeded and goes nowhere;
//  - a packet from a peer this machine has not accepted (EOS_P2P_AcceptConnection, or a packet of its own to that
//    peer on that socket) waits, and the connection request is announced on the next EOS_Platform_Tick;
//  - completions and notifications run inside EOS_Platform_Tick: connection requests and establishments, and the
//    room's member joins, leaves and updates;
//  - order is kept per channel only; with EDF6NET_DELAY one channel arrives later, with EDF6NET_DROP unreliable
//    packets get lost (net_shared.h).
// Everything else EDF.dll imports is exported too, as a stub that returns 0 and is reported (FakeNet_Unimplemented),
// so a test can tell when the game reached a part of EOS this fake does not model.
#include "net_shared.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../../src/eos_min.h"

#define EXPORT extern "C" __declspec(dllexport)

namespace {

using gamenet::Locked;

struct User {
    char text[gamenet::kUserText];
};

struct Fake {
    // The game's threads and the plugin's may all call in: every export takes `lock` once, and what it calls runs
    // under it (Open, Drain, the notices). Completions run after it is let go. `usersLock` guards `users` alone.
    std::mutex lock;
    std::mutex usersLock;
    bool opened = false;
    HANDLE section = nullptr;
    HANDLE netLock = nullptr;
    gamenet::Network* net = nullptr;
    int slot = -1;
    std::string self;
    std::map<std::string, std::unique_ptr<User>> users;
    std::deque<std::function<void()>> completions;  // run by EOS_Platform_Tick
    std::vector<std::string> unimplemented;

    struct Incoming {
        std::string from;
        std::string socket;
        std::uint8_t channel;
        std::vector<std::uint8_t> data;
        ULONGLONG due;  // receivable from (EDF6NET_DELAY)
    };
    std::deque<Incoming> incoming;                         // taken out of the inbox, not yet received
    std::set<std::pair<std::string, std::string>> accepted;  // (peer, socket)
    std::set<std::pair<std::string, std::string>> requested;
    struct Notify {
        std::uint64_t id;
        std::string socket;  // empty: every socket
        void* clientData;
        void* callback;
    };
    std::vector<Notify> connectionRequests, connectionsClosed, connectionsEstablished;
    std::set<std::pair<std::string, std::string>> established;  // announced (peer, socket)
    struct LobbyNotify {
        std::uint64_t id;
        int kind;  // 0 member status, 1 member update, 2 lobby update
        void* clientData;
        void* callback;
    };
    std::vector<LobbyNotify> lobbyNotifies;
    std::uint64_t nextNotify = 1;
    std::uint32_t lobbyVersionSeen = 0;
    std::vector<std::string> membersSeen;  // the room's members at the last tick (status notifications)
    std::string lobbySeen;
    // EDF6NET_DELAY / EDF6NET_DROP
    int delayedChannel = -1;
    ULONGLONG delayMs = 0;
    std::uint32_t dropMinimum = 0;
    int dropsLeft = 0;
    ULONGLONG lobbyDelayMs = 0;  // EDF6NET_LOBBY_DELAY
};

Fake& F() {
    static Fake fake;
    return fake;
}

const User* Handle(const std::string& id) {
    const std::scoped_lock guard(F().usersLock);
    auto& user = F().users[id];
    if (!user) {
        user = std::make_unique<User>();
        strncpy_s(user->text, id.c_str(), _TRUNCATE);
    }
    return user.get();
}

std::string Text(const void* user) { return user ? static_cast<const User*>(user)->text : std::string(); }

void Copy(char* out, std::size_t size, const std::string& text) { strncpy_s(out, size, text.c_str(), _TRUNCATE); }

// Maps the network and takes this machine's slot (from the environment the driver set).
// Under `lock`.
bool Open() {
    Fake& f = F();
    if (f.opened) return f.net != nullptr && f.slot >= 0;  // a machine that found no free slot stays offline
    f.opened = true;
    char setting[64]{};
    if (GetEnvironmentVariableA(gamenet::kDelayVariable, setting, sizeof(setting))) {
        unsigned channel = 0, ms = 0;
        if (sscanf_s(setting, "%u:%u", &channel, &ms) == 2) {
            f.delayedChannel = static_cast<int>(channel);
            f.delayMs = ms;
        }
    }
    if (GetEnvironmentVariableA(gamenet::kLobbyDelayVariable, setting, sizeof(setting)))
        f.lobbyDelayMs = std::strtoull(setting, nullptr, 10);
    if (GetEnvironmentVariableA(gamenet::kDropVariable, setting, sizeof(setting))) {
        unsigned bytes = 0, count = 0;
        if (sscanf_s(setting, "%u:%u", &bytes, &count) == 2) {
            f.dropMinimum = bytes;
            f.dropsLeft = static_cast<int>(count);
        }
    }
    char name[128]{}, user[64]{};
    if (!GetEnvironmentVariableA(gamenet::kSectionVariable, name, sizeof(name)) ||
        !GetEnvironmentVariableA(gamenet::kUserVariable, user, sizeof(user))) {
        std::fprintf(stderr, "FAKE EOS: %s / %s are not set; no network\n", gamenet::kSectionVariable,
                     gamenet::kUserVariable);
        return false;
    }
    f.net = gamenet::MapNetwork(name, false, &f.section, &f.netLock);
    if (!f.net) {
        std::fprintf(stderr, "FAKE EOS: network %s cannot be mapped\n", name);
        return false;
    }
    f.self = user;
    Locked locked(f.netLock);
    for (int i = 0; i < gamenet::kMaxMachines && f.slot < 0; ++i)
        if (!f.net->machines[i].present) {
            f.slot = i;
            Copy(f.net->machines[i].user, sizeof(f.net->machines[i].user), f.self);
            f.net->machines[i].present = 1;
        }
    return f.slot >= 0;
}

gamenet::Station* MachineOf(const std::string& user) {
    if (!F().net) return nullptr;
    for (auto& machine : F().net->machines)
        if (machine.present && user == machine.user) return &machine;
    return nullptr;
}

// Moves what arrived in this machine's inbox to `incoming`.
void Drain() {
    Fake& f = F();
    if (!Open() || f.slot < 0) return;
    Locked locked(f.netLock);
    gamenet::Ring& inbox = f.net->machines[f.slot].inbox;
    while (inbox.tail - inbox.head >= sizeof(gamenet::PacketHeader)) {
        gamenet::PacketHeader header{};
        gamenet::RingPeek(inbox, inbox.head, &header, sizeof(header));
        if (inbox.tail - inbox.head < sizeof(header) + header.size) break;  // not all written (yet)
        const ULONGLONG due = GetTickCount64() + (header.channel == f.delayedChannel ? f.delayMs : 0);
        Fake::Incoming packet{header.from, header.socket, header.channel, std::vector<std::uint8_t>(header.size), due};
        gamenet::RingPeek(inbox, inbox.head + sizeof(header), packet.data.data(), header.size);
        inbox.head += sizeof(header) + header.size;
        f.incoming.push_back(std::move(packet));
    }
}

bool Accepted(const std::string& peer, const std::string& socket) {
    return F().accepted.count({peer, socket}) != 0;
}

// --- lobby ---
struct Details {
    gamenet::Lobby lobby;
};

struct Modification {
    std::vector<gamenet::Attribute> member;  // the local member's
    std::vector<gamenet::Attribute> lobby;   // the lobby's own (kept, not modelled further)
};

struct AttributeData {
    std::int32_t ApiVersion;
    const char* Key;
    union {
        std::int64_t AsInt64;
        double AsDouble;
        std::int32_t AsBool;
        const char* AsUtf8;
    } Value;
    std::int32_t ValueType;
};
struct Attribute {
    std::int32_t ApiVersion;
    AttributeData* Data;
    std::int32_t Visibility;
};
constexpr std::int32_t kInt64 = 1, kString = 3;

gamenet::Member* MemberOf(gamenet::Lobby& lobby, const std::string& user) {
    for (std::uint32_t i = 0; i < lobby.count; ++i)
        if (user == lobby.members[i].user) return &lobby.members[i];
    return nullptr;
}

void SetAttribute(gamenet::Member& member, const gamenet::Attribute& value) {
    gamenet::Attribute* free = nullptr;
    for (auto& attribute : member.attributes) {
        if (!std::strcmp(attribute.key, value.key)) {
            attribute = value;
            return;
        }
        if (!free && !attribute.key[0]) free = &attribute;
    }
    if (free) *free = value;
}

// What this fake hands out by pointer until the matching Release (lobby details, modifications, attributes,
// lobby infos), as EOS does: owned here, under the pointer the caller holds.
template <typename Key, typename T>
class Handed {
public:
    const Key* Give(std::unique_ptr<T> item, const Key* key) {
        const std::scoped_lock guard(lock_);
        items_[key] = std::move(item);
        return key;
    }
    void Take(const Key* key) {
        const std::scoped_lock guard(lock_);
        items_.erase(key);
    }

private:
    std::mutex lock_;
    std::map<const Key*, std::unique_ptr<T>> items_;
};

struct OwnedAttribute {
    Attribute api{};
    AttributeData data{};
    std::string key;
    std::string text;
};
Handed<Attribute, OwnedAttribute>& Attributes() {
    static Handed<Attribute, OwnedAttribute> handed;
    return handed;
}

Attribute* NewAttribute(const gamenet::Attribute& from) {
    auto owned = std::make_unique<OwnedAttribute>();
    owned->key = from.key;
    owned->text = from.text;
    owned->data.ApiVersion = 1;
    owned->data.Key = owned->key.c_str();
    owned->data.ValueType = from.type == 4 ? kString : kInt64;
    if (from.type == 4) owned->data.Value.AsUtf8 = owned->text.c_str();
    else owned->data.Value.AsInt64 = from.number;
    owned->api = Attribute{1, &owned->data, 0};
    Attribute* api = &owned->api;
    Attributes().Give(std::move(owned), api);
    return api;
}

Handed<Details, Details>& DetailsHanded() {
    static Handed<Details, Details> handed;
    return handed;
}
void* GiveDetails(const gamenet::Lobby& lobby) {
    auto details = std::make_unique<Details>(Details{lobby});
    Details* key = details.get();
    DetailsHanded().Give(std::move(details), key);
    return key;
}

using LobbyIdCallback = void (*)(const EOS_Lobby_LobbyIdCallbackInfo*);

void Complete(void* callback, void* clientData, EOS_EResult result, const std::string& lobby) {
    if (!callback) return;
    F().completions.push_back([=]() {
        const EOS_Lobby_LobbyIdCallbackInfo info{result, clientData, lobby.c_str()};
        reinterpret_cast<LobbyIdCallback>(callback)(&info);
    });
}

void Unimplemented(const char* name) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    for (const auto& seen : f.unimplemented)
        if (seen == name) return;
    f.unimplemented.emplace_back(name);
}

}  // namespace

// --- for the machine ---
// The EOS functions the game or the plugin called that this fake only stubs, one per line.
EXPORT std::size_t FakeNet_Unimplemented(char* out, std::size_t size) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    std::string all;
    for (const auto& name : f.unimplemented) all += name + "\n";
    if (out && size) Copy(out, size, all);
    return all.size();
}
EXPORT const void* FakeNet_User(const char* id) { return Handle(id); }
EXPORT const char* FakeNet_Self() {
    const std::scoped_lock guard(F().lock);
    Open();
    return F().self.c_str();
}
// How many are in the room.
EXPORT std::uint32_t FakeNet_RoomCount() {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open()) return 0;
    Locked locked(f.netLock);
    return f.net->lobby.count;
}
// A details handle for the room, as a room search hands one to the game (EOS_LobbyDetails_Release frees it).
EXPORT void* FakeNet_RoomDetails() {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open()) return nullptr;
    Locked locked(f.netLock);
    if (!f.net->lobby.id[0]) return nullptr;
    return GiveDetails(f.net->lobby);
}

// Counts this machine as done (`finish` 1) and returns how many are: machines keep the room's network running
// until everyone is done, as players stay in the room.
EXPORT std::uint32_t FakeNet_Finish(int finish) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open()) return 0;
    Locked locked(f.netLock);
    if (finish) ++f.net->finished;
    return f.net->finished;
}

// --- EOS: platform ---
EXPORT void* EOS_Platform_Create(const void*) { return reinterpret_cast<void*>(0x1000); }
EXPORT void EOS_Platform_Release(void*) {}
EXPORT EOS_EResult EOS_Initialize(const void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_Shutdown() { return EOS_Success; }
EXPORT void* EOS_Platform_GetP2PInterface(void*) { return reinterpret_cast<void*>(0x2000); }
EXPORT void* EOS_Platform_GetLobbyInterface(void*) { return reinterpret_cast<void*>(0x3000); }
EXPORT EOS_EResult EOS_Logging_SetCallback(void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_Logging_SetLogLevel(std::int32_t, std::int32_t) { return EOS_Success; }
EXPORT EOS_Bool EOS_EResult_IsOperationComplete(EOS_EResult result) { return result != 0x99; }
EXPORT const char* EOS_EResult_ToString(EOS_EResult result) {
    switch (result) {
    case EOS_Success: return "EOS_Success";
    case EOS_NoConnection: return "EOS_NoConnection";
    case EOS_InvalidParameters: return "EOS_InvalidParameters";
    case EOS_NotFound: return "EOS_NotFound";
    case EOS_LimitExceeded: return "EOS_LimitExceeded";
    default: return "EOS_Unknown";
    }
}

using ConnectionRequestCallback = void (*)(const void*);
struct ConnectionRequestInfo {
    void* ClientData;
    EOS_ProductUserId LocalUserId;
    EOS_ProductUserId RemoteUserId;
    const EOS_P2P_SocketId* SocketId;
    std::int32_t ConnectionType;
};

namespace {
struct MemberStatusInfo {
    void* ClientData;
    const char* LobbyId;
    const void* TargetUserId;
    std::int32_t CurrentStatus;  // 0 joined, 1 left
};
struct MemberUpdateInfo {
    void* ClientData;
    const char* LobbyId;
    const void* TargetUserId;
};
struct LobbyUpdateInfo {
    void* ClientData;
    const char* LobbyId;
};

// What changed in the room since the last tick, as EOS tells a member: others joining or leaving (not the members
// that were there when this machine came in), and updates of members and the lobby.
void NoticeRoomChanges() {
    Fake& f = F();
    if (!f.net) return;
    gamenet::Lobby lobby;
    {
        Locked locked(f.netLock);
        if (f.net->lobby.version == f.lobbyVersionSeen) return;
        lobby = f.net->lobby;
    }
    f.lobbyVersionSeen = lobby.version;
    std::vector<std::string> members;
    for (std::uint32_t i = 0; i < lobby.count; ++i) members.emplace_back(lobby.members[i].user);
    const bool in = std::find(members.begin(), members.end(), f.self) != members.end();
    const std::string id = lobby.id;
    if (!in || id != f.lobbySeen) {
        f.membersSeen = in ? members : std::vector<std::string>();
        f.lobbySeen = in ? id : std::string();
        return;
    }
    const auto queue = [&](int kind, const std::string& member, std::int32_t status) {
        for (const auto& notify : f.lobbyNotifies) {
            if (notify.kind != kind) continue;
            const auto n = notify;
            f.completions.push_back([=]() {
                const void* user = member.empty() ? nullptr : Handle(member);
                if (kind == 0) {
                    MemberStatusInfo info{n.clientData, id.c_str(), user, status};
                    reinterpret_cast<void (*)(const MemberStatusInfo*)>(n.callback)(&info);
                } else if (kind == 1) {
                    MemberUpdateInfo info{n.clientData, id.c_str(), user};
                    reinterpret_cast<void (*)(const MemberUpdateInfo*)>(n.callback)(&info);
                } else {
                    LobbyUpdateInfo info{n.clientData, id.c_str()};
                    reinterpret_cast<void (*)(const LobbyUpdateInfo*)>(n.callback)(&info);
                }
            });
        }
    };
    for (const auto& member : members)
        if (std::find(f.membersSeen.begin(), f.membersSeen.end(), member) == f.membersSeen.end()) queue(0, member, 0);
    for (const auto& member : f.membersSeen)
        if (std::find(members.begin(), members.end(), member) == members.end()) queue(0, member, 1);
    for (const auto& member : members) queue(1, member, 0);
    queue(2, std::string(), 0);
    f.membersSeen = members;
}

// A connection is established the first time a packet of the peer is received on an accepted socket.
void NoticeEstablished(const std::string& peer, const std::string& socket) {
    Fake& f = F();
    if (!f.established.insert({peer, socket}).second) return;
    for (const auto& notify : f.connectionsEstablished) {
        if (!notify.socket.empty() && notify.socket != socket) continue;
        const auto n = notify;
        const std::string self = f.self;
        f.completions.push_back([=]() {
            EOS_P2P_SocketId id{1, {}};
            Copy(id.SocketName, sizeof(id.SocketName), socket);
            const EOS_P2P_OnPeerConnectionEstablishedInfo info{n.clientData, (EOS_ProductUserId)Handle(self),
                                                               (EOS_ProductUserId)Handle(peer), &id, 0,
                                                               EOS_NCT_DirectConnection};
            reinterpret_cast<void (*)(const EOS_P2P_OnPeerConnectionEstablishedInfo*)>(n.callback)(&info);
        });
    }
}
}  // namespace

EXPORT void EOS_Platform_Tick(void*) {
    Fake& f = F();
    std::deque<std::function<void()>> run;
    {
        const std::scoped_lock guard(f.lock);
        Drain();
        // A connection request for every peer that sent on a socket this machine has not accepted.
        for (const auto& packet : f.incoming) {
            const auto key = std::make_pair(packet.from, packet.socket);
            if (Accepted(packet.from, packet.socket) || f.requested.count(key)) continue;
            f.requested.insert(key);
            for (const auto& notify : f.connectionRequests) {
                if (!notify.socket.empty() && notify.socket != packet.socket) continue;
                const std::string from = packet.from, socket = packet.socket, self = f.self;
                const auto n = notify;
                f.completions.push_back([=]() {
                    EOS_P2P_SocketId id{1, {}};
                    Copy(id.SocketName, sizeof(id.SocketName), socket);
                    ConnectionRequestInfo info{n.clientData, (EOS_ProductUserId)Handle(self),
                                               (EOS_ProductUserId)Handle(from), &id, 0};
                    reinterpret_cast<ConnectionRequestCallback>(n.callback)(&info);
                });
            }
        }
        NoticeRoomChanges();
        run.swap(f.completions);
    }
    for (auto& completion : run) completion();
}

// --- EOS: ids ---
EXPORT EOS_Bool EOS_ProductUserId_IsValid(const void* id) { return id != nullptr && Text(id).size() > 0; }
EXPORT EOS_EResult EOS_ProductUserId_ToString(const void* id, char* out, std::int32_t* length) {
    if (!id || !length) return EOS_InvalidParameters;
    const std::string text = Text(id);
    if (!out || *length < static_cast<std::int32_t>(text.size() + 1)) {
        *length = static_cast<std::int32_t>(text.size() + 1);
        return EOS_LimitExceeded;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    *length = static_cast<std::int32_t>(text.size() + 1);
    return EOS_Success;
}
EXPORT const void* EOS_ProductUserId_FromString(const char* text) { return text && *text ? Handle(text) : nullptr; }

// --- EOS: P2P ---
EXPORT EOS_EResult EOS_P2P_SendPacket(EOS_HP2P, const EOS_P2P_SendPacketOptions* options) {
    Fake& f = F();
    if (!options || !options->RemoteUserId || !options->SocketId || (!options->Data && options->DataLengthBytes))
        return EOS_InvalidParameters;
    const std::scoped_lock guard(f.lock);
    if (!Open()) return EOS_NoConnection;
    const std::string to = Text(options->RemoteUserId), socket = options->SocketId->SocketName;
    Locked locked(f.netLock);
    if (options->DataLengthBytes > gamenet::kMaxPacket) {
        ++f.net->refused;
        return EOS_LimitExceeded;
    }
    gamenet::Station* machine = MachineOf(to);
    if (!machine) return EOS_NoConnection;
    if (!options->bDisableAutoAcceptConnection) f.accepted.insert({to, socket});
    if (options->Reliability == EOS_PR_UnreliableUnordered && f.dropsLeft > 0 &&
        options->DataLengthBytes >= f.dropMinimum) {
        --f.dropsLeft;
        ++f.net->dropped;
        return EOS_Success;  // gone on the way, as unreliable packets may be
    }
    gamenet::PacketHeader header{};
    Copy(header.from, sizeof(header.from), f.self);
    Copy(header.to, sizeof(header.to), to);
    Copy(header.socket, sizeof(header.socket), socket);
    header.channel = options->Channel;
    header.reliability = static_cast<std::uint8_t>(options->Reliability);
    header.size = options->DataLengthBytes;
    if (gamenet::RingFree(machine->inbox) < sizeof(header) + header.size) {
        ++f.net->overflowed;
        return EOS_LimitExceeded;
    }
    gamenet::RingWrite(machine->inbox, &header, sizeof(header));
    gamenet::RingWrite(machine->inbox, options->Data, header.size);
    gamenet::LogWire(f.net->wire, header, options->Data);
    return EOS_Success;
}

EXPORT EOS_EResult EOS_P2P_ReceivePacket(EOS_HP2P, const EOS_P2P_ReceivePacketOptions* options, EOS_ProductUserId* peer,
                                         EOS_P2P_SocketId* socket, std::uint8_t* channel, void* data,
                                         std::uint32_t* size) {
    Fake& f = F();
    if (!options || !peer || !socket || !channel || !data || !size) return EOS_InvalidParameters;
    const std::scoped_lock guard(f.lock);
    Drain();
    const ULONGLONG now = GetTickCount64();
    for (auto it = f.incoming.begin(); it != f.incoming.end(); ++it) {
        if (!Accepted(it->from, it->socket) || it->due > now) continue;
        if (options->RequestedChannel && *options->RequestedChannel != it->channel) continue;
        if (it->data.size() > options->MaxDataSizeBytes) return EOS_LimitExceeded;
        NoticeEstablished(it->from, it->socket);
        *peer = (EOS_ProductUserId)Handle(it->from);
        socket->ApiVersion = 1;
        Copy(socket->SocketName, sizeof(socket->SocketName), it->socket);
        *channel = it->channel;
        std::memcpy(data, it->data.data(), it->data.size());
        *size = static_cast<std::uint32_t>(it->data.size());
        f.incoming.erase(it);
        return EOS_Success;
    }
    return EOS_NotFound;
}

EXPORT EOS_EResult EOS_P2P_GetNextReceivedPacketSize(EOS_HP2P, const EOS_P2P_ReceivePacketOptions* options,
                                                     std::uint32_t* size) {
    Fake& f = F();
    if (!options || !size) return EOS_InvalidParameters;
    const std::scoped_lock guard(f.lock);
    Drain();
    const ULONGLONG now = GetTickCount64();
    for (const auto& packet : f.incoming) {
        if (!Accepted(packet.from, packet.socket) || packet.due > now) continue;
        if (options->RequestedChannel && *options->RequestedChannel != packet.channel) continue;
        *size = static_cast<std::uint32_t>(packet.data.size());
        return EOS_Success;
    }
    return EOS_NotFound;
}

namespace {
std::uint64_t AddNotify(std::vector<Fake::Notify>& list, const EOS_P2P_AddNotifyOptions* options, void* clientData,
                        void* callback) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    const std::string socket = options && options->SocketId ? options->SocketId->SocketName : "";
    list.push_back({f.nextNotify, socket, clientData, callback});
    return f.nextNotify++;
}
void RemoveNotify(std::vector<Fake::Notify>& list, std::uint64_t id) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    for (auto it = list.begin(); it != list.end(); ++it)
        if (it->id == id) {
            list.erase(it);
            return;
        }
}
}  // namespace

EXPORT std::uint64_t EOS_P2P_AddNotifyPeerConnectionRequest(EOS_HP2P, const EOS_P2P_AddNotifyOptions* options,
                                                            void* clientData, void* callback) {
    return AddNotify(F().connectionRequests, options, clientData, callback);
}
EXPORT void EOS_P2P_RemoveNotifyPeerConnectionRequest(EOS_HP2P, std::uint64_t id) { RemoveNotify(F().connectionRequests, id); }
EXPORT std::uint64_t EOS_P2P_AddNotifyPeerConnectionClosed(EOS_HP2P, const EOS_P2P_AddNotifyOptions* options,
                                                           void* clientData, void* callback) {
    return AddNotify(F().connectionsClosed, options, clientData, callback);
}
EXPORT void EOS_P2P_RemoveNotifyPeerConnectionClosed(EOS_HP2P, std::uint64_t id) { RemoveNotify(F().connectionsClosed, id); }
EXPORT std::uint64_t EOS_P2P_AddNotifyPeerConnectionEstablished(EOS_HP2P, const EOS_P2P_AddNotifyOptions* options,
                                                                void* clientData, void* callback) {
    return AddNotify(F().connectionsEstablished, options, clientData, callback);
}
EXPORT std::uint64_t EOS_P2P_AddNotifyPeerConnectionInterrupted(EOS_HP2P, const EOS_P2P_AddNotifyOptions*, void*, void*) {
    return F().nextNotify++;
}
EXPORT std::uint64_t EOS_P2P_AddNotifyIncomingPacketQueueFull(EOS_HP2P, const void*, void*, void*) {
    return F().nextNotify++;
}
EXPORT EOS_EResult EOS_P2P_AcceptConnection(EOS_HP2P, const EOS_P2P_PeerConnectionOptions* options) {
    if (!options || !options->RemoteUserId || !options->SocketId) return EOS_InvalidParameters;
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    f.accepted.insert({Text(options->RemoteUserId), options->SocketId->SocketName});
    return EOS_Success;
}
EXPORT EOS_EResult EOS_P2P_CloseConnection(EOS_HP2P, const EOS_P2P_PeerConnectionOptions* options) {
    if (!options || !options->RemoteUserId || !options->SocketId) return EOS_InvalidParameters;
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    f.accepted.erase({Text(options->RemoteUserId), options->SocketId->SocketName});
    f.requested.erase({Text(options->RemoteUserId), options->SocketId->SocketName});
    return EOS_Success;
}
EXPORT EOS_EResult EOS_P2P_CloseConnections(EOS_HP2P, const EOS_P2P_CloseConnectionsOptions* options) {
    if (!options || !options->SocketId) return EOS_InvalidParameters;
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    const std::string socket = options->SocketId->SocketName;
    for (auto it = f.accepted.begin(); it != f.accepted.end();) it = it->second == socket ? f.accepted.erase(it) : ++it;
    for (auto it = f.requested.begin(); it != f.requested.end();) it = it->second == socket ? f.requested.erase(it) : ++it;
    return EOS_Success;
}
EXPORT EOS_EResult EOS_P2P_SetRelayControl(EOS_HP2P, const void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_P2P_SetPortRange(EOS_HP2P, const void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_P2P_SetPacketQueueSize(EOS_HP2P, const void*) { return EOS_Success; }
EXPORT void EOS_P2P_QueryNATType(EOS_HP2P, const void*, void*, void*) {}
EXPORT EOS_EResult EOS_P2P_GetPacketQueueInfo(EOS_HP2P, const void*, EOS_P2P_PacketQueueInfo* info) {
    if (info) *info = EOS_P2P_PacketQueueInfo{};
    return EOS_Success;
}

// --- EOS: lobby ---
struct CreateLobbyOptionsHead {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    std::uint32_t MaxLobbyMembers;
};
struct LeaveOptions {
    std::int32_t ApiVersion;
    const void* LocalUserId;
    const char* LobbyId;
};
struct CopyDetailsOptions {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LocalUserId;
};
struct UpdateLobbyOptions {
    std::int32_t ApiVersion;
    void* LobbyModificationHandle;
};
struct AddAttributeOptions {
    std::int32_t ApiVersion;
    const AttributeData* Attribute;
    std::int32_t Visibility;
};
struct GetMemberByIndexOptions {
    std::int32_t ApiVersion;
    std::uint32_t MemberIndex;
};
struct CopyMemberAttributeByKeyOptions {
    std::int32_t ApiVersion;
    const void* TargetUserId;
    const char* AttrKey;
};
struct GetMemberAttributeCountOptions {
    std::int32_t ApiVersion;
    const void* TargetUserId;
};
struct CopyMemberAttributeByIndexOptions {
    std::int32_t ApiVersion;
    const void* TargetUserId;
    std::uint32_t AttrIndex;
};
struct LobbyDetailsInfo {
    std::int32_t ApiVersion;
    const char* LobbyId;
    const void* LobbyOwnerUserId;
    std::int32_t PermissionLevel;
    std::uint32_t AvailableSlots;
    std::uint32_t MaxMembers;
};

EXPORT void EOS_Lobby_CreateLobby(void*, const CreateLobbyOptionsHead* options, void* clientData, void* callback) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open() || !options) return Complete(callback, clientData, EOS_InvalidParameters, "");
    std::string id;
    {
        Locked locked(f.netLock);
        gamenet::Lobby& lobby = f.net->lobby;
        if (lobby.id[0]) return Complete(callback, clientData, EOS_InvalidParameters, "");
        // 32 hex digits, as Epic's lobby ids are: the game copies one into the 33-byte P2P socket name (strcpy_s,
        // p2p::Manager::Initialize), and a longer one ends the process there.
        std::uint64_t hash = 14695981039346656037ull;
        for (char c : f.self) hash = (hash ^ static_cast<std::uint8_t>(c)) * 1099511628211ull;
        char text[33]{};
        std::snprintf(text, sizeof(text), "%016llx%016llx", hash, hash * 0x9E3779B97F4A7C15ull);
        id = text;
        lobby = gamenet::Lobby{};
        Copy(lobby.id, sizeof(lobby.id), id);
        Copy(lobby.owner, sizeof(lobby.owner), f.self);
        lobby.maxMembers = options->MaxLobbyMembers;
        lobby.count = 1;
        Copy(lobby.members[0].user, sizeof(lobby.members[0].user), f.self);
        ++lobby.version;
    }
    Complete(callback, clientData, EOS_Success, id);
}

EXPORT void EOS_Lobby_JoinLobby(void*, const EOS_Lobby_JoinLobbyOptionsHead* options, void* clientData, void* callback) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open() || !options || !options->LobbyDetailsHandle) return Complete(callback, clientData, EOS_InvalidParameters, "");
    const std::string wanted = reinterpret_cast<Details*>(options->LobbyDetailsHandle)->lobby.id;
    Locked locked(f.netLock);
    gamenet::Lobby& lobby = f.net->lobby;
    if (wanted != lobby.id) return Complete(callback, clientData, EOS_NotFound, wanted);
    if (!MemberOf(lobby, f.self)) {
        if (lobby.count >= lobby.maxMembers || lobby.count >= gamenet::kMaxMachines)
            return Complete(callback, clientData, EOS_LimitExceeded, wanted);
        gamenet::Member& member = lobby.members[lobby.count++];
        member = gamenet::Member{};
        Copy(member.user, sizeof(member.user), f.self);
        ++lobby.version;
    }
    Complete(callback, clientData, EOS_Success, wanted);
}

namespace {
void Leave(const LeaveOptions* options, void* clientData, void* callback, bool destroy) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open() || !options || !options->LobbyId) return Complete(callback, clientData, EOS_InvalidParameters, "");
    const std::string id = options->LobbyId;
    Locked locked(f.netLock);
    gamenet::Lobby& lobby = f.net->lobby;
    if (id != lobby.id) return Complete(callback, clientData, EOS_NotFound, id);
    if (destroy) {
        const std::uint32_t version = lobby.version;  // stays monotonic: the others compare it
        lobby = gamenet::Lobby{};
        lobby.version = version;
    } else {
        for (std::uint32_t i = 0; i < lobby.count; ++i)
            if (f.self == lobby.members[i].user) {
                for (std::uint32_t j = i + 1; j < lobby.count; ++j) lobby.members[j - 1] = lobby.members[j];
                --lobby.count;
                break;
            }
    }
    ++lobby.version;
    Complete(callback, clientData, EOS_Success, id);
}
}  // namespace

EXPORT void EOS_Lobby_LeaveLobby(void*, const LeaveOptions* options, void* clientData, void* callback) {
    Leave(options, clientData, callback, false);
}
EXPORT void EOS_Lobby_DestroyLobby(void*, const LeaveOptions* options, void* clientData, void* callback) {
    Leave(options, clientData, callback, true);
}

namespace {
Handed<Modification, Modification>& Modifications() {
    static Handed<Modification, Modification> handed;
    return handed;
}
}  // namespace

EXPORT EOS_EResult EOS_Lobby_UpdateLobbyModification(void*, const void*, void** modification) {
    if (!modification) return EOS_InvalidParameters;
    auto owned = std::make_unique<Modification>();
    *modification = owned.get();
    Modifications().Give(std::move(owned), static_cast<Modification*>(*modification));
    return EOS_Success;
}
EXPORT EOS_EResult EOS_LobbyModification_AddMemberAttribute(void* handle, const AddAttributeOptions* options) {
    if (!handle || !options || !options->Attribute || !options->Attribute->Key) return EOS_InvalidParameters;
    gamenet::Attribute value{};
    Copy(value.key, sizeof(value.key), options->Attribute->Key);
    if (options->Attribute->ValueType == kString) {
        value.type = 4;
        Copy(value.text, sizeof(value.text), options->Attribute->Value.AsUtf8 ? options->Attribute->Value.AsUtf8 : "");
    } else {
        value.type = 1;
        value.number = options->Attribute->Value.AsInt64;
    }
    static_cast<Modification*>(handle)->member.push_back(value);
    return EOS_Success;
}
EXPORT EOS_EResult EOS_LobbyModification_AddAttribute(void* handle, const AddAttributeOptions* options) {
    if (!handle || !options || !options->Attribute) return EOS_InvalidParameters;
    return EOS_Success;
}
EXPORT EOS_EResult EOS_LobbyModification_SetMaxMembers(void*, const void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_LobbyModification_SetPermissionLevel(void*, const void*) { return EOS_Success; }
EXPORT EOS_EResult EOS_LobbyModification_SetInvitesAllowed(void*, const void*) { return EOS_Success; }
EXPORT void EOS_LobbyModification_Release(void* handle) { Modifications().Take(static_cast<Modification*>(handle)); }
EXPORT void EOS_Lobby_UpdateLobby(void*, const UpdateLobbyOptions* options, void* clientData, void* callback) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!Open() || !options || !options->LobbyModificationHandle)
        return Complete(callback, clientData, EOS_InvalidParameters, "");
    const auto* modification = static_cast<const Modification*>(options->LobbyModificationHandle);
    Locked locked(f.netLock);
    gamenet::Lobby& lobby = f.net->lobby;
    gamenet::Member* self = MemberOf(lobby, f.self);
    if (!self) return Complete(callback, clientData, EOS_NotFound, lobby.id);
    for (auto value : modification->member) {
        value.visibleAt = GetTickCount64() + f.lobbyDelayMs;
        SetAttribute(*self, value);
    }
    ++lobby.version;
    Complete(callback, clientData, EOS_Success, lobby.id);
}

EXPORT EOS_EResult EOS_Lobby_CopyLobbyDetailsHandle(void*, const CopyDetailsOptions* options, void** details) {
    Fake& f = F();
    if (!options || !options->LobbyId || !details) return EOS_InvalidParameters;
    const std::scoped_lock guard(f.lock);
    if (!Open()) return EOS_NoConnection;
    Locked locked(f.netLock);
    if (std::strcmp(options->LobbyId, f.net->lobby.id) || !MemberOf(f.net->lobby, f.self)) return EOS_NotFound;
    auto* copy = static_cast<Details*>(GiveDetails(f.net->lobby));
    // Another member's attribute only once Epic would have relayed it.
    const ULONGLONG now = GetTickCount64();
    for (std::uint32_t i = 0; i < copy->lobby.count; ++i) {
        if (f.self == copy->lobby.members[i].user) continue;
        for (auto& attribute : copy->lobby.members[i].attributes)
            if (attribute.key[0] && attribute.visibleAt > now) attribute = gamenet::Attribute{};
    }
    *details = copy;
    return EOS_Success;
}
EXPORT void EOS_LobbyDetails_Release(void* details) { DetailsHanded().Take(static_cast<Details*>(details)); }
EXPORT std::uint32_t EOS_LobbyDetails_GetMemberCount(void* details, const void*) {
    return details ? static_cast<Details*>(details)->lobby.count : 0;
}
EXPORT const void* EOS_LobbyDetails_GetMemberByIndex(void* details, const GetMemberByIndexOptions* options) {
    if (!details || !options) return nullptr;
    const gamenet::Lobby& lobby = static_cast<Details*>(details)->lobby;
    return options->MemberIndex < lobby.count ? Handle(lobby.members[options->MemberIndex].user) : nullptr;
}
EXPORT const void* EOS_LobbyDetails_GetLobbyOwner(void* details, const void*) {
    return details ? Handle(static_cast<Details*>(details)->lobby.owner) : nullptr;
}
EXPORT EOS_EResult EOS_LobbyDetails_CopyMemberAttributeByKey(void* details, const CopyMemberAttributeByKeyOptions* options,
                                                             Attribute** out) {
    if (!details || !options || !options->AttrKey || !out) return EOS_InvalidParameters;
    gamenet::Member* member = MemberOf(static_cast<Details*>(details)->lobby, Text(options->TargetUserId));
    if (!member) return EOS_NotFound;
    for (const auto& attribute : member->attributes)
        if (attribute.key[0] && !_stricmp(attribute.key, options->AttrKey)) {
            *out = NewAttribute(attribute);
            return EOS_Success;
        }
    return EOS_NotFound;
}
EXPORT std::uint32_t EOS_LobbyDetails_GetMemberAttributeCount(void* details, const GetMemberAttributeCountOptions* options) {
    if (!details || !options) return 0;
    gamenet::Member* member = MemberOf(static_cast<Details*>(details)->lobby, Text(options->TargetUserId));
    std::uint32_t count = 0;
    if (member)
        for (const auto& attribute : member->attributes) count += attribute.key[0] ? 1 : 0;
    return count;
}
EXPORT EOS_EResult EOS_LobbyDetails_CopyMemberAttributeByIndex(void* details, const CopyMemberAttributeByIndexOptions* options,
                                                               Attribute** out) {
    if (!details || !options || !out) return EOS_InvalidParameters;
    gamenet::Member* member = MemberOf(static_cast<Details*>(details)->lobby, Text(options->TargetUserId));
    if (!member) return EOS_NotFound;
    std::uint32_t index = 0;
    for (const auto& attribute : member->attributes)
        if (attribute.key[0] && index++ == options->AttrIndex) {
            *out = NewAttribute(attribute);
            return EOS_Success;
        }
    return EOS_NotFound;
}
EXPORT std::uint32_t EOS_LobbyDetails_GetAttributeCount(void*, const void*) { return 0; }
EXPORT EOS_EResult EOS_LobbyDetails_CopyAttributeByIndex(void*, const void*, Attribute**) { return EOS_NotFound; }
EXPORT EOS_EResult EOS_LobbyDetails_CopyAttributeByKey(void*, const void*, Attribute**) { return EOS_NotFound; }
namespace {
struct OwnedInfo {
    LobbyDetailsInfo api{};
    std::string id;
};
Handed<LobbyDetailsInfo, OwnedInfo>& Infos() {
    static Handed<LobbyDetailsInfo, OwnedInfo> handed;
    return handed;
}
}  // namespace

EXPORT EOS_EResult EOS_LobbyDetails_CopyInfo(void* details, const void*, LobbyDetailsInfo** out) {
    if (!details || !out) return EOS_InvalidParameters;
    const gamenet::Lobby& lobby = static_cast<Details*>(details)->lobby;
    auto owned = std::make_unique<OwnedInfo>();
    owned->id = lobby.id;
    owned->api = LobbyDetailsInfo{1, owned->id.c_str(), Handle(lobby.owner), 0, lobby.maxMembers - lobby.count,
                                  lobby.maxMembers};
    *out = &owned->api;
    Infos().Give(std::move(owned), *out);
    return EOS_Success;
}
EXPORT void EOS_LobbyDetails_Info_Release(LobbyDetailsInfo* info) { Infos().Take(info); }
EXPORT void EOS_Lobby_Attribute_Release(Attribute* attribute) { Attributes().Take(attribute); }
namespace {
std::uint64_t AddLobbyNotify(int kind, void* clientData, void* callback) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    if (!callback) return 0;
    f.lobbyNotifies.push_back({f.nextNotify, kind, clientData, callback});
    return f.nextNotify++;
}
void RemoveLobbyNotify(std::uint64_t id) {
    Fake& f = F();
    const std::scoped_lock guard(f.lock);
    for (auto it = f.lobbyNotifies.begin(); it != f.lobbyNotifies.end(); ++it)
        if (it->id == id) {
            f.lobbyNotifies.erase(it);
            return;
        }
}
}  // namespace

EXPORT std::uint64_t EOS_Lobby_AddNotifyLobbyMemberStatusReceived(void*, const void*, void* clientData, void* callback) {
    return AddLobbyNotify(0, clientData, callback);
}
EXPORT std::uint64_t EOS_Lobby_AddNotifyLobbyMemberUpdateReceived(void*, const void*, void* clientData, void* callback) {
    return AddLobbyNotify(1, clientData, callback);
}
EXPORT std::uint64_t EOS_Lobby_AddNotifyLobbyUpdateReceived(void*, const void*, void* clientData, void* callback) {
    return AddLobbyNotify(2, clientData, callback);
}
EXPORT void EOS_Lobby_RemoveNotifyLobbyUpdateReceived(void*, std::uint64_t id) { RemoveLobbyNotify(id); }
EXPORT void EOS_Lobby_RemoveNotifyLobbyMemberUpdateReceived(void*, std::uint64_t id) { RemoveLobbyNotify(id); }
EXPORT void EOS_Lobby_RemoveNotifyLobbyMemberStatusReceived(void*, std::uint64_t id) { RemoveLobbyNotify(id); }

// --- everything else EDF.dll imports: reported, answers 0 ---
#define STUB(name)                       \
    EXPORT std::uint64_t name() {        \
        Unimplemented(#name);            \
        return 0;                        \
    }
#include "eos_stubs.inc"
#undef STUB
