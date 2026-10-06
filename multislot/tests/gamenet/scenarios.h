#pragma once
// The rooms GameNetTests plays (included by gamenet_test.cpp, after Spawned, Check and Result).

struct Seat {
    std::string user;  // EOS ProductUserId text
    std::string role;  // roles.cpp
    std::string ini;   // its EDF6Coop.ini
    std::vector<std::pair<std::string, std::string>> env = {};  // this machine's own (roles.cpp: EDF6NET_*_WEAPON*)
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

inline void CheckRoom(const std::vector<Spawned>& machines, const gamenet::Network& network, bool marked = true) {
    for (const auto& machine : machines) {
        int seen = 0;
        for (auto it = machine.results.equal_range("member"); it.first != it.second; ++it.first)
            seen += !marked || it.first->second.find(" marked") != std::string::npos ? 1 : 0;
        Check(seen == static_cast<int>(machines.size()),
              machine.user + " sees all " + std::to_string(machines.size()) + " members" +
                  (marked ? " with the split sync marker" : ""));
        Check(machine.results.count("unimplemented-eos") == 0, machine.user +
                                                                   " reached no EOS function the fake only stubs (first: " +
                                                                   Result(machine, "unimplemented-eos") + ")");
    }
    Check(network.lobby.count == machines.size(), "EOS has every machine in the room");
}

inline void CheckLink(const std::vector<Spawned>& machines, const gamenet::Network& network, bool marked);
inline void CheckMarkedLink(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckLink(m, n, true); }
inline void CheckMarkedRoom(const std::vector<Spawned>& m, const gamenet::Network& n) { CheckRoom(m, n); }

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

inline void CheckLink(const std::vector<Spawned>& machines, const gamenet::Network& network, bool marked = true) {
    CheckRoom(machines, network, marked);
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
    bool rushed = false;  // the sync started before the members' markers reached the others
};

// The mission start sync: everyone ends up with every player's record, byte for byte the same.
inline void CheckMission(const std::vector<Spawned>& machines, const gamenet::Network& network, MissionExpect expect) {
    CheckRoom(machines, network, !expect.rushed);
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
inline void CheckRushedMission(const std::vector<Spawned>& m, const gamenet::Network& n) {
    CheckMission(m, n, {true, false, false, true});
}
// A member without EDF6Coop gets side packets as EDF6Coop sends them beside a split start message (it gets them
// since the fix of mission8rushed: they go to everyone). Its game must drop them and run on. (A start message that
// is split is another matter: the game without EDF6Coop cannot read one and writes past its player records, which
// is why a room of five or more needs EDF6Coop, and why [Test] SplitSyncBudget is for rooms where everyone runs it.)
inline void CheckPlainSidePackets(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckLink(machines, network, false);
    const auto& host = machines.front();
    int sent = 0;
    for (auto it = host.results.equal_range("side-packet"); it.first != it.second; ++it.first)
        sent += it.first->second.find("result 0") != std::string::npos ? 1 : 0;
    Check(sent == 5, std::to_string(sent) + " side packets went to the guest without EDF6Coop");
    Check(Result(machines.back(), "plugin").rfind("none", 0) == 0, "the guest runs the game without EDF6Coop");
}

// Player 4 has a mod's longer weapon table and two weapons past the stock one: its own machine keeps them, every
// other machine replaces them with its own weapon of that class and slot (EDF6Coop's weapon guard, weaponguard.h),
// before the game builds that soldier.
inline void CheckModWeapons(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEosAcceptedAll(network);
    for (std::size_t i = 0; i < machines.size(); ++i) {
        const auto& machine = machines[i];
        Check(Result(machine, "sync").rfind("done", 0) == 0, machine.user + " finished the sync: " + Result(machine, "sync"));
        std::string record;
        for (auto it = machine.results.equal_range("record"); it.first != it.second; ++it.first)
            if (it.first->second.rfind("3 ", 0) == 0) record = it.first->second;
        const bool own = i == 3;
        const std::string weapons = own ? "weapons=1530,1541,1552,1563,1574,1585 " : "weapons=1530,1541,1552,1563,0,0 ";
        Check(record.find(weapons) != std::string::npos,
              machine.user + (own ? " keeps its own mod weapons" : " replaced player 4's two unknown weapons") + " (" +
                  record + ")");
        const bool logged = machine.text.find("names weapon 1574 (slot 5)") != std::string::npos &&
                            machine.text.find("names weapon 1585 (slot 6)") != std::string::npos;
        Check(logged != own, machine.user + (own ? " replaced none of its own" : " logged both replacements"));
    }
}

inline void CheckBusyMission(const std::vector<Spawned>& m, const gamenet::Network& n) {
    CheckMission(m, n, {true, false, false});
    Check(m.front().results.count("shared-batch") > 0,
          "the start message shared a controller record with a background message (" + Result(m.front(), "shared-batch") + ")");
}

// host0...01 hosts; guest...02 and on join one after another, in that order (roles.cpp: EDF6NET_SEAT), so seat n
// is player n+1.
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
        {"room", Seats(2, "room", BaseIni()), 60000, &CheckMarkedRoom},
        {"link", Seats(2, "link", BaseIni()), 90000, &CheckMarkedLink},
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
        {"sidelink", {Seats(2, "sidelink", BaseIni()).front(), Seats(2, "sidelink", "")[1]}, 90000,
         &CheckPlainSidePackets, {{"EDF6NET_RUSH", "1"}}},
        {"mission4modweapon",
         [] {
             auto seats = Seats(4, "mission", BaseIni());
             seats[3].env = {{"EDF6NET_FIRST_WEAPON", "1530"}, {"EDF6NET_WEAPON_ROWS", "1590"}};
             return seats;
         }(),
         150000, &CheckModWeapons},
        // A mission started the moment the last player is in, while Epic takes 2.5 s to relay each member's lobby
        // attributes (the split sync marker among them) to the others.
        {"mission8rushed", Seats(8, "mission", BaseIni()), 150000, &CheckRushedMission,
         {{"EDF6NET_RUSH", "1"}, {"EDF6NET_LOBBY_DELAY", "2500"}}},
    };
    return all;
}


