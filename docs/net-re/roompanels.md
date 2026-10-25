# 房间画面成员面板：不分页，按人数自动排行

EDF.dll Steam 版 TimeDateStamp `0x678CCB46`，地址为 RVA。只做了静态反汇编和 Root.cpk 布局数据读取，没有开游戏验证。
【证实】= 反汇编 / 数据里直接读到，且 `multislot/tests/tests.cpp`（`CheckRoomPanels`，对着真实 EDF.dll 跑）逐字节核对；【推断】= 需要实机确认。

代码：`multislot/src/patches.cpp`（`RoomPanelPatches`、`RoomViewCalls`）、`multislot/src/roomview.cpp`（排行与阵营标签）、
`multislot/src/roomrows.h`（排行规则，纯算术，`tests/roomrows_test.cpp`）。

## 1. 4 个的上限从哪来，怎么去掉

面板构建函数 `0x8F8020`（调用点只有 `0x8FBABE` 重建、`0x8FFE45` 刷新）在自己栈上放了 4 个模板名指针
`[rbp+0xA0..0xB8] = {L"Button_Master", L"Button_Member" ×3}`，再 `for (i = 0; i < members.size; ++i) 0x845BB0(room, table[i])`，
每轮 `rbx += 8`。第 5 个成员读到的是栈 cookie。【证实】

补丁（安装时整段原字节校验，`original` 覆盖整条指令序列，只改其中一条）：

| RVA | 原指令 | 新指令 |
|---|---|---|
| `0x8F807E` | `48 89 85 B8 00 00 00` `mov [rbp+0xB8], rax` | `48 89 45 28 0F 1F 00` `mov [rbp+0x28], rax; nop dword [rax]` |
| `0x8F841D` | `48 8D 5B 08` `lea rbx, [rbx+8]` | `48 8D 5D 28` `lea rbx, [rbp+0x28]` |

第 0 轮读 `[rbp+0xA0]`（Master），之后每轮读 `[rbp+0x28]`（Member），N 不限。`rbp+0x28` 在帧内（`lea rbp,[rsp-0xD0]; sub rsp,0x1D0`，
局部区 rbp-0x100..rbp+0xD0），整个函数（到 `0x8F8A33 ret`）没有别的指令访问它（capstone 全函数反汇编 + 测试里的字节扫描）；
`rbx` 是被调方保存寄存器，`0x845BB0` 不会改它。【证实】

面板数 = 成员数以后，下面这些都按动态长度走，不需要原来的分页和 `UpdateVoiceIconsHook`：填充 `0x902030`（按面板数循环，
面板 +0x200 = members[i]）、语音图标 `0x903160`（按面板数循环，`0x903995 cmp rsi,[room+0x910]` 用成员数做上界）、焦点恢复
`0x8FAF70`（按成员数索引焦点表——面板少于成员时才越界）、面板弱引用表 `room+0xB50`（会扩容）。【证实】

## 2. 排行（`roomrows.h`）

PlayersGroup 是 (1283,160) 506×780；Master 455×210、Member 455×188，flags 8 = 接在前一个兄弟下面，只在创建时算一次，结果就是
组件局部矩阵里的平移（+0x1A0/+0x1A4）。4 个刚好 774 高，第 5 个起掉出组外。【证实（数据）】

| 人数 | 行 | 每行内容 |
|---|---|---|
| 1–4 | 游戏原样 | 全部（有阵营标签时只把通关率换成标签） |
| 5–8 | 一列，高 92–120 | 第一行兵种 / 名字 / 语音；第二行护甲、阵营标签、状态框（缩到 0.4） |
| 9–17 | 一列，高 40–60 | 兵种、名字、阵营标签（没有标签时显示护甲）、语音 |
| 18–32 | 两列各宽 250，高 45–60 | 名字在上、阵营标签在下，语音 |
| 33+ | 只显示前 32 个 | 其余面板隐藏（+0x1F8=0）并 `SetEnabled(false)`，不画、不进焦点 |

上限 32（`rows::kMaxRows`）：两列 16 行是 45 高；再多一行是 42，第三列只有 165 宽，都放不下 16 号字。Master 永远是第一行左上，
其余按游戏的顺序先排满第一列再排第二列。

