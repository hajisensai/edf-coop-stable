# Game-code network tests (GameNet_*)

Rooms of 2 to 8 players, played without the game window, without Steam and without Epic: each player is a
process (`GameMachine.exe`) that loads the installed game's real `EDF.dll` and the real `EDF6Coop.dll` (through
`EML6_Load`, as EDFModLoader does), and the machines reach each other through a fake EOS SDK
(`fake_eos_net.cpp`, named `EOSSDK-Win64-Shipping.dll`) over one shared memory section. `GameNetTests.exe` sets
up the room, starts the machines, collects what they report and checks it.

They need the game installed (`EDF6_GAME_DIR`; only `EDF.dll` is read, nothing in the game folder is written) and
report themselves skipped without it, as the other tests against the game's code do. Each scenario takes 2-4 s.

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
  link's reliable layer, the lobby marker) see the same calls as in the game.

Stand-ins, each as small as the game code allowed:

| Stands in for | Where | What it does |
|---|---|---|
| Epic's EOS SDK | `fake_eos_net.cpp` | lobbies and P2P as EOS behaves where the game and the plugin can tell: 32-digit lobby ids, packets above 1170 bytes refused with `EOS_LimitExceeded`, a peer's packets wait until the connection is accepted, completions run in `EOS_Platform_Tick`. Every other import is a stub that reports when it is called. |
| `eos::internal_Core` | `game.cpp` | the three observables the network subscribes to, and the platform handle; ticked through the game's own `internal_Core::Update` |
| GameDataMgr, net::Network, eos::Core, eos::GameImpl | `missionsync.cpp` | zeroed, with each machine's loadout where `MissionSync_Begin` reads it; GameImpl lists the room's users (the game's own `eos::User` objects of the transport) and holds the sync objects |
| the event controller's two sends (74E1B0, 750130) | `missionsync.cpp` | frame the message as 750380 does, with the game's writers, and send it through the game's packet controller; the receiving side unframes it with the game's readers and hands it to the game's receive (74C570) |

A machine's loadout is shaped like a real one (six weapons with their entries, eight colours) so that a record
has a real record's size: eight of them make a 1132-byte start message, where the game's eight players made 1180.

## Scenarios

| Test | Machines | Checks |
|---|---|---|
| `GameNet_room` | 2 | the room is created and joined through the game's imports; every member publishes the split sync marker |
| `GameNet_link` | 2 | the game's handshake connects them; a record sent through the game's controller arrives; no plaintext crosses EOS |
| `GameNet_mission2` | 2 | the start sync with `[Test] SplitSyncBudget=200`, so the second record goes beside the start message |
| `GameNet_mission4` | 4 | the start sync of a message that fits: nothing goes beside it |
| `GameNet_mission8` | 8 | the start sync of 8 players: 1132 bytes do not fit, a record goes beside the message |

The mission scenarios check, on every machine: the sync finished, the player count, the host's mission and
difficulty, and for every player the class, a marker and the armor that player's own machine set - and that every
machine holds the same bytes for every player (EDF6Coop's sidecars of players 5+ included, read through
`EDF6Coop_LoadoutRecord`). No packet may exceed 1170 bytes; records must go beside the message exactly when it
does not fit, and none may go missing.

With `packetfit.cpp`/`.h` as they were before PR #19 (4141abc, which looked for stubs in the encrypted
datagrams), `GameNet_mission8` fails 23 checks: every guest logs "the loadout record of player index 7 ... never arrived"
and holds an empty record for player 8 - class 0 (Ranger) and default weapons, as players saw in the game.

## Adding to it

A scenario is a list of seats (user, role, INI) and a check (`scenarios.h`); a role is what one machine does
(`roles.cpp`). Machines report with `Result("key", ...)` lines, and the check reads them; every machine's
`EDF6Coop.log` is printed after its output.

`EDF6NET_DEBUGGER=<path of cdb.exe>` runs every machine under the debugger, which prints the stack of a crash.

The RVAs used here are those of the Steam release EDF6Coop supports; `EDF6Coop` refuses any other `EDF.dll`, and
so does every machine.