// --- Netcode rewrite W1 (transport) ---

// The last line of `log` that starts with `prefix` ("" when none).
inline std::string LastLine(const std::string& log, const std::string& prefix) {
    std::string found;
    for (std::size_t at = log.find(prefix); at != std::string::npos; at = log.find(prefix, at + 1)) {
        const std::size_t end = log.find('\n', at);
        found = log.substr(at, end == std::string::npos ? std::string::npos : end - at);
    }
    return found;
}

// The kbps a PATHS line gives after `label` (e.g. "direct "), summed over every PATHS line of `log`.
inline double PathKbps(const std::string& log, const std::string& label) {
    double total = 0;
    for (std::size_t at = log.find("PATHS last "); at != std::string::npos; at = log.find("PATHS last ", at + 1)) {
        const std::size_t end = log.find('\n', at);
        const std::size_t where = log.find(label, at);
        if (where == std::string::npos || where > end) continue;
        total += std::atof(log.c_str() + where + label.size());
    }
    return total;
}

// Every "got-from <user> states=<n> events=<n> gap=<ms>" a machine reported, by sender.
struct Got {
    std::size_t states = 0, events = 0, gapMs = 0;
};
inline std::map<std::string, Got> GotFrom(const Spawned& machine) {
    std::map<std::string, Got> got;
    for (auto it = machine.results.equal_range("got-from"); it.first != it.second; ++it.first) {
        char user[64]{};
        Got g;
        if (sscanf_s(it.first->second.c_str(), "%63s states=%zu events=%zu gap=%zu", user, static_cast<unsigned>(sizeof(user)),
                       &g.states, &g.events, &g.gapMs) == 4)
            got[user] = g;
    }
    return got;
}
inline std::map<std::string, Got> SentTo(const Spawned& machine) {
    std::map<std::string, Got> sent;
    for (auto it = machine.results.equal_range("sent-to"); it.first != it.second; ++it.first) {
        char user[64]{};
        Got g;
        if (sscanf_s(it.first->second.c_str(), "%63s states=%zu events=%zu", user, static_cast<unsigned>(sizeof(user)), &g.states,
                       &g.events) == 3)
            sent[user] = g;
    }
    return sent;
}

// Every reliable record one machine sent another arrived exactly once (the game's controller and the copies the
// transport sends over a second path must not make one count twice, nor lose one), and state kept arriving.
inline void CheckEveryRecord(const std::vector<Spawned>& machines, std::size_t maxGapMs) {
    for (const auto& to : machines) {
        const auto got = GotFrom(to);
        for (const auto& from : machines) {
            if (&from == &to) continue;
            const auto sent = SentTo(from);
            const auto s = sent.find(to.user);
            const auto g = got.find(from.user);
            if (s == sent.end() || g == got.end()) {
                Check(false, from.user + " -> " + to.user + ": no counts reported");
                continue;
            }
            Check(g->second.events == s->second.events,
                  from.user + " -> " + to.user + ": " + std::to_string(g->second.events) + " of " +
                      std::to_string(s->second.events) + " reliable records, each once");
            Check(g->second.states > 0, from.user + " -> " + to.user + ": state arrived");
            Check(g->second.gapMs <= maxGapMs, from.user + " -> " + to.user + ": state never stopped for more than " +
                                                   std::to_string(maxGapMs) + " ms (longest gap " +
                                                   std::to_string(g->second.gapMs) + " ms)");
        }
    }
}

