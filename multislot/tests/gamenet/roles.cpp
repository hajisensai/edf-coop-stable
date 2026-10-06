// What each machine of a game-code test does once EDF.dll and EDF6Coop.dll are in (machine.cpp). Every EOS call
// goes through EDF.dll's import table, as the game makes it, so EDF6Coop's wrappers see it as they do in the game.
#include <cstring>
#include <map>
#include <functional>
#include <vector>

#include "game.h"
#include "missionsync.h"
#include "machine.h"
#include "net_shared.h"
#include "../../../src/eos_min.h"

namespace gamenet {
namespace {

constexpr const char* kSplitSyncKey = "EDF6MS_SPLITSYNC";  // syncmarker.h
void* const kPlatform = reinterpret_cast<void*>(0x1000);     // what the fake's EOS_Platform_Create hands out
void* const kLobbyInterface = reinterpret_cast<void*>(0x3000);

template <typename T>
T Import(const Machine& machine, const char* name) {
    return reinterpret_cast<T>(GameImport(machine, name));
}
template <typename T>
T FakeExport(const char* name) {
    return reinterpret_cast<T>(GetProcAddress(GetModuleHandleW(L"EOSSDK-Win64-Shipping.dll"), name));
}

// One frame of the game's EOS: EOS_Platform_Tick through EDF.dll's import, where EDF6Coop watches the room.
void Tick(const Machine& machine) { Import<void (*)(void*)>(machine, "EOS_Platform_Tick")(kPlatform); }

// Ticks every ~16 ms until `done` or `timeoutMs`.
bool TickUntil(const Machine& machine, unsigned timeoutMs, const std::function<bool()>& done) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        Tick(machine);
        if (done()) return true;
        if (GetTickCount64() > deadline) return false;
        Sleep(16);
    }
}

const void* Self(const Machine& machine) { return FakeExport<const void* (*)(const char*)>("FakeNet_User")(machine.user.c_str()); }

struct LobbyCallbackInfo {
    std::int32_t ResultCode;
    void* ClientData;
    const char* LobbyId;
};
struct Entered {
    bool done = false;
    std::int32_t result = -1;
    std::string lobby;
};
void OnEntered(const LobbyCallbackInfo* info) {
    auto* entered = static_cast<Entered*>(info->ClientData);
    entered->done = true;
    entered->result = info->ResultCode;
    entered->lobby = info->LobbyId ? info->LobbyId : "";
}

// The leading fields of EOS_Lobby_CreateLobbyOptions / JoinLobbyOptions; the rest stays zero.
struct CreateOptions {
    std::int32_t ApiVersion = 8;
    const void* LocalUserId = nullptr;
    std::uint32_t MaxLobbyMembers = 0;
    std::uint8_t rest[256]{};
};
struct JoinOptions {
    std::int32_t ApiVersion = 5;
    void* LobbyDetailsHandle = nullptr;
    const void* LocalUserId = nullptr;
    std::uint8_t rest[256]{};
};
using LobbyCall = void (*)(void*, const void*, void*, void (*)(const LobbyCallbackInfo*));

