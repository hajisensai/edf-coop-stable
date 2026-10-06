# 玩家（SoldierBase）网络复制逆向与 W2 实现

EDF.dll TimeDateStamp 0x678CCB46，纯静态。H=指令直读，M=有证据推断，L=推测。

## 类与 vtable（H）
SoldierBase 0x17D24D8 / AssultSoldier 0x17CDF28 / Engineer 0x17CF100 / HeavyArmor 0x17CF5B8 / PaleWing 0x17D0FF8；NetworkObject 子表在 +0x120。
- NetworkObject slot5 0x125A70 -> slot7 0x59FA80（写）；slot6 0x125180 -> slot8 0x592130（读）。
- 写：0x5781B0 计数 obj+0x1820++；mask=main slot92（0x59FB10，PW 0x582DF0 加 0x800）；u16 mask 经 0x12B54E0；字段 main slot52（0x59FDB0，PW 0x582E40）。读：slot53（0x592180，PW 0x57FCC0）。
- 玩家（slot33 0x550890 为真）mask：0x08/0x10/0x40 每次；位置 bit1 仅 cnt%6==3；朝向 bit2 cnt%6==0（+0x1E41 时每 2 次）。已用位 0x1..0x800，0x1000-0x8000 空闲。
- 位置 bit1：0x77AD00 写 obj+0x90 vec3，经 0x761B20 **三个半精度浮点**（6 字节，>512m 时步长 0.5m）。H
- 每对象 payload 以 bin 元素（0x12B5200，tag 0xA0|len）追加进批，接收按对象有界（M）。

## 调度频率（H）
- 同步循环 0x75D2E0（Room::Update 0x7370D0 调用），每帧一次（门：eos::internal_Core+0x18 帧计数，Core::Update 0x12ADB30 由 Application::Update 0x705530 每帧调）。预算 450 字节/帧减溢出。
- 包控制器 0x12CECC0：数据报每 90ms 才 flush（0x12CEA10，间隔 imm 在 0x12CB66C `41 C7 46 58 00 00 B4 42`）。所以位置 10Hz 写、90ms 成批发出。

## 远端校正（H 代码 / M 语义）
NetworkUpdate = main slot55 0x596130（PW 0x5803C0 jmp 过去）。远端（+0x128 bit0）：
- HumanCharacterSyncControl @obj+0x1830：Init 0x77AC90（快照阈值 100m、120 帧未收敛则下包瞬移）；读 0x77AB00 写目标 +0x1840、+0x1850=0/+0x1851=1。
- Update 0x77AD70：目标每帧按**副本自己模拟的速度** dead-reckon；校正速度 = 60*G*err 写入 obj+0x840（Havok 代理一次性附加速度，步末 0x11B9CBF 清零）。G 由 SoldierBase 构造 0x58E120 设为 (0.075,0,0.075) 空中 / (0.075,0.1,0.075) 地面 —— 空中竖直不校正。死区 PlayerCtrlSyncChecker 0x58EDB0：水平<0.1m 且竖直<0.15m 即停。
- 0x596386 的 0.05 只平滑 +0x1920（校正速度的平滑副本，slot63 0x570D80 用于朝向），不影响位置——这就是改它没用的根因。

## 记录大小与带宽（M，按写入函数推算）
- 标量 float（0x12B5350）：误差 ≤0.1 时 3 字节半精度，否则 0xC2+float32 共 5 字节；vec3 位置（0x761B20）恒为 bin(6)=8 字节。
- 玩家每帧记录约 50-60 字节（0x08/0x10/0x40 每帧，位置每 6 帧），约 25 kbit/s/对端。
- 新块：bin 头 2 + 31 字节 = 33 字节；默认每 2 帧一次（30 Hz）≈ 8 kbit/s/对端/玩家；SendIntervalFrames=1 时 16 kbit/s。
- flush 45ms 使每对端数据报从约 11 个/秒增至 22 个/秒，头部开销约 +5 kbit/s/对端。星型拓扑下 8 人房主中转约 42 路，块流量约 340 kbit/s，接近 320 kbit/s 估计上限——大房间建议 SendIntervalFrames=3，或等 W1 网状直连。

