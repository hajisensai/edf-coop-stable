EDF6DirectNet 0.3.6 - online stability plugin for EARTH DEFENSE FORCE 6
Project page: https://github.com/hajisensai/edf-coop-stable
中文: README_EDF6DirectNet_zh.txt / 日本語: README_EDF6DirectNet_ja.txt
(Experimental, verified in real four-player online play. If something goes wrong, open an Issue
and attach the log.)

[Install / uninstall]
Extract the whole zip first, double-click INSTALL.bat, then start the game from Steam as usual.
Uninstall: double-click UNINSTALL.bat (run it as administrator to remove the firewall rule too). It also
removes the update leftovers (EDF6DirectNet.dll.old etc.) and the UPnP mapping the plugin made (only one that
points to this PC and is named EDF6DirectNet). EDFModLoader and other mods are left alone.
If EDFModLoader is missing, the bundled one is installed: official v1.0.10 with a fix for a multithread bug.
An existing winmm.dll is never overwritten, except one that is byte-for-byte the official v1.0.10 file: it is
replaced by the fixed build and the original is kept as winmm.dll.bak-official.

[Automatic updates]
At game start the plugin fetches a newer release from GitHub; it runs from the next start.
- Releases are signed (ECDSA P-256) with a key held only by the release pipeline; the public key is inside the
  plugin. A file without a valid signature (or with the wrong version / SHA-256) is never installed.
- Rollback: the replaced DLL stays as EDF6DirectNet.dll.old until the new version has run for 20 seconds after the game reaches its title screen. If the
  game ends before that, the next start restores the old version by itself, remembers the failed one
  (EDF6DirectNet.dll.bad, not installed again) and runs that session without the plugin.
- An ini without an AutoUpdate line (older versions did not write it) counts as OFF. To turn it on, add at the
  end of Mods\Plugins\EDF6DirectNet.ini:
      [Update]
      AutoUpdate=1
  A newly created ini has it on. AutoUpdate=0 turns downloading off (rollback still works).
- The UPnP mapping is removed at the next game start that does not host, and by UNINSTALL.bat.

[Works right away, nothing to set up]
- Desync fix: lost online packets are resent automatically. What you send is never lost once you
  have it installed; best when everyone has it.
- Survive drops: when the connection hiccups, the plugin keeps the game from kicking the player and
  reconnects in the background, for up to 30 seconds. Only for players who also run the plugin
  (detected automatically); players without it are handled like the original game.
  While the direct link works, Epic's own room-service hiccups cannot drop anyone either.
- Auto-connect: when you join a room whose host enabled the direct link, you connect straight to the
  host. Nothing to enter.
- Log: Mods\Plugins\EDF6DirectNet.log. Share it after a disconnect to find the cause.

[Hosting with a direct link (optional, host only)]
Everyone connects straight to you instead of through Epic's relay. Settings are in
Mods\Plugins\EDF6DirectNet.ini (created on the first game start; its comments follow the Windows
display language: Chinese / Japanese / otherwise English).
1. Set Mode=host in the ini.
2. Right-click EDF6DirectNet_AllowFirewall.bat in the game folder -> Run as administrator (once).
   The rule allows only EDF6.exe on the ListenPort UDP port from the ini (27015 if unset); run it again after
   changing ListenPort.
3. Make yourself reachable, either:
   a) Leave PublicAddress empty: the plugin maps UDP 27015 via UPnP on your router and also
      advertises your public IPv6 address.
   b) Forward a UDP port to this PC on your router yourself, then set
      PublicAddress=your public IP:external port   e.g. PublicAddress=120.1.2.3:27015
   If the PC dials the internet itself (PPPoE, no router), the plugin cannot recognise the adapter:
   use b).
4. Restart the game and create a room as usual. "DIRECT client ... connected" in your log means a
   player connected directly.
Log hints:
   UPNP no router ... / UPNP port mapping failed   -> UPnP is off on the router, use b)
   UPNP WARNING ... carrier-grade NAT               -> you have no public IPv4 (ISP-level NAT);
                                                      only IPv6 works, or ask your ISP for a public IP
If it cannot connect, nothing breaks: joiners try each address for 10 seconds, then keep playing
through Epic as usual.
The host address is stored in the room info, readable by everyone in the room.

[Key (optional shared secret)]
Key= in the ini stops others from forging direct-link data. It is NOT sent to joiners: if the host
sets a Key, every joiner must enter exactly the same Key in their own ini, or auto-connect fails
(falls back to Epic). Not needed when playing with friends only.

[Other]
- Back to the original game: Enabled=0 in the ini, or delete Mods\Plugins\EDF6DirectNet.dll.
- No "==== EDF6DirectNet ... starting" line in the log = the plugin did not load; run INSTALL.bat again.
- UNINSTALL.bat removes the firewall rule when run as administrator; otherwise run in an administrator command prompt:
    netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"
- Joining a mission already in progress is not possible (the game itself has no such feature).
- Every setting is explained in the comments of the ini file and on the project page.
