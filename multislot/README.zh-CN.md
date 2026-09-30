# EDF6MultiSlot

[English](README.md) | **中文**

地球防卫军 6（PC / Steam）联机最多 **8 人**，另有 **10 人版、12 人版**。以 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 插件形式运行。

原作者 **momotori01**，最初随 [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) 发布（公有领域，见 [LICENSE](LICENSE)），这个目录保留了它的提交历史，在 EDF6MultiSlot 1.5.12 的基础上继续。

给玩家看的说明书（日文）是 [packaging/README_EDF6MultiSlot.txt](packaging/README_EDF6MultiSlot.txt)，会放进安装包。

## 这里的改动（1.5.14）

- **拒绝加载后崩溃（1.5.13 回归）：** `Enabled=0`，或因游戏更新、其他 MOD 冲突而插件什么都不改就退出时，约 0.2 秒后游戏崩溃——写日志的线程活得比已卸载的 DLL 还久。现在只有插件确定留下后才启动写线程，卸载记为 `UNLOADED` 而不是 `SHUTDOWN`。
- **超过本版人数的房间：** 成员列表截断到本版人数，更大的房间（其他 MOD 或恶意房主）不再能让所有人一起崩溃。
- **强制结束也不丢日志：** 日志行先写进内存映射的 `EDF6MultiSlot.log.queue`，下次启动补回，任务管理器结束或 `__fastfail` 不再丢最后几行。成员名不能再伪造日志行，长的中日文名字不再显示成 `(no name)`。
- **复制护甲：** 拾取次数最多 24 步算出，其他成员报出的离谱数值会被忽略，不再能让游戏卡死。
- **崩溃报告：** 跳板和调用桩注册了 unwind 信息（栈回溯和异常能穿过 hook），插件自己受保护的探测不再算作崩溃，插件内部的故障也会写 dump。
- **CI：** `.github/workflows/multislot.yml` 构建 8/10/12 人版，跑所有不需要游戏的测试（`-DMULTISLOT_CI=ON`，使用带标记的占位布局，`package.ps1` 会拒绝打包）。

依赖游戏本体的测试（`PatchTablesMatchEDF`、`PluginLoad_*`）在这个版本上还没跑过；打包前请在装有游戏的机器上跑 `build.cmd 8/10/12`。

## 1.5.13

- **减少卡顿**：写日志不再在游戏线程上逐行打开、写入、关闭文件，改为先放进队列、由写线程批量写出；崩溃报告和退出时的最后一行仍当场写。
- **减少 EOS 开销**：`NetLog=1` 时只对会写进日志的 7 个类别（大厅、P2P、语音等）提高 EOS SDK 日志级别，不再全部提高；日志内容不变。
- **10 人版、12 人版**：各自是独立的房间族（`SEARCH_TYPE` 中心值 0x6E、0x6A），只有同版本能看到、能加入；8 人版不变，仍和 1.5.5 以后的 8 人版互通。大房间版关闭时房间列表只显示普通房，找 10/12 人房要先按 F2 打开。敌人数量到 8 人（1.8 倍）为止。
- **8 人以上房间的修复**：用户槽诊断原来只放得下 8 个，大房间里 `HandshakeRecovery` 会静默失效。

**9 人以上的联机还没有在真机上测过。**

## 构建

需要 Windows x64、带「使用 C++ 的桌面开发」的 Visual Studio 2019/2022（用它自带的 CMake 和 Ninja）、Python 3.10+（`pip install numpy pillow pefile==2024.8.26 capstone==5.0.9`），以及已安装的地球防卫军 6（EDF.dll 版本 `678CCB46`）。

```bat
set EDF6_GAME_DIR=C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6
build.cmd          & rem 8 人  -> dist\
build.cmd 10       & rem 10 人 -> dist-10p\
build.cmd 12       & rem 12 人 -> dist-12p\
powershell -ExecutionPolicy Bypass -File package.ps1 -Players 10
```

- 游戏目录只读：`Root.cpk` 用来生成带「8Player MOD」标签的菜单框（`tools/make_menu_label.py` 写出 `assets/LYT_MAINFRAME.SGO`，它来自游戏文件，所以永远不入库）；`EDF.dll` 用于把每个补丁点和游戏代码逐一核对的测试，没有 `EDF.dll` 时这些测试会跳过。
- `build.cmd` 会下载官方 EDFModLoader v1.0.10 的 `winmm.dll`（按 SHA-256 校验，`tools/fetch_modloader.ps1`），再生成安装包里那个修过竞争问题的加载器（`tools/fix_winmm_proxy.py`）。
- 没有 CI 构建：游戏文件不能放到构建服务器上。

安装包输出在 `release\EDF6MultiSlot-<版本>[-10p|-12p].zip`。

## 许可

EDF6MultiSlot：公有领域（[Unlicense](LICENSE)）。附带的 EDFModLoader（`third_party/EDFModLoader` 以及安装包里的加载器）为 MIT，版权归 BlueAmulet：[third_party/EDFModLoader/LICENSE.txt](third_party/EDFModLoader/LICENSE.txt)。
