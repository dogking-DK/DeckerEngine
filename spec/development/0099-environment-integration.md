---
id: "0099"
created_at: "2026-10-13T10:57:33+08:00"
updated_at: "2026-10-13T11:44:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/render-capture.md
  - ../design/render-pipeline.md
  - ../design/render-atmosphere.md
  - ../design/scripting-luau.md
---

# 0099 M14.6 环境面板、视口与截图闭环

## 目标与设计依据

完成[Roadmap](../roadmap.md)M14.6。按[编辑器](../design/editor.md)、[截图](../design/render-capture.md)和[管线](../design/render-pipeline.md)契约，
将既有环境编辑/持久化与M14.5渲染连接，增加可追溯capture、Python/Luau示例及真实GUI跨入口验收。
不新增三方依赖；关闭环境保持旧unlit。性能基线属M14.7。

## 实施与验证

实现入口：
[环境面板](../../engine/editor/src/EnvironmentPanel.cpp)、[Workspace](../../engine/editor/src/Workspace.cpp)、
[后台预览](../../engine/editor/src/EnvironmentPreview.cpp)、[Viewport](../../engine/editor/src/Viewport.cpp)、
[共用渲染入口](../../engine/render/pipeline/src/ScenePipeline.cpp)、
[截图服务](../../engine/framework/services/src/CaptureService.cpp)、
[截图schema](../../engine/framework/operations/src/RenderOperations.cpp)、[Luau能力](../../engine/scripting/luau/src/Luau.cpp)。

- EnvironmentDraft保留完整值与guard。Apply调用environment.set；外部修改不覆盖草稿，旧Apply冲突，Revert取最新。
  Save提交草稿后保存；Undo/Redo/Reload共用Scene历史。两份本地草稿同时修改时先拒绝，防止部分提交。
- 环境启用时，Edit视口使用独立worker/Device/Queue；一个在途、一个可替换请求、一个结果。
  完整快照/相机/尺寸/曝光/手势身份匹配才发布，旧代丢弃。取消、resize、切换环境与关闭不发布旧结果；
  已提交GPU正常排空后join，错误保留旧帧。关闭环境维持旧同步unlit。
- render_environment共用已验收天空、Lambert和空气透视。首个renderer为候选，整帧成功后才保留，
  首次错队列不固化缓存；已有renderer拒绝外队列。新增near=1m/far=10000m环境相机，绘制/拾取/capture一致。
- capture冻结环境与相机，增加曝光参数和provenance（完整环境/相机/曝光/profile、静态时间0、固定质量、shader指纹）。
  ticket指纹为null，成功时记录源码FNV-1a64诊断指纹；编译前后源码变化拒绝发布。保持原PPM单文件原子替换与Job取消约定。
- Luau显式开放environment.query、jobs.get/wait（所有能力）；environment.set（edit/project）；
  render.capture、jobs.cancel（project）。CPU配置不引入Vulkan；未注册的截图命令仍由Runtime返回not_found。
  [Python](../../examples/automation/environment_capture.py)与[Luau](../../examples/scripting/environment-capture.luau)实际执行相同环境/截图请求。
- 审查修复异步预览期间的Gizmo：启动需当前帧，已捕获手势按原guard继续，外部revision/尺寸变化取消；
  自身预览未返回不误取消手势。真实GUI增加拖动和相同视图capture检查点。

## 验证与修复过程

Windows x64，windows-editor Debug，额外开启DK_BUILD_SCRIPTING_LUAU；独立windows-scripting Debug验证CPU边界。
大气/GUI均使用required Vulkan与同步validation；只在测试进程临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复。
未新增或升级依赖，使用既有固定vcpkg baseline。

初始编译分别暴露schema调用参数错误、AtmosphereRenderer无bool转换、SubmissionQueue无默认构造；
新增恢复测试首次缺少RenderDisk直接依赖（out/verify/20261013-114003-a7921822），随后通过target_link_libraries补齐。
均已修复后重新构建，早期失败记录在out/verify/20261013-110231-d7b8ccef、20261013-111412-16456919、20261013-111549-05196d0e。
最初editor缓存关闭大气，随后用windows-editor预设重新配置启用大气和Luau，未用旧二进制替代验收。