// The game's datagrams as the transport classes them (netstats role, [Netcode] StatsSeconds=1): the plaintext tap
// saw them, the per-type log names the state-like and the reliable record type, and the state datagrams (89
// bytes: nothing else is that size) went to EOS unreliably once their type was learnt.
inline void CheckNetStats(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEosAcceptedAll(network);
    for (const auto& machine : machines) {
        const std::string& log = machine.text;
        const std::string state = LastLine(log, "NETTYPE 0x02800");
        Check(!state.empty() && state.find(" 0 reliable") != std::string::npos,
              machine.user + " logs the state record type, sent unreliably (" + state + ")");
        const std::string probe = LastLine(log, "NETTYPE 0x02700");
        Check(!probe.empty() && probe.find(" 0 reliable") == std::string::npos,
              machine.user + " logs the probe record type, sent reliably (" + probe + ")");
        const std::string classes = LastLine(log, "NETCLASS datagrams: state ");
        Check(!classes.empty() && classes.find("state 0 ") == std::string::npos,
              machine.user + " learnt the state type: state datagrams counted (" + classes + ")");
        Check(LastLine(log, "NETCODE features in room").find(" on: TrafficClasses,Mesh,Fragments") != std::string::npos,
              machine.user + " sees every member run the same netcode");
    }
    CheckEveryRecord(machines, 1000);
    std::vector<bool> reliable;  // the state datagrams in wire order: sent reliably?
    for (const auto& packet : Wire(network))
        if (packet.header.channel == 0 && packet.header.size == 89) reliable.push_back(packet.header.reliability != 0);
    std::size_t lateReliable = 0;
    for (std::size_t i = reliable.size() / 2; i < reliable.size(); ++i) lateReliable += reliable[i] ? 1 : 0;
    std::printf("INFO: %zu state datagrams on the wire, %zu reliable in the second half\n", reliable.size(), lateReliable);
    Check(reliable.size() > 20, "state datagrams crossed EOS");
    // Before a type is learnt (StateLearner::kSamples updates per pair, ~2.4 s here) its datagrams are Unknown and go
    // reliably, as before. Once learnt, never.
    Check(lateReliable == 0, "once learnt, state datagrams go unreliably (" + std::to_string(lateReliable) +
                                 " reliable in the second half)");
}

// A member of another netcode protocol ([Test] NetProtocol=99) is refused by the host: removed from the room, both
// logs say why, and the members that stay run the new netcode once it is gone.
inline void CheckVersionGate(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    const auto& host = machines.front();
    const auto& odd = machines.back();
    Check(host.text.find("NETCODE REFUSED " + odd.user) != std::string::npos, "the host refused the member of protocol 99");
    Check(network.lobby.count == machines.size() - 1, "it is out of the room (" + std::to_string(network.lobby.count) + ")");
    Check(odd.text.find("the room's host " + host.user + " runs netcode protocol 1, this machine 99") != std::string::npos,
          "the refused member says why its netcode is off");
    for (std::size_t i = 0; i + 1 < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        const std::string last = LastLine(log, "NETCODE features in room");
        Check(last.find(" on: TrafficClasses,Mesh,Fragments") != std::string::npos,
              machines[i].user + " runs the new netcode once the odd member is gone (" + last + ")");
        Check(log.find(" off: ") != std::string::npos, machines[i].user + " had it off while the odd member was in");
    }
}

// A message of 1024 loadout records' size from the host to each guest, in fragments, whole on arrival.
inline void CheckBulk(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEosAcceptedAll(network);
    const auto& host = machines.front();
    Check(Result(host, "bulk").rfind("sent", 0) == 0, "the host sent the message: " + Result(host, "bulk"));
    const std::string expected = Result(host, "bulk-fnv");
    for (std::size_t i = 1; i < machines.size(); ++i)
        Check(machines[i].text.find("NETCODE bulk message from " + host.user + ": tag 7, 146432 bytes, fnv " + expected) !=
                  std::string::npos,
              machines[i].user + " got the 146432-byte message whole");
}

// The direct link (Mode=host, joiners Mode=join to it over loopback): the joiners link to each other and their
// traffic goes directly, not through the host.
inline void CheckMesh(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEveryRecord(machines, 1000);
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        Check(log.find("linked directly") != std::string::npos, machines[i].user + " linked directly to another joiner");
        Check(PathKbps(log, ": direct ") > 0,
              machines[i].user + " sent game data directly (" + LastLine(log, "PATHS last ") + ")");
    }
}

