# edf6-coop-stable

EARTH DEFENSE FORCE 6（PC / Steam）联机稳定插件 **EDF6DirectNet**，基于 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 的插件接口。

> 非官方 Mod，与 D3 PUBLISHER / SANDLOT / Epic Games 无关。只修改本机进程内的网络调用，不改动游戏文件。

## 功能

| 功能 | 默认 | 说明 |
|---|---|---|
| 防不同步 | 开 | EDF6 把所有联机数据以 `UnreliableUnordered` 发送（`EDF.dll+0x12c8bc0` 的两个调用点都传 0），丢包即丢数据。插件改为 `ReliableUnordered`：每个包恰好送达一次、可能乱序——这本就是不可靠传输允许的交付方式，游戏逻辑不受影响，只是不再丢包。只需发送方安装。同时把 EOS 收发队列扩到至少 64MB，避免队列满丢包。 |
| 断线宽限 | auto | EOS 因超时/网络错误关闭连接时，暂不通知游戏，后台 `AcceptConnection` 重连；恢复则游戏无感知，最多等 30 秒。仅对确定装了插件的对端生效（见下）。 |
| 公网直连 | 关 | 房主监听 UDP 端口（IPv4/IPv6 双栈，UPnP 自动映射），其他人填房主地址直连；星形拓扑，房主转发。可靠传输（选择确认、令牌桶重传、会话 epoch）、防 TUN 劫持（`IP_UNICAST_IF`）。 |
| 诊断日志 | 开 | NAT 类型、直连/中继、断开原因、大厅成员状态、每分钟统计 → `Mods\Plugins\EDF6DirectNet.log`。 |

### 为什么断线宽限需要双方都装

对方没装插件时，他的游戏会照常把你移出对局；你这边若继续隐瞒断线，两边状态就会分叉。
EDF6 会把收到的任何 EOS 包都当游戏数据解析（`ReceivePacket` 的 `RequestedChannel` 为 NULL，且不检查频道/Socket），所以无法在 EOS 上安全地探测对方是否装了插件。
因此默认只对公网直连成员（必然装了插件）生效；确认所有人都装了时可设 `HoldDisconnects=all`。

## 安装 / 使用

见 [dist/README_EDF6DirectNet.txt](dist/README_EDF6DirectNet.txt)（中文玩家说明）。

## 构建

需要 Visual Studio 2022（MSVC x64）。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1 -Test
```

产物：`build\EDF6DirectNet.dll`（静态 CRT，只依赖系统 DLL）与 `build\edf6_directnet_tests.exe`（单元测试 + 本机回环多节点测试，含 20%~40% 丢包、断网、重启场景；`EDF.dll` 导入表测试需要本机装有游戏，否则跳过）。

## 目录

| 路径 | 内容 |
|---|---|
| `src/eos_min.h` | 所用 EOS SDK 结构体（按官方 1.15.5 头文件，游戏为 1.16.1） |
| `src/eos_hooks.cpp` | 修改 `EDF.dll` 导入表，接管 EOS P2P / 大厅调用 |
| `src/hold.*` | 断线宽限 |
| `src/direct_net.*`, `src/reliable.*`, `src/wire.*` | 直连传输 |
| `src/netif.*`, `src/upnp.*` | 物理网卡识别、UPnP |
| `tests/` | 测试 |

## 已知限制

- 不支持任务中途加入。
- 只解决丢包造成的不同步；游戏逻辑本身的不同步需要具体症状再逆向定位。
- 尚未在真实多人联机中验证，欢迎提交带日志的 Issue。

## 许可证

MIT，见 [LICENSE](LICENSE)。
