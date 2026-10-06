# 房间人数扩到 1024（W6）

分支 `feat/net-roomsize`。EDF.dll TimeDateStamp `0x678CCB46`。本文盘点所有按人数定长的东西，写清每处的改法和现状，以及需要 W1 / W2 配合的接入点。

## 0. 结论

- 插件的容量常量 `kMaxPlayers` 从 32 改成 **1024**（`multislot/src/patches.h`）。每台机器的用户槽、包会话、语音 HUD 记录和任务玩家数组都按 1024 建。
- 游戏代码里写死的人数上界 4 原来是原地改写，最多到 0x7E。装不下 1024 的有 14 处，全部改成跳到代码洞里做 32 位比较（`widecmp.*`），另有 2 处改写成与人数无关的指令。启动时先核对原字节，任何一处对不上，整套补丁都不启用，只写日志。
- 房间大小可选 2..1024。EOS 大厅最多 64 人、Steam 大厅最多 250 人：大厅只按服务允许的人数创建，真实房间大小写进大厅属性 `MS_ROOMSIZE`。
- SEARCH_TYPE 换成新家族，中心值 `0x53`，公布的值是 0x12..0x15。现有解码方式只剩这一个可用的中心值（见 §3）。
- **gamenet 实测：16 台、32 台开局同步全部通过**（32 台共 1379 项检查，每台机器上每个玩家的字节都一致）。50 人左右以上要靠 W1 的通用分片（§6）。
- 带宽：用「优先级累加 + 每链路字节预算」做兴趣管理（`interest.*`，§5），不按距离写死频率。

## 1. 游戏内按人数定长的地方

记号：✅ 已处理；⚠️ 已处理，但按人数的行为待真机确认；❌ 没处理（原因写在后面）。

### 1.1 房间 / 传输层（每台机器始终生效，`SessionPatches` + `SessionCompares`）

| RVA | 内容 | 改法 |
|---|---|---|
| 12B78EA / 12B7954 | eos::Users 槽位 vector 的 size / capacity 检查 `cmp r, 4` | ✅ 改成代码洞里的 32 位比较 |
| 12B795F / 12B796E | Users 槽位 grow / fill `mov r32, 4` | ✅ imm32 改成 1024 |
| 12CB96E | packet::Controller 的会话数 `mov ebx, 4` | ✅ imm32 改成 1024（12CDA40 等用槽位号索引，不检查边界） |
| 96069E / 9606C3 | UiVoiceChat_Notify 记录的 reserve 检查和移动上限 | ✅ 改成代码洞里的 32 位比较 |
| 9606A9 / 9606D4 / 960774 / 960781 | 语音 HUD 记录的分配（0x50×N）、移动数、容量、resize | ✅ imm32 |
| 7468C0 的 3 个调用点 | 房间成员列表构造 | ✅ 截到 kMaxPlayers（`fakemembers.cpp` ClampMembers） |
| 73B5D5 / 73B5CC | 房间信息里的容量 | ✅ 改从 EOS 读，超过 64 时读 `MS_ROOMSIZE`（`rooms.cpp`） |

### 1.2 任务阶段（`[Mission] Extend=1`：`MissionPatches` / `MissionCompares` / `MissionHooks`）

