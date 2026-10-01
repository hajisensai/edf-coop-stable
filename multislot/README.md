# EDF6MultiSlot (the room part of EDF6Coop)

**English** | [中文](README.zh-CN.md)

Online co-op for more than four players in EARTH DEFENSE FORCE 6 (PC / Steam): rooms of 8, 10, 12, 16, 24 or 32 players, the room screen, missions and armor copy ("8Player MOD").

Originally written by **momotori01** and published as part of [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) (released into the public domain, see [LICENSE](LICENSE)); its history is kept in this folder. This copy continued from EDF6MultiSlot 1.5.12, and from 2.0.0 it is built together with the direct link (`../src/`) into one plugin, **EDF6Coop.dll**.

Building, packaging, releasing and the player manuals are described once, in the repository [README](../README.md#build); the bundled manuals are `../dist/README_EDF6Coop*.txt`. In short:

```powershell
$env:EDF6_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6'
powershell -ExecutionPolicy Bypass -File ..\build.ps1 -Players 8 -Test    # -> dist\EDF6Coop.dll
powershell -ExecutionPolicy Bypass -File ..\package.ps1 -Players 8        # -> ..\release\EDF6Coop-<version>-8p.zip
```

`build.cmd [8|10|12|16|24|32]` in this folder does the same as `build.ps1 -Players <N> -Test`.

Changes up to EDF6MultiSlot 1.5.x: [release-notes/](release-notes/); from 2.0.0: [../release-notes/](../release-notes/).

Rooms of 16, 24 and 32 players are new in 2.0.0 and have only been checked offline with ghost players.

## License

EDF6MultiSlot: public domain ([Unlicense](LICENSE)). The rest of EDF6Coop is MIT ([../LICENSE](../LICENSE)). The bundled EDFModLoader (`third_party/EDFModLoader`, and the loader in the package) is MIT, Copyright BlueAmulet: [third_party/EDFModLoader/LICENSE.txt](third_party/EDFModLoader/LICENSE.txt).
