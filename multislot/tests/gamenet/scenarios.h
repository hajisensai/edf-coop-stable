#pragma once
// The rooms GameNetTests plays (included by gamenet_test.cpp, after Spawned, Check and Result).

struct Seat {
    std::string user;  // EOS ProductUserId text
    std::string role;  // roles.cpp
    std::string ini;   // its EDF6Coop.ini
};

struct Scenario {
    std::string name;
    std::vector<Seat> seats;
    DWORD timeoutMs;  // above the longest any role waits, so a machine reports its own failure first
    void (*check)(const std::vector<Spawned>& machines, const gamenet::Network& network);
    // Imperfections of the network (net_shared.h: EDF6NET_DELAY, EDF6NET_DROP; roles.cpp: EDF6NET_CHATTER).
    std::vector<std::pair<std::string, std::string>> network = {};
};

// What every machine runs with: the room part on, no update checks, and the direct link's EOS hooks as players
// have them (Mode=off: no direct link, but its reliable layer and lobby glue sit on the game's EOS calls) - or,
// with `directLink` false, the game's EOS calls alone.
inline std::string BaseIni(const std::string& extra = "", bool directLink = true) {
    return "[MultiSlot]\r\nEnabled=1\r\nEightPlayerRooms=1\r\nMaxPlayers=8\r\nCrashLog=0\r\nNetLog=1\r\n" +
           std::string(directLink ? "[DirectNet]\r\nEnabled=1\r\nMode=off\r\nUPnP=0\r\n" : "[DirectNet]\r\nEnabled=0\r\n") +
           "[Update]\r\nAutoUpdate=0\r\nCheckEDF6VR=0\r\n" + extra;
}

// EOS refused nothing: no packet above its 1170 bytes, no inbox full.
inline void CheckEosAcceptedAll(const gamenet::Network& network) {
    Check(network.refused == 0, std::to_string(network.refused) + " packet(s) above EOS's 1170 bytes");
    Check(network.overflowed == 0, std::to_string(network.overflowed) + " packet(s) refused for a full inbox");
    Check(network.wire.dropped == 0, "the wire log kept every packet");
    std::uint32_t largest = 0;
    for (std::uint64_t at = 0; at + sizeof(gamenet::PacketHeader) <= network.wire.used;) {
        gamenet::PacketHeader header{};
        std::memcpy(&header, network.wire.bytes + at, sizeof(header));
        largest = (std::max)(largest, header.size);
        at += sizeof(header) + header.size;
    }
    std::printf("INFO: the largest packet on the wire had %u bytes\n", largest);
}

inline void CheckRoom(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    for (const auto& machine : machines) {
        int marked = 0;
        for (auto it = machine.results.equal_range("member"); it.first != it.second; ++it.first)
            marked += it.first->second.find(" marked") != std::string::npos ? 1 : 0;
        Check(marked == static_cast<int>(machines.size()),
              machine.user + " sees all " + std::to_string(machines.size()) + " members with the split sync marker");
        Check(machine.results.count("unimplemented-eos") == 0, machine.user +
                                                                   " reached no EOS function the fake only stubs (first: " +
                                                                   Result(machine, "unimplemented-eos") + ")");
    }
    Check(network.lobby.count == machines.size(), "EOS has every machine in the room");
}

// Every packet that crossed the fake EOS, in order.
struct WirePacket {
    gamenet::PacketHeader header;
    std::string data;
};
inline std::vector<WirePacket> Wire(const gamenet::Network& network) {
    std::vector<WirePacket> packets;
    for (std::uint64_t at = 0; at + sizeof(gamenet::PacketHeader) <= network.wire.used;) {
        WirePacket packet;
        std::memcpy(&packet.header, network.wire.bytes + at, sizeof(packet.header));
        packet.data.assign(reinterpret_cast<const char*>(network.wire.bytes + at + sizeof(packet.header)),
                           packet.header.size);
        at += sizeof(packet.header) + packet.header.size;
        packets.push_back(std::move(packet));
    }
    return packets;
}

inline void CheckLink(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    for (const auto& machine : machines) {
        int received = 0;
        for (auto it = machine.results.equal_range("received"); it.first != it.second; ++it.first)
            received += it.first->second.find("PROBE-PLAINTEXT-FROM-") != std::string::npos ? 1 : 0;
        Check(received == static_cast<int>(machines.size()) - 1,
              machine.user + " got every other machine's record through the game's controller");
    }
    const auto wire = Wire(network);
    bool plaintext = false;
    for (const auto& packet : wire) plaintext = plaintext || packet.data.find("PROBE-PLAINTEXT") != std::string::npos;
    Check(!wire.empty(), std::to_string(wire.size()) + " packets crossed EOS");
    Check(!plaintext, "no record crossed EOS in plaintext (the game encrypts every datagram)");
    CheckEosAcceptedAll(network);
}

struct MissionExpect {
    bool split;  // records go beside the start message
    bool late;   // they arrive after it, and the guests wait for them
    bool lossy;  // the network lost start messages, and the game resent them
};

