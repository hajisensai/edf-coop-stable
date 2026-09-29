# edf-coop-stable

**English** | [中文](README.zh-CN.md) | [日本語](README.ja.md)

**EDF6DirectNet** is an online co-op stability plugin for EARTH DEFENSE FORCE 6 (PC / Steam). It runs as an [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) plugin.

Online stability mods for the EARTH DEFENSE FORCE series. Currently supported: **EDF6**. Support for EDF5 and other titles is planned once EDF6 is stable.

> Unofficial mod, not affiliated with D3 PUBLISHER / SANDLOT / Epic Games. It only intercepts network calls inside the game process and never modifies any game files; set `Enabled=0` or delete the DLL to get the vanilla game back.
>
> **Status: experimental.** 900+ automated tests pass; auto direct connect has been verified in real online play between two PCs (0.3.2); the disconnect grace period has not yet been tested across two PCs. If something goes wrong, please open an [Issue](https://github.com/hajisensai/edf-coop-stable/issues) with your log attached.

## What it fixes

| Symptom | Cause | What the plugin does | Who needs to install it |
|---|---|---|---|
| Enemies / positions / health drift apart mid-mission (desync) | EDF6 sends all online data as `UnreliableUnordered`, so one lost packet means one piece of state is lost for good | Sends it as `ReliableUnordered` instead (lost packets are resent automatically, out-of-order delivery is still allowed, so the delivery semantics the game sees are unchanged); enlarges the EOS send/receive queues to at least 64MB so packets are not dropped when a queue fills up | **The sender.** If you install it, the data you send is no longer lost; best when everyone installs it |
| A short network hiccup kicks you out of the room and the mission is wasted | When an EOS connection is closed because of a timeout / network error, the game removes that player immediately | Keeps it from the game for the moment and lets EOS reconnect in the background; if the connection recovers within 30 seconds the game never notices, otherwise it is handled like vanilla | **Both sides** (the plugin detects the other side automatically, see below) |
| High latency through the Epic relay, NAT traversal fails | All traffic goes through EOS P2P / relay | The host can enable "public direct connect": others connect straight to the host after joining the room (star topology, the host relays) | Set up by the host; joiners just need the plugin installed |
| You get disconnected and have no idea why | — | Writes a diagnostic log: NAT type, direct/relay, disconnect reasons, lobby members, per-minute send/receive stats | Yourself |

Everything is on by default (public direct connect has to be enabled manually by the host).

## Install

1. Download `EDF6DirectNet-v*.zip` from [Releases](https://github.com/hajisensai/edf-coop-stable/releases/latest) and **extract the whole archive** to any folder.
2. Double-click **`INSTALL.bat`**: it finds EDF6 in your Steam library automatically and installs.
   - If EDFModLoader is not present, the bundled official build is installed ([BlueAmulet/EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) v1.0.10, MIT); an existing `winmm.dll` is never overwritten.
   - If the game cannot be found, you will be asked to paste the game folder (in your Steam library, right-click EDF6 → Manage → Browse local files).
3. Start the game from Steam as usual. The first launch creates `Mods\Plugins\EDF6DirectNet.ini` (settings) and `EDF6DirectNet.log` (log).

The zip contains README_EDF6DirectNet.txt (English), README_EDF6DirectNet_zh.txt (中文) and README_EDF6DirectNet_ja.txt (日本語); the default settings file is commented in your Windows display language (Chinese / Japanese / otherwise English).

Upgrade: run the new version's `INSTALL.bat` again; your settings file is kept.
Uninstall: double-click `UNINSTALL.bat` (EDFModLoader and other mods are left alone). If you added the firewall rule earlier, the uninstaller shows you the command to remove it.

You can also extract the zip contents into the game folder (the folder containing `EDF6.exe`) by hand.

## Host: enabling public direct connect (optional)

Only the host needs to set this up; joiners who have the plugin installed with `AutoJoin=1` (the default) connect directly on their own after joining the room.

1. Open `Mods\Plugins\EDF6DirectNet.ini` and set `Mode=host`.
2. Right-click `EDF6DirectNet_AllowFirewall.bat` in the game folder → **Run as administrator** (only needed once).
3. Make yourself reachable from the internet, pick one:
   - **Automatic**: leave `PublicAddress` empty. The plugin maps UDP 27015 via your router's UPnP and also advertises this PC's public IPv6 automatically.
   - **Manual**: forward the UDP port to this PC on your router, then set `PublicAddress=public IP:external port` (a DDNS hostname also works, e.g. `myroom.ddns.net:40000`).
   - **PC dials the internet directly (PPPoE, no router)**: the plugin cannot detect this kind of adapter and will not advertise an address automatically; set `PublicAddress=public IP:27015` by hand.
4. Restart the game and create a room as usual.

How to confirm it worked (check the log `Mods\Plugins\EDF6DirectNet.log`):

| Log | Meaning |
|---|---|
| `DIRECT players who join your room connect to ...` | The host address has been determined and written into the room info |
| `UPNP router now forwards UDP ...` | UPnP mapping succeeded |
| `UPNP no router with UPnP port mapping found` / `UPNP port mapping failed` | The router does not support UPnP or it is disabled; use manual port forwarding instead |
| `UPNP WARNING: the router WAN address ... is private (carrier-grade NAT)` | You are behind carrier-grade NAT (common with many ISPs) and have no public IPv4, so IPv4 direct connect is impossible; you can only rely on IPv6 or ask your ISP for a public IP |
| `UPNP UDP 27015 is already forwarded to ...; left alone` | This port on the router is already forwarded to another device on your LAN; the plugin will not delete it. Pick a different `ListenPort`, or forward manually |
| `DIRECT client ... connected from ...` (host) / `DIRECT connected to host ...` (joiner) | Direct connection established |
| `DIRECT auto-connect stopped (the room host did not answer on any advertised address ...)` | The joiner cannot reach the host (firewall / port forwarding / Key mismatch); the game keeps using Epic as usual and retries after 60 seconds |

Joiners try each address for 10 seconds in IPv4 → IPv6 order; if none works they stay on EOS, and normal play is not affected.

**About `Key=`**: an optional passphrase that stops someone who knows your address and a player's EOS ID from forging direct-connect data. It is **not** written into the room info — if the host sets a Key, every joiner must put the same Key in their own ini, otherwise auto direct connect fails and falls back to EOS. If you only play with friends you can leave it unset (the host log will show a `hosting without Key=` reminder).

**Privacy**: the host's public address is stored in the lobby member attributes, so anyone who can see the room can read it.

## Settings reference (`EDF6DirectNet.ini`, restart the game after editing)

| Key | Default | Description |
|---|---|---|
| `[DirectNet] Enabled` | `1` | `0` = the plugin installs no hooks at all, identical to vanilla |
| `Mode` | `off` | `off` regular player / `host` act as direct-connect host / `join` specify the host address manually (rarely needed, auto direct connect covers it) |
| `ListenPort` | `27015` | host: the UDP port to listen on (this is the one to forward and allow through the firewall); join: local port, empty = automatic |
| `PublicAddress` | empty | host: tells others where to connect. Empty = public IPv6 + UPnP-mapped IPv4 |
| `AutoJoin` | `1` | When you join someone else's room and the host has direct connect enabled, connect to it automatically |
| `HostAddress` | empty | `Mode=join` only: host address, e.g. `123.45.67.89:27015` / `[2408:8207::5]:27015` |
| `Key` | empty | Direct-connect passphrase, must be identical for everyone; letters and digits only |
| `UPnP` | `1` | Let the router set up port forwarding automatically when hosting |
| `BindPhysicalInterface` | `1` | Pin direct-connect traffic to the physical network adapter so it is not hijacked by the TUN adapter of Clash / VPN / game accelerator |
| `LinkTimeoutMs` | `60000` | How long without data from the other side before a direct link counts as disconnected (3000–300000) |
| `[EOS] FixedPort` | `0` | EOS uses fixed UDP ports `FixedPort`–`FixedPort+7`; 0 = random |
| `Relay` | `default` | EOS relay: `default` unchanged / `allow` / `norelay` / `force` |
| `[Sync] ReliableGameTraffic` | `1` | Desync prevention (reliable sending). `0` = vanilla |
| `[Resilience] HoldDisconnects` | `auto` | Disconnect grace: `auto` only for players with the plugin / `off` vanilla / `all` no detection, grace for everyone (only if you are sure everyone has the plugin) |
| `GraceSeconds` | `30` | Maximum number of seconds a disconnect is hidden (1–600) |

## How it works

### Reliable sending

Both `EOS_P2P_SendPacket` call sites in `EDF.dll+0x12c8bc0` pass `Reliability = UnreliableUnordered`. The plugin changes it to `ReliableUnordered` at the import-table level: every packet is delivered exactly once, possibly out of order — which is a delivery pattern unreliable transport is already allowed to produce, so game logic is unaffected; packets just stop getting lost. The receiver needs no cooperation.

### Disconnect grace and plugin detection

If the other player does not have the plugin, their game removes you from the match as usual; if your side kept hiding the disconnect, the two sides' state would diverge, so grace can only be given to players who have the plugin.

EDF6 parses every EOS packet it receives as game data (`ReceivePacket` is called with `RequestedChannel` = NULL), so probe packets cannot be sent over P2P. The plugin uses lobby **member attributes** instead: after joining/creating a room it writes `EDF6DN=1` on itself (`EDF.dll` imports no member-attribute functions, so the game cannot see it), and on disconnect it reads from the local lobby data whether the other player has this attribute:

- Marker present and the connection was established before → grace, calling `AcceptConnection` every 2 seconds to request a reconnect;
- No marker → handled like vanilla;
- Public direct-connect member → grace for as long as the direct link is alive (its game data does not go through EOS anyway);
- The other player actually left the lobby → the disconnect is handed to the game immediately.

### Public direct connect

The host additionally writes the member attribute `EDF6DN_ADDR` (a space-separated address list). Other members read it every 2 seconds and try each address for 10 seconds in IPv4, IPv6 order; if all fail they retry after 60 seconds. The transport is a custom UDP protocol: selective acknowledgement, token-bucket rate-limited retransmission, session epochs (packets from an old session never leak into the new session after a reconnect), and an optional `Key=` passphrase (truncated HMAC-SHA256 tag, prevents forgery, no encryption); `IP_UNICAST_IF` binds to the physical adapter to prevent TUN hijacking.

## Troubleshooting

- **Check the log first**: `Mods\Plugins\EDF6DirectNet.log` (rotated to `.log.1` once it exceeds 2MB). A first line `==== EDF6DirectNet x.y.z starting` means the plugin loaded; if that line is missing, EDFModLoader is not installed correctly.
- `EOS hooks FAILED`: a game update changed the import table; the plugin disables direct connect automatically. Please open an Issue.
- `LOBBY plugin detection UNAVAILABLE`: the plugin cannot tell whether the other player has it installed, so disconnect grace only applies to direct-connect members.
- `EOS incoming packet queue FULL`: the EOS queue is full and packets are being dropped. Please open an Issue with your log attached.
- `RESILIENCE ... RECOVERED` means a disconnect was hidden successfully; `did not come back within` means it was handed to the game after the timeout.
- `STATS last 60s: ...` is a one-line send/receive summary every minute; if `send-failures` is not 0, please attach your log.
- `DIRECT ignored hello for ... its link is live`: someone tried to connect from a different address using the identity of a player who is online, and was rejected. An occasional line may just mean that player switched networks (it is accepted automatically after 5 seconds); if it shows up often, someone is messing with you and setting `Key=` is recommended.

## Build

Requires Visual Studio 2022 (MSVC x64).

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
# Package a release by hand: needs the extracted official EDFModLoader.zip folder (with winmm.dll, ModLoader.ini, plus its LICENSE.txt)
powershell -ExecutionPolicy Bypass -File package.ps1 -Version 0.3.3 -ModLoaderDir <folder>
```

Outputs: `build\EDF6DirectNet.dll` (static CRT, depends only on system DLLs), `build\edf6_directnet_tests.exe` (unit tests + local loopback multi-node tests, including 20%–40% packet loss, network drop and restart scenarios; the `EDF.dll` import-table test needs the game installed on this PC, otherwise it is skipped), `build\probe_join.exe` (direct-connect probe for manual debugging).

## Release

Releases are produced automatically by GitHub Actions (`.github/workflows/release.yml`):

1. Bump the version: `kVersionMajor/Minor/Patch` and `kVersionText` in `src/plugin.cpp`, plus the first line of the three bundled readmes `dist/README_EDF6DirectNet*.txt`.
2. Write the release notes `release-notes/<version>.md` (this is the body of the Release page; the pipeline fails without it).
3. Commit to `main` and push the tag: `git tag v0.3.3 && git push origin v0.3.3`.

The pipeline builds, runs the tests, downloads the official EDFModLoader v1.0.10 (verified by SHA-256), packages `EDF6DirectNet-v<version>.zip` and creates the Release. `package.ps1` refuses to package if the tag, the source version and the readme version do not all match. Running it manually from the Actions page only builds and packages (the output is under Artifacts in the run), without publishing.

## Layout

| Path | Contents |
|---|---|
| `src/plugin.cpp` | EDFModLoader entry point, reads the config, starts direct connect |
| `src/config.*` | INI reading and the default settings file (comments written in Chinese / Japanese / English based on the Windows display language) |
| `src/eos_min.h` | The EOS SDK structs in use (from the official 1.15.5 headers; the game uses 1.16.1) |
| `src/eos_hooks.cpp`, `src/iat.*` | Patches the `EDF.dll` import table to take over EOS P2P / lobby calls |
| `src/hold.*` | Disconnect grace |
| `src/lobby_marker.*` | Lobby member attributes: detecting who has the plugin, distributing the host address |
| `src/direct_net.*`, `src/reliable.*`, `src/wire.*`, `src/auth.*` | Direct-connect transport |
| `src/netif.*`, `src/upnp.*` | Physical adapter detection, UPnP |
| `src/log.*` | Logging |
| `dist/` | Install scripts and bundled readmes (Chinese / English / Japanese) |
| `tests/` | Tests |

## Known limitations

- Joining in the middle of a mission is not supported (the game itself has no such feature).
- Only desync caused by packet loss is fixed; desync in the game logic itself needs concrete symptoms before it can be reverse-engineered.
- During the disconnect grace period other players may wait at a sync point, for at most `GraceSeconds` seconds.
- Direct connect is relayed by the host: at the moment a joiner's direct link reconnects, the small amount of data the host is relaying for them that has not been acknowledged yet is lost (the game then falls back to EOS).
- Without `Key=`, direct connect has no authentication: someone who knows the host address and a player's EOS ID can forge that player's data. Setting a Key prevents forgery, but still does not prevent replay of captured `Bye` / member-list packets (this needs a protocol version bump, left for the next major version).
- UPnP mappings are permanent and are not removed automatically when the game exits (harmless while nothing is listening on the port); if needed, delete the mapping named `EDF6DirectNet` in your router's admin page.
- Disconnect grace has not yet been verified in real multiplayer sessions; Issues with logs attached are welcome.

## License

MIT, see [LICENSE](LICENSE). The bundled EDFModLoader is MIT; its license is included in the package as `EDFModLoader\LICENSE.txt`.
