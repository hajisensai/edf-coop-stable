// What each machine of a game-code test does once EDF.dll and EDF6Coop.dll are in (machine.cpp). Every EOS call
// goes through EDF.dll's import table, as the game makes it, so EDF6Coop's wrappers see it as they do in the game.
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <functional>
#include <vector>

#include "game.h"
#include "missionsync.h"
#include "machine.h"
#include "net_shared.h"
#include "../../../src/eos_min.h"
#include "../../src/netplayer.h"

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
    // EDF6NET_EPIC_MEMBERS: how many Epic's lobby lists when it holds fewer than the room (joinfull).
    if (GetEnvironmentVariableA("EDF6NET_EPIC_MEMBERS", text, sizeof(text))) return std::atoi(text);
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
        options.MaxLobbyMembers = gamenet::kMaxMachines;
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
// other member. `listen` runs between the two and subscribes what the machine receives: a member done with its own
// handshakes sends at once, while this machine may still be waiting for someone else's, and the game's controller
// delivers what arrives on every tick of that wait - to whoever listens then, and nowhere else (it acknowledges the
// record all the same, so its sender never sends it again). In the game the subscribers (the event controller among
// them) exist before the network does.
bool Connect(Machine& machine, const Room& room, Transport& transport, const std::function<bool()>& listen = {}) {
    if (!transport.Start(machine, room.lobby, room.members)) {
        Result("link", "the game's network objects could not be built");
        return false;
    }
    if (listen && !listen()) return false;
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
    std::vector<std::string> received;
    const auto listen = [&] {
        transport.Subscribe(kProbeType, [&](int from, const std::uint8_t* data, std::size_t size) {
            received.emplace_back(reinterpret_cast<const char*>(data), size);
            Result("received", "from %d: %.*s", from, static_cast<int>(size), reinterpret_cast<const char*>(data));
        });
        return true;
    };
    if (!Connect(machine, room, transport, listen)) return 1;
    if (host && sidePackets) SendSidePackets(machine, room, sidePackets);
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
int MissionIn(Machine& machine, bool host, const Room& room);

int Mission(Machine& machine, bool host) {
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    // EDF6NET_SETTLE=<ms>: stay in the room that long first (the lobby beat lets features come on, netfeature.h).
    char settle[16]{};
    if (GetEnvironmentVariableA("EDF6NET_SETTLE", settle, sizeof(settle)))
        TickUntil(machine, static_cast<unsigned>(std::atoi(settle)), [] { return false; });
    return MissionIn(machine, host, room);
}

// The start sync among `room.members` (in the order the game adds them), once everyone is in.
int MissionIn(Machine& machine, bool host, const Room& room) {
    int place = 0;
    while (room.members[static_cast<std::size_t>(place)] != machine.user) ++place;
    Loadout loadout{place % 4, 500 + place, 1000 + 37 * place, 100 + 37 * place};
    // EDF6NET_FIRST_WEAPON / EDF6NET_WEAPON_ROWS: this machine's weapons and its WEAPONTABLE (a mod's, say).
    char setting[16]{};
    if (GetEnvironmentVariableA("EDF6NET_FIRST_WEAPON", setting, sizeof(setting))) loadout.firstWeapon = std::atoi(setting);
    if (GetEnvironmentVariableA("EDF6NET_WEAPON_ROWS", setting, sizeof(setting)))
        loadout.weaponRows = static_cast<std::uint32_t>(std::atoi(setting));
    Transport transport;
    MissionSync sync;
    // The event controller listens before the handshake ends (Connect): a member done with its own handshakes may
    // be in the sync already.
    const auto listen = [&] {
        if (sync.Build(transport, room.members, loadout, host ? kHostMission : 0, host ? kHostDifficulty : 0)) return true;
        Result("mission", "the sync could not be set up");
        return false;
    };
    if (!Connect(machine, room, transport, listen)) return 1;
    Result("early-messages", "%zu", sync.Received());  // sync messages that came during the handshake
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
    struct Count {
        std::size_t states = 0, events = 0;
        ULONGLONG last = 0, gap = 0;
    };
    std::map<int, Count> got;          // by sender's network index
    std::map<std::string, Count> sent;  // by member
    const auto listen = [&] {
        transport.Subscribe(kStateType, [&](int from, const std::uint8_t*, std::size_t) {
            Count& c = got[from];
            const ULONGLONG now = GetTickCount64();
            if (c.last) c.gap = (std::max)(c.gap, now - c.last);
            c.last = now;
            ++c.states;
        });
        transport.Subscribe(kProbeType, [&](int from, const std::uint8_t*, std::size_t) { ++got[from].events; });
        return true;
    };
    if (!Connect(machine, room, transport, listen)) return 1;
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

// W2 player sync (netplayer.h): a player record as EDF6Coop extends it - the game's own fields written with the
// game's writers (mask, a float, the position as the game sends it), then the block as a bin element written by
// EDF6Coop's writer, then the same block written by the game's own bin writer (EDF+12B5200) - goes through the
// game's controller to the other machine, which reads it twice: with the game's readers and EDF6Coop's bin reader,
// and with the game's readers and the game's bin reader (EDF+12B49D0). Both must give back every block exactly.
constexpr std::uint32_t kPlayerType = 0x2701;  // a record type nothing in the game subscribes to
constexpr int kPlayerRecords = 8;
constexpr std::uintptr_t kSerializeWriter = 0x79A460, kSerializeReader = 0x79A430;
constexpr std::uintptr_t kWriteInt16 = 0x12B54E0, kWriteFloat = 0x12B5350, kWriteVec3 = 0x761B20, kWriteBin = 0x12B5200;
constexpr std::uintptr_t kReadInt16 = 0x12B4B80, kReadFloat = 0x12B4AD0, kReadVec3 = 0x760C50, kReadBin = 0x12B49D0;
struct alignas(16) GameSerialize {
    std::uint8_t bytes[0x600];
};

multislot::PlayerSample SampleFor(int seat, int i) {
    multislot::PlayerSample s;
    s.seq = static_cast<std::uint16_t>(100 + i);
    s.senderMs = 0xFFFFFFF0u + static_cast<std::uint32_t>(33 * i);  // across the wrap
    s.position = {700.3f + seat * 11.0f + i * 0.217f, 12.7f + i, -950.55f};
    s.velocity = {6.25f, -9.8f + i, 0.125f * seat};
    return s;
}
bool SameSample(const multislot::PlayerSample& a, const multislot::PlayerSample& b) {
    return a.seq == b.seq && a.senderMs == b.senderMs && !std::memcmp(&a.position, &b.position, sizeof(a.position)) &&
           !std::memcmp(&a.velocity, &b.velocity, sizeof(a.velocity));
}

int PlayerSync(Machine& machine, bool host) {
    using namespace multislot;
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    Transport transport;
    const int seat = host ? 0 : 1;
    int good = 0, bad = 0;
    const auto onRecord = [&](int from, const std::uint8_t* data, std::size_t size) {
        const Game& game = transport.game();
        const int i = static_cast<int>(data[0]);
        const PlayerSample expected = SampleFor(1 - seat, i);
        bool ok = size > 1;
        float halfError = 0.0f;
        for (int pass = 0; ok && pass < 2; ++pass) {
            GameSerialize reader{};
            game.Fn<void* (*)(void*, const void*, std::size_t)>(kSerializeReader)(reader.bytes, data + 1, size - 1);
            const std::uint16_t mask = game.Fn<std::uint16_t (*)(void*)>(kReadInt16)(reader.bytes);
            const float heading = game.Fn<float (*)(void*)>(kReadFloat)(reader.bytes);
            alignas(16) float position[4]{};
            game.Fn<bool (*)(void*, float*)>(kReadVec3)(reader.bytes, position);
            ok = ok && mask == (0x58 | kPlayerBlockBit) && std::fabs(heading - 1.5f) < 0.01f;
            halfError = (std::max)(halfError, std::fabs(position[0] - expected.position.x));
            for (int copy = 0; copy < 2; ++copy) {
                std::uint8_t block[64]{};
                std::size_t got = 0;
                if (pass == 0) {
                    ok = ok && ReadBinElement(reader.bytes, block, sizeof(block), got);
                } else {
                    std::size_t capacity = sizeof(block);
                    ok = ok && game.Fn<bool (*)(void*, void*, std::size_t*)>(kReadBin)(reader.bytes, block, &capacity);
                    got = capacity;
                }
                PlayerSample sample;
                ok = ok && got == kPlayerBlockBytes && DecodePlayerBlock(block, got, sample) && SameSample(sample, expected);
            }
        }
        if (ok)
            ++good;
        else
            ++bad;
        Result("player-record", "from %d #%d %s; the game's half-float position is off by %.3f m", from, i,
               ok ? "ok" : "MISMATCH", halfError);
    };
    const auto listen = [&] {
        transport.Subscribe(kPlayerType, onRecord);
        return true;
    };
    if (!Connect(machine, room, transport, listen)) return 1;
    const Game& game = transport.game();
    for (int i = 0; i < kPlayerRecords; ++i) {
        GameSerialize writer{};
        game.Fn<void* (*)(void*)>(kSerializeWriter)(writer.bytes);
        const PlayerSample sample = SampleFor(seat, i);
        game.Fn<bool (*)(void*, std::int16_t)>(kWriteInt16)(writer.bytes, static_cast<std::int16_t>(0x58 | kPlayerBlockBit));
        game.Fn<bool (*)(void*, float)>(kWriteFloat)(writer.bytes, 1.5f);
        alignas(16) const float position[4] = {sample.position.x, sample.position.y, sample.position.z, 1.0f};
        game.Fn<bool (*)(void*, const float*)>(kWriteVec3)(writer.bytes, position);
        std::uint8_t block[kPlayerBlockBytes];
        EncodePlayerBlock(sample, block, sizeof(block));
        const bool ours = WriteBinElement(writer.bytes, block, sizeof(block));
        game.Fn<bool (*)(void*, const void*, std::size_t)>(kWriteBin)(writer.bytes, block, sizeof(block));
        std::uint64_t length = 0;
        std::memcpy(&length, writer.bytes + kSerializeEnd, sizeof(length));
        std::vector<std::uint8_t> record(1 + length);
        record[0] = static_cast<std::uint8_t>(i);
        std::memcpy(record.data() + 1, writer.bytes + kSerializeData, length);
        for (const auto& member : room.members)
            if (member != machine.user && (!ours || !transport.SendReliable(member, kPlayerType, record.data(), record.size())))
                Result("send", "player record %d to %s refused", i, member.c_str());
        if (i == 0) Result("player-record-bytes", "%llu", static_cast<unsigned long long>(length));
    }
    const bool all = TickUntil(machine, 10000, [&] {
        transport.Tick();
        return good + bad >= kPlayerRecords;
    });
    TickUntil(machine, 1500, [&] {
        transport.Tick();
        return false;
    });
    Result("player-records", "%d ok %d bad", good, bad);
    return all && bad == 0 ? 0 : 1;
}

// I1: rooms above Epic's lobby (EDF6NET_LOBBY_CAP=2 plays Epic's 64 small). The host and the first guest are in
// Epic's lobby; the last guest finds it full and comes in over the direct link to the host, which lets it in by its
// own member list.
// The game's P2P receive, as its network polls it every frame: EDF6Coop learns the local EOS user from it (a direct
// link host welcomes nobody before), and it drains what arrives.
void PollP2P(const Machine& machine) {
    EOS_P2P_ReceivePacketOptions options{2, static_cast<EOS_ProductUserId>(const_cast<void*>(Self(machine))), 4096, nullptr};
    std::vector<std::uint8_t> data(4096);
    EOS_ProductUserId peer = nullptr;
    EOS_P2P_SocketId socket{};
    std::uint8_t channel = 0;
    std::uint32_t size = 0;
    const auto receive = Import<EOS_EResult (*)(void*, const EOS_P2P_ReceivePacketOptions*, EOS_ProductUserId*, EOS_P2P_SocketId*,
                                                std::uint8_t*, void*, std::uint32_t*)>(machine, "EOS_P2P_ReceivePacket");
    while (receive(kPlatform, &options, &peer, &socket, &channel, data.data(), &size) == EOS_Success) {
    }
}

// The game's lobby manager listens to member statuses (012B3380 registers one handler): what EDF6Coop tells the game
// about members beyond Epic's lobby goes there, and the room the game has follows it. Each one is reported
// ("member-status <id> <status>", EOS_ELobbyMemberStatus: 0 joined, 1 left).
struct MemberStatusInfo {  // EOS_Lobby_LobbyMemberStatusReceivedCallbackInfo
    void* ClientData;
    const char* LobbyId;
    const void* TargetUserId;
    std::int32_t CurrentStatus;
};
void OnMemberStatus(const MemberStatusInfo* info) {
    char text[64]{};
    std::int32_t length = sizeof(text);
    if (info && info->TargetUserId)
        FakeExport<std::int32_t (*)(const void*, char*, std::int32_t*)>("EOS_ProductUserId_ToString")(info->TargetUserId, text, &length);
    Result("member-status", "%s %d", text, info ? info->CurrentStatus : -1);
}
void ListenToMembers(const Machine& machine) {
    struct Options {
        std::int32_t ApiVersion;
    } options{1};
    Import<std::uint64_t (*)(void*, const void*, void*, void (*)(const MemberStatusInfo*))>(
        machine, "EOS_Lobby_AddNotifyLobbyMemberStatusReceived")(kLobbyInterface, &options, nullptr, &OnMemberStatus);
}

// The room's members as the game reads them when it is told one joined (12BD460: EOS_Lobby_CopyLobbyDetailsHandle,
// then EOS_LobbyDetails_GetMemberCount / GetMemberByIndex, through EDF.dll's imports - where EDF6Coop's wrappers
// are), in that order: each gets an eos::User (Users::Add) flagged as in the room (User+0x10 bit 2), and those are
// what the room member list (7468C0: the room screen, the voice chat HUD) and the start sync have.
std::vector<std::string> GameRoomMembers(const Machine& machine, const std::string& lobby) {
    struct CopyOptions {
        std::int32_t ApiVersion;
        const char* LobbyId;
        const void* LocalUserId;
    } copy{1, lobby.c_str(), Self(machine)};
    void* details = nullptr;
    std::vector<std::string> members;
    if (Import<std::int32_t (*)(void*, const void*, void**)>(machine, "EOS_Lobby_CopyLobbyDetailsHandle")(kLobbyInterface, &copy,
                                                                                                          &details) != 0 ||
        !details)
        return members;
    struct CountOptions {
        std::int32_t ApiVersion;
    } countOptions{1};
    const std::uint32_t count =
        Import<std::uint32_t (*)(void*, const void*)>(machine, "EOS_LobbyDetails_GetMemberCount")(details, &countOptions);
    for (std::uint32_t i = 0; i < count && i < 64; ++i) {
        struct ByIndex {
            std::int32_t ApiVersion;
            std::uint32_t MemberIndex;
        } byIndex{1, i};
        const void* member = Import<const void* (*)(void*, const void*)>(machine, "EOS_LobbyDetails_GetMemberByIndex")(details, &byIndex);
        char text[64]{};
        std::int32_t length = sizeof(text);
        if (member) FakeExport<std::int32_t (*)(const void*, char*, std::int32_t*)>("EOS_ProductUserId_ToString")(member, text, &length);
        members.push_back(text);
    }
    Import<void (*)(void*)>(machine, "EOS_LobbyDetails_Release")(details);
    return members;
}

// Waits until the game reads `expected` members in the room, reports them ("game-members <n> <ids>"), and plays the
// start sync among them in that order.
int FullRoomMission(Machine& machine, bool host, Room room, std::size_t expected) {
    std::vector<std::string> members;
    TickUntil(machine, 20000, [&] {
        PollP2P(machine);
        members = GameRoomMembers(machine, room.lobby);
        return members.size() >= expected;
    });
    std::string list;
    for (const auto& member : members) list += " " + member;
    Result("game-members", "%zu%s", members.size(), list.c_str());
    Result("room-count", "%u", FakeExport<std::uint32_t (*)()>("FakeNet_RoomCount")());
    if (members.size() < expected) return 1;
    room.members = members;
    return MissionIn(machine, host, room);
}

// EDF6NET_ROOM_MEMBERS: how many the room holds once everyone is in (joinfull: 3).
std::size_t RoomMembers() {
    char text[16]{};
    GetEnvironmentVariableA("EDF6NET_ROOM_MEMBERS", text, sizeof(text));
    return text[0] ? static_cast<std::size_t>(std::atoi(text)) : 3;
}

int FullRoom(Machine& machine, bool host) {
    ListenToMembers(machine);
    Room room;
    if (!EnterRoom(machine, host, room)) return 1;
    return FullRoomMission(machine, host, room, RoomMembers());
}

int FullJoin(Machine& machine) {
    ListenToMembers(machine);
    const auto roomDetails = FakeExport<void* (*)()>("FakeNet_RoomDetails");
    const auto roomCount = FakeExport<std::uint32_t (*)()>("FakeNet_RoomCount");
    const auto copyByKey = FakeExport<std::int32_t (*)(void*, const void*, void**)>("EOS_LobbyDetails_CopyAttributeByKey");
    const auto release = FakeExport<void (*)(void*)>("EOS_LobbyDetails_Release");
    struct ByKey {
        std::int32_t ApiVersion;
        const char* AttrKey;
    } key{1, "EDF6DN_HOSTADDR"};
    void* details = nullptr;
    // Epic's lobby full, and its host's direct-link address on it.
    const bool ready = TickUntil(machine, 20000, [&] {
        if (details) release(details);
        details = roomCount() >= 2 ? roomDetails() : nullptr;
        void* attribute = nullptr;
        if (!details || copyByKey(details, &key, &attribute) != 0) return false;
        FakeExport<void (*)(void*)>("EOS_Lobby_Attribute_Release")(attribute);
        return true;
    });
    if (!ready) {
        Result("fulljoin", "no full room with its host's address to join");
        return 1;
    }
    Entered entered;
    JoinOptions options;
    options.LobbyDetailsHandle = details;
    options.LocalUserId = Self(machine);
    Import<LobbyCall>(machine, "EOS_Lobby_JoinLobby")(kLobbyInterface, &options, &entered, &OnEntered);
    TickUntil(machine, 30000, [&] {
        PollP2P(machine);
        return entered.done;
    });
    release(details);
    Result("fulljoin", "%s %d", entered.done ? "completed" : "NOT completed", entered.result);
    if (!entered.done || entered.result != 0) return 1;
    Room room;
    room.lobby = entered.lobby;
    return FullRoomMission(machine, false, room, RoomMembers());
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
    if (step == "playersync") return PlayerSync(machine, host);
    if (step == "netstats") return NetStats(machine, host);
    if (step == "versiongate") return VersionGate(machine, host);
    if (step == "bulk") return Bulk(machine, host);
    if (step == "fullroom") return FullRoom(machine, host);
    if (step == "fulljoin") return FullJoin(machine);
    Result("role", "unknown role %s", role.c_str());
    return 2;
}

}  // namespace gamenet
