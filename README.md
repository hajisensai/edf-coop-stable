# edf-coop-stable

**English** | [中文](README.zh-CN.md) | [日本語](README.ja.md)

**EDF6Coop** makes online co-op in EARTH DEFENSE FORCE 6 (PC / Steam) bigger and steadier: rooms for up to 8, 10, 12, 16, 24 or 32 players, picked by the host in the game (Player MOD), a direct link between players that survives Epic hiccups, and lost-packet resending. It is one [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) plugin, `EDF6Coop.dll`; it replaces the two earlier plugins EDF6DirectNet and EDF6MultiSlot.

Online stability mods for the EARTH DEFENSE FORCE series. Currently supported: **EDF6**. Support for EDF5 and other titles is planned once EDF6 is stable.

> Unofficial mod, not affiliated with D3 PUBLISHER / SANDLOT / Epic Games. It only intercepts network calls inside the game process and never modifies any game files; set `Enabled=0` or delete the DLL to get the vanilla game back.
>
> **Status: experimental.** 900+ automated tests pass; auto direct connect and the disconnect grace period have been verified in real four-player online play (0.3.2: every player connected directly, and 7 connection drops were hidden from the game). If something goes wrong, please open an [Issue](https://github.com/hajisensai/edf-coop-stable/issues) with your log attached.

## Room sizes and the old plugins

There is one package for everyone: `EDF6Coop-<version>.zip`. Every install has memory for 32 players; the **host** picks the room size in the game. On a menu screen outside a room press **F2** (or click the left stick) to cycle OFF -> 8 -> 10 -> 12 -> 16 -> 24 -> 32 -> OFF; the lower left of the menu shows e.g. `F2/LS 12Player MOD :ON` or `F2/LS Player MOD :OFF`. The choice is saved as `RoomSize=` under `[MultiSlot]` in `EDF6Coop.ini` (`0` = normal 4-player room, `5`..`32` = MultiSlot room of that size; an old `EightPlayerRooms=1` is read as `RoomSize=8` and replaced on the next save). OFF creates a normal 4-player room that anyone can join, with or without the mod. ON creates a MultiSlot room of the chosen size, visible only to EDF6Coop 2.3.0 or newer. Whatever the setting, the room list shows both normal rooms and MultiSlot rooms of every size; a guest can join any size, and a room keeps the size it was created with. **16, 24 and 32 players have only been checked offline with ghost players**, not yet in real online play with that many people. Rooms of 24 and 32 have no in-game voice chat: Epic allows a voice chat room only up to 16 players. The bigger the room, the more upload bandwidth the host needs: with the direct link every player's data goes through the host.

The room part (8Player MOD: more than four players, the room screen, missions, armor copy) was written by **momotori01** as EDF6MultiSlot (public domain, [multislot/LICENSE](multislot/LICENSE)); its history and tests live in [multislot/](multislot/README.md). From 2.0.0 it and EDF6DirectNet are one DLL with one log (`EDF6Coop.log`) and one settings file (`EDF6Coop.ini`). Installing renames an old `EDF6DirectNet.dll` / `EDF6MultiSlot.dll` to `.disabled`; their settings files are kept and their values carried into `EDF6Coop.ini` at the first start.

## What it fixes

| Symptom | Cause | What the plugin does | Who needs to install it |
|---|---|---|---|
| Enemies / positions / health drift apart mid-mission (desync) | EDF6 sends all online data as `UnreliableUnordered`, so one lost packet means one piece of state is lost for good | Sends it as `ReliableUnordered` instead (lost packets are resent automatically, out-of-order delivery is still allowed, so the delivery semantics the game sees are unchanged); enlarges the EOS send/receive queues to at least 64MB so packets are not dropped when a queue fills up | **The sender.** If you install it, the data you send is no longer lost; best when everyone installs it |
| A short network hiccup kicks you out of the room and the mission is wasted | When an EOS connection is closed because of a timeout / network error, the game removes that player immediately | Keeps it from the game for the moment and lets EOS reconnect in the background; if the connection recovers within 30 seconds the game never notices, otherwise it is handled like vanilla | **Both sides** (the plugin detects the other side automatically, see below) |
| High latency through the Epic relay, NAT traversal fails | All traffic goes through EOS P2P / relay | The host can enable "public direct connect": others connect straight to the host after joining the room (star topology, the host relays) | Set up by the host; joiners just need the plugin installed |
| You get disconnected and have no idea why | — | Writes a diagnostic log: NAT type, direct/relay, disconnect reasons, lobby members, per-minute send/receive stats | Yourself |

Everything is on by default (public direct connect has to be enabled manually by the host).

## Install

1. Download `EDF6Coop-<version>.zip` from [Releases](https://github.com/hajisensai/edf-coop-stable/releases/latest) and **extract the whole archive** to any folder.
2. Double-click **`INSTALL.bat`**: it finds EDF6 in your Steam library automatically and installs.
   - If EDFModLoader is not present, the bundled loader is installed: [BlueAmulet/EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) v1.0.10 (MIT) **with a fix for a multithread bug in the official build** (its call forwarders shared one target variable, so a call from several threads at once could jump to the wrong function; details in [multislot/packaging/LOADER_FIX_JA.md](multislot/packaging/LOADER_FIX_JA.md)). An existing `winmm.dll` is never overwritten, with one exception: if it is byte-for-byte the official v1.0.10 file, it is replaced by the fixed build and the original is kept as `winmm.dll.bak-official`. Any other (newer or patched) loader is left alone.
   - If the game cannot be found, you will be asked to paste the game folder (in your Steam library, right-click EDF6 → Manage → Browse local files).
3. Start the game from Steam as usual. The first launch creates `Mods\Plugins\EDF6Coop.ini` (settings) and `EDF6Coop.log` (log).

The zip contains README_EDF6Coop.txt (English), README_EDF6Coop_zh.txt (中文) and README_EDF6Coop_ja.txt (日本語); the default settings file is commented in your Windows display language (Chinese / Japanese / otherwise English).

Upgrade: the plugin updates itself. At game start it asks GitHub for the latest release in the background; when there is a newer one it downloads `EDF6Coop.dll` and the signed manifest `EDF6Coop.dll.sig` and puts the DLL in place as `EDF6Coop.dll`, and it runs from the next game start (the log says `UPDATE installed ...`). Everything else keeps working if GitHub cannot be reached (the request uses the system proxy). Running a newer `INSTALL.bat` still works as before and keeps your settings. What you can rely on:

- **Signed releases.** The manifest (version and SHA-256 of the DLL) is signed with ECDSA P-256 by a key that only the release pipeline holds; the matching public key is built into the plugin. A download is rejected unless the signature verifies, the signed version is the release being installed and newer than the running one, and the DLL has the signed SHA-256 and says it is that version. Files without a valid signature are never installed, and downloads only come from this repository's release URLs. Someone who can alter the release assets or your connection but cannot sign gets nothing installed.
- **Automatic rollback.** The replaced DLL stays next to the new one as `EDF6Coop.dll.old` until the new version has run for 20 seconds after the game reaches its title screen. If the game crashes or is killed before that, the next start puts the old version back by itself, remembers the failed version (`EDF6Coop.dll.bad`, it is not installed again) and runs that session without the plugin. Quitting the game normally before then is no failure: the new version simply stays on trial at the next start.
- **Settings files from older versions.** A settings file that has no `AutoUpdate` line (older versions did not write one) keeps automatic updates **on**, as 0.3.6 did; the log says so at every start. To turn them off, add these two lines at the end of `Mods\Plugins\EDF6Coop.ini`:

  ```
  [Update]
  AutoUpdate=0
  ```

  A settings file created by a current version has `AutoUpdate=1`. `AutoUpdate=0` turns downloading off (rollback still works).
Uninstall: double-click `UNINSTALL.bat` (EDFModLoader and other mods are left alone, including a loader the installer upgraded; its backup `winmm.dll.bak-official` stays). It removes the plugin, settings, log and update leftovers (`EDF6Coop.dll.old` and friends), and the UPnP mapping the plugin made on your router (only one that points to this PC and is named `EDF6DirectNet`; a router without UPnP is simply skipped). If you added the firewall rule, run `UNINSTALL.bat` as administrator to remove it too; without administrator rights it shows the command to remove it.

You can also extract the zip contents into the game folder (the folder containing `EDF6.exe`) by hand.

## Host: enabling public direct connect (optional)

Only the host needs to set this up; joiners who have the plugin installed with `AutoJoin=1` (the default) connect directly on their own after joining the room.

1. Open `Mods\Plugins\EDF6Coop.ini` and set `Mode=host`.
2. Right-click `EDF6Coop_AllowFirewall.bat` in the game folder → **Run as administrator** (only needed once). The rule allows inbound UDP for `EDF6.exe` on the `ListenPort` from your ini only (27015 if unset), not every UDP port; run it again after changing `ListenPort`.
3. Make yourself reachable from the internet, pick one:
   - **Automatic**: leave `PublicAddress` empty. The plugin maps UDP 27015 via your router's UPnP and also advertises this PC's public IPv6 automatically.
   - **Manual**: forward the UDP port to this PC on your router, then set `PublicAddress=public IP:external port` (a DDNS hostname also works, e.g. `myroom.ddns.net:40000`).
   - **PC dials the internet directly (PPPoE, no router)**: the plugin cannot detect this kind of adapter and will not advertise an address automatically; set `PublicAddress=public IP:27015` by hand.
4. Restart the game and create a room as usual.

How to confirm it worked (check the log `Mods\Plugins\EDF6Coop.log`):

| Log | Meaning |
|---|---|
| `DIRECT players who join your room connect to ...` | The host address has been determined and written into the room info |
| `UPNP router now forwards UDP ...` | UPnP mapping succeeded |
| `UPNP no router with UPnP port mapping found` / `UPNP port mapping failed` | The router does not support UPnP or it is disabled; use manual port forwarding instead |
| `UPNP WARNING: the router WAN address ... is private (carrier-grade NAT)` | You are behind carrier-grade NAT (common with many ISPs) and have no public IPv4, so IPv4 direct connect is impossible; you can only rely on IPv6 or ask your ISP for a public IP |
| `UPNP UDP 27015 is already forwarded to ...; left alone` | This port on the router is already forwarded to another device on your LAN; the plugin will not delete it. Pick a different `ListenPort`, or forward manually |
| `DIRECT client ... connected from ...` (host) / `DIRECT connected to host ...` (joiner) | Direct connection established |
| `DIRECT auto-connect stopped (the room host did not answer on any advertised address ...)` | The joiner cannot reach the host (firewall / port forwarding / Key mismatch / a different EDF6Coop version); the game keeps using Epic as usual and retries after 60 seconds |

Joiners try each address for 10 seconds in IPv4 → IPv6 order; if none works they stay on EOS, and normal play is not affected.

**About `Key=`**: an optional passphrase; you do not need it to be safe. Every plugin player publishes, in its own lobby member info, the fingerprint of a key made for this game session. The host lets someone connect as player X only after they prove they hold X's key, and a joiner accepts only a host that proves it holds the room owner's key. Every direct-link packet is then authenticated with keys only those two have, so nobody else — not another player in the room, not someone on the network path — can connect as someone else, take over a link, or alter, inject or replay packets. The Key, when set, is mixed into those keys as an extra shared secret. It is **not** written into the room info — if the host sets a Key, every joiner must put the same Key in their own ini, otherwise auto direct connect fails and falls back to EOS. If you only play with friends you can leave it unset.

**Privacy**: the host's public address is stored in the lobby member attributes, so anyone who can see the room can read it. A player who joins directly (`AutoJoin=1`) connects to the host from their own public address, so the host sees it (vanilla EOS peer-to-peer usually exposes both addresses to each other too). Set `AutoJoin=0` if you do not want hosts of rooms you join to see your address; you then stay on EOS.

## Settings reference (`EDF6Coop.ini`, restart the game after editing)

The direct link's keys are below. The same file also has `[MultiSlot]`, `[Smoothing]`, `[RoomScreen]`, `[Mission]`, `[CopyArmor]` and `[HostData]` for the bigger rooms; the comments in the file explain every key. `[MultiSlot] Enabled=0` and `[DirectNet] Enabled=0` together unload the plugin.

| Key | Default | Description |
|---|---|---|
| `[DirectNet] Enabled` | `1` | `0` = the plugin installs no hooks at all, identical to vanilla |
| `Mode` | `off` | `off` regular player / `host` act as direct-connect host / `join` specify the host address manually (rarely needed, auto direct connect covers it) |
| `ListenPort` | `27015` | host: the UDP port to listen on (this is the one to forward and allow through the firewall); join: local port, empty = automatic |
| `PublicAddress` | empty | host: tells others where to connect. Empty = public IPv6 + UPnP-mapped IPv4 |
| `AutoJoin` | `1` | When you join someone else's room and the host has direct connect enabled, connect to it automatically (the host then sees your public address; `0` stays on EOS) |
| `HostAddress` | empty | `Mode=join` only: host address, e.g. `123.45.67.89:27015` / `[2408:8207::5]:27015` |
| `Key` | empty | Direct-connect passphrase, must be identical for everyone; letters and digits only |
| `UPnP` | `1` | Let the router set up port forwarding automatically when hosting. Only used with `Mode=host`; with the default `Mode=off` nothing is opened |
| `BindPhysicalInterface` | `1` | Pin direct-connect traffic to the physical network adapter so it is not hijacked by the TUN adapter of Clash / VPN / game accelerator |
| `LinkTimeoutMs` | `60000` | How long without data from the other side before a direct link counts as disconnected (3000–300000) |
| `[EOS] FixedPort` | `0` | EOS uses fixed UDP ports `FixedPort`–`FixedPort+7`; 0 = random |
| `Relay` | `default` | EOS relay: `default` unchanged / `allow` / `norelay` / `force` |
| `[Sync] ReliableGameTraffic` | `1` | Desync prevention (reliable sending). `0` = vanilla |
| `[Resilience] HoldDisconnects` | `auto` | Disconnect grace: `auto` only for players with the plugin / `off` vanilla / `all` no detection, grace for everyone (only if you are sure everyone has the plugin) |
| `GraceSeconds` | `30` | Maximum number of seconds a disconnect is hidden (1–600) |
| `[Update] AutoUpdate` | `1` | Install newer signed releases from GitHub automatically (they run from the next game start). A settings file without this line counts as `1` too |

## How it works

### Reliable sending

Both `EOS_P2P_SendPacket` call sites in `EDF.dll+0x12c8bc0` pass `Reliability = UnreliableUnordered`. The plugin changes it to `ReliableUnordered` at the import-table level: every packet is delivered exactly once, possibly out of order — which is a delivery pattern unreliable transport is already allowed to produce, so game logic is unaffected; packets just stop getting lost. The receiver needs no cooperation.

### Disconnect grace and plugin detection

If the other player does not have the plugin, their game removes you from the match as usual; if your side kept hiding the disconnect, the two sides' state would diverge, so grace can only be given to players who have the plugin.

EDF6 parses every EOS packet it receives as game data (`ReceivePacket` is called with `RequestedChannel` = NULL), so probe packets cannot be sent over P2P. The plugin uses lobby **member attributes** instead: after joining/creating a room it writes `EDF6DN=1` on itself (`EDF.dll` imports no member-attribute functions, so the game cannot see it), and on disconnect it reads from the local lobby data whether the other player has this attribute:

- Marker present and the connection was established before → grace, calling `AcceptConnection` every 2 seconds to request a reconnect;
- No marker → handled like vanilla;
- Public direct-connect member → grace for as long as the direct link answers (it pings every second, in menus too); its game data does not go through EOS anyway;
- Epic's lobby service reports a member as disconnected (`DISCONNECTED`: it lost Epic's lobby service, not the game) while that member's direct link answers → hidden from the game. When EOS puts the member back into the lobby (`JOINED`), both events are swallowed and the game never learns about it. The game is told only once the direct link has been silent for `GraceSeconds`, or the member leaves or is kicked for real. The same applies to ourselves;
- The other player actually left the lobby → the disconnect is handed to the game immediately.

### Public direct connect

The host additionally writes the member attribute `EDF6DN_ADDR` (a space-separated address list). Other members read it every 2 seconds and try each address for 10 seconds in IPv4, IPv6 order; if all fail they retry after 60 seconds. The transport is a custom UDP protocol: selective acknowledgement, token-bucket rate-limited retransmission, and per-link keys (below); `IP_UNICAST_IF` binds to the physical adapter to prevent TUN hijacking.

Every plugin player also writes `EDF6DN_ID`: the fingerprint (SHA-256) of an ECDSA P-256 key made for this game session. Connecting starts with a cookie round trip: the host answers a hello with a cookie bound to the sender's address and keeps nothing until the cookie comes back (so forged sender addresses and hello floods achieve nothing), then the joiner repeats the hello with the cookie, signed with that key. The host accepts it for EOS ID X only if X is in the room and published that key's fingerprint; a signed hello of an earlier session is rejected as a replay. The joiner in turn accepts the host's welcome only if it is signed by the key whose fingerprint the room owner published. Hello and welcome carry fresh ECDH keys under those signatures, so every link gets its own keys (with `Key=` mixed in when set), and every packet on it carries a tag with those keys and a counter: altered, injected or replayed packets are dropped. So only room members who proved their identity can send data. There is no encryption. EDF6DirectNet versions with different protocols (0.3.6 and older speak 2, 0.4.0 speaks 3) ignore each other's packets and keep using EOS with each other.

### The host's say, and rejoining without Epic

The game learns its room's members from the lobby: the list when it enters, then one status event per change. Epic's lobby service is one source of those events, the host is the other: from protocol 6 the host sends its game's member list (the `Room` message) with the roster to every directly linked member, and every member's game follows it. Both sources can say the same thing (someone left: Epic and the host both report it), so what the plugin would tell the game waits 5 s for Epic to say it first: while Epic works the game sees Epic's events, as without the plugin. When Epic is late, every event goes through a local view: a member already in does not join again, one already gone does not leave again, and that we were removed or the room closed is said once (log `ROOM ... not told again`). Members in the room by their direct link only also show in the room's member list the game reads.

- Host: a player Epic does not list, with a direct link, whom the game does not have is let into the room while it has space (their plugin dials the host only while their game is in the room); one in the room by its direct link only has left once that link stayed down for `GraceSeconds`; one that just left is not brought back by its old link before that times out, only by a new one. A player the host kicked is not let back in by their direct link in that room, unless they come in through Epic again. Kicking a player Epic does not know of is done by the plugin.
- Member: while in someone else's room, the plugin notes it every 2 s (host, the host's published identity and addresses, the room's attributes). For 30 minutes after leaving, a room search Epic cannot run (its lobby service down) gets that room as its result; a search Epic answers reaches the game as it is. choosing it dials the host at the noted addresses (5 s each, 30 s in all), and the room is entered once the host's member list includes you. It is forgotten when the room closes, when you are kicked, when you enter another room, or when the room's host changes. That room is the host's real room, but Epic does not know you are in it: leaving or closing it in the game completes locally.

### Weapon pages and the room's weapon files

A weapon page is a set of modified weapon and vehicle files you can switch in the game. Put them in `Mods\Variants\<page name>\WEAPON\*.SGO` and, for vehicles, `Mods\Variants\<page name>\OBJECT\V*.SGO` / `VEHICLE*.SGO`. Each file takes the place of the game's weapon or vehicle with the same file name, so the weapon table, your save and other players' weapon lists stay as they are: there are no new weapons, a modified weapon sits on an existing weapon's slot, you need to own that original weapon to equip it, and menus still show the original's name. ASCII folder names are recommended (the menu font may not show other characters).

- On any menu screen **F6** steps through off -> page 1 -> page 2 ... -> off, in folder name order. The lower left shows `F6 Page:<name>` or `F6 Page:off` (only when at least one page exists). The choice is saved as `[HostData] Page=<name>` and takes effect from the next mission (the game reads weapon files when a mission starts). It works offline too.
- In a room everyone uses the same files: the host's modified files (`Mods\WEAPON\*.SGO`, `Mods\OBJECT\V*.SGO` / `VEHICLE*.SGO`) and every member's selected page are shared. When two of them have the same file, this order decides: the host's Mods first, then the host's page, then the other members' pages in the order they joined (lobby order).
- Every machine publishes the SHA-256 of what it offers on its own lobby entry, which only it can write. Every machine works out the same order, fetches what it does not have over the P2P link the game already has, and checks every byte against the SHA-256 that member published.
- `[HostData] Accept=Always` (default) takes the room's files without a key press. The menu shows this first: `ROOM WEAPONS 42%` while fetching, then `ROOM WEAPONS :ON (2)` (2 = how many of the room's sources are not your own), with ` 1 failed` when one could not be fetched and ` -3 of yours` when 3 files of your page gave way to an earlier source. With `Accept=Ask` it says `F1 ROOM WEAPONS :OFF (2)` and F1 takes them or gives them back; with `Never` it says `ROOM WEAPONS differ (2)` and takes nothing.
- Only data travels: weapon files (not `WEAPONTABLE` or `WEAPONTEXT`, so no new weapons and nothing that stays in your save) and vehicle files, at most 128 files, 256 KB each, 4 MB in all per page, up to 16 pages; files that do not fit are named in the log. Never a DLL, a patch or anything else, and a file that is not an SGO is refused.
- The files are kept in `Mods\Plugins\EDF6Coop.hostdata`, never in your Mods folders. The game is pointed at them file by file, in memory only, and the switch happens between missions only: leaving the room, quitting or a crash all bring back your own files.
- Players without the plugin see the original weapons: the ids do not change, so nothing crashes.
- `[HostData] Share=0` offers nothing (neither your Mods nor your page; you still use your page yourself); `Enabled=0` turns all of this off and leaves the game's file loading alone.

