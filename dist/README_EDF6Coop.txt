EDF6Coop 2.5.0 - bigger and steadier online co-op for EARTH DEFENSE FORCE 6
Project page: https://github.com/hajisensai/edf-coop-stable
中文: README_EDF6Coop_zh.txt / 日本語: README_EDF6Coop_ja.txt
If something goes wrong, open an Issue and attach Mods\Plugins\EDF6Coop.log.

EDF6Coop is one EDFModLoader plugin (Mods\Plugins\EDF6Coop.dll). It does what EDF6DirectNet and
EDF6MultiSlot did, in one DLL with one log and one settings file:
- rooms for more than four players (Player MOD, 8 to 1024 as the host picks), and missions for them;
- a direct link between players that keeps a room together when Epic's service hiccups;
- resending of lost game packets, and survival of short connection drops.
Left switched off (Player MOD OFF, no direct link configured) you play exactly like the original game,
also with players who do not have the mod.

[Coming from EDF6DirectNet / EDF6MultiSlot]
Nothing to do by hand. INSTALL.bat (or the first game start) renames EDF6DirectNet.dll and
EDF6MultiSlot.dll in Mods\Plugins to .disabled, so the loader skips them. Their settings files stay
where they are; on its first start EDF6Coop writes Mods\Plugins\EDF6Coop.ini with your old values
carried over. A setting that no longer exists is named in the log ("is no longer a setting").
Going back is a rename: delete EDF6Coop.dll and remove ".disabled" from the old DLL.

[Room sizes: the host picks]
One package serves every room size. F2 / left-stick click cycles OFF, 8, 10, 12, 16, 24, 32, 48, 64,
128, 256, 512 and 1024. The INI also accepts RoomSize=2..1024; 0 is the normal four-player mode.
Everyone in a modded room must update to 2.5.0: the new slot layout and network protocol are not
compatible with 2.4.x modded rooms. Normal four-player rooms remain available.
1024 is an implementation limit, not a demonstrated practical multiplayer capacity. The new large-room
paths have automated/native-code coverage, but have not been verified in live play at those sizes.
Rooms above 16 players have no in-game voice chat. Required bandwidth and CPU grow with room size.

