---
id: "0127"
created_at: "2026-10-16T15:05:57+08:00"
updated_at: "2026-10-16T15:24:29+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/render-capture.md
  - ../design/render-pathtracing.md
---

# 0127 M17.5 Editor 与固定捕获统一介质

## 目标与设计依据

完成 Roadmap M17.5：[Editor](../design/editor.md)、[捕获](../design/render-capture.md)、[统一路径](../design/render-pathtracing.md)。
实现显式 Scene media、命令 v2、Console/IPC 和冻结来源；验证真实 GUI 与固定捕获一致性，保留 v1 表面兼容。

## 实际变更

- Editor Path tracing 面板增加 Surface only / Scene media、固定时间、云反照率/各向异性，来源取自已显示完整帧；物理输入/模式/会话和 Refresh 重置累积，曝光保留。
- PathPreview 接入 configure_medium_view；保留同几何 GPU owner 供实际内容校验/AS 复用。CPU 场只保留已提交 owner 和局部候选，configure 成功后交换，连续失败不会把候选留下阻塞恢复。
- capture pathtrace v2 显式 medium_mode；v1 旧表面确认兼容，禁止版本字段混用/无效范围。冻结环境/时间/云物理设置与工作额度，补全 geometry/field/model/sampler/shader 来源，shader 闭包包含空气和云。
- Console 从只读日志扩展为有界 JSON method/params 输入，复用 Workspace/Runtime；显式 guard、草稿保护、jobs.wait 只允许零超时，执行后刷新快照。IPC 无新传输逻辑。
- 复用两个 Python 验收入口增加 --media，新建 Luau v2 示例，Python 示例增加 --media；同步命令发现/参考、Editor/GPU 指南、设计/开发索引与测试选择表。无依赖升级。

## 验证记录

环境：Windows Release，现有 windows-editor 构建；GPU 验收要求 RT、Khronos validation/sync，临时在子进程环境禁用已知 AMD switchable 隐式层。所有 Editor 自动化使用 --background 和独立复制夹具。

实际命令均通过 scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release，显式目标/筛选：

| 范围 | Target | TestRegex | 结果与证据 |
| --- | --- | --- | --- |
| capture schema/失败状态与 Console 初测 | dk_protocol_tests、dk_editor_tests、dk_editor_app、dk_run、dk_ctl | `^dk\.(protocol\.capture\|editor\.workspace console)` | 6 项 capture 通过；Console 初测失败，见下文；out/verify/20261016-150903-7ee89d2c |
| v2 capture 与真实媒体 GUI | dk_editor_tests、dk_editor_app、dk_run、dk_ctl | `^dk\.(editor\.(workspace console\|media_pathtracing_gpu_validation)\|pathtracing\.media_capture_gpu_required)` | 2 项 GPU 通过，Console 夹具失败；out/verify/20261016-151407-b1c168aa |
| Console 与旧入口回归 | dk_editor_tests、dk_editor_app、dk_run、dk_ctl | `^dk\.(editor\.(workspace console\|pathtracing_gpu_validation\|desktop_render_gpu_validation)\|pathtracing\.capture_gpu_required)` | 4/4 通过，无跳过；out/verify/20261016-151741-0d37bfa7 |

首次 Console 失败是 effect 名称误用 read，改为注册表中的 query；第二次是新增 entity.create 夹具误带 name，按现有 schema 删除后通过。
媒体 GUI 的 7 组 capture 对照（表面、空气、云、时间、恢复、返回表面、重载）各 96×96，最大线性绝对误差全部 0；真实 Console 捕获误差也是 0。
覆盖两次非法云尺度/不同 seed 后恢复、旧画面/旧字段来源保留、曝光保留历史、模式/会话重置、在途关闭和 IPC 响应；GPU validation errors/warnings 与 liveAllocations 均为 0。
捕获覆盖固定 spp/重复结果/冻结输入、PPM 与线性 JSON、完整 uint64 seed、v2 surface 与 v1 相等、时间改变字段复用、两次不支持域恢复、工作额度失败、运行/排队/关闭取消保护旧文件、实际 Python/Luau 相等。
正常视口回归继续覆盖相机/Gizmo、resize/minimize、save/load；Sponza 回归覆盖原路径与 Raster 大气/云切换，不作为 Scene media 固定质量性能结果。

最终复验：同一 verify 入口，Target=dk_editor_app、dk_run、dk_ctl，TestRegex=`^dk\.(editor\.media_pathtracing_gpu_validation|pathtracing\.media_capture_gpu_required)$`，2/2 通过、无跳过（out/verify/20261016-152035-486a7af5）。补充确认显式 Refresh 增加 generation、相同样本图像不变；实际 commands.describe 发布 v2/mode/额度 schema。累计 12 个唯一目标检查全部通过，失败项均修正复验。

文档检查：scripts/check-spec.ps1 -Path 限定本次 12 个 Markdown 文件，通过 748 个本地链接及元数据/表格/索引/测试入口/JSON manifests 检查；git diff --check 通过。
最终媒体 GUI 证据：out/build/windows-editor/test-artifacts/Release/media-pathtrace-862852d2a3634a89a1bd807d4f038bcb/report.json；媒体命令证据：同目录下 media-capture-af552549ef5942d4bcc796e87d953ddb/evidence.json。6 次活动介质渲染时 IPC 查询最大 0.0303 秒，仅是该小夹具观测。

## 偏差与决策

现有 Console 仅日志，补充 Runtime 命令输入。M17.6 的 Sponza 固定质量性能基线不属于本次完成条件。

## 遗留问题与下一步

M17.6：Sponza 在冻结画质门槛下进行缓存/majorant/采样/调度优化及三轮性能/显存基线。未运行全量/Debug 双配置或 CPU-only 重新配置；本次不改变底层 GPU shader 或 CPU 介质算法。capture 仍限小场景 4096 三角形，未扩为 Sponza 离屏捕获。

## 修改记录

- 2026-10-16T15:05:57+08:00：创建设计与记录。

- 2026-10-16T15:23:31+08:00：完成实现与 12 个唯一检查；同步阶段和使用说明。
