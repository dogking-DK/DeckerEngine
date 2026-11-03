---
id: "0155"
created_at: "2026-11-03T14:36:46+08:00"
updated_at: "2026-11-03T15:06:13+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/runtime.md
  - ../design/commands.md
  - ../design/application-services.md
---

# 0155 M22.8 Edit 视口会话与设置命令

## 目标与设计依据

完成 [M22.8](../roadmap.md#m22console-命令文本输入与-editor-交互)，基线 dea66ee，初始工作区干净。
先补齐 CPU 宿主服务、五条命令、会话 guard、三入口统一和异步图像来源契约，再实现。
相机命令、面板搜索和位置参数不属于本阶段。

## 实际变更

新增 [EditorViewportService](../../engine/framework/services/include/dk/services/EditorViewportService.hpp) 与
[Operations](../../engine/framework/operations/src/EditorViewportOperations.cpp)，以 dk_editor_services/dk_editor_operations 目标隔离 CPU 会话逻辑。
Runtime 显式 attach 才注册 editor.viewport.query/set/pause/resume/reset；普通 runner 无这些命令。
注册完整预检，handler 读取当前宿主槽；detach、shutdown 和 Runtime 析构均关闭服务，外部保留的 shared_ptr 也不能再调用 owner 回调。

Workspace 提供当前 Runtime 文档身份、草稿/手势阻止原因；独立 guard 为 view_id/document_id/revision。
换文档立即使旧 guard 失效；set 构造完整候选、全部验证后发布，无变化不增修订号。
control 不进入 Scene 事务/历史，不修改 dirty 或文件，也不使用 batch Scene auto-guard。
关闭前先解除上下文回调，再释放视口与后台 worker。

GUI 的模式、路径参数、光栅曝光和 unlit 状态迁入服务；GUI/Console/IPC 共享，worker 使用拥有型值副本。
保留原有曝光/目标/暂停不丢采样、物理/算法改变重新累积的规则；新增常量环境开关，保留 RGB 原值。
路径 seed 控件使用 uint32，支持完整范围。相关 GUI 控件在草稿和活动 Edit Gizmo/导航期间禁用，服务同样拒绝写入。

[ViewportSessionUi](../../engine/editor/src/ViewportSessionUi.cpp) 发布实际图像来源及 worker 观察；
设置接受与画面完成分开，错误保留旧图像/来源。对关闭、旧会话与迟到观察分别拒绝；不增加协议分支或后台业务 Job。
完整参数/边界、结果字段、错误、Console/IPC/Python 示例见 [命令参考](../commands/editor-viewport.md)。
共享 schema/help 自然接入已有补全；省略字段保持当前值，因此 help 提供初始值示例而不冒称固定缺省替换值。

新增 [CPU 契约测试](../../tests/unit/EditorViewportTests.cpp) 与
[真实三入口验收](../../tests/integration/EditorViewportCommandTest.py) / [原生驱动](../../engine/editor/src/ViewportCommandSmoke.cpp)。
--viewport-command-smoke 仅用于标记为可丢弃的独立夹具，自动启动使用 --background；路径尺寸限制 96×96 仅用于该验收。
同步设计、模块/开发索引、Roadmap、使用指南、命令发现覆盖和测试选择表；无依赖升级。

## 验证记录

配置 windows-editor / Debug；使用 scripts/verify.ps1，每次显式 Target/TestRegex，未做全量或双配置。
以下为不同测试的有效结果，共 13 项 CPU、3 项后台 GUI，无最终失败/跳过；重复定向复核不重复计数。

| 范围 | Target / TestRegex | 实际结果和证据 |
| --- | --- | --- |
| Workspace 目录与 Console 上下文回归 | dk_editor_tests、dk_editor_app、dk_run、dk_ctl；`^dk\.editor\.(catalog \|console context )` | 7/7 通过；[144123](../../out/verify/20261103-144123-1128fb18/summary.json) |
| 真实工作台/Console 输入、历史、补全/诊断回归 | dk_editor_tests、dk_editor_app、dk_run、dk_ctl；`^dk\.editor\.(viewport \|workbench_gpu_validation$\|viewport_commands_gpu_validation$)` | 工作台通过，原有 26+48+66+46 步；该轮新增视口验收的退出失败另列下文；[145257](../../out/verify/20261103-145257-cf5e52f5/summary.json) |
| 既有 ReSTIR Surface/Scene media GUI/IPC/capture 对照 | dk_editor_app、dk_run、dk_ctl；`^dk\.editor\.(viewport_commands_gpu_validation\|restir_pathtracing_gpu_validation)$` | 2/2 通过，ReSTIR 验收含正交/返回/关闭；[145459](../../out/verify/20261103-145459-de11dc0f/summary.json) |
| 最终 CPU 状态/发现/guard/生命周期及初版三入口 | dk_editor_tests、dk_editor_app、dk_run、dk_ctl；`^dk\.editor\.(viewport \|viewport_commands_gpu_validation$)` | 7/7 通过；[150048](../../out/verify/20261103-150048-f51f4ef6/summary.json) |
| 最终三入口、可见几何、注册表与文档 Python 示例 | dk_editor_app、dk_run、dk_ctl；`^dk\.editor\.viewport_commands_gpu_validation$` | 1/1 通过；[150322](../../out/verify/20261103-150322-5b929d78/summary.json) |

新增 CPU 六项覆盖宿主独有发现/枚举补全、schema 与完整整数、混合坏参数原子拒绝、no-op、pause/resume/reset、
能力缺失（包括显式同值路径字段）、Inspector 草稿/手势、文档和视口替换、Runtime 析构、关闭后/迟到观察、
图像失败保留来源、Scene/Undo/文件不变及绝不自动填 host guard。

新增真实验收 11 个检查点：raster → IPC path → GUI exposure → Console pause → IPC reset → resume → GUI ReSTIR →
IPC Scene media → 破坏夹具资产后的失败 → 恢复资产/重载 → 活动渲染关闭。
曝光保持 generation 并改变像素，暂停/reset 保留旧像素且标过期，算法变化重置 generation；
坏/旧/缺 guard 与错误整数输入不改变设置。每个检查点核对 GUI、IPC 状态和 Scene revision/dirty/Undo，失败保留 shown_guard。
runner commands.list 无 editor.*（本配置 49 条）；Editor list/describe 中恰有新增五条命令及 help（本配置 54 条）。
执行参考页 Python query/set(pathtrace,restir_di,target_spp=12)/pause 示例并使用真实 guard；随后恢复执行活动关闭。
最终 GPU 为 RTX 4070 Laptop / 596.49，验证错误、警告、liveAllocations 均为 0。

测试产物保存在 out/build/windows-editor/test-artifacts/Debug/viewport-commands-* 与 restir-pathtrace-*，
含 summary、五条 describe、逐检查点 JSON、stdout/stderr 和 ReSTIR PPM。检查窗口截图与视口像素，不能把 96×96 验收当作性能基准。
最终三入口产物为 [viewport-commands-11984287258e412599de828b95a01372](../../out/build/windows-editor/test-artifacts/Debug/viewport-commands-11984287258e412599de828b95a01372/summary.json)。
check-spec.ps1 定向扫描 13 个 Markdown，949 条本地链接及元数据/表格/索引/测试入口检查通过；git diff --check 通过。

失败与修正：144443 构建因新测试 detail 命名歧义和遗漏 dispatch 参数失败；144751 构建因新驱动遗漏 File.hpp 失败，均修正后重建，未用旧二进制验收。
145257 的 11 个新检查点通过，但进程因本机 AMD switchable 层 API 1.3/1.4 警告返回 1。
后续 GPU 验收在调用 verify.ps1 前仅设置进程环境 `$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'`，
保留引擎警告检查、required Vulkan/sync validation；未改驱动/注册表或屏蔽验证层。
收尾补齐 Runtime 直接析构保护（[145720](../../out/verify/20261103-145720-b41489be/summary.json)，6/6 通过）。
初版新夹具相机只看到单面几何背面，截图仅背景；改成与既有路径验收一致的双面材质，增加非均匀表面像素断言，
[150218](../../out/verify/20261103-150218-5aa9eaef/summary.json) 及最终验收均通过。

## 偏差与决策

参数使用命名字段；target_spp 表示绝对采样目标。Scene guard 与视口会话 guard 分离。
服务是 CPU 会话状态，不反向依赖 ImGui/Render；异步 worker 维持既有提交/取消模型。
sky_enabled 控制路径常量环境 RGB，不修改场景大气；云参数为现有路径 Scene media 会话参数，光栅云质量面板保留原入口。

## 遗留问题与下一步

本阶段完成；下一阶段 M22.9 相机设置命令。面板统一帮助/搜索、Simulation 视口命令、位置参数别名和精确追加 N 样本后置。
未运行 Release、全量引擎、无路径模块的独立构建、多 GPU/平台矩阵或大型场景性能；能力缺失通过 CPU 注入状态验证。
本次不扩展路径材质/介质支持域，受既有渲染器限制；命令接受仍可能在之后渲染时报具体场景错误。

## 修改记录

- 2026-11-03T14:36:46+08:00：建立设计与记录，明确命令表、状态所有者和失败边界。
- 2026-11-03T15:04:00+08:00：完成服务/命令、三入口接入、定向验收和使用文档，更新路线图。