每次构建后（两处调用都经过 `BuildPanelsHook`：先调原函数，再 `LayoutRows`）：
- 面板：`0x7E7FA0 HUiButton::SetArea(panel, {0,0,w,h})`（游戏自己在 `0x80F588` 运行时改按钮尺寸就是这么调的，紧接着直接写
  +0x1A0）【证实】，再写平移 +0x1A0/+0x1A4。
- 子元素按序号（+0x110，布局树里在父节点下的位置）处理，遍历面板的两个子项链表 +0x138 / +0x158（`0x8E1950` 也是两个都找）：
  0 兵种、1 名字、2 护甲标题、3 护甲、4 通关率标题、5 通关率、6–11 武器、12 状态框；Master 13 是房主标记、14–17 语音图标，
  Member 13–16 是语音图标。【证实（布局树）】移动写 +0x1A0/+0x1A4，文字框改宽 / 高写 area +0x1B8/+0x1BC，缩放写 +0x170/+0x184，
  隐藏写 +0x1F8=0。语音图标、房主标记、状态框只移动、不改显隐（语音图标每帧由游戏切换）。

## 3. 阵营标签

All Forces 的 `EDF6AF_PvpMemberLabel(memberId, out, chars, &argb)`（EDF6VehicleCrew.dll 导出，UI 线程；每个房间画面
`GetProcAddress` 一次）。有这个导出时，每个显示的行在构建后用 `0x8456D0 CreateByName(room, &out, &panelCopy, L"_pl_Name")`
新建一个 TextField 挂在面板下（`_pl_Name` 是 LYT_HUIONLINEROOM.SGO 的顶层定义；父项副本被调方消费，先给 uses+1；返回的强引用
立即释放，标签随面板一起销毁）。插件记住标签指针，排子元素时跳过它，不写它的 +0x110。

成员 id：面板 +0x200 = MemberInfo（`0x902328`）；MemberInfo +0x98/+0xA0 = `weak_ptr<eos::RoomInfo::PlayerInfo>`（`0x901B52`）；
PlayerInfo +0/+8 = `weak_ptr<eos::User>`（`0x744B31`）；User +0x18 = `EOS_ProductUserId`（与 Users 槽位同一字段），
`EOS_ProductUserId_ToString` 得到与成员属性接口相同的文本。每个 weak_ptr 都先查控制块 uses>0。【证实（写入点）】

每 200 ms 在 `HUiRoom::OnUpdate` 钩子里刷新：返回 1 时写字（`0x7F8BB0(textField, text, nullptr)`，和填充写名字同一个函数，
对象用游戏的 `__RTDynamicCast(Component 0x2062F00 → ui::TextField 0x2062F28)` 转型，同 `0x8E1991`）、写字色 +0x2B0..+0x2BC
（RGBA float，`0x7F890C` 绘制时读），显示标签并隐藏它让位的子元素；返回 0 时隐藏标签、恢复那些子元素。

## 4. 只做了静态验证、需要实机确认的

1. 改面板的矩阵和 area 后，皮肤（Window10_RoomMember_skin 九宫格，高 40–120）、子元素是否跟着移动 / 缩放；鼠标命中与焦点高亮是否对齐。
2. PlayersGroup 的 flags 0x100（纵向列表）/ 0x800 会不会每帧重排或裁剪子项（会的话，构建后写的位置会被覆盖）。
3. TextField 改 area 宽 / 高后文字是否裁剪、竖直方向怎么对齐（两列模式的两行字各 20 高）。
4. 状态框（`_pl_Location`）缩到 0.4 是否正常显示。
5. `CreateByName(_pl_Name)` 新建的标签的初始位置、`+0x110`；`SetText` / 字色在它上面是否生效；字体里是否有标签用到的字形。
6. 隐藏并禁用的第 33 个以后的面板：焦点恢复 `0x8FAF70` 若把焦点给了其中一个（原来聚焦的成员排在 32 名以后），焦点会落在看不见的面板上。
7. 两列时上下键按子项顺序走（第一列底部再往下到第二列顶部），左右键不跨列。
8. 成员很多（几百个）时游戏一次建出全部面板的开销。
