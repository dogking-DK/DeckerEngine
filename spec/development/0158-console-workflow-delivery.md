---
id: "0158"
created_at: "2026-11-03T18:22:23+08:00"
updated_at: "2026-11-03T18:40:35+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/commands.md
---

# 0158 M22.11 Console 与 Editor 扩展集成交付

## 目标与设计依据

从干净基线 2124602 完成 M22.11。设计先补充[跨入口闭环契约](../design/editor-console.md#m2211-实施契约扩展集成闭环与交付)，
再实现真实窗口验收及显式 guard 的 Python 示例；汇总 M22 阶段证据，保留已有失败与未验证边界。

## 实际变更

- 新增 [ConsoleWorkflowSmoke](../../engine/editor/src/ConsoleWorkflowSmoke.cpp) 与 [外部集成驱动](../../tests/integration/EditorConsoleWorkflowTest.py)，
  通过 --console-workflow-smoke 接入现有一次性工程/--background/required RT 与同步验证约束。普通运行、命令语法、schema 和服务行为不变。
- 8 个检查点连接 35 次原生输入操作：IPC 发现命令/帮助，真实命令名与 renderer 枚举补全，显式 Run/Enter，Settings 中文搜索/定位，
  读取 Camera/Path 输入框实际值，GUI Apply 后旧 guard 拒绝，JSON 与命名参数产生相同相机和像素，最后通过 Python 示例设置 ReSTIR 并 Orbit。
  每个检查点等待对应当前画面，输出独立状态、输入轨迹和 PPM；外部查询核对 guard/参数/图像来源，Scene/Undo/文件与 Simulation 相机保持不变。
- 新增 [editor_settings.py](../../examples/automation/editor_settings.py)：发现/预检→显式 guard 的视口设置→用成功回执 guard 执行角度 Orbit→有界查询实际图像。
  检查 current、文档/相机来源、renderer 和绝对 spp；不自动重试写入或刷新 guard、不保存/关闭宿主。失败报告已收到成功回执的写入，不声称跨命令事务。
- 新增 [五项脚本边界测试](../../tests/integration/EditorSettingsExampleTest.py)，覆盖旧失败观察、当前失败、并发 guard、接受后超时及第二条写入失败。
  当前失败需 observed_guard 匹配本次回执；尚未被 owner 更新的旧错误不导致误报。本机真实测试另覆盖打开 Camera 弹层时示例无写入拒绝。
- [完整指南](../guides/console-workflow.md)汇总规范命令、补全/面板操作、可运行 Python 用法和能力/限制。
  同步 README、Editor/Python 指南、相机/视口命令参考、模块设计/索引和验证选择表；完成 M22.11 后关闭 M22。

## 验证记录

使用现有 windows-editor / Debug；按每轮范围构建 dk_editor_tests、dk_editor_app、dk_run、dk_ctl，构建与 CTest 串行。
所有 GUI 都使用 --background。required Vulkan/synchronization validation 开启；本机设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，
仅屏蔽已知 AMD 可选切换层版本警告，不关闭验证层或放宽断言。

| 验证目录 | 实际结果 |
| --- | --- |
| out/verify/20261103-182745-a24c12be | 17 项首期 Console/Workspace/历史 CPU 通过；原 Workbench GUI 通过，24.45s。新闭环完成前两个检查点，随后 Windows 检查点文件短暂共享冲突导致读取失败；未作为通过 |
| out/verify/20261103-183049-a65cc18a | 仅为验收确认文件增加受全程时限约束的读取轮询；新闭环全部 8 检查点通过，13.30s；没有业务命令重试 |
| out/verify/20261103-183352-e81f4e98 | 最终新闭环通过，13.36s；示例状态边界的 1 个 CPU CTest（含 5 项 unittest）通过，0.41s |

本阶段共 20 个不同 CTest 通过：17 个首期兼容 CPU + 1 个示例脚本 CPU + 2 个后台 GUI。
首期检查覆盖命名参数/兼容 JSON、精确整数、Unicode/错误定位、整行失败、输入预算、显式 guard/草稿/Undo、零等待、提交后刷新失败不重试及有界历史。
原 Workbench GUI 保留真实输入/历史/粘贴/组合输入与基础/嵌套/动态补全检查；没有重复已通过且未受后续修改影响的检查。

最终产物目录 out/build/windows-editor/test-artifacts/Debug/console-workflow-a88e69ff3f9745d7b772528f36a0ec29：
summary.json、descriptions.json、stdout/stderr、workflow.ppm 与 project/.dk-console-workflow 下的逐点 JSON/PPM。
实际 Editor 61 条、runner 49 条命令，runner 无 editor.*；列表四字段兼容，相关描述 help v1/单位/renderer 枚举核对通过。
GUI 输入框显示 FOV=35、target_spp=2；GUI Apply 为 45 后旧 guard 拒绝且状态保留；JSON 恢复 35 与命名参数画面像素一致。
示例以 2 spp、yaw=5、pitch=2 实际运行，ReSTIR 和新相机画面发布后确认；成功回执与画面完成分开验证。
Scene revision=1、dirty=false、Undo/Redo=0，scene.json SHA-256 不变，Simulation 相机修订号不变。
正常关闭 validation errors=0、warnings=0、liveAllocations=0；构建无新增编译告警。
最终视口图像和工作台截图转为 out/m2211-view.png、out/m2211-workflow.png 并目视检查，控件/帮助和场景图像完整。
验收只使用 96×96、2 spp 的快速路径图像，不作质量或实时性能结论。
文档检查 scripts/check-spec.ps1 通过：14 个 Markdown、1037 条本地链接、元数据、表格、索引、测试入口和 JSON manifests；git diff --check 通过。

可复现筛选（实际按上表分轮执行，没有声称重复合并全跑）：

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_tests','dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.(console (named|quotes|nested JSON|full integer|malformed|limits|diagnostics|committed|history|logs)|workspace console|console_workflow_gpu_validation$|settings_example_unit$|workbench_gpu_validation$)' -Reason 'M22.11 workflow and original Console compatibility'
```

## M22 分阶段交付证据

下列为各阶段独立结果，测试范围可能重叠，不相加冒充独立用例总数；历史失败与修正保留在各记录中。

| 阶段 | 证据和边界 |
| --- | --- |
| M22.1 / M22.4 | [0148](0148-console-command-input-plan.md)、[0151](0151-console-assistance-plan.md)：设计/路线/文档验收，不是功能运行证据 |
| M22.2 / M22.3 | [0149](0149-console-command-parser.md)、[0150](0150-console-history-ui.md)：命名参数、JSON、guard/预算、历史与实际输入；本阶段再次通过 17 CPU 和原 GUI |
| M22.5 | [0152](0152-command-help-catalog.md)：37 CPU、实际 runner 发现，共享可选 help/只读目录及坏元数据拒绝 |
| M22.6 / M22.7 | [0153](0153-console-completion.md)、[0154](0154-console-context-assistance.md)：23/36 CPU、分别新增 66/46 GUI 步，覆盖基础/嵌套/动态候选、输入诊断与失效 |
| M22.8 | [0155](0155-editor-viewport-commands.md)：6 视口 CPU、7 目录/上下文及 3 GUI，11 个三入口检查点；设置与异步画面来源分离 |
| M22.9 | [0156](0156-editor-camera-commands.md)：26 CPU、4 GUI，14 个三入口检查点；相机/视口共享 guard 和 session-only；另行 Simulation 频率失败见下文 |
| M22.10 | [0157](0157-editor-settings-help.md)：12 CPU、6 GUI，69 步帮助/搜索/定位与模板/草稿保护 |
| M22.11 | 本记录：新增完整跨入口闭环与可执行示例，首期兼容回归及能力/限制交付 |

## 偏差与决策

不新增依赖、命令别名、自动 guard、持久化设置或传输协议。示例按条提交，不声称跨命令原子性。
首次真实验收暴露的是 Windows 确认文件可见/可读之间的短暂窗口：原子更名后重新按帧读取，整个驱动仍受 240 秒/CTest 300 秒上限约束。
示例通过 observed_guard 区分旧观察；current 仍由现有服务判断。没有修改渲染/服务核心逻辑来迁就验收。

## 遗留问题与下一步

既有 Simulation GPU 发布频率失败见 [0156](0156-editor-camera-commands.md)；不在本阶段修改性能实现或验收门槛。
该频率失败在同配置 M22.8 隔离基线复现，详见 0156；本阶段未重跑，不能计为通过。
未运行：全量引擎、Release、其他平台/GPU/裁剪模块矩阵、生产场景质量性能、人工逐个操作系统 IME。
M22 已按既定范围关闭；位置参数别名、自动 guard、持久化相机偏好、Simulation 相机命令仍未实现。
不自动推进 M20/M21 或未授权的新阶段。

## 修改记录

- 2026-11-03T18:22:23+08:00：建立实施契约与记录。
- 2026-11-03T18:36:00+08:00：完成真实跨入口闭环、Python 示例、失败边界及首期兼容验收，汇总 M22 交付证据。