// The room's members as EOS lists them, and whether each publishes the split start message marker.
struct Seen {
    std::vector<std::string> members;
    std::vector<bool> marked;
};
Seen LookAtRoom(const Machine& machine, const std::string& lobby) {
    Seen seen;
    struct CopyOptions {
        std::int32_t ApiVersion;
        const char* LobbyId;
        const void* LocalUserId;
    } copy{1, lobby.c_str(), Self(machine)};
    void* details = nullptr;
    if (FakeExport<std::int32_t (*)(void*, const void*, void**)>("EOS_Lobby_CopyLobbyDetailsHandle")(kLobbyInterface, &copy,
                                                                                                    &details))
        return seen;
    const std::uint32_t count = FakeExport<std::uint32_t (*)(void*, const void*)>("EOS_LobbyDetails_GetMemberCount")(details, nullptr);
    for (std::uint32_t i = 0; i < count; ++i) {
        struct ByIndex {
            std::int32_t ApiVersion;
            std::uint32_t MemberIndex;
        } byIndex{1, i};
        const void* member = FakeExport<const void* (*)(void*, const void*)>("EOS_LobbyDetails_GetMemberByIndex")(details, &byIndex);
        char text[64]{};
        std::int32_t length = sizeof(text);
        FakeExport<std::int32_t (*)(const void*, char*, std::int32_t*)>("EOS_ProductUserId_ToString")(member, text, &length);
        struct ByKey {
            std::int32_t ApiVersion;
            const void* TargetUserId;
            const char* AttrKey;
        } byKey{1, member, kSplitSyncKey};
        // As the plugin reads it (syncmarker.cpp): an int64 of kSplitSyncFormat (1) or more.
        struct AttributeData {
            std::int32_t ApiVersion;
            const char* Key;
            std::int64_t AsInt64;
            std::int32_t ValueType;
        };
        struct Attribute {
            std::int32_t ApiVersion;
            const AttributeData* Data;
            std::int32_t Visibility;
        };
        Attribute* attribute = nullptr;
        const bool marked = FakeExport<std::int32_t (*)(void*, const void*, Attribute**)>(
                                "EOS_LobbyDetails_CopyMemberAttributeByKey")(details, &byKey, &attribute) == 0 &&
                            attribute && attribute->Data && attribute->Data->ValueType == 1 && attribute->Data->AsInt64 >= 1;
        if (attribute) FakeExport<void (*)(void*)>("EOS_Lobby_Attribute_Release")(attribute);
        seen.members.push_back(text);
        seen.marked.push_back(marked);
    }
    FakeExport<void (*)(void*)>("EOS_LobbyDetails_Release")(details);
    return seen;
}

bool AllMarked(const Seen& seen, std::size_t members) {
    if (seen.members.size() != members) return false;
    for (bool marked : seen.marked)
        if (!marked) return false;
    return true;
}

int expectedMembers() {
    char text[16]{};
    GetEnvironmentVariableA("EDF6NET_MEMBERS", text, sizeof(text));
    return text[0] ? std::atoi(text) : 2;
}

struct Room {
    std::string lobby;
    std::vector<std::string> members;  // in lobby order, as the game adds them
};

// Creates (host) or joins (guest) the room the way the game does, then waits until every member is in and
// publishes the split start message marker (EDF6Coop does that on its own, from the game's EOS ticks).
bool EnterRoom(Machine& machine, bool host, Room& room) {
    Entered entered;
    if (host) {
        CreateOptions options;
        options.LocalUserId = Self(machine);
        options.MaxLobbyMembers = 8;
        Import<LobbyCall>(machine, "EOS_Lobby_CreateLobby")(kLobbyInterface, &options, &entered, &OnEntered);
    } else {
        // Guests come in one after another, in seat order (EDF6NET_SEAT): seat n joins once n are in.
        char seatText[8]{};
        GetEnvironmentVariableA("EDF6NET_SEAT", seatText, sizeof(seatText));
        const std::uint32_t seat = static_cast<std::uint32_t>(std::atoi(seatText));
        void* details = nullptr;
        const auto roomDetails = FakeExport<void* (*)()>("FakeNet_RoomDetails");
        const auto roomCount = FakeExport<std::uint32_t (*)()>("FakeNet_RoomCount");
        if (!TickUntil(machine, 10000, [&] {
                if (roomCount() < seat) return false;
                return (details = roomDetails()) != nullptr;
            })) {
            Result("room", "no room to join");
            return false;
        }
        JoinOptions options;
        options.LobbyDetailsHandle = details;
        options.LocalUserId = Self(machine);
        Import<LobbyCall>(machine, "EOS_Lobby_JoinLobby")(kLobbyInterface, &options, &entered, &OnEntered);
        if (TickUntil(machine, 5000, [&] { return entered.done; })) FakeExport<void (*)(void*)>("EOS_LobbyDetails_Release")(details);
    }
    if (!TickUntil(machine, 5000, [&] { return entered.done; }) || entered.result != 0) {
        Result("room", "%s failed (%d)", host ? "create" : "join", entered.result);
        return false;
    }
    room.lobby = entered.lobby;
    Result("lobby", "%s", room.lobby.c_str());
    const std::size_t members = static_cast<std::size_t>(expectedMembers());
    // EDF6NET_RUSH=1: go on as soon as everyone is in, before their markers can have reached the others (a mission
    // started right after someone joined).
    char rush[4]{};
    GetEnvironmentVariableA("EDF6NET_RUSH", rush, sizeof(rush));
    const bool everyone = TickUntil(machine, 20000, [&] {
        const Seen seen = LookAtRoom(machine, room.lobby);
        return rush[0] == '1' ? seen.members.size() == members : AllMarked(seen, members);
    });
    const Seen seen = LookAtRoom(machine, room.lobby);
    for (std::size_t i = 0; i < seen.members.size(); ++i)
        Result("member", "%s %s", seen.members[i].c_str(), seen.marked[i] ? "marked" : "unmarked");
    room.members = seen.members;
    return everyone;
}

