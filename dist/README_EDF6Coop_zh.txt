EDF6Coop 2.0.0 - 地球防卫军6 联机扩容 + 稳定插件
项目主页：https://github.com/hajisensai/edf-coop-stable
English: README_EDF6Coop.txt / 日本語: README_EDF6Coop_ja.txt
遇到问题请提 Issue，并附上 Mods\Plugins\EDF6Coop.log。

EDF6Coop 是一个 EDFModLoader 插件（Mods\Plugins\EDF6Coop.dll），把原来的 EDF6DirectNet 和
EDF6MultiSlot 合成了一个 DLL、一份日志、一个设置文件：
- 超过 4 人的房间（8Player MOD）和对应的任务；
- 玩家之间的直连，Epic 服务抽风时房间不散；
- 丢包自动重发，短时间断线不被踢。
不开 8Player MOD、不配直连时，玩法和原版完全一样，也能和没装 MOD 的人一起玩。

[从 EDF6DirectNet / EDF6MultiSlot 升级]
什么都不用手动做。INSTALL.bat（或第一次启动游戏时）会把 Mods\Plugins 里的 EDF6DirectNet.dll 和
EDF6MultiSlot.dll 改名为 .disabled，加载器就不再加载它们。旧的设置文件原样保留；EDF6Coop 第一次启动
时会写出 Mods\Plugins\EDF6Coop.ini，把你原来的设置值搬过来。已经不存在的设置项会在日志里点名
（"is no longer a setting"）。
想退回旧版：删掉 EDF6Coop.dll，再把旧 DLL 名字末尾的 ".disabled" 去掉即可。

[房间人数：8p、10p、12p、16p、24p、32p]
每种人数一个安装包（EDF6Coop-2.0.0-8p.zip 等）。某个人数的房间只有同人数版本才能加入：同一个房间里
所有人必须用同一个包。拿不准就用 8p。
8p 实际玩得最多；10p、12p 玩得少一些；16p、24p、32p 是 2.0.0 新增的，目前只用离线幽灵队员验证过，
还没有真的那么多人联机测试过。
房间越大，房主需要的上传带宽越大：开了直连时所有玩家的数据都经过房主转发。

[安装 / 卸载]
解压整个 zip，双击 INSTALL.bat，然后照常从 Steam 启动游戏。（也可以把 zip 里的内容直接拖进游戏文件夹，
和 EDF6.exe 放在一起：压缩包里的目录结构就是游戏文件夹的结构。）
没有 EDFModLoader（winmm.dll）时会装上自带的：官方 v1.0.10 加多线程问题的修复（见 LOADER_FIX_JA.md）。
已有的 winmm.dll 一律不覆盖，唯一例外是与官方 v1.0.10 字节完全相同的那个：会换成修复版，原文件保存为
winmm.dll.bak-official。
卸载：先在 EDF6Coop.ini 的 [MultiSlot] 段设 Enabled=0 并启动一次游戏（这样会删掉 MOD 写出的菜单布局
文件 Mods\UI\LYT_MAINFRAME.SGO），再双击 UNINSTALL.bat（用管理员身份运行可同时删除防火墙规则）。
它会删除 EDF6Coop 的文件、设置、日志、自动更新的残留和它建立的 UPnP 映射；EDFModLoader、其他 MOD、
以及你旧的 EDF6DirectNet / EDF6MultiSlot 设置文件都不会动。

[大房间（8Player MOD）]
- 在房间外的菜单按 F2 或按下左摇杆切换「8Player MOD」（显示在菜单左下角）。
  OFF（默认）：建的是谁都能进的普通 4 人房；搜房两种房间都显示。
  ON：建的是最多可容纳本版本人数、只有装了同版本 EDF6Coop 的人才能看到的房间；搜房只显示这种房间。
- 在房间里按 F3 / Tab / 右摇杆按下 切换队员页（每页 4 人）。
- 在房间里按 F4 / 左摇杆按下 开关「copy armor」：仅在任务中把你的护甲提到房间里其他人中最低的那位
  （优先同兵种）。给新手用，不写存档。
- 超过 4 人的任务：多出来的玩家有装备、出生点和道具。敌人数量随人数增加：5 人 ×1.2、6 人 ×1.4、
  7 人 ×1.6、8 人及以上 ×1.8（固定物体和大型 BOSS 不增加）。[Mission] ExtraEnemies=0 保持原版数量
  （房间里所有人要设成一样）。
- 按键在 ini 的 [RoomScreen] / [CopyArmor] 里改。F2 专用于 8Player MOD。

[直连（可选，只有房主需要设置）]
进你房间的玩家会直接连到你。ini 里的注释（按 Windows 显示语言：中文 / 日文 / 英文）解释了每个设置。
1. 在 EDF6Coop.ini 的 [DirectNet] 段设 Mode=host。
2. 右键游戏文件夹里的 EDF6Coop_AllowFirewall.bat → 以管理员身份运行（一次；改了 ListenPort 后再运行一次）。
3. 让别人连得到你：PublicAddress 留空（插件会用 UPnP 映射 UDP 端口，同时公布你的公网 IPv6），
   或者自己在路由器上转发一个 UDP 端口，然后设 PublicAddress=你的公网IP:端口。
4. 照常建房。日志里出现 "DIRECT client ... connected" 就是有人直连上了。
加入的人什么都不用设：房间信息里带着房主地址。连不上也不影响，照常走 Epic 继续玩。
Key= 是可选的共享密钥：房主设了，所有加入者必须填一样的 Key。
直连正常时，Epic 自己房间服务的抽风不会让任何人掉线。

[自动更新]
游戏启动时 EDF6Coop 会从 GitHub 获取同人数的新版本，下次启动生效。
发布文件有签名（ECDSA P-256），签名不对的文件绝不安装。新版本如果在进入标题画面后 20 秒内崩溃，会自动回滚。
关闭下载：在 EDF6Coop.ini 设 [Update] AutoUpdate=0。

[日志]
Mods\Plugins\EDF6Coop.log：所有内容一份日志（直连相关的行以 [DN] 开头），会自动保持在 2MB 左右以内。
正常退出时最后一行是 "SHUTDOWN the game exited"；没有这一行说明游戏崩溃或被强制结束。报问题时请附上。

[关闭功能]
[MultiSlot] Enabled=0：不扩房间，房间画面恢复原版。
[DirectNet] Enabled=0：不直连，不碰游戏的 Epic 调用。
两个都是 0：插件自己卸载。

[附带内容]
- EDF6Coop：Mods\Plugins\EDF6Coop.dll（MIT，LICENSE.txt；房间部分为公有领域）。它写出的菜单布局是游戏
  自己的 UI/LYT_MAINFRAME.SGO 加一个显示栏，数据在 DLL 里。
- EDFModLoader（BlueAmulet 作，MIT，EDFModLoader_LICENSE.txt）：winmm.dll，含 LOADER_FIX_JA.md 里的修复。