| RVA | 内容 | 改法 |
|---|---|---|
| 78D24C | MissionSync_Res 丢掉 index≥4 的回复 | ✅ 32 位比较 |
| 7908A0 | MissionSync_Update 只为 index<4 存记录 | ✅ 32 位比较（gamenet 走的就是这条真实代码路径） |
| 5959E1 / 59DCEA / 0DFD19 | CreateOnlinePlayerObject、玩家记录查找、颜色查找 | ✅ 32 位比较 |
| 1DA3F8 | FindPlayerIndex 的循环上界 | ✅ 32 位比较 |
| 1DA402 | FindPlayerIndex「没找到」 `lea eax, [rbp-(N+1)]`（disp8） | ✅ 改成 `or eax, -1`：只有循环正常结束才落到这里，没有任何跳转指向它，后面的 epilogue 也不读标志位（tests.cpp 有守卫） |
| 1B8139 / 1B82B9 | PlayerIgnoreDamageEventMode / PlayerStealthMode 的玩家循环 | ✅ 32 位比较 |
| 78FFAC / 78FFDC | ResultSync 道具数的重置 / 存储 | ✅ 32 位比较 |
| 1D6C9C / 1DD5DC | MissionContext 分配（+0x300 起 N×16） | ✅ imm32，0x4310 字节 |
| 1D6EDC | MissionContext 析构的条目数 `lea r8d, [rdx-0xC]`（disp8） | ✅ 中间 hook：rdx=0x10、r8=1024 |
| 1A19F3 等 6 处 `lea end, [begin+0x40]` | 遍历玩家数组的循环 | ✅ handler 按 kMaxPlayers 算终点 |
| 1A3281 / 1A3451 | EventFactor_ObjectAreaIn::Initialize(…, players, count) | ✅ 传 1024 个条目；207870 把它们 push 进一个 vector，没有固定容量。16 KiB 的拷贝缓冲放在 thread_local，不占游戏栈 |
| 1D98B6 / 1D9A3B | CreatePlayers 栈上 8 字节的 remote 标志 | ✅ 搬到插件的 1024 字节缓冲 |
| 1D9A98 | 出生点 | ✅ 1..32 号保持原来的布局；33 号起在 1 号周围排成环形，间距等于 1、2 号的距离（`SpawnOffset`，单测确认 1024 人在 200 m 内、不重叠） |
| 595A03 等 8 处 `imul r, idx, 0xD4` | GameStatus+0x14C78 的装备记录 | ✅ 4 号起用 sidecar（1020×0xD4 ≈ 211 KiB 静态数组）。全二进制扫描确认 `imul 0xD4` 只有这 8 处 |
| 7FFD95 | 在线 HUD 颜色表的玩家下标 | ✅ 两种模式都循环取模：有插件 HUD 归档时 % 32，没有时 % 4 |
| 8073F5… / 802A8A… | HUD 状态灯、聊天气泡表 | ✅ 维持 32 项（`kHudTablePlayers`）。1024 色的灯贴图没意义，imm8 也够用 |
| 0D7770 / 54F0DF / 54F1B1 | 难度 × 人数系数（CONFIG 表只有 1..4 人） | ✅ 5 人以上用 4 人的系数 |
| `ScaledEnemyCount` | 敌人数量 ×(1+0.2×超过 4 的人数) | ⚠️ 只算到 32 人（×6.6）就不再增加（`kEnemyScalePlayers`）。原因：每台机器都要模拟全部敌人，×205 不可行 |
| **21F3CF / 222761 / 228AED / 22A685** | **MissionScriptBVMImplement+0x168 的玩家表（4 项×0x18）** | ✅ **本轮新修的越界读**，见 §1.3 |

### 1.3 21F3C2：脚本 VM 的玩家表（本轮逆向结论）

- 构造函数 20DC10 在 +0x168 用 eh vector constructor（12D8D44，元素 0x18 字节，个数 4）建了一张 4 项的表。每项最后 16 字节是 weak_ptr（+0x170 对象指针，+0x178 控制块指针）。填表的是 22B1C0（脚本 VM 的 CreatePlayers）：它按 GameStatus+0x14FF4（分屏人数，1~2）逐项填写，所以表里永远不超过 4 项。
- 21F380（一个脚本操作码）用 `for i < GetOnlinePlayerCount()` 遍历这张表。225290 在线时返回的是 GameStatus+0x14FF8，也就是在线人数，读的是 `[this + 0x178 + i*0x18]`。另外 222740、228AC0、22A650 直接拿脚本传进来的下标读同一张表，三者都不检查边界。
- **第 5 人起确实越界**：i=4 读到 +0x1D8，这是 +0x1C8 处那个 vector 的字段；i 再往上会读进 +0x210 起的另一张表，以及更后面的对象。读到非零值就当控制块做 `lock cmpxchg [p+8]`，相当于改写别处的内存。8 人以上已经可能出问题，1024 人时会读到对象外 24 KiB 的地方。
- 修法（`BvmPlayerTableHooks`，随 Extend=1 安装）：这 4 处 `mov rdx, [entry+0x178]` 换成 handler，下标在 0..3 之外（包括负数）就返回空。这与游戏读到一个未用条目的效果完全一样，后面走它自己的「空」分支。为什么不把表扩到 1024：表里只放本机的分屏玩家，在线玩家本来就不在这张表里。真正的问题是游戏拿在线人数去遍历一张分屏表，所以限住读取就是根因修复。
- 单测在 `mission_test.cpp`（4 项内的读取与游戏一致；越界和负数返回空且不碰内存）；`tests.cpp` 守卫表的构造参数和 4 个读取点的原字节。

