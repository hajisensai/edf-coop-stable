# 已合并 PR 的后续分支更新

基线：`origin/main` 的 `9761a17`。仅整合已合并 PR 原分支上尚未进入 main 的后续提交。

## 纳入范围

- PR #21：`69a5eee`、`d5cd321`、`b85f3ec`。HostData 默认改为 Ask；菜单上的独立窗口先说明文件来源、数量、大小和选择影响，确认后才开始下载；Auto 保持自动下载，Never 不下载；发布 `EDF6CO_HDS` 大小元数据，旧版本没有元数据时显示未知。旧配置 Always 按 Ask 读取。
- PR #3：`8df722e`。三语言 README 补充 MultiSlot 原作者的致谢及项目链接。

## 整合与修复

- CMake 冲突保留 main 的完整 netcode 源列表，仅追加 `hostdataprompt.cpp`；README 保留现行单 DLL、32 人房间、安装升级与许可证文件路径的说明，补充致谢段落。
- `HostDataLink::Tick()` 的每个 peer 公平调度保持不变；`UnreachableAskerDoesNotBlockOthers` 继续覆盖其他 peer 完成和不可达 peer 恢复发送。
- 修复新增窗口的陈旧回答：原实现只比较 lobby ID，离开后重进同一房间时旧窗口仍能授权新会话；F1 的更新选择也会被旧窗口答案覆盖。`HostDataAnswers` 在房间变化和 F1 切换时递增问题代次并清空待处理回答，旧窗口回调不再生效，当前回答仍只在游戏菜单线程应用。
- 回归覆盖：窗口未回答时不产生授权、旧会话回调不改变新问题、排队回答阻止重复提问、只提交本次提问的 bundle、重复回调、F1 更新决定、离房清理队列，以及有效 Yes/No。

## 本地验证

- Windows MSVC/Ninja、RelWithDebInfo、`MULTISLOT_CI=OFF`，`CMAKE_BUILD_PARALLEL_LEVEL=3`：完整构建成功。真实游戏资源和 `EDF.dll` 仅只读使用。
- `ctest --test-dir multislot/build --output-on-failure --no-tests=error -R 'HostData|DefaultIni|ModFiles|PluginLoad|PluginRefusal|SyncMarker|Handshake|Lobby'`：18/18 通过，0 跳过，包括 10 种插件加载配置；27.82 秒。
- `git diff --check` 通过。

本轮没有执行真实双机联机、游戏内弹窗手工操作或远端 CI；未重跑与该变更无关的完整 DirectNet 套件。
