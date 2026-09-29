EDF6DirectNet 0.3.0 —— EDF6 联机稳定插件
项目主页：https://github.com/hajisensai/edf6-coop-stable
（实验性：还没有经过两台电脑的真实联机验证，遇到问题请带日志提 Issue）

【安装 / 卸载】
先把 zip 完整解压，再双击 INSTALL.bat，然后从 Steam 正常启动游戏。
卸载：双击 UNINSTALL.bat。升级：直接运行新版的 INSTALL.bat，设置保留。
没装过 EDFModLoader 会顺便装上官方版；已经装了的不会动。

【装上就生效，不用设置】
・防不同步：联机数据丢包后自动重发。只要你装了，你发出的数据就不会丢；大家都装效果最好。
・掉线不重来：网络抖一下断开时，插件先不让游戏踢人，后台自动重连，最多等 30 秒。
  只对同样装了插件的人生效（自动识别），和没装的人一起玩时照原版处理。
・自动直连：进别人的房间时，如果房主开了公网直连，自动直接连房主，不用填任何东西。
・掉线日志：Mods\Plugins\EDF6DirectNet.log。掉线了把它发出来就能查原因。

【当房主开公网直连（可选，只有房主要设）】
不走 Epic 中继，大家直接连你。设置文件是 Mods\Plugins\EDF6DirectNet.ini（第一次启动游戏后生成）。
1. ini 里改成 Mode=host
2. 右键游戏目录里的 EDF6DirectNet_AllowFirewall.bat → 以管理员身份运行（只需一次）
3. 让外面能连到你，二选一：
   a) PublicAddress 留空：插件自动用路由器 UPnP 映射 UDP 27015，并自动带上本机公网 IPv6。
   b) 自己在路由器上把 UDP 端口映射到本机，然后在 ini 里填 PublicAddress=你的公网IP:外部端口
      例：PublicAddress=120.1.2.3:27015
   电脑直接拨号上网（PPPoE，没有路由器）的，插件认不出网卡，请用 b) 填 PublicAddress。
4. 重启游戏，正常建房。别人进房后你的日志出现 DIRECT client ... connected 就是连上了。
日志里的提示：
   UPNP no router ... / UPNP port mapping failed   → 路由器没开 UPnP，改用 b)
   UPNP WARNING ... carrier-grade NAT               → 你没有公网 IPv4（运营商大内网），
                                                      只能靠 IPv6，或找运营商要公网 IP
连不上也没关系：加入的人每个地址试 10 秒，不通就照常走 Epic，不影响游戏。
房主的地址会写在房间信息里，进房的人能读到。

【Key（暗号，可选）】
ini 里的 Key= 用来防止别人伪造直连数据。它不会自动发给加入的人：
房主设了 Key，每个加入者都要在自己的 ini 里填一模一样的 Key，否则自动直连会失败（回落到 Epic）。
只和熟人玩可以不设。

【其他】
・恢复原版：ini 里 Enabled=0，或删掉 Mods\Plugins\EDF6DirectNet.dll。
・日志里没有「==== EDF6DirectNet ... starting」这一行 = 插件没加载，重新运行 INSTALL.bat。
・卸载后若不再需要防火墙规则：以管理员身份打开命令提示符运行
    netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"
・不能在任务进行中途加入（游戏本身没有这个功能）。
・所有设置项的说明见 ini 文件里的注释，或项目主页。
