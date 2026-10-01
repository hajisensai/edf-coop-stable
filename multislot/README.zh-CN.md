# EDF6MultiSlot

[English](README.md) | **中文**

地球防卫军 6（PC / Steam）联机最多 **8 人**，另有 **10 人版、12 人版**。以 [EDFModLoader](https://github.com/BlueAmulet/EDFModLoader) 插件形式运行。

原作者 **momotori01**，最初随 [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) 发布（公有领域，见 [LICENSE](LICENSE)），这个目录保留了它的提交历史，在 EDF6MultiSlot 1.5.12 的基础上继续。

给玩家看的说明书（日文）是 [packaging/README_EDF6MultiSlot.txt](packaging/README_EDF6MultiSlot.txt)，会放进安装包。

各版本的改动见 [release-notes/](release-notes/)。

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