首次required GPU失败于near=1/far=10000反算舍入后略超上界。增加1e-10相对容差，
CPU用例验证恰好上限接受、超过1e-6拒绝；未放宽渲染质量门槛。
修复后GUI环境/冻结截图走通，最后Luau脚本因旧白名单失败；按设计增加精确命令能力边界后闭环通过。
失败批次out/verify/20261013-112139-f6252125和20261013-112442-f7ad1b98保留，不将失败/未运行记为通过。

| 验证范围 | 结果 | 证据目录（相对仓库根） |
| --- | --- | --- |
| Workspace/相机/环境草稿、capture CPU参数与失败保护，旧unlit capture、旧交互与GUI/IPC、环境GUI/脚本 | 18通过，0失败/跳过 | out/verify/20261013-113117-60b0592f |
| Luau环境/capture能力与原白名单、实际环境GUI/脚本 | 3通过，0失败/跳过 | out/verify/20261013-112728-5e722c0b |
| 独立CPU Luau权限/环境持久化及发现 | 4通过，0失败/跳过 | out/verify/20261013-113219-7f16f4be |
| 最终管线CPU、首次/热缓存错队列恢复、worker生命周期及完整环境GUI/脚本 | 8通过，0失败/跳过 | out/verify/20261013-114147-d073cf31 |
| 异步Gizmo真实手势及旧交互/旧GUI一致性 | 3通过，0失败/跳过 | out/verify/20261013-113510-0599df0d |

所有批次使用scripts/verify.ps1，summary.json记录实际target、regex、选中测试、配置与结果，tests.log/results.xml保存输出。

[后台GPU测试](../../tests/unit/EnvironmentPreviewTests.cpp)覆盖首次/热缓存错队列恢复、请求替换、resize、取消、错误后恢复及在途析构；
关闭后验证消息和Memory live allocations回零。
[真实GUI验收](../../tests/integration/EditorEnvironmentTest.py)使用--background及一次性夹具：
initial/gui/external/draft/conflict/undo/redo/saved/reloaded/gizmo十个检查点。
夹具材质显式doubleSided，使固定相机能看到原单面quad背面；断言确实存在有色几何接收面，避免仅比较天空。
旧版本冻结capture、重载capture、Gizmo完成capture与对应GUI视口PPM逐字节相等；
Python和独立Luau示例生成图像逐字节相等，元数据对应冻结场景/相机/曝光/固定时间质量及shader指纹。
最终产物：out/build/windows-editor/test-artifacts/Debug/environment-85e4507a09034386acda152b5192905b，
包含report.json、10个视口JSON/PPM、3次对照capture、两种脚本PPM与工作台截图。
GUI正常退出后validation errors/warnings及liveAllocations均为0。GUI截图另转换PNG目视检查，环境字段布局无截断。

文档、命令发现、参考页、设计索引与测试选择表同步；check-spec校验本次文档、链接/元数据/索引和测试入口。
原有用户修改spec/README.md及未跟踪spec/guides/ai-documentation-workflow.md保留，不纳入提交。

## 限制与下一步

仅Edit场景环境，Simulation视口不接入；沿用M14.5刚体forward-Z、大气/地表/材质域，环境相机近于1m会裁剪。
视口RGBA8读回上传桥及独立GPU设备仍有成本，未进行M14.7冷/热LUT和端到端性能测量。
PPM无嵌入元数据/sidecar，调用者保存Job结果；FNV指纹仅用于源码追踪，不是加密认证，不保证资产外部文件变化的快照。
未运行全量、Release或跨GPU/跨驱动比较。静态time_seconds=0，固定质量，不新增时域历史、云或PBR。
下一项M14.7；M14父阶段保持未完成。