// The mission start sync: everyone ends up with every player's record, byte for byte the same.
inline void CheckMission(const std::vector<Spawned>& machines, const gamenet::Network& network, MissionExpect expect) {
    CheckRoom(machines, network);
    const int members = static_cast<int>(machines.size());
    for (const auto& machine : machines) {
        Check(Result(machine, "sync").rfind("done", 0) == 0, machine.user + " finished the sync: " + Result(machine, "sync"));
        Check(Result(machine, "players") == std::to_string(members),
              machine.user + " has " + std::to_string(members) + " players (has " + Result(machine, "players") + ")");
        Check(Result(machine, "mission") == "7 3",
              machine.user + " starts the host's mission 7 on difficulty 3 (has " + Result(machine, "mission") + ")");
    }
    for (int slot = 0; slot < members; ++slot) {
        const std::string expected = std::to_string(slot) + " class=" + std::to_string(slot % 4) +
                                     " marker=" + std::to_string(500 + slot) + " armor=" + std::to_string(1000 + 37 * slot);
        std::string first;
        bool same = true;
        for (const auto& machine : machines) {
            std::string record;
            for (auto it = machine.results.equal_range("record"); it.first != it.second; ++it.first)
                if (it.first->second.rfind(std::to_string(slot) + " ", 0) == 0) record = it.first->second;
            Check(record.rfind(expected + " ", 0) == 0,
                  machine.user + " has player " + std::to_string(slot + 1) + "'s own loadout (" + record + ")");
            if (first.empty()) first = record;
            same = same && record == first;
        }
        Check(same, "every machine holds the same bytes for player " + std::to_string(slot + 1));
    }
    CheckEosAcceptedAll(network);
    const auto& host = machines.front();
    const bool sentBeside = host.text.find("sent beside the start message: result 0") != std::string::npos;
    Check(sentBeside == expect.split, expect.split ? "the host sent loadout records beside the start message"
                                                   : "the start message fit: nothing was sent beside it");
    int waited = 0;
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        const bool arrived = log.find("arrived beside the start message") != std::string::npos;
        Check(arrived == expect.split,
              machines[i].user + (expect.split ? " got records beside the start message" : " needed no records beside it"));
        Check(log.find("never arrived") == std::string::npos, machines[i].user + " missed no record");
        const std::size_t wait = log.find("MISSION sync: waited ");
        waited += wait != std::string::npos && log.find(": here", wait) != std::string::npos ? 1 : 0;
    }
    if (expect.late)
        Check(waited == members - 1, std::to_string(waited) + " of " + std::to_string(members - 1) +
                                         " guests read the start message before its records came and waited for them");
    if (expect.lossy)
        Check(network.dropped > 0, std::to_string(network.dropped) + " start message datagram(s) lost and resent by the game");
}
inline void CheckWholeMission(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, {false, false, false}); }
inline void CheckSplitMission(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, {true, false, false}); }
inline void CheckLateRecords(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, {true, true, false}); }
inline void CheckLossyMission(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, {true, false, true}); }
inline void CheckBusyMission(const std::vector<Spawned>& m, const gamenet::Network& n) {
    CheckMission(m, n, {true, false, false});
    Check(m.front().results.count("shared-batch") > 0,
          "the start message shared a controller record with a background message (" + Result(m.front(), "shared-batch") + ")");
}

// host0...01 hosts; guest...02 and on join, in that order.
inline std::vector<Seat> Seats(int count, const std::string& step, const std::string& ini) {
    std::vector<Seat> seats;
    for (int i = 0; i < count; ++i) {
        char user[33];
        std::snprintf(user, sizeof(user), "%s%027d", i ? "guest" : "host0", i + 1);
        seats.push_back({user, std::string(i ? "guest-" : "host-") + step, ini});
    }
    return seats;
}

inline const std::vector<Scenario>& Scenarios() {
    static const std::vector<Scenario> all = {
        {"room", Seats(2, "room", BaseIni()), 60000, &CheckRoom},
        {"link", Seats(2, "link", BaseIni()), 90000, &CheckLink},
        // Two players, a test budget small enough that the second record goes beside the start message: what two
        // people testing the split by hand ran (SplitSyncBudget).
        {"mission2", Seats(2, "mission", BaseIni("[Test]\r\nSplitSyncBudget=200\r\n")), 150000, &CheckSplitMission},
        // Four players: the start message fits and is the game's own.
        {"mission4", Seats(4, "mission", BaseIni()), 150000, &CheckWholeMission},
        // Eight players: their records make the start message 1132 bytes, above the 1100 it may have, so one goes
        // beside it.
        {"mission8", Seats(8, "mission", BaseIni()), 150000, &CheckSplitMission},
        // The records travel on a channel of their own, and EOS keeps no order between channels: here they come
        // 400 ms after the start message, which every guest reads first and has to wait at.
        {"mission8late", Seats(8, "mission", BaseIni()), 150000, &CheckLateRecords, {{"EDF6NET_DELAY", "77:400"}}},
        // Without the direct link's reliable layer the game's datagrams are unreliable: the network loses the first
        // start message, and the game's own resend has to bring it.
        {"mission8lossy", Seats(8, "mission", BaseIni("", false)), 150000, &CheckLossyMission, {{"EDF6NET_DROP", "900:1"}}},
        // Every frame every machine says something else too (an event message of 16 bytes), which shares the
        // controller record with the start message: the room the plugin leaves for it (kBatchedAllowance).
        {"mission8busy", Seats(8, "mission", BaseIni()), 150000, &CheckBusyMission, {{"EDF6NET_CHATTER", "16"}}},
    };
    return all;
}

inline const Scenario* FindScenario(const std::string& name) {
    for (const auto& scenario : Scenarios())
        if (scenario.name == name) return &scenario;
    return nullptr;
}