### 1.4 还没处理或没法处理的

- ❌ **EOS P2P 连接数 / 星型拓扑**：游戏里每个客人只连房主，房主要维持 1023 条 P2P 链路，每 90 ms 一次的包控制器要轮询 1024 个会话。会话本身每项大小没测过。这是 W1 网状拓扑和发送调度要解决的事；W6 只保证槽位和会话够数。
- ⚠️ **语音聊天 HUD**：每个成员写一条记录，1024×0x50 = 80 KiB，够用。但超过 16 人的房间本来就没有 Epic 语音房间（`kVoiceRoomMaxPlayers`）。
- ⚠️ **房间画面**：每页 4 人，1024 人就是 256 页。功能不受影响，只是翻页没法用。
- ❌ **引擎渲染 / 模拟 1024 个士兵**：每台机器都要为每个玩家创建一个 SoldierBase 并跑完整物理（online-re §1）。1024 个玩家对象的 CPU、显存、动画、HUD 名字标签能不能撑住，只有真机能回答；预计是硬性瓶颈，与网络无关。
- ❌ **出生点**：环形布局只解决重叠；任务地图上的出生区域放不下这么多人时可能出现在墙里。待真机看。

## 2. 插件自己的表

| 位置 | 原来 | 现在 |
|---|---|---|
| `mission.cpp` sidecars / sidecarItems / remoteFlags | 按 kMaxPlayers | 1024（约 220 KiB 静态） |
| `mission.cpp` LogOnce 位 | 一个 64 位掩码（16 + kMaxPlayers ≤ 64 的 static_assert） | 改成 `atomic<uint64_t>[]`，按位数分段 |
| `mission.cpp` AreaFactorCallHandler | 栈上 2×kMaxPlayers 个 u64 | thread_local |
| `hud.cpp` / `hudcolours.h` | kMaxPlayers ≤ kHudColourCount | 表固定 32 项，下标取模 |
| `roomview.cpp` | `size > 64` 判定「不是成员 vector」 | 改成 `> kMaxPlayers` |
| `fakemembers.cpp` | 截到 kMaxPlayers | 同（1024） |
| `lobbystate.cpp` | CapacityToKeep 只认 5..32 的 MaxMembers | 先认 `MS_ROOMSIZE`，其次 MaxMembers（2..1024）；2..4 人的 MultiSlot 房间靠 SEARCH_TYPE 和 `MS_ROOMSIZE` 判定类型 |
| `rooms.cpp` | 容量 ≤ 64 | 64 人满员的大厅且公布了更大的 `MS_ROOMSIZE` 时，用公布值 |
| `hostmode.*` | F2：8/10/12/16/24/32 | INI 可填 2..1024；F2 循环 8/10/12/16/24/32/48/64/128/256/512/1024；大厅容量用 `EosLobbyCapacity` / `SteamLobbyCapacity` 截断 |
| `spawn.cpp` | 系数算到 kMaxPlayers | 算到 32（`kEnemyScalePlayers`） |
| `packetfit.*`（W1 的文件，没改） | kRecordStoreEntries = 4×kMaxPlayers，kBatchRecords / kMaxStubsPerPacket = kMaxPlayers | 跟着常量变大（约 1 MiB 静态）。单测 `packetfit_test.cpp` 里的填充记录原来只有 256 种，填不满新的 4096 项，只改了这一行测试 |
| `joinlog.h` / `peertimeout.cpp` / `netlog.cpp` | 按 kMaxPlayers 的环形缓冲 | 跟着变大，没有固定上限问题 |
| `code.cpp` ThunkPage | 16 KiB | **64 KiB**。改动前一次完整安装已经用到 16296 / 16384 字节，再加 4 个 BVM hook 就放不下，插件整个拒绝打补丁（"compare cave ... out of reach"）。其它工作流新增 hook 也会撞到这个上限 |