// Starts the game's packet controller for the room and waits until the game's P2P handshake connected every
// other member.
bool Connect(Machine& machine, const Room& room, Transport& transport) {
    if (!transport.Start(machine, room.lobby, room.members)) {
        Result("link", "the game's network objects could not be built");
        return false;
    }
    const bool connected = TickUntil(machine, 15000, [&] {
        transport.Tick();
        for (const auto& member : room.members)
            if (member != machine.user && !transport.Connected(member)) return false;
        return true;
    });
    for (const auto& member : room.members)
        Result("network-index", "%s %d%s", member.c_str(), transport.NetworkIndex(member),
               member == machine.user ? " (self)" : transport.Connected(member) ? " connected" : " NOT connected");
    return connected;
}

constexpr std::uint32_t kProbeType = 0x2700;  // a record type nothing in the game subscribes to

// Each machine sends every other one a reliable record through the game's controller and waits for theirs.
// Side packets as EDF6Coop sends them beside a split start message (packetfit.cpp: kSideMagic, index, size, hash,
// the record), through EDF.dll's EOS import, to everyone else: a game without EDF6Coop must take no harm from them.
void SendSidePackets(const Machine& machine, const Room& room, int count) {
    const auto send = Import<std::int32_t (*)(void*, const EOS_P2P_SendPacketOptions*)>(machine, "EOS_P2P_SendPacket");
    EOS_P2P_SocketId socket{1, {}};
    strncpy_s(socket.SocketName, room.lobby.c_str(), _TRUNCATE);  // the game's socket is named after the lobby
    for (const auto& member : room.members) {
        if (member == machine.user) continue;
        for (int i = 0; i < count; ++i) {
            std::vector<std::uint8_t> packet = {'M', 'S', 'l', 'o', 't', 'S', 'i', 'd', 1, 141, 0};
            for (int b = 0; b < 8 + 141; ++b) packet.push_back(static_cast<std::uint8_t>(b * 37 + i));
            EOS_P2P_SendPacketOptions options{};
            options.ApiVersion = 3;
            options.LocalUserId = static_cast<EOS_ProductUserId>(const_cast<void*>(Self(machine)));
            options.RemoteUserId =
                static_cast<EOS_ProductUserId>(const_cast<void*>(FakeExport<const void* (*)(const char*)>("FakeNet_User")(member.c_str())));
            options.SocketId = &socket;
            options.Channel = 0x4D;  // packetfit.h: kSideChannel
            options.DataLengthBytes = static_cast<std::uint32_t>(packet.size());
            options.Data = packet.data();
            options.Reliability = EOS_PR_ReliableOrdered;
            options.bAllowDelayedDelivery = 1;
            Result("side-packet", "to %s: result %d", member.c_str(), send(kPlatform, &options));
        }
    }
}