// The joiners cannot reach each other: everything between them goes through the host, and nothing is lost.
inline void CheckMeshBlocked(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEveryRecord(machines, 1000);
    for (std::size_t i = 1; i < machines.size(); ++i)
        Check(machines[i].text.find("linked directly") == std::string::npos, machines[i].user + " never linked directly");
    Check(PathKbps(machines.front().text, "relayed for others ") > 0,
          "the host relayed the joiners' traffic (" + LastLine(machines.front().text, "PATHS last ") + ")");
}

// The joiners' direct link breaks for 1.5 s mid-game and comes back: state keeps arriving (the relay takes over,
// with copies while the direct link is in doubt), every reliable record arrives exactly once, copies are dropped on
// arrival, and the direct link carries again afterwards.
inline void CheckMeshFlap(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEveryRecord(machines, 1000);
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        Check(log.find("linked directly") != std::string::npos, machines[i].user + " linked directly");
        Check(PathKbps(log, "through the host ") > 0 && PathKbps(log, ": direct ") > 0,
              machines[i].user + " used both the relay and the direct link");
    }
    std::size_t copies = 0;
    for (std::size_t i = 1; i < machines.size(); ++i)
        for (std::size_t at = machines[i].text.find("PATHS last "); at != std::string::npos;
             at = machines[i].text.find("PATHS last ", at + 1)) {
            const std::size_t end = machines[i].text.find('\n', at);
            const std::size_t c = machines[i].text.rfind(" copies dropped on arrival", end);
            const std::size_t start = machines[i].text.rfind(", ", c);
            if (c != std::string::npos && c > at) copies += std::strtoull(machines[i].text.c_str() + start + 2, nullptr, 10);
        }
    Check(copies > 0, std::to_string(copies) + " copies (event over two paths) dropped on arrival by the joiners");
}

// A room on the direct link: the host listens on a port of this run, the joiners dial it over loopback. `extra`
// goes to every machine.
inline std::vector<Seat> DirectSeats(int count, const std::string& step, const std::string& extra, const std::string& guestExtra = "") {
    auto seats = Seats(count, step, "");
    const std::string common = "[MultiSlot]\r\nEnabled=1\r\nEightPlayerRooms=1\r\nMaxPlayers=8\r\nCrashLog=0\r\nNetLog=1\r\n"
                               "[Update]\r\nAutoUpdate=0\r\nCheckEDF6VR=0\r\n[Netcode]\r\nStatsSeconds=1\r\n";
    for (std::size_t i = 0; i < seats.size(); ++i)
        seats[i].ini = common + extra + (i ? guestExtra : "") +
                       (i ? "[DirectNet]\r\nEnabled=1\r\nMode=join\r\nHostAddress=127.0.0.1:@PORT@\r\nUPnP=0\r\nBindPhysicalInterface=0\r\n"
                          : "[DirectNet]\r\nEnabled=1\r\nMode=host\r\nListenPort=@PORT@\r\nUPnP=0\r\nBindPhysicalInterface=0\r\nPublicAddress=127.0.0.1:@PORT@\r\n");
    return seats;
}

inline const std::vector<Scenario>& NetScenarios() {
    static const std::vector<Scenario> all = {
        {"netstats", Seats(3, "netstats", BaseIni("[Netcode]\r\nStatsSeconds=1\r\n")), 90000, &CheckNetStats,
         {{"EDF6NET_SECONDS", "8"}}},
        {"versiongate",
         [] {
             auto seats = Seats(3, "versiongate", BaseIni());
             seats[2].ini = BaseIni("[Test]\r\nNetProtocol=99\r\n");
             return seats;
         }(),
         90000, &CheckVersionGate},
        {"bulk", Seats(3, "bulk", BaseIni("[Test]\r\nBulkEcho=1\r\n")), 90000, &CheckBulk},
        {"mesh", DirectSeats(3, "netstats", ""), 120000, &CheckMesh, {{"EDF6NET_SECONDS", "8"}}},
        {"meshblocked", DirectSeats(3, "netstats", "", "[Test]\r\nPeerBlockAfterMs=0\r\n"), 120000, &CheckMeshBlocked,
         {{"EDF6NET_SECONDS", "8"}}},
        {"meshflap", DirectSeats(3, "netstats", "", "[Test]\r\nPeerBlockAfterMs=7000\r\nPeerBlockForMs=1500\r\n"), 120000,
         &CheckMeshFlap, {{"EDF6NET_SECONDS", "10"}}},
    };
    return all;
}

inline const Scenario* FindScenario(const std::string& name) {
    for (const auto& scenario : Scenarios())
        if (scenario.name == name) return &scenario;
    for (const auto& scenario : NetScenarios())
        if (scenario.name == name) return &scenario;
    return nullptr;
}
