EDF6DirectNet 0.3.5 - online stability plugin for EARTH DEFENSE FORCE 6
Project page: https://github.com/hajisensai/edf-coop-stable
中文: README_EDF6DirectNet_zh.txt / 日本語: README_EDF6DirectNet_ja.txt
(Experimental, verified in real four-player online play. If something goes wrong, open an Issue
and attach the log.)

[Install / uninstall]
Extract the whole zip first, double-click INSTALL.bat, then start the game from Steam as usual.
Uninstall: double-click UNINSTALL.bat. Upgrade: run INSTALL.bat of the new version; settings are kept.
If EDFModLoader is missing, the official build is installed too; an existing one is left alone.

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
- To remove the firewall rule after uninstalling, run in an administrator command prompt:
    netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"
- Joining a mission already in progress is not possible (the game itself has no such feature).
- Every setting is explained in the comments of the ini file and on the project page.
