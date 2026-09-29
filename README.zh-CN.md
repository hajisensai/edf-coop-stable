# edf-coop-stable

[English](README.md) | **中文** | [日本語](README.ja.md)

EARTH DEFENSE FORCE 6（PC / Steam）联机稳定插件 **EDF6DirectNet**，以 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 插件形式运行。

地球防卫军系列的联机稳定 Mod。目前支持：**EDF6**。EDF6 稳定后计划支持 EDF5 等其他作品。

> 非官方 Mod，与 D3 PUBLISHER / SANDLOT / Epic Games 无关。只在本机进程内接管网络调用，不改动任何游戏文件；`Enabled=0` 或删掉 DLL 即恢复原版。
>
> **状态：实验性。** 900+ 项自动化测试通过；自动直连和断线宽限已在真实的四人联机中验证（0.3.2：所有人都走直连，7 次连接中断都没让游戏察觉）。出问题请附日志提 [Issue](https://github.com/hajisensai/edf-coop-stable/issues)。

## EDF6MultiSlot：8 人联机（可选 10 / 12 人）

8 人联机插件 EDF6MultiSlot（原作者 momotori01，公有领域）在 [multislot/](multislot/README.zh-CN.md) 目录，有自己的构建、打包和说明，可以和 EDF6DirectNet 一起用。

## 它解决什么

| 症状 | 原因 | 插件的做法 | 需要谁装 |
|---|---|---|---|
| 联机中途敌人/位置/血量对不上（不同步） | EDF6 把全部联机数据按 `UnreliableUnordered` 发送，丢一个包就永久丢一份状态 | 改为 `ReliableUnordered` 发送（丢了自动重发，仍允许乱序，游戏收到的交付语义不变）；EOS 收发队列扩到至少 64MB，避免队列满时丢包 | **发送方**。你装了，你发出的数据就不丢；所有人都装效果最好 |
| 网络抖一下就被踢出房间、任务白打 | EOS 连接因超时/网络错误关闭时，游戏立刻移除该玩家 | 暂不告诉游戏，后台让 EOS 重连；30 秒内恢复则游戏无感知，否则按原版处理 | **双方都装**（插件自动识别对方，见下文） |
| Epic 中继延迟高、NAT 打洞失败 | 所有流量走 EOS P2P / 中继 | 房主可开「公网直连」：其他人进房后自动直接连房主（星形拓扑，房主转发） | 房主设置，加入者装插件即可 |
| 掉线了不知道为什么 | — | 写诊断日志：NAT 类型、直连/中继、断开原因、大厅成员、每分钟收发统计 | 自己 |

默认全部开启（公网直连需房主手动开）。

## 安装

1. 到 [Releases](https://github.com/hajisensai/edf-coop-stable/releases/latest) 下载 `EDF6DirectNet-v*.zip`，**完整解压**到任意文件夹。
2. 双击 **`INSTALL.bat`**：从 Steam 库自动找到 EDF6 并安装。
   - 没有 EDFModLoader 时会装上附带的官方版（[BlueAmulet/EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) v1.0.10，MIT）；已有的 `winmm.dll` 不会覆盖。
   - 找不到游戏时会让你粘贴游戏目录（Steam 库里右键 EDF6 → 管理 → 浏览本地文件）。
3. 从 Steam 正常启动游戏。第一次启动后生成 `Mods\Plugins\EDF6DirectNet.ini`（设置）和 `EDF6DirectNet.log`（日志）。

zip 内附 README_EDF6DirectNet.txt（English）、README_EDF6DirectNet_zh.txt（中文）、README_EDF6DirectNet_ja.txt（日本語）；默认设置文件的注释按 Windows 显示语言写成中文 / 日文 / 其他语言一律英文。

升级：重新运行新版的 `INSTALL.bat`，设置文件保留。
卸载：双击 `UNINSTALL.bat`（EDFModLoader 与其他 Mod 不动）。若当初添加过防火墙规则，卸载程序会提示删除命令。

也可以手动把 zip 内容解压到游戏目录（`EDF6.exe` 所在文件夹）。

## 房主开启公网直连（可选）

只有房主需要设置；加入者只要装了插件、`AutoJoin=1`（默认），进房后就会自动直连。

1. 打开 `Mods\Plugins\EDF6DirectNet.ini`，改 `Mode=host`。
2. 右键游戏目录里的 `EDF6DirectNet_AllowFirewall.bat` → **以管理员身份运行**（只需一次）。
3. 让外网能连到你，二选一：
   - **自动**：`PublicAddress` 留空。插件用路由器 UPnP 映射 UDP 27015，并自动带上本机公网 IPv6。
   - **手动**：在路由器把 UDP 端口映射到本机，然后填 `PublicAddress=公网IP:外部端口`（也可以填 DDNS 域名，如 `myroom.ddns.net:40000`）。
   - **电脑直接拨号上网（PPPoE，没有路由器）**：插件识别不到这类网卡，不会自动公布地址，请手动填 `PublicAddress=公网IP:27015`。
4. 重启游戏，正常建房。

怎么确认成功（看日志 `Mods\Plugins\EDF6DirectNet.log`）：

| 日志 | 含义 |
|---|---|
| `DIRECT players who join your room connect to ...` | 房主地址已确定并写进房间信息 |
| `UPNP router now forwards UDP ...` | UPnP 映射成功 |
| `UPNP no router with UPnP port mapping found` / `UPNP port mapping failed` | 路由器不支持或没开 UPnP，改用手动端口映射 |
| `UPNP WARNING: the router WAN address ... is private (carrier-grade NAT)` | 你在运营商大内网里，没有公网 IPv4，IPv4 直连不可能；只能靠 IPv6 或找运营商要公网 IP |
| `UPNP UDP 27015 is already forwarded to ...; left alone` | 路由器上这个端口已经映射给了局域网里别的设备，插件不会删它；换一个 `ListenPort`，或手动映射 |
| `DIRECT client ... connected from ...`（房主） / `DIRECT connected to host ...`（加入者） | 直连已建立 |
| `DIRECT auto-connect stopped (the room host did not answer on any advertised address ...)` | 加入者连不上房主（防火墙/端口映射/Key 不一致），游戏照常走 Epic，60 秒后重试 |

加入者按 IPv4 → IPv6 顺序每个地址试 10 秒，都不通就留在 EOS，不影响正常游戏。

**关于 `Key=`**：可选暗号，用来防止知道你地址和玩家 EOS ID 的人伪造直连数据。它**不会**写进房间信息——房主设了 Key，每个加入者都必须在自己的 ini 里填同样的 Key，否则自动直连失败、回落到 EOS。只和熟人玩可以不设（房主日志会有一条 `hosting without Key=` 的提醒）。

**隐私**：房主的公网地址写在大厅成员属性里，能看到这个房间的人都读得到。

## 设置参考（`EDF6DirectNet.ini`，改完重启游戏生效）

| 键 | 默认 | 说明 |
|---|---|---|
| `[DirectNet] Enabled` | `1` | `0` = 插件不加载任何钩子，与原版完全一致 |
| `Mode` | `off` | `off` 普通玩家 / `host` 当直连房主 / `join` 手动指定房主地址（一般用不到，自动直连已覆盖） |
| `ListenPort` | `27015` | host：监听的 UDP 端口（端口映射、防火墙放行的就是它）；join：本机端口，留空 = 自动 |
| `PublicAddress` | 空 | host：告诉别人连哪里。空 = 公网 IPv6 + UPnP 映射的 IPv4 |
| `AutoJoin` | `1` | 进入别人房间时，若房主开了直连就自动连过去 |
| `HostAddress` | 空 | 仅 `Mode=join`：房主地址，如 `123.45.67.89:27015` / `[2408:8207::5]:27015` |
| `Key` | 空 | 直连暗号，所有人必须一致；只用英文字母和数字 |
| `UPnP` | `1` | host 时自动让路由器做端口映射 |
| `BindPhysicalInterface` | `1` | 直连流量固定走物理网卡，不被 Clash / 加速器的 TUN 网卡劫持 |
| `LinkTimeoutMs` | `60000` | 直连多久收不到对方数据才算断开（3000–300000） |
| `[EOS] FixedPort` | `0` | EOS 使用固定 UDP 端口 `FixedPort`～`FixedPort+7`；0 = 随机 |
| `Relay` | `default` | EOS 中继：`default` 不改 / `allow` / `norelay` / `force` |
| `[Sync] ReliableGameTraffic` | `1` | 防不同步（可靠发送）。`0` = 原版 |
| `[Resilience] HoldDisconnects` | `auto` | 断线宽限：`auto` 只对装了插件的人 / `off` 原版 / `all` 不识别、对所有人宽限（仅当确定全员都装了插件） |
| `GraceSeconds` | `30` | 断线最多隐瞒多少秒（1–600） |

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

房主另写成员属性 `EDF6DN_ADDR`（空格分隔的地址列表）。其他成员每 2 秒读一次，按 IPv4、IPv6 顺序每个地址试 10 秒，全失败则 60 秒后重试。传输层是自己的 UDP 协议：选择确认、令牌桶限速重传、会话 epoch（旧会话的包不会串进重连后的新会话）、可选 `Key=` 暗号（HMAC-SHA256 截断标签，防伪造，不加密）；用 `IP_UNICAST_IF` 绑定物理网卡防 TUN 劫持。

## 排障

- **先看日志**：`Mods\Plugins\EDF6DirectNet.log`（超过 2MB 轮转为 `.log.1`）。开头一行 `==== EDF6DirectNet x.y.z starting` 说明插件已加载；没有这一行说明 EDFModLoader 没装好。
- `EOS hooks FAILED`：游戏版本更新导致导入表不符，插件自动停用直连，请提 Issue。
- `LOBBY plugin detection UNAVAILABLE`：无法识别对方是否装了插件，断线宽限只对直连成员生效。
- `EOS incoming packet queue FULL`：EOS 队列满开始丢包，请附日志提 Issue。
- `RESILIENCE ... RECOVERED` 表示一次断线被成功隐藏；`did not come back within` 表示超时后交给了游戏。
- `RESILIENCE ... lost Epic's lobby service but the direct link is up` / `back in Epic's lobby service`：Epic 房间服务把某人踢掉又放了回来，游戏没察觉。`direct link silent for ...` 表示他真的掉了，已交给游戏。
- `GAME kicks ... from the room (direct link up/down, ...)`：游戏自己把某人移出了房间（或者是你手动踢的），并记下当时直连是否还显示他在玩。
- `STATS last 60s: ...` 每分钟一行收发统计，`send-failures` 不为 0 时请附日志。
- `DIRECT ignored hello for ... its link is live`：有人用某个在线玩家的身份从别的地址发起连接，已被拒绝。偶尔一条可能是对方换了网络（5 秒后会自动接受）；频繁出现说明有人在捣乱，建议设 `Key=`。

## 构建

需要 Visual Studio 2022（MSVC x64）。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
# 手动打发布包：需要官方 EDFModLoader.zip 解压后的目录（含 winmm.dll、ModLoader.ini，外加它的 LICENSE.txt）
powershell -ExecutionPolicy Bypass -File package.ps1 -Version 0.3.3 -ModLoaderDir <目录>
```

产物：`build\EDF6DirectNet.dll`（静态 CRT，只依赖系统 DLL）、`build\edf6_directnet_tests.exe`（单元测试 + 本机回环多节点测试，含 20%～40% 丢包、断网、重启场景；`EDF.dll` 导入表测试需要本机装有游戏，否则跳过）、`build\probe_join.exe`（手动联调用的直连探针）。

## 发布

发布由 GitHub Actions（`.github/workflows/release.yml`）自动完成：

1. 改版本号：`src/plugin.cpp` 的 `kVersionMajor/Minor/Patch` 与 `kVersionText`，以及三份随包说明书 `dist/README_EDF6DirectNet*.txt` 的第一行。
2. 写发布说明 `release-notes/<版本>.md`（就是 Release 页面的正文，没有它流水线会失败）。
3. 提交到 `main`，推送 tag：`git tag v0.3.3 && git push origin v0.3.3`。

流水线会构建、跑测试、下载官方 EDFModLoader v1.0.10（按 SHA-256 校验）、打包 `EDF6DirectNet-v<版本>.zip` 并创建 Release。tag、源码版本号、说明书版本号三者不一致时 `package.ps1` 会拒绝打包。在 Actions 页面手动运行只构建打包（产物在运行记录的 Artifacts 里），不发布。

## 目录

| 路径 | 内容 |
|---|---|
| `src/plugin.cpp` | EDFModLoader 入口，读配置、启动直连 |
| `src/config.*` | INI 读取与默认设置文件（注释按 Windows 显示语言写中文 / 日文 / 英文） |
| `src/eos_min.h` | 所用 EOS SDK 结构体（按官方 1.15.5 头文件，游戏为 1.16.1） |
| `src/eos_hooks.cpp`, `src/iat.*` | 修改 `EDF.dll` 导入表，接管 EOS P2P / 大厅调用 |
| `src/hold.*` | 断线宽限 |
| `src/lobby_marker.*` | 大厅成员属性：识别谁装了插件、分发房主地址 |
| `src/direct_net.*`, `src/reliable.*`, `src/wire.*`, `src/auth.*` | 直连传输 |
| `src/netif.*`, `src/upnp.*` | 物理网卡识别、UPnP |
| `src/log.*` | 日志 |
| `dist/` | 安装脚本与随包说明书（中 / 英 / 日） |
| `tests/` | 测试 |
| `multislot/` | EDF6MultiSlot：8 / 10 / 12 人联机插件（独立构建和说明） |

## 已知限制

- 不支持任务中途加入（游戏本身没有这个功能）。
- 只解决丢包造成的不同步；游戏逻辑本身的不同步需要具体症状再逆向定位。
- 断线宽限期间其他玩家可能在同步点等待，最多 `GraceSeconds` 秒。
- 直连由房主转发：某个加入者的直连在重连的那一瞬间，房主正替他转发、尚未被确认的少量数据会丢失（游戏随后回落到 EOS）。
- 不设 `Key=` 时直连没有身份认证：知道房主地址和某玩家 EOS ID 的人可以伪造该玩家的数据。设了 Key 能防伪造，但仍不能防截获后的 `Bye`/成员表重放（需要升级协议版本，留待下个大版本）。
- UPnP 映射是永久的，游戏退出后不会自动删除（端口上没有程序监听时无害）；需要时在路由器管理页删除名为 `EDF6DirectNet` 的映射。
- 直连本身断了就什么都瞒不住：某个玩家自己的网络断开超过 `GraceSeconds` 秒，游戏照原版处理。

## 许可证

MIT，见 [LICENSE](LICENSE)。附带的 EDFModLoader 为 MIT，许可证随包在 `EDFModLoader\LICENSE.txt`。
