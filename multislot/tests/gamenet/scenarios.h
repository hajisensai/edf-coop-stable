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
    bool bulk = false;    // every record went in one bulk message (the room reads fragments, packetfit.h)
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
    const bool sentBulk = host.text.find("sent in bulk ahead of the start message: sent") != std::string::npos;
    Check(sentBulk == expect.bulk, expect.bulk ? "the host sent every loadout record in bulk" : "nothing went in bulk");
    Check(sentBeside == (expect.split && !expect.bulk), expect.split && !expect.bulk ? "the host sent loadout records beside the start message"
                                                   : "nothing was sent beside the start message");
    int waited = 0;
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        const bool arrived = log.find("arrived beside the start message") != std::string::npos;
        Check(arrived == (expect.split && !expect.bulk),
              machines[i].user + (expect.split && !expect.bulk ? " got records beside the start message" : " needed no records beside it"));
        if (expect.bulk) {
            Check(log.find("loadout records arrived in bulk") != std::string::npos, machines[i].user + " got the records in bulk");
            // The game's packets from the host waited while the records came (packetfit.h BulkIncoming): its frame
            // never waited for them inside the start message.
            Check(log.find("for the " + std::to_string(members) + " loadout records in bulk") == std::string::npos,
                  machines[i].user + " never waited inside the start message for the records");
        }
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
// Player 2's packets from player 4 come 1.5 s late, so its P2P handshake with player 4 ends late, while the host is
// done with its own handshakes and sends its sync message at once: player 2's controller hands that message over
// (and acknowledges it, so the host never sends it again) while player 2 still waits for player 4.
inline void CheckSlowPeerMission(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckMission(machines, network, {false, false, false});
    const std::string early = Result(machines[1], "early-messages");
    Check(std::atoi(early.c_str()) > 0, machines[1].user + " got a sync message while its handshake with player 4 still ran (" +
                                            early + ")");
}
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