int Link(Machine& machine, bool host, int sidePackets = 0) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    Transport transport;
    if (!Connect(machine, room, transport)) return 1;
    if (host && sidePackets) SendSidePackets(machine, room, sidePackets);
    std::vector<std::string> received;
    transport.Subscribe(kProbeType, [&](int from, const std::uint8_t* data, std::size_t size) {
        received.emplace_back(reinterpret_cast<const char*>(data), size);
        Result("received", "from %d: %.*s", from, static_cast<int>(size), reinterpret_cast<const char*>(data));
    });
    const std::string probe = "PROBE-PLAINTEXT-FROM-" + machine.user;
    for (const auto& member : room.members)
        if (member != machine.user && !transport.SendReliable(member, kProbeType, probe.data(), probe.size()))
            Result("send", "SendReliable to %s refused", member.c_str());
    const std::size_t others = room.members.size() - 1;
    const bool all = TickUntil(machine, 10000, [&] {
        transport.Tick();
        return received.size() >= others;
    });
    // Keep ticking a little: acknowledgements and resends of the others still need this machine.
    TickUntil(machine, 1500, [&] {
        transport.Tick();
        return false;
    });
    return all ? 0 : 1;
}

constexpr std::int32_t kSyncId = 0x5EED;  // the id the mission script passes (any; every machine the same)
constexpr std::int32_t kHostMission = 7, kHostDifficulty = 3;

// FNV-1a of a record, for comparing machines.
std::uint64_t Fnv(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (std::uint8_t b : bytes) hash = (hash ^ b) * 1099511628211ull;
    return hash;
}

// Keeps the room's network going until every machine is done, as players stay in the room.
void StayUntilEveryoneIsDone(Machine& machine, Transport& transport, const MissionSync& sync, std::size_t members) {
    const auto finish = FakeExport<std::uint32_t (*)(int)>("FakeNet_Finish");
    finish(1);
    const auto frame = [&] {
        transport.Tick();
        sync.EndFrame();
    };
    TickUntil(machine, 15000, [&] {
        frame();
        return finish(0) >= members;
    });
    TickUntil(machine, 300, [&] {
        frame();
        return false;
    });
}

// EDF6NET_CHATTER=<bytes>: every frame, every machine also sends an event message of that size to everyone.
std::size_t ChatterBytes() {
    char text[16]{};
    GetEnvironmentVariableA("EDF6NET_CHATTER", text, sizeof(text));
    return static_cast<std::size_t>(std::atoi(text));
}

