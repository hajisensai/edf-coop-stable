# 玩家（SoldierBase）网络复制逆向（WIP）

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

## 计划中的实现（未写代码）
发送：slot92/52 包装，加 bit 0x1000 + bin 块（版本、seq、发送端 ms、全精度位置、速度），每 2 帧；flush 间隔 90→45ms。
接收：slot53 包装读块；slot55 包装：pre 置 +0x1850=1、+0x1851=0 抑制原版校正；post 用外推估计 + 时间常数收敛 + 前馈写 +0x840，误差超阈值调 0x11B9850 瞬移。版本门 NetFeatureActive(PlayerSync)。

## hook 清单（拟）
vtable slot：0x17CE0E0 0x17CF2B8 0x17CF770 0x17D2690 0x17D11B0（slot55）；0x17CE208 0x17CF3E0 0x17CF898 0x17D27B8 0x17D12D8（slot92）；0x17CE0C8 0x17CF2A0 0x17CF758 0x17D2678 0x17D1198（slot52）；0x17CE0D0 0x17CF2A8 0x17CF760 0x17D2680 0x17D11A0（slot53）；代码 0x12CB670（flush imm，与 W1 共享）。与 all-forces 补丁地址无重叠（已 grep）。
