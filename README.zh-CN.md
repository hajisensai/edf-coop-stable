# edf-coop-stable

[English](README.md) | **中文** | [日本語](README.ja.md)

**EDF6Coop** 让 EARTH DEFENSE FORCE 6（PC / Steam）的联机人数更多、更稳：最多 8、10、12、16、24 或 32 人的房间（人数由房主在游戏里选，Player MOD），Epic 抽风也不散的玩家直连，以及丢包重发。它是一个 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 插件 `EDF6Coop.dll`，取代原来的两个插件 EDF6DirectNet 和 EDF6MultiSlot。

地球防卫军系列的联机稳定 Mod。目前支持：**EDF6**。EDF6 稳定后计划支持 EDF5 等其他作品。

> 非官方 Mod，与 D3 PUBLISHER / SANDLOT / Epic Games 无关。只在本机进程内接管网络调用，不改动任何游戏文件；`Enabled=0` 或删掉 DLL 即恢复原版。
>
> **状态：实验性。** 900+ 项自动化测试通过；自动直连和断线宽限已在真实的四人联机中验证（0.3.2：所有人都走直连，7 次连接中断都没让游戏察觉）。出问题请附日志提 [Issue](https://github.com/hajisensai/edf-coop-stable/issues)。

## 房间人数与旧插件

所有人用同一个安装包：`EDF6Coop-<版本>.zip`。每个安装都带 32 人的内存，房间人数由**房主**在游戏里选：在房间外的菜单画面按 **F2**（或按下左摇杆），依次切换 关 → 8 → 10 → 12 → 16 → 24 → 32 → 关，菜单左下角显示如 `F2/LS 12Player MOD :ON` 或 `F2/LS Player MOD :OFF`。选择保存在 `EDF6Coop.ini` 的 `[MultiSlot]` 下的 `RoomSize=`（`0` = 普通 4 人房间，`5`～`32` = 该人数的 MultiSlot 房间；旧的 `EightPlayerRooms=1` 读作 `RoomSize=8`，下次保存时被替换）。关：创建谁都能加入的普通 4 人房间。开：创建所选人数的 MultiSlot 房间，只有 EDF6Coop 2.3.0 及以上能看到。不管开关如何，房间列表都同时显示普通房间和所有人数的 MultiSlot 房间；客人可以加入任何人数的房间，房间保持创建时的人数。**16、24、32 人目前只用离线幽灵队员验证过**，还没有真的这么多人联机测试过。24、32 人房间没有游戏内语音：Epic 的语音聊天只支持 16 人以内的房间。房间越大，房主需要的上传带宽越大：开了直连时所有玩家的数据都经过房主。

房间部分（8Player MOD：超过 4 人、房间画面、任务、护甲复制）原作者是 **momotori01**，原名 EDF6MultiSlot（公有领域，[multislot/LICENSE](multislot/LICENSE)），历史和测试在 [multislot/](multislot/README.zh-CN.md)。从 2.0.0 起它和 EDF6DirectNet 合成一个 DLL、一份日志（`EDF6Coop.log`）、一个设置文件（`EDF6Coop.ini`）。安装时旧的 `EDF6DirectNet.dll` / `EDF6MultiSlot.dll` 会改名为 `.disabled`；旧设置文件保留，第一次启动时把值搬进 `EDF6Coop.ini`。

## 它解决什么

| 症状 | 原因 | 插件的做法 | 需要谁装 |
|---|---|---|---|
| 联机中途敌人/位置/血量对不上（不同步） | EDF6 把全部联机数据按 `UnreliableUnordered` 发送，丢一个包就永久丢一份状态 | 改为 `ReliableUnordered` 发送（丢了自动重发，仍允许乱序，游戏收到的交付语义不变）；EOS 收发队列扩到至少 64MB，避免队列满时丢包 | **发送方**。你装了，你发出的数据就不丢；所有人都装效果最好 |
| 网络抖一下就被踢出房间、任务白打 | EOS 连接因超时/网络错误关闭时，游戏立刻移除该玩家 | 暂不告诉游戏，后台让 EOS 重连；30 秒内恢复则游戏无感知，否则按原版处理 | **双方都装**（插件自动识别对方，见下文） |
| Epic 中继延迟高、NAT 打洞失败 | 所有流量走 EOS P2P / 中继 | 房主可开「公网直连」：其他人进房后自动直接连房主（星形拓扑，房主转发） | 房主设置，加入者装插件即可 |
| 掉线了不知道为什么 | — | 写诊断日志：NAT 类型、直连/中继、断开原因、大厅成员、每分钟收发统计 | 自己 |

默认全部开启（公网直连需房主手动开）。

## 安装

1. 到 [Releases](https://github.com/hajisensai/edf-coop-stable/releases/latest) 下载 `EDF6Coop-<版本>.zip`，**完整解压**到任意文件夹。
2. 双击 **`INSTALL.bat`**：从 Steam 库自动找到 EDF6 并安装。
   - 没有 EDFModLoader 时会装上附带的加载器：[BlueAmulet/EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) v1.0.10（MIT），且**修复了官方版的一个多线程问题**（它的函数转发口共用同一个跳转目标变量，多个线程同时调用时可能跳到错误的函数；详见 [multislot/packaging/LOADER_FIX_JA.md](multislot/packaging/LOADER_FIX_JA.md)）。已有的 `winmm.dll` 不会覆盖，只有一个例外：如果它与官方 v1.0.10 的文件逐字节相同，就换成修复版，原文件备份为 `winmm.dll.bak-official`。其他加载器（更新的或打过补丁的）一律不动。
   - 找不到游戏时会让你粘贴游戏目录（Steam 库里右键 EDF6 → 管理 → 浏览本地文件）。
3. 从 Steam 正常启动游戏。第一次启动后生成 `Mods\Plugins\EDF6Coop.ini`（设置）和 `EDF6Coop.log`（日志）。

zip 内附 README_EDF6Coop.txt（English）、README_EDF6Coop_zh.txt（中文）、README_EDF6Coop_ja.txt（日本語）；默认设置文件的注释按 Windows 显示语言写成中文 / 日文 / 其他语言一律英文。

升级：插件会自动更新。每次启动游戏，它在后台向 GitHub 查询最新版本；有新版就下载 `EDF6Coop.dll` 和带签名的清单 `EDF6Coop.dll.sig`，把 DLL 放到原位（`EDF6Coop.dll`），下次启动游戏生效，日志里会出现 `UPDATE installed ...`。连不上 GitHub 不影响游戏（请求走系统代理）。手动运行新版的 `INSTALL.bat` 也照样可以，设置文件保留。可以依赖的保证：

- **发布带签名。** 清单（版本号和 DLL 的 SHA-256）用 ECDSA P-256 签名，私钥只在发布流水线里；对应的公钥编译在插件里。只有签名校验通过、签名里的版本就是正在安装的那个发布且比当前运行的更新、DLL 的 SHA-256 与签名一致且里面写的版本号相符，才会安装；没有有效签名的文件一律不安装，下载也只来自本仓库的发布地址。能改发布文件或你的网络连接、但无法签名的人，装不上任何东西。
- **自动回滚。** 被替换的 DLL 会以 `EDF6Coop.dll.old` 留在旁边，直到游戏进入标题画面后新版本又运行满 20 秒。如果游戏在这之前崩溃或被强制结束，下次启动会自动把旧版本放回去，记下失败的版本（`EDF6Coop.dll.bad`，不会再装它），这一次游戏不加载插件。在这之前正常退出游戏不算失败：下次启动新版本继续试用。
- **旧版本的设置文件。** 没有 `AutoUpdate` 这一行的设置文件（旧版本不写这一行）和 0.3.6 一样视为**开启**，每次启动日志都会写明。要关闭，在 `Mods\Plugins\EDF6Coop.ini` 末尾加上这两行：

  ```
  [Update]
  AutoUpdate=0
  ```

  当前版本新生成的设置文件里已经是 `AutoUpdate=1`。`AutoUpdate=0` 关闭下载（回滚仍然有效）。
卸载：双击 `UNINSTALL.bat`（EDFModLoader 与其他 Mod 不动，安装程序升级过的加载器也保留，备份 `winmm.dll.bak-official` 留在原处）。它会删除插件、设置、日志和更新留下的文件（`EDF6Coop.dll.old` 等），以及插件在路由器上建立的 UPnP 映射（只删指向本机、名为 `EDF6DirectNet` 的那一条；路由器不支持 UPnP 就直接跳过）。若当初添加过防火墙规则，请以管理员身份运行 `UNINSTALL.bat` 一并删除；没有管理员权限时它会显示删除命令。

也可以手动把 zip 内容解压到游戏目录（`EDF6.exe` 所在文件夹）。

## 房主开启公网直连（可选）

只有房主需要设置；加入者只要装了插件、`AutoJoin=1`（默认），进房后就会自动直连。

1. 打开 `Mods\Plugins\EDF6Coop.ini`，改 `Mode=host`。
2. 右键游戏目录里的 `EDF6Coop_AllowFirewall.bat` → **以管理员身份运行**（只需一次）。这条规则只放行 `EDF6.exe` 在 ini 里 `ListenPort` 指定的 UDP 端口（没设置则为 27015），而不是所有 UDP 端口；改了 `ListenPort` 之后请再运行一次。
3. 让外网能连到你，二选一：
   - **自动**：`PublicAddress` 留空。插件用路由器 UPnP 映射 UDP 27015，并自动带上本机公网 IPv6。
   - **手动**：在路由器把 UDP 端口映射到本机，然后填 `PublicAddress=公网IP:外部端口`（也可以填 DDNS 域名，如 `myroom.ddns.net:40000`）。
   - **电脑直接拨号上网（PPPoE，没有路由器）**：插件识别不到这类网卡，不会自动公布地址，请手动填 `PublicAddress=公网IP:27015`。
4. 重启游戏，正常建房。

怎么确认成功（看日志 `Mods\Plugins\EDF6Coop.log`）：

| 日志 | 含义 |
|---|---|
| `DIRECT players who join your room connect to ...` | 房主地址已确定并写进房间信息 |
| `UPNP router now forwards UDP ...` | UPnP 映射成功 |
| `UPNP no router with UPnP port mapping found` / `UPNP port mapping failed` | 路由器不支持或没开 UPnP，改用手动端口映射 |
| `UPNP WARNING: the router WAN address ... is private (carrier-grade NAT)` | 你在运营商大内网里，没有公网 IPv4，IPv4 直连不可能；只能靠 IPv6 或找运营商要公网 IP |
| `UPNP UDP 27015 is already forwarded to ...; left alone` | 路由器上这个端口已经映射给了局域网里别的设备，插件不会删它；换一个 `ListenPort`，或手动映射 |
| `DIRECT client ... connected from ...`（房主） / `DIRECT connected to host ...`（加入者） | 直连已建立 |
| `DIRECT auto-connect stopped (the room host did not answer on any advertised address ...)` | 加入者连不上房主（防火墙/端口映射/Key 不一致/EDF6Coop 版本不同），游戏照常走 Epic，60 秒后重试 |

加入者按 IPv4 → IPv6 顺序每个地址试 10 秒，都不通就留在 EOS，不影响正常游戏。

**关于 `Key=`**：可选暗号，安全不靠它。每个装了插件的玩家都会在自己的大厅成员信息里公布一把本局游戏专用密钥的指纹。房主只有在对方证明自己持有玩家 X 的密钥后，才允许他以 X 的身份直连；加入者也只接受证明自己持有房间所有者密钥的房主。之后每个直连包都用只有这两方才有的密钥认证，所以其他任何人——同房间的其他玩家、网络路径上的人——都无法冒充别人直连、抢走链路，或篡改、注入、重放数据包。设了 Key 时它作为额外的共享密钥混入这些密钥。它**不会**写进房间信息——房主设了 Key，每个加入者都必须在自己的 ini 里填同样的 Key，否则自动直连失败、回落到 EOS。只和熟人玩可以不设。

**隐私**：房主的公网地址写在大厅成员属性里，能看到这个房间的人都读得到。加入者直连（`AutoJoin=1`）时是从自己的公网地址连向房主，所以房主能看到加入者的地址（原版 EOS 点对点通常也会让双方互相看到地址）。不想让你加入的房间的房主看到你的地址，就设 `AutoJoin=0`，继续走 EOS。

## 设置参考（`EDF6Coop.ini`，改完重启游戏生效）

下面是直连的设置项。同一个文件里还有大房间用的 `[MultiSlot]`、`[Smoothing]`、`[RoomScreen]`、`[Mission]`、`[CopyArmor]` 和 `[HostData]`，文件里的注释解释了每一项。`[MultiSlot] Enabled=0` 和 `[DirectNet] Enabled=0` 同时设置时插件自己卸载。

| 键 | 默认 | 说明 |
|---|---|---|
| `[DirectNet] Enabled` | `1` | `0` = 插件不加载任何钩子，与原版完全一致 |
| `Mode` | `off` | `off` 普通玩家 / `host` 当直连房主 / `join` 手动指定房主地址（一般用不到，自动直连已覆盖） |
| `ListenPort` | `27015` | host：监听的 UDP 端口（端口映射、防火墙放行的就是它）；join：本机端口，留空 = 自动 |
| `PublicAddress` | 空 | host：告诉别人连哪里。空 = 公网 IPv6 + UPnP 映射的 IPv4 |
| `AutoJoin` | `1` | 进入别人房间时，若房主开了直连就自动连过去（房主会看到你的公网地址；`0` 则继续走 EOS） |
| `HostAddress` | 空 | 仅 `Mode=join`：房主地址，如 `123.45.67.89:27015` / `[2408:8207::5]:27015` |
| `Key` | 空 | 直连暗号，所有人必须一致；只用英文字母和数字 |
| `UPnP` | `1` | host 时自动让路由器做端口映射。只在 `Mode=host` 时生效；默认的 `Mode=off` 什么端口都不开 |
| `BindPhysicalInterface` | `1` | 直连流量固定走物理网卡，不被 Clash / 加速器的 TUN 网卡劫持 |
| `LinkTimeoutMs` | `60000` | 直连多久收不到对方数据才算断开（3000–300000） |
| `[EOS] FixedPort` | `0` | EOS 使用固定 UDP 端口 `FixedPort`～`FixedPort+7`；0 = 随机 |
| `Relay` | `default` | EOS 中继：`default` 不改 / `allow` / `norelay` / `force` |
| `[Sync] ReliableGameTraffic` | `1` | 防不同步（可靠发送）。`0` = 原版 |
| `[Resilience] HoldDisconnects` | `auto` | 断线宽限：`auto` 只对装了插件的人 / `off` 原版 / `all` 不识别、对所有人宽限（仅当确定全员都装了插件） |
| `GraceSeconds` | `30` | 断线最多隐瞒多少秒（1–600） |
| `[Update] AutoUpdate` | `1` | 自动从 GitHub 安装带签名的新版本（下次启动游戏生效）。没有这一行的设置文件同样视为 `1` |

## 工作原理

### 可靠发送

`EDF.dll+0x12c8bc0` 的两个 `EOS_P2P_SendPacket` 调用点都传 `Reliability = UnreliableUnordered`。插件在导入表层把它改成 `ReliableUnordered`：每个包恰好送达一次、可能乱序——这本就是不可靠传输允许出现的交付方式，所以游戏逻辑不受影响，只是不再丢包。接收方不需要任何配合。

### 断线宽限与插件识别

对方没装插件时，他的游戏会照常把你移出对局；你这边若继续隐瞒断线，两边状态就会分叉，所以只能对装了插件的人宽限。

EDF6 会把收到的任何 EOS 包都当游戏数据解析（`ReceivePacket` 的 `RequestedChannel` 为 NULL），不能在 P2P 上发探测包。插件改用大厅**成员属性**：进房/建房成功后给自己写 `EDF6DN=1`（`EDF.dll` 不导入任何成员属性函数，游戏看不到），断线时从本地大厅数据读对方有没有这个属性：

- 有标记，且之前连通过 → 宽限，期间每 2 秒调用 `AcceptConnection` 请求重连；
- 没有标记 → 原版处理；
- 公网直连成员 → 只要直连还有回应就一直宽限（每秒一次心跳，菜单里也有）；它的游戏数据本就不走 EOS；
- Epic 房间服务报告某成员掉线（`DISCONNECTED`：他和 Epic 房间服务断了，不是和游戏断了），而他的直连还有回应 → 不告诉游戏。EOS 之后把他放回房间（`JOINED`）时，两条通知一起吞掉，游戏完全察觉不到。只有直连连续 `GraceSeconds` 秒没回应，或者他真的离开、被踢，才告诉游戏。对自己也一样；
- 对方真的离开大厅 → 立即把断线交给游戏。

### 公网直连

房主另写成员属性 `EDF6DN_ADDR`（空格分隔的地址列表）。其他成员每 2 秒读一次，按 IPv4、IPv6 顺序每个地址试 10 秒，全失败则 60 秒后重试。传输层是自己的 UDP 协议：选择确认、令牌桶限速重传、每条链路独立的密钥（见下）；用 `IP_UNICAST_IF` 绑定物理网卡防 TUN 劫持。

每个装插件的玩家还会写 `EDF6DN_ID`：本局游戏生成的 ECDSA P-256 密钥的指纹（SHA-256）。建立连接先走一次 cookie 往返：房主对 hello 回一个绑定发送方地址的 cookie，cookie 回来之前什么都不保存（所以伪造源地址、hello 洪泛都没有用）；加入者带上 cookie、用这把密钥签名后再发一次 hello。房主只有在 EOS ID X 在房间里、且公布的指纹与这把密钥一致时才接受；更早会话的签名 hello 当作重放拒绝。反过来，加入者只接受由房间所有者公布指纹的那把密钥签名的 welcome。hello 和 welcome 在签名下各带一把新的 ECDH 密钥，于是每条链路有自己的密钥（设了 `Key=` 时一并混入），链路上的每个包都带用它算的标签和计数器：被篡改、注入或重放的包一律丢弃。所以只有证明了身份的房间成员才能发数据。不加密。协议版本不同的 EDF6DirectNet（0.3.6 及更早是协议 2，0.4.0 是协议 3）互相忽略对方的包，彼此照常走 EOS。

### 房主说了算，不经 Epic 重进

游戏从大厅得知房间成员：进房时的成员列表，之后每次变化一个状态事件。Epic 房间服务是这些事件的一个来源，房主是另一个：协议 6 起，房主把自己游戏里的成员列表（`Room` 消息）随名单发给每个直连成员，每个成员的游戏都跟着它走。两个来源会说同一件事（某人离开，Epic 和房主都会报），插件自己要告诉游戏的事先等 5 秒，让 Epic 先说：Epic 正常时游戏看到的就是 Epic 的事件，和没装插件一样；Epic 迟到时，每个事件再过一遍本地视图：已经进来的不再进一次，已经走了的不再走一次，“自己被踢 / 房间关闭”只说一次（日志 `ROOM ... not told again`）。只在直连上的成员也会出现在游戏读到的房间成员里。

- 房主：Epic 列表里没有、有直连、游戏里也没有的玩家，在房间没满时会被放进房间（他的插件只有在游戏在房间里时才会连房主）；只靠直连在房间里的玩家，直连断了 `GraceSeconds` 秒就算离开；刚离开的人不会被还没超时的旧直连拉回来，要重新连上才行。房主踢掉的人在这个房间里不会再被直连放进来，除非他重新经 Epic 进房。踢一个只在直连上、Epic 不知道的人，由插件直接完成。
- 成员：在别人房间里时每 2 秒记一次这个房间（房主、房主公布的身份指纹和地址、房间属性）。离开后 30 分钟内，Epic 搜房失败时（房间服务挂了），搜房结果里会出现这个房间；Epic 能搜时结果原样交给游戏。选它就按记下的地址直连房主（每个地址 5 秒，总共 30 秒），房主的成员列表里出现自己就算进房。房间关闭、被踢、进了别的房间、房主换人时忘掉它。这时的房间是房主那边的真房间，Epic 不知道你在里面：游戏里的离开 / 解散在本地完成。

### 房主的武器和载具文件

改过武器或载具（`Mods\WEAPON\*.SGO`、`Mods\OBJECT\V*.SGO` / `VEHICLE*.SGO`）时，房间里其他人加载的还是自己的文件，房主的魔改枪在别人屏幕上就是原版。EDF6Coop 让房主提供自己的文件，房间里的玩家可以拿来用，只在这个房间有效：

- 每台机器把自己武器/载具文件的 SHA-256 发布在自己的大厅成员信息上（只有本人能写）。房主的和你的不一样时，菜单显示 `F1 host weapons :OFF`。
- 按 F1 通过游戏本来就有的、到房主的 P2P 连接下载（`host weapons 37%`），每个字节都对照房主发布的 SHA-256 校验，下一个菜单画面起生效（`F1 host weapons :ON`）。再按 F1 换回自己的文件。`[HostData] Accept=Always` 不问直接用，`Never` 只提示不一样。
- 只传数据：武器文件（不含 `WEAPONTABLE` 和 `WEAPONTEXT`，所以不会多出新武器，也不会有东西留在存档里）和载具文件，最多 128 个、每个 256 KB、合计 4 MB。绝不传 DLL、补丁或别的东西，不是 SGO 的文件会被拒绝。
- 文件放在 `Mods\Plugins\EDF6Coop.hostdata\<SHA-256>`（保留最近四套），绝不写进你的 Mods 文件夹。使用期间游戏按文件逐个改读那里，只在内存里：退出房间、退出游戏或崩溃，都会回到你自己的文件。
- 你自己有而房主没有的文件照常使用，菜单会计数（`+2 own`）。
- `[HostData] Share=0` 当房主时不提供；`Enabled=0` 全部关闭，游戏读文件的方式完全不动。

## 排障

- **先看日志**：`Mods\Plugins\EDF6Coop.log`（自动保持在 2MB 左右以内；直连相关的行以 `[DN]` 开头）。出现 `==== EDF6Coop x.y.z-<N>p ====` 这一行说明插件已加载；没有这一行说明 EDFModLoader 没装好。
- `EOS hooks FAILED`：游戏版本更新导致导入表不符，插件自动停用直连，请提 Issue。
- `LOBBY plugin detection UNAVAILABLE`：无法识别对方是否装了插件，断线宽限只对直连成员生效。
- `EOS incoming packet queue FULL`：EOS 队列满开始丢包，请附日志提 Issue。
- `RESILIENCE ... RECOVERED` 表示一次断线被成功隐藏；`did not come back within` 表示超时后交给了游戏。
- `RESILIENCE ... lost Epic's lobby service but the direct link is up` / `back in Epic's lobby service`：Epic 房间服务把某人踢掉又放了回来，游戏没察觉。`direct link silent for ...` 表示他真的掉了，已交给游戏。
- `REJOIN ...`：不经 Epic 重进的过程。`remembering room` 记下了房间；`the room list gets room` Epic 搜房失败，列表里放了它；`dialling the room's host` / `in room ... again` 正在连 / 已经回到房间；`its host did not let us in within` 房主 30 秒内没放你进来（房主不在、或已把你踢出）。
- `GAME kicks ... from the room (direct link up/down, ...)`：游戏自己把某人移出了房间（或者是你手动踢的），并记下当时直连是否还显示他在玩。
- `STATS last 60s: ...`：游戏有收发时每分钟一行。内容有游戏自己发了多少数据（平均值和最忙那一秒，单位 kbps；游戏会把常规同步压在约 320 kbps 以内，接近上限时跳过次要更新）、其中有多少是发给多个人的同一份数据、有多少包在 5 秒内把同一份数据又发给了同一个人（即游戏自己的重发，如果有的话）、直连在线路上实际占用的上传 / 下载（`wire up`/`down`，含重发和房主替别人转发的部分）、包数，以及每条直连的 `retx`（我方重发）、`dup`（对方重发了我们已收到的包）、`gaveup`（游戏以不可靠方式发的包，2 秒后放弃重发）、`skipped`（对方放弃、我们没收到的包）、`held`（链路送达太少、重发被压住的时长）、`credit`（此刻允许的重发数）。`send-failures` 或 `send-refused` 不为 0 时请附日志。
- `DIRECT ... timed out: nothing received for ...` 表示对方没声了；`DIRECT ... stalled: a packet the game sent reliably is unacknowledged ...` 表示对方还在应答，但游戏需要的某个包一直送不到。
- `DIRECT refused hello for ...`：有人想以某个玩家的身份直连，但证明不了。玩家刚进房的一两秒内出现 `published no direct-link identity` 是正常的（他的房间信息还没到房主这里，对方每秒重试）；对没装插件或 0.3.6 及更早版本的玩家，意思是他继续走 EOS。`not signed by the identity that player published` 说明有人冒充该玩家，已被拒绝，影响不到那个玩家。
- `DIRECT ... speaks direct-link protocol 5, we speak 6`：对方的版本不同（协议 6 是 EDF6Coop 2.2.0，5 是 2.0.0-2.1.0，4 是 EDF6DirectNet 0.4.1，3 是 0.4.0，2 是 0.3.6 或更早）。你们之间不走直连，游戏照常通过 EOS 进行；把双方更新到同一版本即可。

## 构建

需要 Windows x64、装了「使用 C++ 的桌面开发」的 Visual Studio 2022（用它带的 CMake 和 Ninja）、Python 3.10+ 并 `pip install numpy pillow pefile==2024.8.26 capstone==5.0.9`，以及已安装的地球防卫军6（EDF.dll 版本 `678CCB46`）。

```powershell
$env:EDF6_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6'
powershell -ExecutionPolicy Bypass -File build.ps1 -Test    # -> multislot\dist\EDF6Coop.dll
powershell -ExecutionPolicy Bypass -File package.ps1        # -> release\EDF6Coop-<version>.zip (+ .sha256)
```

- 所有房间人数只有一个构建（人数在游戏里选，见上文）。`-Test` 跑测试（单元测试、本机回环多节点直连测试，以及逐个核对补丁位置与游戏 `EDF.dll` 代码的测试，没装游戏时跳过）。
- 游戏目录只读不写：`Root.cpk` 用来生成带「Player MOD」标签的菜单框（`multislot/tools/make_menu_label.py` 写出 `multislot/assets/LYT_MAINFRAME.SGO`，它来自游戏文件，所以从不提交），`HUD/ONLINEHUDTEXTURE.RAB` 用来生成每个玩家一种颜色的 HUD 贴图（`multislot/tools/make_hud_colours.py` 写出 `multislot/assets/ONLINEHUDTEXTURE.RAB`，同样来自游戏文件、从不提交），`EDF.dll` 给测试用。
- `build.ps1` 会获取官方 EDFModLoader v1.0.10 的 `winmm.dll`（校验 SHA-256），并生成随包的修复版加载器（`multislot/tools/fix_winmm_proxy.py`）。
- `package.ps1` 拒绝打包版本不符的构建、比源码旧的 DLL，以及 CI 构建（`build.ps1 -CI` 不带游戏的菜单和 HUD 资源，内嵌占位数据）。

## 发布

发布包需要游戏的菜单资源，而它不能放到构建服务器上，所以发布包在装了游戏的机器上构建；GitHub Actions 跑 CI 构建和测试（`.github/workflows/ci.yml`）。

1. 改版本号：`multislot/CMakeLists.txt` 里的 `project(EDF6Coop VERSION x.y.z)`，以及三份随包说明 `dist/README_EDF6Coop*.txt` 的第一行。
2. 写发布说明 `release-notes/<版本>.md`（即 Release 页面正文，也是包里的 `RELEASE_NOTES_EDF6Coop.md`）。
3. 提交到 `main`，打 tag 并推送：`git tag v2.0.0 && git push origin v2.0.0`。
4. 在装了游戏的机器上、在该提交处运行 `powershell -ExecutionPolicy Bypass -File release.ps1 -Upload`。它执行 `build.ps1 -Test` 和 `package.ps1`，然后创建一个**草稿** Release，带上 `EDF6Coop.dll`、`EDF6Coop-<版本>.zip` 及其 `.sha256`，另外同一个 DLL 再以六个旧名 `EDF6Coop-<N>p.dll`（8、10、12、16、24、32）各放一份，让 2.2.x 各人数版都能更新（不加 `-Upload` 时只放到 `release\upload-<版本>\`）。它从不签名。
5. `gh workflow run release.yml -f tag=v2.0.0`（`.github/workflows/release.yml`）：检查 tag 在 `main` 上且与版本一致、草稿里 DLL 和 zip 都齐、每个 zip 与其 `.sha256` 相符且装的正是那个 DLL、没有 CI 构建；用 `sign-update.ps1` 给 DLL 签名（密钥是仓库 secret `EDF6DN_UPDATE_SIGNING_KEY`，只在只读 token 的 job 里）；上传 `.dll.sig` 和 `.dll.sha256`（旧名的也一并上传）；把 Release 发布为最新。

所有已安装插件的自动更新都从最新 Release 下载 `EDF6Coop.dll` / `.dll.sig`；2.2.x 还在找自己人数的 `EDF6Coop-<N>p.dll`，所以 Release 同时带同一个 DLL 的六个旧名。

## 目录

| 路径 | 内容 |
|---|---|
| `multislot/src/plugin.cpp` | EDFModLoader 入口：读 `EDF6Coop.ini`（搬入旧设置文件的值），启动房间部分和直连 |
| `multislot/src/` | 房间部分（Player MOD）：补丁、房间画面、任务、护甲复制、菜单布局、HUD 颜色、日志 |
| `src/config.*` | 直连的设置项和注释（按 Windows 显示语言写中文 / 日文 / 英文） |
| `src/eos_min.h` | 用到的 EOS SDK 结构体（取自官方 1.15.5 头文件；游戏用的是 1.16.1） |
| `src/eos_hooks.cpp`、`src/iat.*` | 改 `EDF.dll` 导入表，接管 EOS P2P / 大厅调用 |
| `src/hold.*` | 断线宽限 |
| `src/lobby_marker.*` | 大厅成员属性：识别谁装了插件、分发房主地址 |
| `src/direct_net.*`、`src/reliable.*`、`src/wire.*`、`src/auth.*` | 直连传输 |
| `src/netif.*`、`src/upnp.*` | 物理网卡识别、UPnP |
| `src/updater.*`、`src/product.h` | 带签名的自动更新；每个发布带的版本 |
| `multislot/CMakeLists.txt` | 构建和唯一的版本号（`project(EDF6Coop VERSION x.y.z)`） |
| `multislot/packaging/` | 随包附带的加载器修复和握手恢复说明 |
| `dist/` | 安装脚本和随包说明（英文 / 中文 / 日文） |
| `tests/`、`multislot/tests/` | 测试 |

## 已知限制

- 不支持任务中途加入（游戏本身没有这个功能）。
- 只解决丢包造成的不同步；游戏逻辑本身的不同步需要具体症状再逆向定位。
- 断线宽限期间其他玩家可能在同步点等待，最多 `GraceSeconds` 秒。
- 直连由房主转发：某个加入者的直连在重连的那一瞬间，房主正替他转发、尚未被确认的少量数据会丢失（游戏随后回落到 EOS）。
- 直连流量只做认证、不加密：网络路径上的人能看到内容（就像能看到谁和谁在玩），也总能把包丢掉（游戏随即回落到 EOS）。
- 游戏退出时不会删除 UPnP 映射（那时没有安全的时机做网络调用）：插件会在之后第一次不做房主的启动（`Mode` 不是 `host`，或 `UPnP=0`）时删掉它建立的那一条，`UNINSTALL.bat` 也会删。它不会动别的设备建立的映射。在此之前，端口上没有程序监听时无害；也可以在路由器管理页删除名为 `EDF6DirectNet` 的映射。
- 直连本身断了就什么都瞒不住：某个玩家自己的网络断开超过 `GraceSeconds` 秒，游戏照原版处理。

- 房主的武器和载具文件只在任务之间切换，且只传上面列出的那几类：加新武器的 mod（`WEAPONTABLE`）、`mod.cpk` / `mod.dll` 类 mod 和补丁都不共享。

## 许可证

MIT，见 [LICENSE](LICENSE)。附带的 EDFModLoader 为 MIT，许可证随包在 `EDFModLoader_LICENSE.txt`。房间部分（`multislot/`，momotori01 作）为公有领域（[Unlicense](multislot/LICENSE)）。
