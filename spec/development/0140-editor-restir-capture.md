---
id: "0140"
created_at: "2026-10-19T16:45:00+08:00"
updated_at: "2026-10-19T17:07:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
  - ../design/render-capture.md
  - ../design/editor.md
---

# 0140 M19.7 Editor 与独立 ReSTIR 捕获

## 实施契约

- Editor 将直接光照算法与 Surface only / Scene media 正交配置；提供初始候选数、时间/空间复用开关，显示实际已发布帧来源与复用统计，spp 仍只计完整 epoch。
- `render.capture` 增加显式 v3：必填 `medium_mode`、`direct_lighting`，ReSTIR 模式必填 `restir` 对象；v1/v2 不接受新字段且继续经典 MIS。仅 ReSTIR 接受其配置；不扩展 raster sequence。
- v3 linear JSON 使用 schema_version 2；provenance 增加版本、有效配置、SDK 标识、空历史起点与完成后统计。每次捕获独立 renderer，epoch/sample 从 0 开始；不借用 Editor 历史。
- 公共 CaptureService 请求仅含引擎自有值类型，保持无 GPU/SDK 头依赖。worker 私有转换到 renderer，配置校验先于提交；旧输出只在 owner 原子发布点替换。
- ReSTIR shader 指纹覆盖本地入口/bridge 和固定 RTXDI Include 内容，编译前后/输出前校验；旧请求格式保持兼容。
- 定向验证命令发现、非法配置/旧文件、四种复用模式及介质、独立捕获复现、GUI 等价/失效/后台关闭与 Python/Luau。数值核心与 DLSS 接入不在此阶段。

## 实现与验证

当前已完成：

- [命令注册](../../engine/framework/operations/src/RenderOperations.cpp)与 [服务请求](../../engine/framework/services/include/dk/services/CaptureService.hpp)：v3 显式算法、嵌套版本/预算/复用字段、默认补全与严格非法组合拒绝，v1/v2 保持兼容；Scene/Undo 不变。
- [捕获 worker](../../engine/framework/services/src/PathCapture.cpp)：使用新 renderer 与完整视图，表面/介质分别选择已有 configure_restir 入口；严格验证 sample/epoch 0 和无起始时间历史。记录 v3 schema 2、provenance_version 1、SDK/采样版本和完整完成统计，维持单点原子发布与 4096-byte Job 摘要。
- [面板](../../engine/editor/src/PathPanel.cpp)与 [worker](../../engine/editor/src/PathPreview.cpp)：经典/初始/时间/空间/联合复用选择；实际图像来源与候选/M/age/计数、最近重置原因；复用原资源/取消/失败保护。曝光/暂停/target 不增加 spp 或清空样本。
- shader-only RTXDI target 仅被 capture PRIVATE 使用，以固定 Include 内容扩展 ReSTIR shader 指纹；依赖版本和 baseline 未变，无 SDK 类型泄漏，无 DLSS 集成。
- [命令参考](../commands/render.md#restir-直接光照v3)、[Editor 指南](../guides/editor.md#restir-直接光照)、Python 可选 --restir 与独立 Luau 联合复用示例同步。

验证配置：out/build/windows-editor / Release，MSVC 19.51.36260.0；NVIDIA GeForce RTX 4070 Laptop GPU / driver 596.49。GPU 用例要求 RT/validation/sync；仅进程环境设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，避免 AMD 隐式层 API 版本警告，未关闭 Khronos 验证。所有构建使用 scripts/verify.ps1。

| 范围 | 命令范围 | 结果 / 证据 |
| --- | --- | --- |
| capture 单元/注册/非法版本与组合、旧文件/Scene/Undo | targets dk_protocol_tests、dk_run、dk_ctl、dk_editor_app；regex `^dk\.protocol\.capture` | 9 通过；out/verify/20261019-164921-22960385 |
| 独立 ReSTIR 捕获 | targets dk_run、dk_ctl；regex `^dk\.pathtracing\.restir_capture_gpu_required$` | 通过，83.34 s；out/verify/20261019-165504-f9c5aa26 |
| worker / 后台 GUI | targets dk_editor_render_benchmark、dk_editor_app、dk_ctl；regex `^dk\.editor\.restir_(worker\|pathtracing)_gpu_validation$` | 2 通过，2.11/35.41 s；out/verify/20261019-170016-18a64960 |

捕获覆盖两种介质 × 四种复用模式的重复像素、实际默认配置与来源、v3 classic/v2 等价、正交/曝光、预算失败、排队/运行取消、进程退出和 Python/Luau 同输入产物。实际 runner commands.describe 保存 schema，旧捕获回归另核对 commands.list 的 effect/undoable。
worker 覆盖暂停/target/曝光保留、手动 reset 复现、相机保留 sampling owner、Refresh/尺寸/候选/算法切换失效、非法配置不发布、挂起与恢复/析构。
GUI 使用 --background 与真实控件切换初始/时间/空间/联合和 classic、空气/云/固定时间、曝光、两次非法云场与恢复、文档重载、Console、投影/正交高度和在途关闭，并逐模式对照独立线性 capture。

中间失败与修正：首次沙盒构建不能访问已配置 C:/code/third_lib/vcpkg，改用授权工具链环境；新 worker 夹具误将 MemoryResult 当成 dk::Result，修正为既有 .value()；异步错误可能已进入 Suspended，断言改为持久 error 字段。一次 GUI 使用了编辑同时编译留下的旧对象，重新标记受影响 Editor 源并完整重编译后通过；所有失败保留在 out/verify，未用失败构建后的旧产物作通过依据。

旧 v1/v2 与原 GUI 直接回归：targets dk_run、dk_ctl、dk_editor_app，regex `^dk\.(pathtracing\.(media_)?capture_gpu_required$\|editor\.(media_)?pathtracing_gpu_validation$)`，4 项通过（32.23/44.48/29.75/29.87 s），证据 out/verify/20261019-170216-72f6a067。

ReSTIR 捕获生成 24 份有效报告并逐对重复像素检查；GUI 16 个检查点、10 次独立捕获比较和 1 次 Console 比较，线性 max_abs 均为 0，IPC 最大 0.049 s。产物分别在 out/build/windows-editor/test-artifacts/Release/restir-capture-1a6c4dceef004809950490552dc67c2d 和 restir-pathtrace-7395d8c8450143569618762369f43152。后台 GUI 日志确认 validation errors=0 warnings=0 liveAllocations=0。

截图人工检查发现窄侧栏 Spatial reuse 和部分来源文字截断，已改成分行开关与自动换行诊断，GUI 单独复验通过（35.12 s，out/verify/20261019-170549-5a86d22b）；最新截图/报告为 restir-pathtrace-935e654c764447148a92150ffadbca57，线性误差仍为 0。文档首次检查发现新增索引行落在表格外，移回编号表后通过 13 个 Markdown/914 个本地链接及索引/测试入口检查。未运行全量/双配置、M19.8 质量/三轮性能或 M19.6 数值核心长测；本阶段未改动 shader/传输数学，保持默认 Classic MIS。DLSS 与重建模式仍另行接入。

## 交付范围

M19.7 完成，共 16 个独立定向用例通过（9 CPU 命令用例、7 required GPU 消费端用例），无最终失败或跳过；GUI 布局修正后额外复验通过。默认仍为 Classic MIS，下一阶段为 M19.8 质量/性能与默认策略；CPU-only 独立构建未重跑，公开服务类型保持无 GPU/SDK 头依赖。
