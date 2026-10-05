# Game-code network tests (GameNet_*)

Rooms of 2 to 8 players, played without the game window, without Steam and without Epic: each player is a
process (`GameMachine.exe`) that loads the installed game's real `EDF.dll` and the real `EDF6Coop.dll` (through
`EML6_Load`, as EDFModLoader does), and the machines reach each other through a fake EOS SDK
(`fake_eos_net.cpp`, named `EOSSDK-Win64-Shipping.dll`) over one shared memory section. `GameNetTests.exe` sets
up the room, starts the machines, collects what they report and checks it.

They need the game installed (`EDF6_GAME_DIR`; from the game folder only `EDF.dll`, `steam_api64.dll` and
`umbra_sandlot.dll` are loaded, by their full paths, and nothing there is written) and report themselves skipped
without it, as the other tests against the game's code do. Each scenario takes 1-4 s.

```powershell
$env:EDF6_GAME_DIR = 'D:\steam\steamapps\common\EARTH DEFENSE FORCE 6'
powershell -ExecutionPolicy Bypass -File ..\..\..\build.ps1 -Test     # all tests, GameNet_* among them
ctest --test-dir ..\..\build -R GameNet --output-on-failure           # only these
```

## What is the game, and what stands in for it

The game's code, unchanged except for EDF6Coop's own patches and hooks:

- the P2P handshake that connects the members (`p2p::Manager`, `Link`), the packet controller
  (`packet::ControllerImpl`), the per-peer datagrams, their AES-CTR encryption (keyed by the lobby id, as in the
  game), decryption, de-duplication, acknowledgement and resend, and the dispatch of records to subscribers;
- the mission start sync: `MissionSync_Begin` (78E9A0) builds each machine's request from its loadout,
  the sync objects (`Syncronize::Object`, 74BDF0, 74C570, 74CEF0) gather them, the host's `MissionSync_Res`
  (78D0E0) writes the start message and every machine's `MissionSync_Update` (790600) reads it;
- every EOS call goes through EDF.dll's import table, so EDF6Coop's wrappers (packet fit, net log, the direct
  link's reliable layer, the lobby marker, the weapon guard) see the same calls as in the game.

Stand-ins, each as small as the game code allowed:

| Stands in for | Where | What it does |
|---|---|---|
| Epic's EOS SDK | `fake_eos_net.cpp` | lobbies and P2P as EOS behaves where the game and the plugin can tell: 32-digit lobby ids, packets above 1170 bytes refused with `EOS_LimitExceeded`, a peer's packets wait until the connection is accepted, order kept per channel only, completions and notifications (connection requests and establishments, members joining, leaving and updating) in `EOS_Platform_Tick`. Every other import is a stub that reports when it is called. |
| `eos::internal_Core` | `game.cpp` | the three observables the network subscribes to, and the platform handle; ticked through the game's own `internal_Core::Update` |
| GameDataMgr, net::Network, eos::Core, eos::GameImpl | `missionsync.cpp` | zeroed, with each machine's loadout where `MissionSync_Begin` reads it and a WEAPONTABLE row count where the weapon guard asks for it; GameImpl lists the room's users (the game's own `eos::User` objects of the transport) and holds the sync objects |
| the event controller (74E1B0, 750130 and its receive) | `missionsync.cpp` | frames each message as 750380 does, with the game's writers, batches them per member as 761E60 does (sent above 250 bytes, and at the end of each frame) and sends them through the game's packet controller; the receiving side unframes them with the game's readers and hands them to the game's receive (74C570) |

A machine's loadout is shaped like a real one (six weapons with their entries, eight colours) so that a record
has a real record's size: eight of them make a 1132-byte start message, where the game's eight players made 1180.
Guests join one after another in seat order, so seat n is player n+1.

## Scenarios

| Test | Machines | What it plays |
|---|---|---|
| `GameNet_room` | 2 | the room is created and joined through the game's imports; every member publishes the split sync marker |
| `GameNet_link` | 2 | the game's handshake connects them; a record sent through the game's controller arrives; no plaintext crosses EOS |
| `GameNet_sidelink` | 2 | the guest runs the game without EDF6Coop and gets side packets: its game drops them and runs on |
| `GameNet_mission2` | 2 | the start sync with `[Test] SplitSyncBudget=200`, so the second record goes beside the start message |
| `GameNet_mission4` | 4 | a start message that fits: nothing goes beside it |
| `GameNet_mission4modweapon` | 4 | player 4 has a mod's weapon table (1590 rows) and two weapons past the stock 1564: the others replace them with their own (weapon guard), player 4 keeps them |
| `GameNet_mission8` | 8 | 8 players: 1132 bytes do not fit the 1100 the message may have, a record goes beside it |
| `GameNet_mission8late` | 8 | the side packets' channel arrives 400 ms after the message: every guest reads the message first and waits |
| `GameNet_mission8lossy` | 8 | without the direct link's reliable layer, the network loses the first start message; the game resends it |
| `GameNet_mission8busy` | 8 | every frame every machine also sends a 16-byte event message, which shares the controller record with the start message (the plugin's `kBatchedAllowance`) |
| `GameNet_mission8rushed` | 8 | Epic relays lobby attributes 2.5 s late and the mission starts as the last player is in, before anyone's marker reached the host |

The mission scenarios check, on every machine: the sync finished, the player count, the host's mission and
difficulty, and for every player the class, a marker and the armor that player's own machine set - and that every
machine holds the same bytes for every player (EDF6Coop's sidecars of players 5+ included, read through
`EDF6Coop_LoadoutRecord`). EOS may refuse nothing (size, full inbox); records must go beside the message exactly
when it does not fit, and none may go missing. Each imperfect scenario also checks that its imperfection happened
(the guests waited, a datagram was lost, a record was shared).

Network imperfections, set per scenario: `EDF6NET_DELAY=<channel>:<ms>`, `EDF6NET_DROP=<bytes>:<count>` (only
unreliable packets), `EDF6NET_LOBBY_DELAY=<ms>`, `EDF6NET_CHATTER=<bytes>`, `EDF6NET_RUSH=1`.

## What they caught

- With `packetfit.cpp`/`.h` as they were before PR #19 (4141abc, which looked for stubs in the encrypted
  datagrams), every split scenario fails 23-24 checks: every guest logs "the loadout record of player index 7 ...
  never arrived" and holds an empty record for player 8 - class 0 (Ranger) and default weapons, as players saw
  in the game.
- After PR #19 the records still waited for each member's lobby marker; the message itself cannot wait (it is
  encrypted), so a member whose marker was late got the message without its records and, past 1.5 s, built that
  player from an empty record again. `GameNet_mission8rushed` failed 23 checks; the records now go to every member.
- With `kRecordWaitMs` set to 0, `mission8` and `mission8late` fail: guests do read the message before its
  records arrive.
- The game without EDF6Coop cannot read a split start message: it writes past its player records (heap
  corruption, measured). That is why a room of five or more needs EDF6Coop and `SplitSyncBudget` is only for rooms
  where everyone runs it.

## Adding to it

A scenario is a list of seats (user, role, INI, environment) and a check (`scenarios.h`); a role is what one
machine does (`roles.cpp`). A seat without an INI runs the game without EDF6Coop. Machines report with
`Result("key", ...)` lines, and the check reads them; every machine's `EDF6Coop.log` is printed after its output.

`EDF6NET_DEBUGGER=<path of cdb.exe>` runs every machine under the debugger, which prints the stack of a crash.

The RVAs used here are those of the Steam release EDF6Coop supports; `EDF6Coop` refuses any other `EDF.dll`, and
so does every machine.