## 3. SEARCH_TYPE 家族

`kSearchTypeCenter = 0x53`，公布的值是 0x12..0x15。约束有三条：解码改写里的 `0x90 - 最低镜像值` 必须 < 0x80，也就是中心值 > 0x52；要低于上一个家族 0x18，也就是中心值 ≤ 0x54；0x54 落在 upstream 的 4 步网格上不能用。所以 **0x53 是这套方案的最后一个家族**。以后再改人数，就要换成另一种 SEARCH_TYPE 编码（比如 bit 标志加属性）。

旧版本（2.3.0..2.4.x，32 槽）列不出也进不了新房间。新版本照样能列出普通房间。

**协议版本门（W1 的 `netfeature.h`）**：如果 W1 也要靠换 SEARCH_TYPE 家族来做版本门，会和这里冲突。建议版本门走成员属性（syncmarker 的机制），SEARCH_TYPE 只用来区分人数能力。

## 4. 超过 64 人：成员资格

- 房主建 EOS 大厅时用 min(size, 64)，Steam 大厅用 min(size, 250)；每次房间更新由大厅所有者加上 `MS_ROOMSIZE`（`lobbystate.cpp` AddRoomSize，只有所有者加，否则 EOS 会拒掉整个更新）。
- 房间列表的容量读 `MS_ROOMSIZE`。读属性走 EDF.dll 当前的导入表项（`rooms.cpp` GameImport），所以直连部分伪造的大厅句柄也能被正确识别。
- **加入满员大厅、房主放人**：W1 已接好（57c92f6）。房主把直连地址和身份公布成大厅属性；Epic 拒绝加入时客人改走直连；`roomCapacity()` 用 `CurrentLobbyCapacity()`。

### 4.1 游戏怎样认到大厅外的成员（逆向结论）

- 成员表的唯一来源是 eos::User 集合。12BD460 是成员同步：`EOS_Lobby_CopyLobbyDetailsHandle` 拿到大厅副本，`GetMemberCount`/`GetMemberByIndex` 逐个读成员；没有 User 的就用 Users::Add（12B7F50）新建，并打上「在房间里」标志（User+0x10 bit 2）。游戏收到成员状态通知（12B3380 注册的处理函数）时会走到这里。
- 房间成员列表 7468C0（房间画面、语音 HUD）遍历全部 User，过滤条件是 12BE510 → 12AC6F0 读的 bit 2。开局同步的 GameImpl 列的也是这些 User。PlayerInfo 不用另外造：只要 12BD460 读到某个成员，房间画面、HUD 记录和开局同步记录就都有它。
- W1 的 `hookDetailsMemberCount`/`MemberByIndex`（`extraMembers`）在本房间的大厅副本上补上 RoomView 里 Epic 没列的成员，`tellGame` 再发 JOINED 通知。所以 W1 已经让游戏认到这些成员，W6 没有重复造。
- **成员编号就是 eos::Users 的槽位，不会紧凑**（逆向核实）：
  - Users::Add（12B7F50）在 12B802E–12B806E 的循环里找第一个空槽，结果存在 ecx 和 [rbp]，User 的构造函数（12B7610）拿到 &[rbp]；
  - Users::Remove（12B87C0）只把 slots[User+0x40] 置空，其余成员不动，下一个加入的人就落进这个空槽；
  - 这个编号（User+0x40，网络序号）写进游戏的包里，开局同步时每台机器也会写上自己的编号（gamenet 实测：只交换两个客人的编号，第 2 条记录就丢了）。
- 所以要求的是每台游戏里每个成员都占同一个槽位，光有同一个添加顺序不够：
  - 例子：房主上 A 离开后 X 补进 A 空出的槽位 1。X 按 Epic 列表的顺序（房主、B、X）添加，会把自己放到槽位 2，和房主对不上。
  - 不经插件的原版游戏也有同样的缺陷：原版也是按 Epic 列表顺序添加成员。
