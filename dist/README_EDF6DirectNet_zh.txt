EDF6DirectNet 0.3.6 —— EDF6 联机稳定插件
项目主页：https://github.com/hajisensai/edf-coop-stable
English: README_EDF6DirectNet.txt / 日本語: README_EDF6DirectNet_ja.txt
（实验性：已在真实的四人联机中验证。遇到问题请带日志提 Issue）

【安装 / 卸载】
先把 zip 完整解压，再双击 INSTALL.bat，然后从 Steam 正常启动游戏。
卸载：双击 UNINSTALL.bat（以管理员身份运行还会一并删除防火墙规则）。它也会删除更新留下的文件
（EDF6DirectNet.dll.old 等）和插件建立的 UPnP 映射（只删指向本机、名为 EDF6DirectNet 的那一条）。
EDFModLoader 和其他 Mod 不动。
没装过 EDFModLoader 会顺便装上附带的版本：官方 v1.0.10，并修复了一个多线程问题。已有的 winmm.dll 不会覆盖，
只有与官方 v1.0.10 逐字节相同的那个例外：它会换成修复版，原文件备份为 winmm.dll.bak-official。

【自动更新】
启动游戏时插件会从 GitHub 下载新版，下次启动生效。
・发布带签名（ECDSA P-256），私钥只在发布流水线里，公钥编译在插件里。没有有效签名（或版本号 / SHA-256 不符）
  的文件一律不安装。
・自动回滚：被替换的 DLL 会以 EDF6DirectNet.dll.old 留着，直到新版本运行满 2 分钟。在这之前游戏就结束的话，
  下次启动会自动换回旧版，记下失败的版本（EDF6DirectNet.dll.bad，不再安装它），这一次不加载插件。
・没有 AutoUpdate 这一行的 ini（旧版本不写）视为关闭。要开启，在 Mods\Plugins\EDF6DirectNet.ini 末尾加上：
      [Update]
      AutoUpdate=1
  新生成的 ini 默认开启。AutoUpdate=0 关闭下载（回滚仍有效）。
・UPnP 映射会在之后第一次不做房主的启动时删除，UNINSTALL.bat 也会删除。

【装上就生效，不用设置】
・防不同步：联机数据丢包后自动重发。只要你装了，你发出的数据就不会丢；大家都装效果最好。
・掉线不重来：网络抖一下断开时，插件先不让游戏踢人，后台自动重连，最多等 30 秒。
  只对同样装了插件的人生效（自动识别），和没装的人一起玩时照原版处理。
  只要直连还通，Epic 房间服务自己的抽风也踢不掉任何人。
・自动直连：进别人的房间时，如果房主开了公网直连，自动直接连房主，不用填任何东西。
・掉线日志：Mods\Plugins\EDF6DirectNet.log。掉线了把它发出来就能查原因。

【当房主开公网直连（可选，只有房主要设）】
不走 Epic 中继，大家直接连你。设置文件是 Mods\Plugins\EDF6DirectNet.ini（第一次启动游戏后生成，
注释按 Windows 显示语言写成中文 / 日文 / 英文）。
1. ini 里改成 Mode=host
2. 右键游戏目录里的 EDF6DirectNet_AllowFirewall.bat → 以管理员身份运行（只需一次）
   这条规则只放行 EDF6.exe 在 ini 里 ListenPort 指定的 UDP 端口（没设置为 27015）；改了 ListenPort 请再运行一次。
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
・UNINSTALL.bat 以管理员身份运行时会删除防火墙规则；否则请以管理员身份打开命令提示符运行
    netsh advfirewall firewall delete rule name="EDF6 DirectNet (UDP in)"
・不能在任务进行中途加入（游戏本身没有这个功能）。
・所有设置项的说明见 ini 文件里的注释，或项目主页。
