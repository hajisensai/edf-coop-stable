# EDF6MultiSlot

**English** | [中文](README.zh-CN.md)

Online co-op for up to **8 players** in EARTH DEFENSE FORCE 6 (PC / Steam), with separate **10- and 12-player** builds. An [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) plugin.

Originally written by **momotori01** and published as part of [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) (released into the public domain, see [LICENSE](LICENSE)); its history is kept in this folder. This copy continues from EDF6MultiSlot 1.5.12.

The player-facing manual (Japanese) is [packaging/README_EDF6MultiSlot.txt](packaging/README_EDF6MultiSlot.txt); it ships in the package.

Changes in each version: [release-notes/](release-notes/).

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