- 做法：房主游戏的槽位表是唯一的依据。
  - 采集：`multislot/src/userslots.*` 在 Users::Add 选槽处（12B806E）和 Users::Remove 置空处（12B8A24）各挂一个 hook，按房间记录本机游戏的槽位表。
  - 下发：房主的 `Room` 列表直接发这张表，下标就是槽位，`""` 表示空槽（`eos_hooks.cpp` hostRoomTick）。
  - 客人：跟随房主槽位（`RoomView::slotted`）以后，Users::Add 把成员放进房主给它的槽位（`ChooseUserSlot`，要求这个槽位在本机是空的）。
  - 没进房主槽位表的成员先不交给游戏：大厅副本只列房主槽位表里的成员，按槽位顺序（`slottedMembers`）；Epic 发来的这类成员的 JOINED 先扣住（`admitStatus`），等房主的游戏有了它，再由 followHost 补发。
  - 通过 Epic 加入一个房主开了直连的房间（大厅上有 `EDF6DN_HOSTADDR`）：游戏的 JoinLobby 完成回调要等房主槽位到了再发（`lobbyEnteredWrapper` 先挂起，`parkedEntryTick` 处理），游戏一进房就按房主槽位添加全部成员（`RoomView::adoptHost`）。最多等 10 s，超时就按 Epic 列表进房。
  - 房主迁移：新房主发的是它自己游戏的槽位表。这张表本来就跟旧房主一致，所以沿用了旧序列。
- 还没覆盖的：
  - 和房主没有直连的成员拿不到槽位表，按原版方式添加；
  - 房主刚建房、大厅属性还没发布时就进来的人不会被挂起。这时房主的槽位还没有空洞，也没有大厅外成员，Epic 顺序和槽位一致，所以不受影响。

### 4.2 人数显示

- 房间列表一项的人数和容量由 73B5CC 改成 `RoomCountAndCapacity`，房主的 HIDDEN 判定由 78BDE2 改成 `RoomFullCount`。纯判定函数是 `rooms.cpp` 的 `RoomCountFromInfo`。
- 大于 EOS 大厅的房间（大厅满编 64 且 `MS_ROOMSIZE` 更大）：
  - 人数取游戏读到的成员数（本房间包括大厅外成员）和房主公布的 `MS_MEMBERS` 中较大的一个，且不少于 Epic 的人数；
  - 原来 `members + available == max` 的一致性检查遇到大厅外成员就会判不一致，退回容量 4，房主的房间会被当成已满而隐藏。这个问题已修。
- 房主每次房间更新都会写 `MS_MEMBERS`（`lobbystate.cpp` AddRoomSize）。值是游戏通过导入表读到的本房间成员数（`rooms.cpp` GameRoomMemberCount，包括 W1 补上的成员）。
- 待真机核实：78BB60 只在房间对象的脏标志（+0x88）置位时才运行。大厅外成员加入时游戏是否会置这个标志，静态逆向没追完，所以 `MS_MEMBERS` 的刷新时机还要实测。

## 5. 带宽：兴趣管理（`multislot/src/interest.*`）

按用户纠正后的设计：**由带宽上限驱动，不按距离写死频率**。

