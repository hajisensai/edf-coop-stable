EDF6Coop 2.3.2 - 地球防卫军6 联机扩容 + 稳定插件
项目主页：https://github.com/hajisensai/edf-coop-stable
English: README_EDF6Coop.txt / 日本語: README_EDF6Coop_ja.txt
遇到问题请提 Issue，并附上 Mods\Plugins\EDF6Coop.log。

EDF6Coop 是一个 EDFModLoader 插件（Mods\Plugins\EDF6Coop.dll），把原来的 EDF6DirectNet 和
EDF6MultiSlot 合成了一个 DLL、一份日志、一个设置文件：
- 超过 4 人的房间（Player MOD，8 到 32 人由房主选）和对应的任务；
- 玩家之间的直连，Epic 服务抽风时房间不散；
- 丢包自动重发，短时间断线不被踢。
Player MOD 为 OFF、不配直连时，玩法和原版完全一样，也能和没装 MOD 的人一起玩。

[从 EDF6DirectNet / EDF6MultiSlot 升级]
什么都不用手动做。INSTALL.bat（或第一次启动游戏时）会把 Mods\Plugins 里的 EDF6DirectNet.dll 和
EDF6MultiSlot.dll 改名为 .disabled，加载器就不再加载它们。旧的设置文件原样保留；EDF6Coop 第一次启动
时会写出 Mods\Plugins\EDF6Coop.ini，把你原来的设置值搬过来。已经不存在的设置项会在日志里点名
（"is no longer a setting"）。
想退回旧版：删掉 EDF6Coop.dll，再把旧 DLL 名字末尾的 ".disabled" 去掉即可。

[房间人数：房主在游戏里选]
从 2.3.0 起只有一个安装包（EDF6Coop-<版本>.zip），所有人都装它。房间人数由建房的人在菜单里选
（8、10、12、16、24、32，见下面的「大房间」），加入的人不用管：装了 2.3.0 的人能进任何人数的房间。
2.3.0 和 2.2.x（任何人数的旧包）互相看不到、进不了对方的大房间，请大家都更新到 2.3.0
（开着自动更新的话，启动一次游戏就会更新）。普通 4 人房照旧谁都能进。
8 人实际玩得最多；10、12 人玩得少一些；16、24、32 人目前只用离线幽灵队员验证过，
还没有真的那么多人联机测试过。
24、32 人房间没有游戏内语音：Epic 的语音聊天只支持 16 人以内的房间。
房间越大，房主需要的上传带宽越大：开了直连时所有玩家的数据都经过房主转发。

[安装 / 卸载]
解压整个 zip，双击 INSTALL.bat，然后照常从 Steam 启动游戏。（也可以把 zip 里的内容直接拖进游戏文件夹，
和 EDF6.exe 放在一起：压缩包里的目录结构就是游戏文件夹的结构。）
没有 EDFModLoader（winmm.dll）时会装上自带的：官方 v1.0.10 加多线程问题的修复（见 LOADER_FIX_JA.md）。
已有的 winmm.dll 一律不覆盖，唯一例外是与官方 v1.0.10 字节完全相同的那个：会换成修复版，原文件保存为
winmm.dll.bak-official。
卸载：先在 EDF6Coop.ini 的 [MultiSlot] 段设 Enabled=0 并启动一次游戏（这样会删掉 MOD 写出的文件
Mods\UI\LYT_MAINFRAME.SGO 和 Mods\HUD\ONLINEHUDTEXTURE.RAB），再双击 UNINSTALL.bat（用管理员身份运行可同时删除防火墙规则）。
它会删除 EDF6Coop 的文件、设置、日志、自动更新的残留和它建立的 UPnP 映射；EDFModLoader、其他 MOD、
以及你旧的 EDF6DirectNet / EDF6MultiSlot 设置文件都不会动。

[大房间（Player MOD）]
- 在房间外的菜单按 F2 或按下左摇杆切换房间人数，依次是 OFF → 8 → 10 → 12 → 16 → 24 → 32 → OFF，
  显示在菜单左下角（例如「F2/LS 12Player MOD :ON」）。选择保存在 EDF6Coop.ini 的 [MultiSlot] RoomSize。
  OFF（默认）：建的是谁都能进的普通 4 人房；搜房只显示普通房间（要找大房间先切到 ON，选几人都行）。
  ON：建的是你选的人数的房间，只有装了 EDF6Coop 2.3.0 及以上的人才能看到；搜房显示所有人数的这种房间。
  房间建好后人数不变；在房间里时菜单显示的是这个房间的人数。
- 在房间里按 F3 / Tab / 右摇杆按下 切换队员页（每页 4 人）。
- 在房间里按 F4 / 左摇杆按下 开关「copy armor」：仅在任务中把你的护甲提到房间里其他人中最低的那位
  （优先同兵种）。给新手用，不写存档。
- 超过 4 人的任务：多出来的玩家有装备、出生点和道具。敌人数量超过 4 人后每多一人 +0.2 倍：
  5 人 ×1.2、8 人 ×1.8、12 人 ×2.6、16 人 ×3.4、24 人 ×5.0、32 人 ×6.6
  （固定物体和大型 BOSS 不增加）。[Mission] ExtraEnemies=0 保持原版数量
  （房间里所有人要设成一样）。
- 任务里每个玩家在 HUD 上都有自己的颜色：状态灯、聊天气泡、雷达标记。1-4 号保持游戏原来的黄、绿、蓝、红，
  5-8 号是橙、粉、紫、青，以此类推直到 32 人。为此 MOD 会写出 Mods\HUD\ONLINEHUDTEXTURE.RAB（游戏自己的
  HUD 贴图加上新的灯和气泡）。如果那里已经有别的 MOD 的文件，则不动它，5 号以后的玩家沿用 1-4 号的颜色。
- 按键在 ini 的 [RoomScreen] / [CopyArmor] 里改。F2 专用于 Player MOD。

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
谁在房间里由房主的游戏说了算：房主的成员列表经直连发给每个人，大家的游戏都跟着它走。
掉出房间后 30 分钟内，如果 Epic 搜房失败（房间服务挂了），搜房列表里会出现你刚才那个房间，选它就直接连
房主重进，不经过 Epic。Epic 正常时一切照旧。被房主踢出、房间关闭或进了别的房间后就不再显示。房主和你都要是 2.2.0 及以上。

[自动更新]
游戏启动时 EDF6Coop 会从 GitHub 获取新版本，下次启动生效（2.2.x 的各人数版本也会更新到同一个 2.3.0）。
结果显示在菜单左下角（房间外）：「EDF6Coop 2.3.2 (latest)」是已最新；
「EDF6Coop 2.3.1 -> 2.3.2 downloaded, restart the game」是已下载、重启生效；重启后显示
「EDF6Coop 2.3.2 (updated from 2.3.1)」。失败时显示「update failed, see EDF6Coop.log」。
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
  自己的 UI/LYT_MAINFRAME.SGO 加一个显示栏；HUD 贴图是游戏的 HUD/ONLINEHUDTEXTURE.RAB 加上更多的灯和
  聊天气泡，数据都在 DLL 里。5-8 号的颜色沿用 FevGrave 做的 8 人 HUD 贴图，要改的 HUD 代码也是 FevGrave 找到的。
- EDFModLoader（BlueAmulet 作，MIT，EDFModLoader_LICENSE.txt）：winmm.dll，含 LOADER_FIX_JA.md 里的修复。