## Troubleshooting

- **Check the log first**: `Mods\Plugins\EDF6Coop.log` (it keeps itself below about 2MB; direct-link lines start with `[DN]`). A line `==== EDF6Coop x.y.z-<N>p ====` means the plugin loaded; if that line is missing, EDFModLoader is not installed correctly.
- `EOS hooks FAILED`: a game update changed the import table; the plugin disables direct connect automatically. Please open an Issue.
- `LOBBY plugin detection UNAVAILABLE`: the plugin cannot tell whether the other player has it installed, so disconnect grace only applies to direct-connect members.
- `EOS incoming packet queue FULL`: the EOS queue is full and packets are being dropped. Please open an Issue with your log attached.
- `RESILIENCE ... RECOVERED` means a disconnect was hidden successfully; `did not come back within` means it was handed to the game after the timeout.
- `RESILIENCE ... lost Epic's lobby service but the direct link is up` / `back in Epic's lobby service`: Epic's lobby service dropped a player and let them back in; the game never saw it. `direct link silent for ...` means the player really was gone and the game was told.
- `REJOIN ...`: coming back without Epic. `remembering room` noted the room; `the room list gets room` Epic could not search, so the list got it; `dialling the room's host` / `in room ... again` connecting / back in the room; `its host did not let us in within` the host did not let you in within 30 s (the host is gone, or kicked you).
- `GAME kicks ... from the room (direct link up/down, ...)`: the game removed a player by itself (or you kicked them). It records whether the direct link still showed that player playing at that moment.
- `STATS last 60s: ...` is one line per minute while the game sends or receives anything: how much the game itself sends (average and busiest second, in kbps; the game keeps its routine sync under about 320 kbps and drops less important updates near its budget), how much of it is the same data sent to several players, how many packets repeat one sent to the same player within 5 s (the game's own resends, if it has any), what the direct link really uses on the wire (`wire up`/`down`, resends included, plus what the host relays for others), packet counts, and per link: `retx` our resends, `dup` the other side's resends we already had, `gaveup` packets the game sent unreliably that we stopped resending after 2 s, `skipped` theirs that never came, `held` how long resends waited because the link delivered too little, `credit` resends allowed right now. If `send-failures` or `send-refused` is not 0, please attach your log.
- `DIRECT ... timed out: nothing received for ...` means the other side went silent; `DIRECT ... stalled: a packet the game sent reliably is unacknowledged ...` means the other side still answers but one packet the game needs never got through.
- `DIRECT refused hello for ...`: someone tried to connect directly as a player and could not prove it. `published no direct-link identity` for a second or two after a player joins is normal (their room info has not reached the host yet; they retry every second); for a player without the plugin, or with 0.3.6 and older, it means they stay on EOS. `not signed by the identity that player published` means someone else claimed to be that player; they were rejected and cannot disturb that player.
- `DIRECT ... speaks direct-link protocol 5, we speak 6`: that player runs a different version (6 is EDF6Coop 2.2.0; 5 is 2.0.0-2.1.0; 4 is EDF6DirectNet 0.4.1, 3 is 0.4.0, 2 is 0.3.6 or older). There is no direct link between you, the game keeps working over EOS; update both to the same version.

## Build

Requires Windows x64, Visual Studio 2022 with *Desktop development with C++* (its CMake and Ninja are used), Python 3.10+ with `pip install numpy pillow pefile==2024.8.26 capstone==5.0.9`, and EARTH DEFENSE FORCE 6 installed (EDF.dll build `678CCB46`).

```powershell
$env:EDF6_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6'
powershell -ExecutionPolicy Bypass -File build.ps1 -Test    # -> multislot\dist\EDF6Coop.dll
powershell -ExecutionPolicy Bypass -File package.ps1        # -> release\EDF6Coop-<version>.zip (+ .sha256)
```

- There is one build for all room sizes (the room size is chosen in the game, see above). `-Test` runs the tests (unit tests, loopback multi-node direct-link tests, and the checks of every patch site against the game's `EDF.dll`, which are skipped without the game).
- The game folder is only read: `Root.cpk` for the menu frame with the "Player MOD" label (`multislot/tools/make_menu_label.py` writes `multislot/assets/LYT_MAINFRAME.SGO`, which is derived from the game and therefore never committed) , `HUD/ONLINEHUDTEXTURE.RAB` for the per-player HUD colours (`multislot/tools/make_hud_colours.py` writes `multislot/assets/ONLINEHUDTEXTURE.RAB`, also derived and never committed) and `EDF.dll` for the tests.
- `build.ps1` fetches the official EDFModLoader v1.0.10 `winmm.dll` (checked by SHA-256) and builds the race-fixed loader the package ships (`multislot/tools/fix_winmm_proxy.py`).
- `package.ps1` refuses a build that is not this version, a DLL older than its sources, and a CI build (`build.ps1 -CI` builds without the game's menu and HUD assets and embeds placeholders).

## Release

Release packages need the game's menu asset, which cannot be on a build server, so they are built on a machine with the game; GitHub Actions runs the CI builds and tests (`.github/workflows/ci.yml`).

1. Bump the version: `project(EDF6Coop VERSION x.y.z)` in `multislot/CMakeLists.txt` and the first line of the three bundled readmes `dist/README_EDF6Coop*.txt`.
2. Write the release notes `release-notes/<version>.md` (the body of the Release page and `RELEASE_NOTES_EDF6Coop.md` in the package).
3. Commit to `main`, tag it and push the tag: `git tag v2.0.0 && git push origin v2.0.0`.
4. On the machine with the game, at that commit: `powershell -ExecutionPolicy Bypass -File release.ps1 -Upload`. It runs `build.ps1 -Test` and `package.ps1`, then creates a **draft** Release with `EDF6Coop.dll`, `EDF6Coop-<version>.zip` and its `.sha256`, plus the same DLL under the six old names `EDF6Coop-<N>p.dll` (8, 10, 12, 16, 24, 32) so that 2.2.x installs of every size can update (without `-Upload` it only stages them in `release\upload-<version>\`). It never signs.
5. `gh workflow run release.yml -f tag=v2.0.0` (`.github/workflows/release.yml`): checks that the tag is on `main` and matches the version, that the DLL and the zip are in the draft, that each zip matches its `.sha256` and carries exactly that DLL, and that none is a CI build; signs the DLL with `sign-update.ps1` (the key is the repository secret `EDF6DN_UPDATE_SIGNING_KEY`, only in a job with a read-only token); uploads the `.dll.sig` and `.dll.sha256` (also for the old names); and publishes the Release as latest.

The auto-updater of every installed copy downloads `EDF6Coop.dll` / `.dll.sig` from the latest Release; 2.2.x copies still ask for `EDF6Coop-<N>p.dll` of their own size, which is why the Release also carries the same DLL under all six old names.

## Layout

| Path | Contents |
|---|---|
| `multislot/src/plugin.cpp` | EDFModLoader entry point: reads `EDF6Coop.ini` (carrying over the old settings files), starts the room part and the direct link |
| `multislot/src/` | The room part (Player MOD): patches, room screen, missions, armor copy, menu layout, HUD colours, log |
| `src/config.*` | The direct link's settings and their comments (Chinese / Japanese / English after the Windows display language) |
| `src/eos_min.h` | The EOS SDK structs in use (from the official 1.15.5 headers; the game uses 1.16.1) |
| `src/eos_hooks.cpp`, `src/iat.*` | Patches the `EDF.dll` import table to take over EOS P2P / lobby calls |
| `src/hold.*` | Disconnect grace |
| `src/lobby_marker.*` | Lobby member attributes: detecting who has the plugin, distributing the host address |
| `src/direct_net.*`, `src/reliable.*`, `src/wire.*`, `src/auth.*` | Direct-connect transport |
| `src/netif.*`, `src/upnp.*` | Physical adapter detection, UPnP |
| `src/updater.*`, `src/product.h` | Signed automatic updates; the version every release carries |
| `multislot/CMakeLists.txt` | The build and the one version number (`project(EDF6Coop VERSION x.y.z)`) |
| `multislot/packaging/` | Loader fix and handshake recovery notes shipped in the package |
| `dist/` | Install scripts and bundled readmes (English / Chinese / Japanese) |
| `tests/`, `multislot/tests/` | Tests |

## Known limitations

- Joining in the middle of a mission is not supported (the game itself has no such feature).
- Only desync caused by packet loss is fixed; desync in the game logic itself needs concrete symptoms before it can be reverse-engineered.
- During the disconnect grace period other players may wait at a sync point, for at most `GraceSeconds` seconds.
- Direct connect is relayed by the host: at the moment a joiner's direct link reconnects, the small amount of data the host is relaying for them that has not been acknowledged yet is lost (the game then falls back to EOS).
- Direct-link traffic is authenticated, not encrypted: someone on the network path can read it (as they can see who plays with whom), and can always drop it (the game then falls back to EOS).
- UPnP mappings are not removed when the game exits (there is no safe point for the network calls then): the plugin removes the one it made at the next game start that does not host (`Mode` not `host`, or `UPnP=0`), and `UNINSTALL.bat` removes it too. It never touches mappings that belong to other devices. Until then it is harmless while nothing listens on the port; you can also delete the mapping named `EDF6DirectNet` in your router's admin page.
- Nothing can be hidden when the direct link itself is down: if a player's own internet drops for longer than `GraceSeconds`, the game handles it as usual.

- Weapon pages and the room's weapon files switch between missions only, and only the kinds listed above travel: mods that add weapons (`WEAPONTABLE`), `mod.cpk` / `mod.dll` mods and patches are not shared.

## License

MIT, see [LICENSE](LICENSE). The bundled EDFModLoader is MIT; its license is included in the package as `EDFModLoader_LICENSE.txt`. The room part (`multislot/`, by momotori01) is public domain ([Unlicense](multislot/LICENSE)).
