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
・掉线日志：Mods\Plugins\EDF6DirectNet.log。掉线了把它发出来就能查原因。

【公网直连（可选）】
不走 Epic 中继，直接连房主。设置文件是 Mods\Plugins\EDF6DirectNet.ini（第一次启动游戏后生成）。

房主：
1. ini 里改成 Mode=host
2. 右键 EDF6DirectNet_AllowFirewall.bat → 以管理员身份运行（只需一次）
3. 启动游戏，在日志里找下面其中一行，把地址发给朋友：
     NET public IPv6 for friends: HostAddress=[2408:....]:27015
     UPNP router now forwards UDP 27015 ... router WAN address: x.x.x.x   （发 x.x.x.x:27015）
   日志写 UPNP port mapping unavailable：去路由器把 UDP 27015 转发到本机。
   日志写 carrier-grade NAT：运营商没给公网 IPv4，只能用 IPv6。
4. 正常建房。

加入的人：
1. ini 里改成 Mode=join，HostAddress=房主给的地址
2. 正常进房间。日志出现 DIRECT connected to host 就是连上了。

【其他】
・恢复原版：ini 里 Enabled=0，或删掉 Mods\Plugins\EDF6DirectNet.dll。
・不能在任务进行中途加入（游戏本身没有这个功能）。