// The mission start sync: every machine runs MissionSync_Begin and then MissionSync_Update until it answers 0,
// and reports what its game holds afterwards.
int Mission(Machine& machine, bool host) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    // EDF6NET_SETTLE=<ms>: stay in the room that long first (the lobby beat lets features come on, netfeature.h).
    char settle[16]{};
    if (GetEnvironmentVariableA("EDF6NET_SETTLE", settle, sizeof(settle)))
        TickUntil(machine, static_cast<unsigned>(std::atoi(settle)), [] { return false; });
    Transport transport;
    if (!Connect(machine, room, transport)) return 1;
    int place = 0;
    while (room.members[static_cast<std::size_t>(place)] != machine.user) ++place;
    Loadout loadout{place % 4, 500 + place, 1000 + 37 * place, 100 + 37 * place};
    // EDF6NET_FIRST_WEAPON / EDF6NET_WEAPON_ROWS: this machine's weapons and its WEAPONTABLE (a mod's, say).
    char setting[16]{};
    if (GetEnvironmentVariableA("EDF6NET_FIRST_WEAPON", setting, sizeof(setting))) loadout.firstWeapon = std::atoi(setting);
    if (GetEnvironmentVariableA("EDF6NET_WEAPON_ROWS", setting, sizeof(setting)))
        loadout.weaponRows = static_cast<std::uint32_t>(std::atoi(setting));
    MissionSync sync;
    if (!sync.Build(transport, room.members, loadout, host ? kHostMission : 0, host ? kHostDifficulty : 0)) {
        Result("mission", "the sync could not be set up");
        return 1;
    }
    Result("loadout", "%d class=%d marker=%d armor=%d", place, loadout.soldierClass, loadout.marker, loadout.armor);
    const std::size_t chatter = ChatterBytes();
    // A frame of the game: other messages, the network, the mission script, then the event builders go out.
    sync.Begin(kSyncId);
    sync.EndFrame();
    std::int32_t answer = 1;
    const ULONGLONG start = GetTickCount64();
    const bool done = TickUntil(machine, 30000, [&] {
        if (chatter) sync.Chatter(chatter);
        transport.Tick();
        answer = sync.Update(kSyncId);
        sync.EndFrame();
        return answer == 0;
    });
    Result("sync", "%s after %llu ms (MissionSync_Update answered %d)", done ? "done" : "NOT done",
           GetTickCount64() - start, answer);
    if (!done) sync.Dump();
    const std::int32_t players = sync.Players();
    Result("players", "%d", players);
    Result("mission", "%d %d", sync.Mission(), sync.Difficulty());
    for (int slot = 0; slot < static_cast<int>(room.members.size()); ++slot) {
        const auto record = sync.Record(slot);
        if (record.size() < 0x24) {
            Result("record", "%d unavailable", slot);
            continue;
        }
        std::int32_t soldierClass = 0, marker = 0, armor = 0;
        std::memcpy(&soldierClass, record.data(), 4);
        std::memcpy(&marker, record.data() + 4, 4);
        std::memcpy(&armor, record.data() + 0x20, 4);
        std::int32_t weapons[6]{};
        std::memcpy(weapons, record.data() + 8, sizeof(weapons));  // the six ids (weaponguard.h: kLoadoutWeapons)
        Result("record", "%d class=%d marker=%d armor=%d weapons=%d,%d,%d,%d,%d,%d fnv=%016llx", slot, soldierClass, marker,
               armor, weapons[0], weapons[1], weapons[2], weapons[3], weapons[4], weapons[5], Fnv(record));
    }
    StayUntilEveryoneIsDone(machine, transport, sync, room.members.size());
    return done ? 0 : 1;
}


// Netcode rewrite W1: what the game's controller sends in a room, as the traffic classes see it. Every machine sends
// every other one a state-like record (kStateType, unreliable, every 4th frame, a size no other datagram has) and
// now and then a reliable one, for EDF6NET_SECONDS (default 4) s, with [Netcode] StatsSeconds=1 in its INI.
constexpr std::uint32_t kStateType = 0x2800;
constexpr std::size_t kStateBytes = 77;  // a datagram of only this record: 8 + 4 + 77 = 89 bytes

int Seconds(int fallback) {
    char text[16]{};
    GetEnvironmentVariableA("EDF6NET_SECONDS", text, sizeof(text));
    return text[0] ? std::atoi(text) : fallback;
}