// W2 player sync: every machine installed its hooks on the real EDF.dll (every checked byte and slot matched), and
// every extended player record crossed the game's controller and read back exactly, with EDF6Coop's bin reader and
// with the game's own.
inline void CheckPlayerSync(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEosAcceptedAll(network);
    for (const auto& machine : machines) {
        Check(machine.text.find("PLAYER sync: hooks installed (20 vtable slots + flush interval)") != std::string::npos,
              machine.user + " installed the player sync hooks against EDF.dll");
        Check(Result(machine, "player-records") == "8 ok 0 bad",
              machine.user + " read every player record back exactly (" + Result(machine, "player-records") + ")");
    }
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
        {"mission2", Seats(2, "mission", BaseIni("[Test]\r\nSplitSyncBudget=200\r\n[Netcode]\r\nFragments=0\r\n")), 150000, &CheckSplitMission},
        // Four players: the start message fits and is the game's own.
        {"mission4", Seats(4, "mission", BaseIni()), 150000, &CheckWholeMission},
        // Eight players: their records make the start message 1132 bytes, above the 1100 it may have, so one goes
        // beside it.
        {"mission8", Seats(8, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 150000, &CheckSplitMission},
        // The records travel on a channel of their own, and EOS keeps no order between channels: here they come
        // 400 ms after the start message, which every guest reads first and has to wait at.
        {"mission8late", Seats(8, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 150000, &CheckLateRecords, {{"EDF6NET_DELAY", "77:400"}}},
        // Without the direct link's reliable layer the game's datagrams are unreliable: the network loses the first
        // start message, and the game's own resend has to bring it.
        {"mission8lossy", Seats(8, "mission", BaseIni("", false)), 150000, &CheckLossyMission, {{"EDF6NET_DROP", "900:1"}}},
        // Every frame every machine says something else too (an event message of 16 bytes), which shares the
        // controller record with the start message: the room the plugin leaves for it (kBatchedAllowance).
        {"mission8busy", Seats(8, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 150000, &CheckBusyMission, {{"EDF6NET_CHATTER", "16"}}},
        {"sidelink", {Seats(2, "sidelink", BaseIni()).front(), Seats(2, "sidelink", "")[1]}, 90000,
         &CheckPlainSidePackets, {{"EDF6NET_RUSH", "1"}}},
        {"mission4modweapon",
         [] {
             auto seats = Seats(4, "mission", BaseIni());
             seats[3].env = {{"EDF6NET_FIRST_WEAPON", "1530"}, {"EDF6NET_WEAPON_ROWS", "1590"}};
             return seats;
         }(),
         150000, &CheckModWeapons},
        {"mission4slowpeer",
         [] {
             auto seats = Seats(4, "mission", BaseIni());
             seats[1].env = {{"EDF6NET_DELAY_FROM", seats[3].user + ":1500"}};
             return seats;
         }(),
         150000, &CheckSlowPeerMission},
        // A mission started the moment the last player is in, while Epic takes 2.5 s to relay each member's lobby
        // attributes (the split sync marker among them) to the others.
        {"mission8rushed", Seats(8, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 150000, &CheckRushedMission,
         {{"EDF6NET_RUSH", "1"}, {"EDF6NET_LOBBY_DELAY", "2500"}}},
        {"playersync", Seats(2, "playersync", BaseIni()), 90000, &CheckPlayerSync},
        // Rooms past eight (docs/net-re/roomsize.md): 16 and 32 players, each record beside the start message but
        // the few that fit, every machine with every player's bytes.
        {"mission16", Seats(16, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 240000, &CheckSplitMission},
        {"mission32", Seats(32, "mission", BaseIni("[Netcode]\r\nFragments=0\r\n")), 400000, &CheckSplitMission},
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
    std::size_t unreliable = 0;
    for (bool r : reliable) unreliable += r ? 0 : 1;
    std::printf("INFO: %zu state datagrams on the wire, %zu of them unreliable\n", reliable.size(), unreliable);
    Check(reliable.size() > 20 && unreliable > 0, "state datagrams crossed EOS, unreliably once learnt");
    // Before a type is learnt (StateLearner::kSamples updates in a row) its datagrams are Unknown and go reliably, as
    // before. From the period after the one it was learnt in, no datagram of the run is Unknown any more.
    for (const auto& machine : machines) {
        const std::string& log = machine.text;
        const std::size_t learnt = log.find("NETCLASS record type 0x02800 behaves as state");
        Check(learnt != std::string::npos, machine.user + " learnt that record type 0x02800 is state");
        if (learnt == std::string::npos) continue;
        std::size_t at = log.find("NETCLASS datagrams: ", learnt);
        if (at != std::string::npos) at = log.find("NETCLASS datagrams: ", at + 1);  // the period it was learnt in may straddle
        std::size_t unknownLater = 0;
        for (; at != std::string::npos; at = log.find("NETCLASS datagrams: ", at + 1)) {
            const std::size_t u = log.find(" unknown ", at);
            if (u != std::string::npos) unknownLater += std::strtoull(log.c_str() + u + 9, nullptr, 10);
        }
        Check(unknownLater == 0, machine.user + ": once learnt, every datagram has a class (" + std::to_string(unknownLater) +
                                     " unknown later)");
        // Sending stopped: the last state datagram to each member went once more, reliably (nothing replaces it).
        std::size_t trails = 0;
        for (std::size_t p = log.find("PATHS last "); p != std::string::npos; p = log.find("PATHS last ", p + 1)) {
            const std::size_t c = log.find(" last state datagrams sent again reliably", p);
            const std::size_t start = log.rfind(", ", c);
            if (c != std::string::npos && start != std::string::npos && start > p) trails += std::strtoull(log.c_str() + start + 2, nullptr, 10);
        }
        Check(trails >= 2, machine.user + ": the last state datagram to each of the others went again reliably (" +
                               std::to_string(trails) + ")");
    }
}

// One member has the traffic classes off: the room runs without them. Every state datagram goes as before this
// existed - reliably, nothing held back - while the log still names its class.
inline void CheckNetStatsMixed(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEveryRecord(machines, 1000);
    std::size_t states = 0, unreliable = 0;
    for (const auto& packet : Wire(network))
        if (packet.header.channel == 0 && packet.header.size == 89) {
            ++states;
            unreliable += packet.header.reliability == 0 ? 1 : 0;
        }
    std::printf("INFO: %zu state datagrams on the wire, %zu of them unreliable\n", states, unreliable);
    Check(states > 20 && unreliable == 0, "with one member without the classes every state datagram went reliably");
    for (const auto& machine : machines)
        Check(LastLine(machine.text, "NETCODE features in room").find(" on: TrafficClasses") == std::string::npos,
              machine.user + " does not run the classes (" + LastLine(machine.text, "NETCODE features in room") + ")");
}

// [Netcode] Mesh=0 at the joiners: the room runs without the mesh, everything between them goes through the host.
inline void CheckMeshOff(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    CheckEveryRecord(machines, 1000);
    for (std::size_t i = 1; i < machines.size(); ++i)
        Check(machines[i].text.find("linked directly") == std::string::npos, machines[i].user + " never linked directly");
    Check(PathKbps(machines.front().text, "relayed for others ") > 0, "the host relayed the joiners' traffic");
}

// A member of another netcode protocol ([Test] NetProtocol=99) is refused by the host: removed from the room, both
// logs say why, and the members that stay run the new netcode once it is gone.
inline void CheckVersionGate(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    const auto& host = machines.front();
    const auto& odd = machines.back();
    Check(host.text.find("NETCODE REFUSED " + odd.user) != std::string::npos, "the host refused the member of protocol 99");
    Check(network.lobby.count == machines.size() - 1, "it is out of the room (" + std::to_string(network.lobby.count) + ")");
    Check(odd.text.find("the room's host " + host.user + " runs netcode protocol " + std::to_string(multislot::kNetProtocol) +
                        ", this machine 99") != std::string::npos,
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

// XPRESS on the game's plaintext (netcompress.h): eight players start a mission while every machine also sends a
// 16-byte event every frame (more would not fit beside the start message, kBatchedAllowance). Datagrams are packed once the room shows that everyone unpacks them, and
// the start sync and every record still arrive as sent.
inline void CheckXpressMission(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckMission(machines, network, {true, false, false, false, true});
    std::size_t packing = 0, unpacking = 0;
    for (const auto& machine : machines) {
        const std::string line = LastLine(machine.text, "NETCODE XPRESS: ");
        if (line.empty()) continue;
        packing += line.find("XPRESS: 0 datagrams packed") == std::string::npos ? 1 : 0;
        unpacking += line.find(", 0 unpacked") == std::string::npos ? 1 : 0;
    }
    Check(packing > 0 && unpacking > 0, std::to_string(packing) + " machines packed datagrams, " +
                                            std::to_string(unpacking) + " unpacked them");
    // A packed datagram keeps its class: the send finds the records of its flush by the packed size.
    for (const auto& machine : machines)
        for (std::size_t at = machine.text.find("NETCLASS datagrams: "); at != std::string::npos;
             at = machine.text.find("NETCLASS datagrams: ", at + 1)) {
            const std::size_t end = machine.text.find('\n', at);
            const std::string line = machine.text.substr(at, end - at);
            Check(line.find(", 0 without their plaintext") != std::string::npos, machine.user + ": every datagram had its plaintext (" + line + ")");
        }
}


// Epic's lobby full (EDF6NET_LOBBY_CAP=2 stands for its 64), the room larger (8): the host puts its direct-link address
// and identity on the lobby, the third player is turned away by Epic and comes in over the direct link instead, and the
// host lets it in by its own member list.
inline void CheckJoinFull(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    const auto& host = machines.front();
    const auto& late = machines.back();
    Check(network.lobby.count == 2, "Epic's lobby holds its 2 (" + std::to_string(network.lobby.count) + ")");
    Check(Result(late, "fulljoin") == "completed 0", late.user + " is in the room: " + Result(late, "fulljoin"));
    Check(late.text.find("is full") != std::string::npos && late.text.find("coming in over the direct link") != std::string::npos,
          late.user + " was turned away by Epic and came in over the direct link");
    Check(host.text.find("members outside it may come in over the direct link") != std::string::npos,
          "the host admits members beyond Epic's lobby once it is full");
    // Not on its word: its first hello is refused until it proved its EOS id over EOS itself.
    const std::size_t refused = host.text.find("refused hello for " + late.user.substr(0, 8));
    const std::size_t proved = host.text.find(late.user.substr(0, 8) + " proved its EOS id over EOS");
    Check(proved != std::string::npos && (refused == std::string::npos || refused < proved),
          "the third player came in only after proving its EOS id over EOS");
    Check(host.text.find("DIRECT client " + late.user.substr(0, 8) + " connected") != std::string::npos,
          "the host linked the third player");
    Check(host.text.find("ROOM " + late.user.substr(0, 8) + " -> JOINED for the game") != std::string::npos,
          "the host's game was told the third player joined");
    // Every game has all three in its room, in one order (the network index each adds them with), and the start
    // sync gives each of them every player's own record.
    std::string order;
    for (const auto& machine : machines) order += " " + machine.user;
    const std::string everyone = std::to_string(machines.size()) + order;
    for (std::size_t i = 0; i < machines.size(); ++i) {
        const auto& machine = machines[i];
        Check(Result(machine, "game-members") == everyone,
              machine.user + "'s game has everyone in the room in the host's order (" + Result(machine, "game-members") + ")");
        if (i + 1 < machines.size()) {
            bool told = false;
            for (auto it = machine.results.equal_range("member-status"); it.first != it.second; ++it.first)
                told = told || it.first->second == late.user + " 0";
            // Or it entered the room after the third did, with its host's member slots (eos_hooks parked entry).
            const bool enteredWith = machine.text.find("with its host's member slots") != std::string::npos;
            Check(told || enteredWith, machine.user + "'s game was told " + late.user + " joined, or entered with it");
        }
        Check(Result(machine, "sync").rfind("done", 0) == 0, machine.user + " finished the start sync: " + Result(machine, "sync"));
        Check(Result(machine, "players") == std::to_string(machines.size()),
              machine.user + " has " + std::to_string(machines.size()) + " players (has " + Result(machine, "players") + ")");
        for (std::size_t slot = 0; slot < machines.size(); ++slot) {
            const std::string expected = std::to_string(slot) + " class=" + std::to_string(slot % 4) +
                                         " marker=" + std::to_string(500 + slot) + " armor=" + std::to_string(1000 + 37 * slot);
            std::string record;
            for (auto it = machine.results.equal_range("record"); it.first != it.second; ++it.first)
                if (it.first->second.rfind(std::to_string(slot) + " ", 0) == 0) record = it.first->second;
            Check(record.rfind(expected + " ", 0) == 0,
                  machine.user + " has player " + std::to_string(slot + 1) + "'s own loadout (" + record + ")");
        }
    }
}

inline std::vector<Seat> JoinFullSeats() {
    const std::string common = "[MultiSlot]\r\nEnabled=1\r\nEightPlayerRooms=1\r\nMaxPlayers=8\r\nCrashLog=0\r\nNetLog=1\r\n"
                               "[Update]\r\nAutoUpdate=0\r\nCheckEDF6VR=0\r\n[Test]\r\nLoopbackHosts=1\r\n";
    auto seats = Seats(3, "fullroom", "");
    seats[0].ini = common + "RoomCapacity=8\r\n[DirectNet]\r\nEnabled=1\r\nMode=host\r\nListenPort=@PORT@\r\nUPnP=0\r\nBindPhysicalInterface=0\r\n"
                            "PublicAddress=127.0.0.1:@PORT@\r\n";
    seats[1].ini = common + "[DirectNet]\r\nEnabled=1\r\nMode=off\r\nUPnP=0\r\nBindPhysicalInterface=0\r\n";
    seats[2].ini = seats[1].ini;
    seats[2].role = "guest-fulljoin";
    return seats;
}


// The host's bulk of records never leaves ([Test] DropRecordsBulk=1): every guest waits for it once, then leaves the
// records out at once - not once per record (32 records would freeze a guest's frame for minutes).
inline void CheckBulkLost(const std::vector<Spawned>& machines, const gamenet::Network& network) {
    CheckRoom(machines, network);
    for (std::size_t i = 1; i < machines.size(); ++i) {
        const std::string& log = machines[i].text;
        std::size_t waits = 0;
        for (std::size_t at = log.find("loadout records in bulk: still missing"); at != std::string::npos;
             at = log.find("loadout records in bulk: still missing", at + 1))
            ++waits;
        Check(waits == 1, machines[i].user + " waited for the lost bulk once (" + std::to_string(waits) + ")");
        Check(Result(machines[i], "sync").rfind("done", 0) == 0, machines[i].user + " finished the sync: " + Result(machines[i], "sync"));
        unsigned long long ms = 0;
        sscanf_s(Result(machines[i], "sync").c_str(), "done after %llu", &ms);
        Check(ms < 15000, machines[i].user + " was not held for long (" + std::to_string(ms) + " ms)");
    }
}

// Member slots (multislot userslots.h): Epic's lobby of 3 (EDF6NET_LOBBY_CAP stands for its 64), a room of 8. A and B
// join through Epic, D comes in over the direct link (Epic's lobby full), A leaves and X takes its place in Epic's
// lobby. The host's game gave A slot 1, D slot 3; A's slot is empty after it left and X takes it - so X, which reads
// Epic's lobby as host, B, X and D beyond it, must still have X in 1 and B in 2. Every game numbers the four the same
// way, and the start sync among them gives every machine every player's own record.
inline void CheckSlotOrder(const std::vector<Spawned>& machines, const gamenet::Network&) {
    const auto& h = machines[0];
    const auto& a = machines[1];
    const auto& b = machines[2];
    const auto& d = machines[3];
    const auto& x = machines[4];
    Check(Result(a, "left").rfind("4 members 0", 0) == 0, a.user + " saw the room of four and left it (" + Result(a, "left") + ")");
    Check(d.text.find("coming in over the direct link") != std::string::npos, d.user + " came in over the direct link");
    const std::string expected = "0:" + h.user + " 1:" + x.user + " 2:" + b.user + " 3:" + d.user;
    const std::vector<const Spawned*> stay = {&h, &x, &b, &d};  // in slot order
    for (const Spawned* machine : stay) {
        Check(Result(*machine, "slots") == expected,
              machine->user + "'s game numbers the members as the host's does (" + Result(*machine, "slots") + ")");
        Check(Result(*machine, "sync").rfind("done", 0) == 0, machine->user + " finished the start sync: " + Result(*machine, "sync"));
        Check(Result(*machine, "players") == "4", machine->user + " has 4 players (has " + Result(*machine, "players") + ")");
        for (std::size_t slot = 0; slot < stay.size(); ++slot) {
            const std::string want = std::to_string(slot) + " class=" + std::to_string(slot % 4) + " marker=" +
                                     std::to_string(500 + slot) + " armor=" + std::to_string(1000 + 37 * slot);
            std::string record;
            for (auto it = machine->results.equal_range("record"); it.first != it.second; ++it.first)
                if (it.first->second.rfind(std::to_string(slot) + " ", 0) == 0) record = it.first->second;
            Check(record.rfind(want + " ", 0) == 0,
                  machine->user + " has " + stay[slot]->user + "'s own loadout in slot " + std::to_string(slot) + " (" + record + ")");
        }
    }
    Check(x.text.find("with its host's member slots") != std::string::npos,
          x.user + "'s game entered the room with its host's member slots, not Epic's order");
    Check(b.text.find(x.user.substr(0, 8) + " JOINED: held back until the room's host's game has it") != std::string::npos,
          b.user + " held Epic's word of " + x.user + " back until the host's game had it");
}

inline std::vector<Seat> SlotSeats() {
    const std::string common = "[MultiSlot]\r\nEnabled=1\r\nEightPlayerRooms=1\r\nMaxPlayers=8\r\nCrashLog=0\r\nNetLog=1\r\n"
                               "[Update]\r\nAutoUpdate=0\r\nCheckEDF6VR=0\r\n[Test]\r\nLoopbackHosts=1\r\n";
    const std::string member = common + "[DirectNet]\r\nEnabled=1\r\nMode=off\r\nUPnP=0\r\nBindPhysicalInterface=0\r\n";
    auto seats = Seats(5, "slots", member);
    seats[0].ini = common + "RoomCapacity=8\r\n[DirectNet]\r\nEnabled=1\r\nMode=host\r\nListenPort=@PORT@\r\nUPnP=0\r\n"
                            "BindPhysicalInterface=0\r\nPublicAddress=127.0.0.1:@PORT@\r\n";
    seats[0].role = "host-slots-host";
    seats[1].role = "guest-slots-leave";
    seats[2].role = "guest-slots-epic";
    seats[3].role = "guest-slots-direct";
    seats[4].role = "guest-slots-late";
    const std::string final = seats[0].user + "," + seats[2].user + "," + seats[3].user + "," + seats[4].user;
    for (auto& seat : seats) {
        seat.env.push_back({"EDF6NET_FINAL", final});
        seat.env.push_back({"EDF6NET_LEAVE_AT", "4"});
    }
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
        {"netstatsmixed",
         [] {
             auto seats = Seats(3, "netstats", BaseIni("[Netcode]\r\nStatsSeconds=1\r\n"));
             seats[2].ini = BaseIni("[Netcode]\r\nStatsSeconds=1\r\nTrafficClasses=0\r\n");
             return seats;
         }(),
         90000, &CheckNetStatsMixed, {{"EDF6NET_SECONDS", "8"}}},
        {"slotorder", SlotSeats(), 150000, &CheckSlotOrder, {{"EDF6NET_LOBBY_CAP", "3"}, {"EDF6NET_EPIC_MEMBERS", "3"}}},
        {"joinfull", JoinFullSeats(), 120000, &CheckJoinFull,
         {{"EDF6NET_LOBBY_CAP", "2"}, {"EDF6NET_EPIC_MEMBERS", "2"}, {"EDF6NET_SECONDS", "12"}}},
        {"meshoff", DirectSeats(3, "netstats", "", "Mesh=0\r\n"), 120000, &CheckMeshOff, {{"EDF6NET_SECONDS", "6"}}},
        // 32 players once the room shows that everyone reads fragments: the start message carries a bulk marker only,
        // every record goes in one bulk message.
        {"mission32bulk", Seats(32, "mission", BaseIni("[Netcode]\r\nStatsSeconds=1\r\n")), 400000,
         [](const std::vector<Spawned>& m, const gamenet::Network& n) { CheckMission(m, n, {true, false, false, false, true}); },
         {{"EDF6NET_SETTLE", "4000"}}},
        {"mission8bulklost",
         [] {
             auto seats = Seats(8, "mission", BaseIni("[Netcode]\r\nStatsSeconds=1\r\n"));
             seats[0].ini = BaseIni("[Netcode]\r\nStatsSeconds=1\r\n[Test]\r\nDropRecordsBulk=1\r\n");
             return seats;
         }(),
         150000, &CheckBulkLost, {{"EDF6NET_SETTLE", "2500"}}},
        {"mission8xpress", Seats(8, "mission", BaseIni("[Netcode]\r\nStatsSeconds=1\r\n")), 150000,
         &CheckXpressMission, {{"EDF6NET_CHATTER", "16"}, {"EDF6NET_SETTLE", "2500"}}},
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
