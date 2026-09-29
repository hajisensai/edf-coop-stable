EDF6DirectNet 0.3.0 —— EDF6 联机稳定插件
项目主页：https://github.com/hajisensai/edf6-coop-stable

【安装】
解压 zip，双击 INSTALL.bat，然后从 Steam 正常启动游戏。
卸载：双击 UNINSTALL.bat。
没装过 EDFModLoader 会顺便装上官方版；已经装了的不会动。

【装上就生效，不用设置】
・防不同步：联机数据丢包后自动重发。只要你装了，你发出的数据就不会丢。
・掉线不重来：网络抖一下断开时，插件先不让游戏踢人，后台自动重连，最多等 30 秒。
  只对同样装了插件的人生效（自动识别），和没装的人一起玩时照原版处理。
・自动直连：进别人的房间时，如果房主开了公网直连，自动直接连房主，不用填任何东西。
・掉线日志：Mods\Plugins\EDF6DirectNet.log。掉线了把它发出来就能查原因。

【当房主开公网直连（可选，只有房主要设）】
不走 Epic 中继，大家直接连你。设置文件是 Mods\Plugins\EDF6DirectNet.ini（第一次启动游戏后生成）。
1. ini 里改成 Mode=host
2. 右键 EDF6DirectNet_AllowFirewall.bat → 以管理员身份运行（只需一次）
3. 让外面能连到你，二选一：
   a) 自己在路由器上把 UDP 端口映射到本机，然后在 ini 里填 PublicAddress=你的公网IP:外部端口
      例：PublicAddress=120.1.2.3:27015
   b) PublicAddress 留空：插件自动用路由器 UPnP 映射 UDP 27015，并自动带上本机公网 IPv6。
      日志写 UPNP port mapping unavailable 就只能用 a)。
4. 重启游戏，正常建房。别人进房后你的日志出现 DIRECT client ... connected 就是连上了。
房主的地址会写在房间信息里，进房的人能读到。

【其他】
・恢复原版：ini 里 Enabled=0，或删掉 Mods\Plugins\EDF6DirectNet.dll。
・不能在任务进行中途加入（游戏本身没有这个功能）。