int NetStats(Machine& machine, bool host) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    Transport transport;
    if (!Connect(machine, room, transport)) return 1;
    struct Count {
        std::size_t states = 0, events = 0;
        ULONGLONG last = 0, gap = 0;
    };
    std::map<int, Count> got;          // by sender's network index
    std::map<std::string, Count> sent;  // by member
    transport.Subscribe(kStateType, [&](int from, const std::uint8_t*, std::size_t) {
        Count& c = got[from];
        const ULONGLONG now = GetTickCount64();
        if (c.last) c.gap = (std::max)(c.gap, now - c.last);
        c.last = now;
        ++c.states;
    });
    transport.Subscribe(kProbeType, [&](int from, const std::uint8_t*, std::size_t) { ++got[from].events; });
    const std::vector<std::uint8_t> state(kStateBytes, 0x5A);
    const std::string probe = "EVENT-" + machine.user;
    int frame = 0;
    TickUntil(machine, static_cast<unsigned>(Seconds(4)) * 1000, [&] {
        for (const auto& member : room.members) {
            if (member == machine.user) continue;
            if (frame % 4 == 0 && transport.SendUnreliable(member, kStateType, state.data(), state.size()))
                ++sent[member].states;
            if (frame % 30 == 15 && transport.SendReliable(member, kProbeType, probe.data(), probe.size()))
                ++sent[member].events;
        }
        ++frame;
        transport.Tick();
        return false;
    });
    // The others still send: keep receiving (and acknowledging) until everyone is done.
    const auto finish = FakeExport<std::uint32_t (*)(int)>("FakeNet_Finish");
    finish(1);
    TickUntil(machine, 15000, [&] {
        transport.Tick();
        return finish(0) >= room.members.size();
    });
    TickUntil(machine, 1500, [&] {
        transport.Tick();
        return false;
    });
    for (const auto& [member, c] : sent) Result("sent-to", "%s states=%zu events=%zu", member.c_str(), c.states, c.events);
    for (const auto& member : room.members) {
        if (member == machine.user) continue;
        const Count& c = got[transport.NetworkIndex(member)];
        Result("got-from", "%s states=%zu events=%zu gap=%llu", member.c_str(), c.states, c.events, c.gap);
    }
    return 0;
}

// Netcode rewrite W1: a message of 1024 loadout records' size (146432 bytes) from the host to every guest, in
// fragments ([Test] BulkEcho=1 logs what arrives).
int Bulk(Machine& machine, bool host) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    Transport transport;
    if (!Connect(machine, room, transport)) return 1;
    if (host) {
        std::vector<std::uint8_t> message(1024 * 143);
        for (std::size_t i = 0; i < message.size(); ++i) message[i] = static_cast<std::uint8_t>(i * 31 + (i >> 9));
        Result("bulk-fnv", "%016llx", Fnv(message));
        using SendBulk = bool (*)(const char*, std::uint16_t, const void*, std::size_t);
        const auto send = reinterpret_cast<SendBulk>(GetProcAddress(machine.plugin, "EDF6Coop_SendBulk"));
        std::size_t done = 0;
        // Until the room shows that everyone reads fragments (the lobby beat, once a second).
        TickUntil(machine, 8000, [&] {
            transport.Tick();
            done = 0;
            for (const auto& member : room.members)
                if (member != machine.user && send && send(member.c_str(), 7, message.data(), message.size())) ++done;
            return done == room.members.size() - 1;
        });
        Result("bulk", "%s to %zu", done == room.members.size() - 1 ? "sent" : "NOT sent", done);
    }
    const auto finish = FakeExport<std::uint32_t (*)(int)>("FakeNet_Finish");
    TickUntil(machine, 4000, [&] {
        transport.Tick();
        return false;
    });
    finish(1);
    TickUntil(machine, 10000, [&] {
        transport.Tick();
        return finish(0) >= room.members.size();
    });
    return 0;
}

// Netcode rewrite W1: the version gate. Every machine stays in the room for EDF6NET_SECONDS (default 5) s, ticking,
// and says how many the room has at the end.
int VersionGate(Machine& machine, bool host) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    TickUntil(machine, static_cast<unsigned>(Seconds(5)) * 1000, [] { return false; });
    Result("room-count", "%u", FakeExport<std::uint32_t (*)()>("FakeNet_RoomCount")());
    return 0;
}

}  // namespace

int RunRole(Machine& machine, const std::string& role) {
    const bool host = role.rfind("host-", 0) == 0;
    const std::string step = role.substr(role.find('-') + 1);
    if (step == "room") {
        Room room;
        return EnterRoom(machine, host, room) ? 0 : 1;
    }
    if (step == "link") return Link(machine, host);
    if (step == "sidelink") return Link(machine, host, 5);
    if (step == "mission") return Mission(machine, host);
    if (step == "netstats") return NetStats(machine, host);
    if (step == "versiongate") return VersionGate(machine, host);
    if (step == "bulk") return Bulk(machine, host);
    Result("role", "unknown role %s", role.c_str());
    return 2;
}

}  // namespace gamenet
