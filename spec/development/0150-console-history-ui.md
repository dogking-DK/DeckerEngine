---
id: "0150"
created_at: "2026-11-03T11:35:01+08:00"
updated_at: "2026-11-03T11:57:47+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
---

# 0150 M22.3 Console 历史与界面交付

## 目标与设计依据

完成 [M22.3](../roadmap.md#m22console-命令文本输入首期)，依据 [Console 设计](../design/editor-console.md)。
基线 9b35943，工作区干净。实现有界历史/草稿回填、清晰提交反馈、粘贴完整性与真实后台 GUI 验收。
不新增 Runtime 命令、依赖、协议或自动 guard；不扩展 ReSTIR/相机控制。

## 实际变更

- [ConsoleHistory](../../engine/editor/src/ConsoleHistory.cpp)：最多 64 条、每条 8192 UTF-8 字节的会话历史，
  非空提交尝试（包括失败）、首尾空白裁剪、相邻去重、Up/Down 边界与草稿恢复；编辑退出浏览。
- [ConsoleInput](../../engine/editor/src/ConsoleInput.cpp)：固定缓冲、字符字节限额、剪贴板整段预检与选区替换预算。
  拒绝多行/非法 UTF-8/超限粘贴并保留原输入；读写回调按原 user data 转发，RAII 恢复本 ImGui context。
- [WorkbenchUi](../../engine/editor/src/WorkbenchUi.cpp)：命名参数提示、命令回显、Succeeded/Accepted/错误区分，
  Enter/Run 共用一次提交、成功清空/失败保留与再聚焦；SDL 组合事件保护历史/提交，失焦恢复。
  输入法保护维持至当帧 Run 处理结束；日志最多 64 条、每条最多 4096 字节，不截断 UTF-8 字符。
- [ConsoleHistoryTests](../../tests/unit/ConsoleHistoryTests.cpp) 新增 4 项 CPU 用例；
  [ConsoleSmoke](../../engine/editor/src/ConsoleSmoke.cpp) 扩展既有真实工作台输入流程，共新增 48 步，未增加测试 target/CLI。
  测试剪贴板回调与操作系统剪贴板隔离，异常/正常结束均恢复。
- 同步 Editor/Console 设计、索引、Roadmap、[指南](../guides/editor.md#console-命令文本m22)、
  [命令参考](../commands/README.md) 和[测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)。

## 验证记录

Windows / VS2026 / windows-editor Debug，NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49。
构建目录按已有固定 baseline 同步依赖缓存；没有修改依赖版本或清单。定向命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
    -Target @('dk_editor_tests','dk_editor_app') `
    -TestRegex '^dk\.editor\.(console |workspace console|workbench_gpu_validation$)' `
    -Reason 'M22.3 Console history, input integrity and real background UI acceptance'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
    -Target @('dk_editor_app') -TestRegex '^dk\.editor\.workbench_gpu_validation$' `
    -Reason 'M22.3 preserve composition guard through Run processing on focus loss; final background UI verification'
```

结果分开记录：

- `out/verify/20261103-114652-1e4017d5`：构建成功，17 项 CPU 检查全部通过；GUI 最后一项空白输入断言失败，
  合计 17 通过、1 失败、0 跳过。原因是测试将 Tab 放入字符队列，而 ImGui 忽略该字符并从按键事件读取 Tab；
  改为真实 Tab key 注入，保留检查空白原文/不提交/不追加历史。
- `out/verify/20261103-115338-9eef7d0d`：修正测试后 GUI 1/1 通过，0 失败、0 跳过。
- `out/verify/20261103-115501-b67fc20e`：补齐当帧失焦与 Run 的组合保护顺序后，最终 GUI 1/1 通过，0 失败、0 跳过。
  验证后的源代码未再修改。CPU 模型/解析/分派未在首次 CPU 通过后改变，无需重复运行其 17 项检查。

最终按范围合计 17 项 CPU 与 1 项真实后台 GUI 均通过。GUI 原工作台 26 步与 Console 新增 48 步全部通过，
覆盖文本/旧 JSON、中文/引号/空格编辑、历史只回填/草稿/编辑脱离、成功清空/失败保留、Enter 与 Run 重叠一次提交、
完整 guard 回填后冲突、Undo、8192/8193 字节与中文超限、选区替换/组合超限、多行拒绝、SDL 组合/提交与空白忽略。
整个流程持续检查 revision、实体数量、历史数量和预期错误数；工作台保存后的磁盘内容与截图检查通过。

最终程序输出：`workbench frames=605 revision=6 draws=3 ... smoke=passed`，
`validation errors=0 warnings=1 liveAllocations=0 knownLoaderWarnings=1`。
唯一运行告警为既有 `VK_LAYER_AMD_switchable_graphics` API 1.3/应用 1.4 Loader 告警，沿既有 smoke 策略记录接受；
没有验证错误或新增验证告警。首次构建有未修改的 RenderOperations.cpp:103 的 C4456 警告，本阶段源文件无编译警告。
最终 `summary.json`、build.log、tests.log、results.xml 与额外保留的 LastTest.log 位于上述最终证据目录。
截图与场景位于 `out/build/windows-editor/test-artifacts/Debug/editor-1da7bbf1186b49a39c1480b077153e10`；
已转换 PNG 并目视确认 Console 输入/Run、日志和工作台布局。

`scripts/check-spec.ps1 -Path` 检查本次 9 份 Markdown、849 个本地链接及元数据/表格/索引/测试入口/JSON manifests，通过；
`git diff --check` 通过。未运行全量/Release 双配置、其他平台、完整渲染或 IPC/Python/Luau 进程矩阵。
组合保护通过 SDL 事件注入验收，未逐个人工核验操作系统输入法候选窗口。

## 偏差与决策

先同步剪贴板整段预检、固定缓冲和 RAII 回调恢复、SDL 组合输入、测试剪贴板隔离设计；
不允许截断后执行粘贴前缀，历史不修改 guard 或自动提交。

## 遗留问题与下一步

M22.3 无剩余实现项，M22 首期完成。补全/schema 提示、位置参数别名、自动 guard、持久历史、脚本语言及
`restir on` / `raytracing iterate 12` / `cam.orbit 15 20` 仍需独立后续设计；M20/M21 不因本路线完成而推进。

## 修改记录

- 2026-11-03T11:35:01+08:00：建立 M22.3 记录并细化 UI/粘贴与 GUI 验收契约。
- 2026-11-03T11:57:00+08:00：17 项 CPU 与最终后台 GUI 验收通过，记录中间失败与修正，更新 M22 完成状态。
- 2026-11-03T11:57:47+08:00：9 份文档、849 个本地链接及差异检查通过，准备独立本地提交。