- 每个（观察者，被观察者）有一个累加优先级。每个 tick 加上「相关性 × 距上个 tick 的毫秒数」。
- 相关性 `Relevance(observer, subject)` 是几个系数相乘：距离（60 m 处为 1/2，再远按平方衰减）× 在视野 60° 内 ×2 × 交战中 ×4 × 队友 ×1.5，最低 0.01，保证再远也会累加。
- `PickSendsThisTick(observer, subjects, budgetBytes, nowMs)` 的取法：先发超过最大间隔（默认 1 s）的，不受预算限制；再发从没发过的；然后按累加值从高到低，装进本 tick 的预算为止。装不下的跳过，后面更小的还可以继续装。发出去的累加值清零，跳过的不丢，下次发最新状态。
- 本 tick 的预算：`TickBudgetBytes(LinkBudgetBytesPerSec(peer), tickMs)`。`LinkBudgetBytesPerSec` 现在是桩，固定返回 64 KiB/s；W1 用 `SetLinkBudgetSource(fn)` 换成实测估计（拥塞控制 / RTT / 丢包）。
- 单测（`InterestManagement`，24 项）：带宽充足时每 tick 全员发；带宽减半时近处、交战中的接近全频，远处降频但不会饿死，最长间隔不超过 1 s，频率随距离单调下降；带宽恢复后回到全频；预算为 0 时每个对象仍每秒至少发一次；1023 个对象、64 KiB/s、90 ms tick、每次 40 字节时，1 秒内全部发到，且每 tick 不超预算。
- **注意下限成本**：最大间隔保证意味着每条链路至少要有「对象数 × 字节数 / 间隔」的带宽。1023 人 × 140 字节 × 1 次/秒 ≈ 143 KB/s。所以远处的对象需要 W2 提供紧凑更新（只有位置，约 40 字节），或者大房间把间隔调长（构造参数 `Scheduler(maxIntervalMs)`）。

接入点：

- **W1 发送调度**：每个对端一个 `Scheduler` 实例（或者一个实例按观察者区分）。每个发送 tick 对每个对端调用一次 `PickSendsThisTick`，只为返回的对象打包状态。对端断开时调用 `Forget(peer)`。
- **W2 玩家同步**：提供 `Subject`（id = 玩家下标、位置、队伍、交战标志、本次更新的字节数）和 `Observer`（对端玩家的位置、朝向、队伍）。远处对象用紧凑格式时，把 `bytes` 填成紧凑格式的大小。

## 6. 开局同步：需要 W1 的通用分片

实测（gamenet）：32 人时开局消息里 29 条记录用 21 字节的占位代替，消息共 1037 字节（上限 1100）。占位本身也占消息空间，所以**大约 50 人时消息只装占位就放不下了**，游戏的 0x5E0 字节流也有同样的上限。

对 W1 的要求：

1. 记录多到占位也放不下时，开局消息里只写头部、人数，加一个「全部记录在旁路」的标记。MissionSync_Update 的读取已经被重定向到插件（`PacketFitCalls` 790873），它按读取顺序从旁路取每一条记录，所以游戏的流里不需要每人一个占位。
2. 旁路数据（1024 × 约 140 字节 ≈ 140 KiB）走通用分片，并要求可靠、有序或能重组、能并发发给每个成员（星型拓扑下房主要发 1023 份，这正是网状拓扑和中继的意义）。
3. 记录仓库 `kRecordStoreEntries` 已经是 4×1024 项。等待超时（`kRecordWaitMs`）要按数据量放宽：140 KiB 在 64 KiB/s 链路上要 2 秒以上。

## 7. 测试与验证

- 新增单测：`WideCompares`（代码洞在进程内实际执行：寄存器操作数和内存操作数、短跳和近跳、无符号比较、位置无关）、`InterestManagement`；扩充了 `MissionSlots`（BVM 表、HUD 取模、出生环）、`LobbyStateAndGhostMembers`（`MS_ROOMSIZE`、2..4 人房间、只有所有者发布）、`HostModeAndMenuLabel`（2..1024、大厅容量截断）、`PatchTablesMatchEDF`（每个代码洞的原字节、BVM 守卫、FindPlayerIndex 守卫）、`PluginLoad_*`（每个代码洞都指向与 `WideCompareCode` 逐字节一致的只读可执行页）。
- gamenet：`GameNet_mission16`、`GameNet_mission32` 通过（2.8 s / 3.2 s）。32 台运行时可用内存只降了不到 0.3 GB，内存不是瓶颈；限制在于开局消息里的占位列表（§6）。`kMaxMachines` 现在是 32。
- **只能真机验证的**：1024 槽位下的帧时间（包控制器每 90 ms 轮询 1024 个会话、脚本每帧遍历 1024 个玩家条目）；BVM 越界修复对相关脚本操作码的实际效果；33 号以后的出生环在真实地图上的位置；HUD 颜色取模的显示；64 人以上的完整加入流程（依赖 §4 的 W1 接线）；引擎能否承受几十到上千个玩家对象。