## 实现（feat/net-player）
- `multislot/src/netplayer.*`（无游戏依赖，单元测试覆盖）：块格式 v1 = version u8、seq u16、发送端 QPC ms u32、位置 3×f32、速度 3×f32；
  bin 元素读写（与 0x12B5200/0x12B49D0 同格式，带越界检查，读失败不移动读指针）；发送端速度 = 两次块之间的位移 / 墙钟时间（瞬移 >20m、间隔 >250ms 置零，上限 80 m/s）；
  接收端：seq 去重/乱序丢弃；时钟基线取最快样本的 本地−发送端 时间差、慢路径每样本最多上爬 0.5ms；估计 = 位置 + 速度 × 年龄（上限 200ms，停下时速度为 0 即停在原地）；
  每帧驱动：误差 > 8m 用游戏的 0x11B9850 瞬移；否则附加位移 = 误差 × (1−e^(−dt/80ms))（三轴，空中竖直也校正）+ 前馈（发送端速度 × dt − 副本自身实测位移），单步上限 60 m/s，死区 3cm。
- `multislot/src/netplayer_game.*`：20 个 vtable 槽包装 + flush 常量；安装前核对 12 处指令字节和 20 个槽位原值，任一不符整体不装、只写日志。
  - slot92：本机玩家（+0x128 bit1 且 slot33）且 obj+0x1820 % SendIntervalFrames == 0 时加 mask 0x1000；
  - slot52：原版字段后追加块；slot53：原版读完后读块，仅远端副本记样本；
  - slot55：有新鲜样本（400ms 内）的远端副本，调用前置 obj+0x1850=1、obj+0x1851=0（0x77AD70 直接返回，原版校正关闭），调用后若同步控件启用（obj+0x1830）则把附加位移 ×60 加到 obj+0x840（与原版校正同一路径）。样本过期或控件停用（乘车等）即回到原版路径。
- 版本门：`netfeature.h` 桩（[NetFeature] PlayerSync=1），W1 集成时替换。不发块的成员，其副本在所有机器上都走原版代码；收不到块的机器忽略 0x1000 位和尾部元素（每对象 payload 是独立 bin 元素，M）。
- ini：[NetPlayer] SendIntervalFrames=2、FlushIntervalMs=45（0=保持 90）、ConvergeMs=80、MaxExtrapolateMs=200、SnapDistance=8、FeedForwardPercent=100。

## hook 清单（最终）
| 地址 | 原值 | 用途 |
|---|---|---|
| 0x17CE208 0x17CF3E0 0x17CF898 0x17D27B8 | 0x59FB10 | slot92 mask（AS/Eng/HA/SB） |
| 0x17D12D8 | 0x582DF0 | slot92 mask（PaleWing） |
| 0x17CE0C8 0x17CF2A0 0x17CF758 0x17D2678 | 0x59FDB0 | slot52 写 |
| 0x17D1198 | 0x582E40 | slot52 写（PW） |
| 0x17CE0D0 0x17CF2A8 0x17CF760 0x17D2680 | 0x592180 | slot53 读 |
| 0x17D11A0 | 0x57FCC0 | slot53 读（PW） |
| 0x17CE0E0 0x17CF2B8 0x17CF770 0x17D2690 | 0x596130 | slot55 NetworkUpdate |
| 0x17D11B0 | 0x5803C0 | slot55（PW） |
| 0x12CB66C+4（imm） | 90.0f | 包控制器 flush 间隔 → 45.0f。**与 W1 传输层共享，集成时由主代理协调归属**；W1 若已改这里，本模块核对失败会整体不装 |

只读调用：0x11AF6F0（代理矩阵）、0x11B9850（代理瞬移）、vtable slot33（IsPlayer）。与 all-forces 补丁地址无重叠（已 grep）；不碰 0x596386（smoothing.cpp 的补丁位点）。

## 验证
- NetPlayerSync（45 项检查）：块往返、bin 越界、发送端速度、seq/时钟基线、外推上限；模拟副本跑步/停下/未复现的跳跃（含竖直）/抖动+副本快 30%/远距瞬移。
- GameNet_playersync：两台真 EDF.dll + EDF6Coop，双方安装日志确认 20 槽 + flush 核对通过；扩展记录经游戏控制器往返，用本插件和游戏自己的 bin 读写器交叉读，块逐字节一致。
- 全量 build.ps1 -Test：48/48 通过（含 45ms flush 下的全部 mission 场景）。

## 真机缺口（待真机验证）
- 远端副本 +0x340/+0x1ED0（slot33）是否为真不影响接收端（按收到的块驱动），但发送端要求本机玩家 slot33 为真（M）。
- +0x840 附加速度的手感、与地面碰撞/坡道/墙的交互，前馈测量在被挡住时是否推墙；Wing Diver 飞行与 Fencer 冲刺。
- slot55 在物理步之前调用（推断自 +0x840 由步末清零，M）。
- 关掉原版校正后 +0x1900=0，slot63 不再按校正方向转身（外观）；+0x1210 的翻面修正不再执行。
- 单向延迟无法测出，显示约落后一个最短单向延迟；8 人房带宽见上。
