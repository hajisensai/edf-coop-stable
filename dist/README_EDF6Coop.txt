EDF6Coop 2.0.0 - bigger and steadier online co-op for EARTH DEFENSE FORCE 6
Project page: https://github.com/hajisensai/edf-coop-stable
中文: README_EDF6Coop_zh.txt / 日本語: README_EDF6Coop_ja.txt
If something goes wrong, open an Issue and attach Mods\Plugins\EDF6Coop.log.

EDF6Coop is one EDFModLoader plugin (Mods\Plugins\EDF6Coop.dll). It does what EDF6DirectNet and
EDF6MultiSlot did, in one DLL with one log and one settings file:
- rooms for more than four players (8Player MOD), and missions for them;
- a direct link between players that keeps a room together when Epic's service hiccups;
- resending of lost game packets, and survival of short connection drops.
Left switched off (8Player MOD OFF, no direct link configured) you play exactly like the original game,
also with players who do not have the mod.

[Coming from EDF6DirectNet / EDF6MultiSlot]
Nothing to do by hand. INSTALL.bat (or the first game start) renames EDF6DirectNet.dll and
EDF6MultiSlot.dll in Mods\Plugins to .disabled, so the loader skips them. Their settings files stay
where they are; on its first start EDF6Coop writes Mods\Plugins\EDF6Coop.ini with your old values
carried over. A setting that no longer exists is named in the log ("is no longer a setting").
Going back is a rename: delete EDF6Coop.dll and remove ".disabled" from the old DLL.

[Room sizes: 8p, 10p, 12p, 16p, 24p, 32p]
There is one package per room size (EDF6Coop-2.0.0-8p.zip and so on). A room of one size can only be
joined with the same size build: everyone in a room must use the same package. When unsure, use 8p.
8p is the size played the most. 10p and 12p are played less. 16p, 24p and 32p are new in 2.0.0 and have
only been checked offline with ghost players, not yet in real online play with that many people.
The bigger the room, the more upload bandwidth the host needs: with the direct link every player's data
goes through the host.

[Install / uninstall]
Extract the whole zip, double-click INSTALL.bat, then start the game from Steam as usual. (Or drop the
contents of the zip into the game folder, next to EDF6.exe: the layout is the game folder's.)
If EDFModLoader (winmm.dll) is missing, the bundled one is installed: official v1.0.10 with a fix for a
multithread bug (LOADER_FIX_JA.md). An existing winmm.dll is never overwritten, except one that is
byte-for-byte the official v1.0.10 file: it is replaced and kept as winmm.dll.bak-official.
Uninstall: first set Enabled=0 in the [MultiSlot] section of EDF6Coop.ini and start the game once (this
removes the menu layout file the mod wrote, Mods\UI\LYT_MAINFRAME.SGO), then double-click UNINSTALL.bat
(as administrator to remove the firewall rule too). It removes EDF6Coop's files, its settings, log, update
leftovers and its UPnP mapping. EDFModLoader, other mods and your old EDF6DirectNet / EDF6MultiSlot
settings files are left alone.

[Bigger rooms (8Player MOD)]
- Outside a room, F2 or pressing the left stick switches "8Player MOD" (shown bottom left of the menu).
  OFF (default): you create normal 4-player rooms anyone can join; the room search shows both kinds.
  ON: you create rooms for up to the build's size that only players with the same EDF6Coop build see;
  the room search shows only those rooms.
- In a room, F3 / Tab / right stick switches the member page (four members per page).
- In a room, F4 / left stick turns "copy armor" on: your armor is raised, for the mission only, to the
  lowest armor of the others in the room (same class first). For new players; nothing is saved.
- Missions with more than four players: extra players get their gear, spawn points and items. Enemy
  counts grow with the room: 5p x1.2, 6p x1.4, 7p x1.6, 8p and more x1.8 (fixed objects and big bosses
  do not). [Mission] ExtraEnemies=0 keeps the original counts (everyone in the room should set the same).
- Keys and buttons are in [RoomScreen] / [CopyArmor] of the ini. F2 is reserved for 8Player MOD.

[Direct link (optional, host only)]
Players joining your room connect straight to you. The ini comments (Chinese / Japanese / English, after
the Windows display language) explain every setting.
1. Set Mode=host in the [DirectNet] section of EDF6Coop.ini.
2. Right-click EDF6Coop_AllowFirewall.bat in the game folder -> Run as administrator (once, and again after
   changing ListenPort).
3. Be reachable: leave PublicAddress empty (the plugin maps the UDP port with UPnP and also advertises your
   public IPv6), or forward a UDP port yourself and set PublicAddress=your public IP:port.
4. Create a room as usual. "DIRECT client ... connected" in the log means a player is linked directly.
Joiners need nothing: they find the host's address in the room. If the link cannot be made, play goes on
through Epic as usual. Key= is an optional shared secret: every joiner must enter the same Key.
While the direct link works, Epic's own room-service hiccups cannot drop anyone.

[Automatic updates]
At game start EDF6Coop fetches a newer release of the same room size from GitHub; it runs from the next start.
Releases are signed (ECDSA P-256) and a file without a valid signature is never installed. A new version
that crashes before it has run for 20 seconds past the title screen is rolled back by itself.
To turn downloading off: [Update] AutoUpdate=0 in EDF6Coop.ini.

[Log]
Mods\Plugins\EDF6Coop.log: one log for everything (direct-link lines start with [DN]). It keeps itself
below about 2 MB. A clean exit ends with "SHUTDOWN the game exited"; without it the game crashed or was
killed. Please attach it to bug reports.

[Turning it off]
[MultiSlot] Enabled=0: no bigger rooms, the room screen is the original one.
[DirectNet] Enabled=0: no direct link, the game's Epic calls are left alone.
Both 0: the plugin unloads itself.

[Included]
- EDF6Coop: Mods\Plugins\EDF6Coop.dll (MIT, LICENSE.txt; the room part is public domain). The menu layout it writes is the game's own
  UI/LYT_MAINFRAME.SGO with one more label, built into the DLL.
- EDFModLoader by BlueAmulet (MIT, EDFModLoader_LICENSE.txt): winmm.dll, with the fix in LOADER_FIX_JA.md.
