# EDF6MultiSlot

**English** | [中文](README.zh-CN.md)

Online co-op for up to **8 players** in EARTH DEFENSE FORCE 6 (PC / Steam), with separate **10- and 12-player** builds. An [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) plugin.

Originally written by **momotori01** and published as part of [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) (released into the public domain, see [LICENSE](LICENSE)); its history is kept in this folder. This copy continues from EDF6MultiSlot 1.5.12.

The player-facing manual (Japanese) is [packaging/README_EDF6MultiSlot.txt](packaging/README_EDF6MultiSlot.txt); it ships in the package.

## What changed here (1.5.14)

- **Whole room left when a newcomer could not connect:** when the P2P handshake with someone never completed, the game made every member except the host leave the room 20 s after they joined (`788ABF`: not the host and any link timed out → leave), however long the members had been playing there. Now a member who had finished its own join before the newcomer came stays, and the newcomer stays unconnected until their own game gives up and leaves (the host already behaved like this); someone whose own join fails still leaves. `[MultiSlot] KeepRoomOnPeerTimeout=1` (default; 0 = the game's behaviour). The timeout, which the game re-checks every frame, is now logged once per link instead of about 500 times a second.
- **Crash after refusing to load (1.5.13 regression):** with `Enabled=0`, or when a game update or another mod made the plugin back out without changing anything, the game crashed about 0.2 s later: the log writer thread outlived the unloaded DLL. The writer now starts only once the plugin stays loaded, and an unload is logged as `UNLOADED`, not `SHUTDOWN`.
- **Rooms above this build's size:** the member list is cut to the build's player count, so a larger room (another mod, or a hostile host) can no longer crash everyone.
- **Log survives a hard kill:** lines are queued in a memory-mapped `EDF6MultiSlot.log.queue` that the next start replays, so a task-manager kill or `__fastfail` no longer loses the last lines. Member names can no longer forge log lines, and long CJK names are no longer shown as `(no name)`.
- **Copy armor:** the pickup count is computed in at most 24 steps and absurd values reported by other members are ignored, so they can no longer freeze the game.
- **Crash reports:** thunks and call stubs register unwind data (stack walks and exceptions pass through hooks), the plugin's own guarded probes no longer count as crashes, and faults inside the plugin get a dump too.
- **CI:** `.github/workflows/multislot.yml` builds 8/10/12 and runs every test that does not need the game (`-DMULTISLOT_CI=ON`, a marked placeholder layout that `package.ps1` refuses to package).

The game-dependent tests (`PatchTablesMatchEDF`, `PluginLoad_*`) have not been run on this version yet; run `build.cmd 8/10/12` on a machine with the game before packaging.

## 1.5.13

- **Less stutter:** log lines no longer open, write and close the log file on the game's own threads. They are queued and written in batches by a writer thread; crash reports and the shutdown line are still written at once.
- **Less EOS work:** with `NetLog=1` the EOS SDK log level is raised only for the seven categories the log keeps (lobby, P2P, voice chat, ...), not for all of them. The log content is unchanged.
- **10- and 12-player builds:** each is a room family of its own (`SEARCH_TYPE` centres 0x6E and 0x6A). Only the same build lists or joins those rooms; the 8-player build is unchanged and still plays with 1.5.5+ players. With the setting OFF a larger build lists normal rooms only; turn it ON (F2) to find 10/12-player rooms. Enemy counts stop growing at eight players (x1.8).
- **Fix for rooms above eight:** the user-slot diagnostics held eight slots, which silently disabled `HandshakeRecovery` in a larger room.

Online play with **more than eight players has not been tested** on real machines.

## Build

Requires Windows x64, Visual Studio 2019/2022 with *Desktop development with C++* (its CMake and Ninja are used), Python 3.10+ with `pip install numpy pillow pefile==2024.8.26 capstone==5.0.9`, and EARTH DEFENSE FORCE 6 installed (EDF.dll build `678CCB46`).

```bat
set EDF6_GAME_DIR=C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6
build.cmd          & rem 8 players  -> dist\
build.cmd 10       & rem 10 players -> dist-10p\
build.cmd 12       & rem 12 players -> dist-12p\
powershell -ExecutionPolicy Bypass -File package.ps1 -Players 10
```

- The game folder is only read: `Root.cpk` for the menu frame with the "8Player MOD" label (`tools/make_menu_label.py` writes `assets/LYT_MAINFRAME.SGO`, which is derived from the game and therefore never committed), and `EDF.dll` for the tests that check every patch site against the game's code. Without `EDF.dll` those tests are skipped.
- `build.cmd` fetches the official EDFModLoader v1.0.10 `winmm.dll` (checked by SHA-256, `tools/fetch_modloader.ps1`) and builds the race-fixed loader the package ships (`tools/fix_winmm_proxy.py`).
- There is no CI build: the game's files cannot be put on a build server.

Packages land in `release\EDF6MultiSlot-<version>[-10p|-12p].zip`.

## License

EDF6MultiSlot: public domain ([Unlicense](LICENSE)). The bundled EDFModLoader (`third_party/EDFModLoader`, and the loader in the package) is MIT, Copyright BlueAmulet: [third_party/EDFModLoader/LICENSE.txt](third_party/EDFModLoader/LICENSE.txt).
