# EDF6MultiSlot（EDF6Coop 的房间部分）

[English](README.md) | **中文**

地球防卫军 6（PC / Steam）超过 4 人的联机：8、10、12、16、24 或 32 人房间、房间画面、任务和护甲复制（「8Player MOD」）。

原作者 **momotori01**，最初随 [EARTH-DEFENSE-FORCE-6-VR-MOD](https://github.com/momotori01/EARTH-DEFENSE-FORCE-6-VR-MOD) 发布（公有领域，见 [LICENSE](LICENSE)），这个目录保留了它的提交历史。这一份从 EDF6MultiSlot 1.5.12 继续，从 2.0.0 起和直连部分（`../src/`）一起编进同一个插件 **EDF6Coop.dll**。

构建、打包、发布和玩家说明书只在仓库 [README](../README.zh-CN.md#构建) 里写一遍；随包说明书是 `../dist/README_EDF6Coop*.txt`。简单说：

```powershell
$env:EDF6_GAME_DIR = 'C:\Program Files (x86)\Steam\steamapps\common\EARTH DEFENSE FORCE 6'
powershell -ExecutionPolicy Bypass -File ..\build.ps1 -Players 8 -Test    # -> dist\EDF6Coop.dll
powershell -ExecutionPolicy Bypass -File ..\package.ps1 -Players 8        # -> ..\release\EDF6Coop-<版本>-8p.zip
```

这个目录里的 `build.cmd [8|10|12|16|24|32]` 等同于 `build.ps1 -Players <N> -Test`。

EDF6MultiSlot 1.5.x 及以前的改动见 [release-notes/](release-notes/)；2.0.0 起见 [../release-notes/](../release-notes/)。

16、24、32 人房间是 2.0.0 新增的，目前只用离线幽灵队员验证过。

## 许可

EDF6MultiSlot：公有领域（[Unlicense](LICENSE)）。EDF6Coop 其余部分为 MIT（[../LICENSE](../LICENSE)）。附带的 EDFModLoader（`third_party/EDFModLoader` 以及安装包里的加载器）为 MIT，版权归 BlueAmulet：[third_party/EDFModLoader/LICENSE.txt](third_party/EDFModLoader/LICENSE.txt)。