[Install / uninstall]
Extract the whole zip, double-click INSTALL.bat, then start the game from Steam as usual. (Or drop the
contents of the zip into the game folder, next to EDF6.exe: the layout is the game folder's.)
If EDFModLoader (winmm.dll) is missing, the bundled one is installed: official v1.0.10 with a fix for a
multithread bug (LOADER_FIX_JA.md). An existing winmm.dll is never overwritten, except one that is
byte-for-byte the official v1.0.10 file: it is replaced and kept as winmm.dll.bak-official.
Uninstall: first set Enabled=0 in the [MultiSlot] section of EDF6Coop.ini and start the game once (this
removes the files the mod wrote, Mods\UI\LYT_MAINFRAME.SGO and Mods\HUD\ONLINEHUDTEXTURE.RAB), then
double-click UNINSTALL.bat
(as administrator to remove the firewall rule too). It removes EDF6Coop's files, its settings, log, update
leftovers and its UPnP mapping. EDFModLoader, other mods and your old EDF6DirectNet / EDF6MultiSlot
settings files are left alone.

[Bigger rooms (Player MOD)]
- Outside a room, F2 or pressing the left stick steps the room size OFF -> 8 -> 10 -> 12 -> 16 -> 24 -> 32 -> 48 -> 64 -> 128 -> 256 -> 512 -> 1024
  -> OFF, shown bottom left of the menu (e.g. "F2/LS 12Player MOD :ON") and saved as [MultiSlot] RoomSize
  in EDF6Coop.ini.
  OFF (default): you create normal 4-player rooms anyone can join.
  ON: you create a room of the size you picked that only players with EDF6Coop 2.5.0 see.
  The room search shows normal rooms and bigger rooms of every size, whatever the setting.
  A room keeps the size it was created with; inside a room the menu shows that room's size.
- In a room, F3 / Tab / right stick switches the member page (four members per page).
- In a room, F4 / left stick turns "copy armor" on: your armor is raised, for the mission only, to the
  lowest armor of the others in the room (same class first). For new players; nothing is saved.
- Missions with more than four players: extra players get their gear, spawn points and items. Enemy
  counts grow by x0.2 per player above four: 5p x1.2, 8p x1.8, 12p x2.6, 16p x3.4, 24p x5.0, 32p x6.6
  (fixed objects and big bosses do not). [Mission] ExtraEnemies=0 keeps the original counts (everyone in the room should set the same).
- Every player of a mission has a colour of their own on the HUD: status lamp, chat balloon and radar
  marker. Players 1-4 keep the game's yellow, green, blue and red; 5-8 are orange, pink, purple and cyan,
  with a 32-colour palette reused for larger rooms. For this the mod writes Mods\HUD\ONLINEHUDTEXTURE.RAB (the game's HUD textures
  plus the new lamps and balloons). If another mod already has a file there, it is left alone and
  players 5 and up share the colours of players 1-4.
- Weapon pages: put modified weapon files in Mods\Variants\<page name>\WEAPON\*.SGO (vehicles in
  Mods\Variants\<page name>\OBJECT\V*.SGO or VEHICLE*.SGO). Each file takes the place of the game's weapon or
  vehicle with the same file name, so the weapon table and your save do not change: no new weapons, you need
  the original weapon to equip it, and its name stays the original's. Use ASCII folder names. F6 on any menu
  screen steps through off -> page 1 -> page 2 ... -> off ("F6 Page:<name>"); it takes effect from the next
  mission, offline too.
- In a room everyone uses the same files: the host's Mods\WEAPON / Mods\OBJECT\V*/VEHICLE*.SGO files and every
  member's page (the host's Mods first, then the host's page, then the others' pages in join order). They are
  downloaded only after consent by default: the menu says "ROOM WEAPONS 42%", then "ROOM WEAPONS :ON (2)". Each file is checked
  against the SHA-256 its owner published, switched between missions only, and you get your own files back
  when you leave the room. Only weapon (not WEAPONTABLE/WEAPONTEXT) and vehicle files travel, at most 128
  files and 4 MB per page, never a DLL or a patch. They are kept in Mods\Plugins\EDF6Coop.hostdata, never in
  your Mods folders. Players without the plugin see the original weapons. [HostData] in the ini:
  Accept=Ask (default, ask before downloading) / Auto (no prompt) / Never, WeaponPageKey, Page, Share=0 (offer nothing),
  Enabled=0 (all off).
- Keys and buttons are in [RoomScreen] / [CopyArmor] of the ini. F2 is reserved for Player MOD.

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
Who is in the room is the host's game's say: its member list goes to everyone over the direct link and
every game follows it. For 30 minutes after you drop out of a room, a room search Epic cannot run (its
lobby service down) shows that room; choosing it connects you straight to the host, without Epic. While
Epic works, nothing changes. It is
no longer shown once the host kicked you, the room closed or you entered another room. You and the host
both need 2.2.0 or later.

[Automatic updates]
At game start EDF6Coop fetches a newer release from GitHub; it runs from the next start. (2.2.x of every
room size updates to the same 2.5.0.)
The lower left of the menu (outside a room) says what happened: "EDF6Coop 2.3.2 (latest)",
"EDF6Coop 2.3.1 -> 2.3.2 downloaded, restart the game", then after the restart
"EDF6Coop 2.3.2 (updated from 2.3.1)". A failure says "update failed, see EDF6Coop.log".
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
  UI/LYT_MAINFRAME.SGO with one more label, and the HUD textures are the game's HUD/ONLINEHUDTEXTURE.RAB
  with more lamps and chat balloons, both built into the DLL. The colours of players 5-8 follow the
  8-player HUD textures FevGrave made, who also found the HUD code to change.
- EDFModLoader by BlueAmulet (MIT, EDFModLoader_LICENSE.txt): winmm.dll, with the fix in LOADER_FIX_JA.md.
