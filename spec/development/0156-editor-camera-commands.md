---
id: "0156"
created_at: "2026-11-03T16:17:45+08:00"
updated_at: "2026-11-03T17:13:21+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor-interaction.md
  - ../design/editor.md
  - ../design/runtime.md
  - ../design/application-services.md
---

# 0156 M22.9 Edit 相机设置与操作命令

## 目标与设计依据

完成 [M22.9](../roadmap.md#m22console-命令文本输入与-editor-交互)，基线 3ebada6，开始工作区干净。
先明确命令表、单位、共享 guard、GUI 候选提交、Frame 来源与关闭寿命，再实施。

## 实际变更

采用 [Console M22.9 契约](../design/editor-console.md#m229-实施契约edit-相机命令)。

- [EditorCamera](../../engine/framework/services/include/dk/services/EditorCamera.hpp) 与 CPU 数学实现移入 dk_editor_services，PUBLIC Geometry/Math；保留 dk/editor/Camera.hpp 兼容入口。CameraNavigation 留在 editor_model；没有反向 Editor/ImGui/SDL/Render 依赖或第三方版本变化。
- [EditorCameraService](../../engine/framework/services/src/EditorCameraService.cpp) 让原 EditorViewportService 拥有唯一 Edit Camera。GUI 在副本上应用输入后整体提交；Simulation 相机继续独立。视口设置/相机偏好共用 guard，实际姿态/投影另有 render_revision，纯移速/模式等变化不丢路径样本。
- [EditorCameraOperations](../../engine/framework/operations/src/EditorCameraOperations.cpp) 新增 query/set/orbit/pan/zoom/frame/reset，单位、严格 schema、help v1、枚举/实体候选和错误行为同步[命令参考](../commands/editor-camera.md)。Runtime 附加时对 12 条宿主命令完整预检；普通 runner 不注册，控制命令不进入 Scene 事务/历史。
- Runtime document_lighting 只读实时文档环境；自动裁剪的派生上下文在查询时刷新，单调推进实际渲染版本，无业务 Task/GPU/磁盘访问。presentation 同时核对当前相机来源，不能把旧纹理误报为新姿态已完成。
- Frame entity 回调核对当前文档/Scene revision、Edit 显示和已发布几何，再复用 GUI 子树边界与相机取景数学；没有几何时明确拒绝。close 清除该回调，文档替换使旧 guard 失效，旧 Camera 弹层在下一次绘制取消。
- [CPU 用例](../../tests/unit/EditorCameraCommandTests.cpp) 与 [GUI/Console/IPC 验收](../../tests/integration/EditorCameraCommandTest.py)覆盖发现/隔离、共享 guard、几何意义、候选原子性、草稿/手势/关闭及真实图像来源。原视口 GUI 用例按新增命令组修正发现断言。

## 验证记录

配置：windows-editor / Debug；四个目标 dk_editor_tests、dk_editor_app、dk_run、dk_ctl，按各次筛选选择；单构建目录串行执行。
所有 GUI 都用 --background。GPU 验收启用 required Vulkan/synchronization validation；本机进程环境设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，
仅屏蔽既有 AMD 可选切换层 API 版本警告，不关闭 validation 或降低断言。

最终覆盖 26 项不同 CPU 检查（7 项新相机命令、13 项原相机/导航/正交、6 项视口），以及 4 项真实后台 GUI/GPU 验收（交互、视口命令、相机命令、ReSTIR），均通过。
另行检查的 Simulation GPU 导航频率失败；同一机器/依赖/配置的 M22.8 隔离基线复现相同失败，因此不算本阶段回归通过，也不把它作为新功能缺口隐藏。

| 验证目录 | 范围与结果 |
| --- | --- |
| out/verify/20261103-162507-2f291bb3 | 首次编译失败：Catch2 INFO 中条件表达式缺外层括号，测试未运行；已修正 |
| out/verify/20261103-162652-ed4dbdd7 | 相机/导航/正交和视口 CPU、既有视口 GUI，共 26/26 通过；当时新相机 CPU 为 6 项 |
| out/verify/20261103-162957-69b4cc3b | GUI 夹具首次编译缺 WorkbenchOptions 标志，测试未运行；补齐标志并清除未用参数警告 |
| out/verify/20261103-163220-85ea973b | 新相机 7 项与视口 6 项 CPU 通过；两项 GUI 失败：旧导航被 Scene 草稿拦截；新正交像素断言高于纯色夹具的实际颜色数；下述修正后复验 |
| out/verify/20261103-163445-1aa00b4a | 相机 CPU 7 项及原交互 GUI 通过；新验收到导航检查点后输入状态未持续保持，外部 reset 被接受；保持后台合成输入后复验 |
| out/verify/20261103-164110-d1af79e7 | 沙箱 MSBuild 子节点通信停滞，终止本次已核实进程；构建未完成、测试未运行，改在本机权限下跑同一范围 |
| out/verify/20261103-164433-e8b3d8f1 | 新相机 CPU 7 项、视口 GUI、ReSTIR GUI 通过；新相机导航检查点超时，Simulation GPU 导航 1 秒仅 4 次发布而要求至少 5 次 |
| out/verify/20261103-170521-2f6941aa | 相机 GUI 14 个检查点全部通过，8.70 秒；Simulation GPU 同一频率断言仍失败（4 次、最大间隔 262.8 ms），进行隔离基线对照 |
| out/verify/20261103-170858-2389e6bd | git archive 3ebada6 隔离基线，重新构建 Debug Editor 并运行同一个 Simulation 用例；GPU 同阶段 1002.8 ms 内 4 次发布、最大间隔 223.7 ms，原频率断言仍失败，确认既有问题 |

相机验收目录 out/build/windows-editor/test-artifacts/Debug/camera-commands-9fc2dfb47f304f85a4b3f5a52b757fd8：
Editor 发现 61 条、runner 49 条；7 条新相机描述及 help 完整。实际 Console 输入正交设置和米制 Pan，GUI Apply/Cancel/Reset/移速/鼠标导航，
IPC Orbit/Frame/模式与采样/冲突写入/换文档/活动关闭交替通过。每个检查点对照 shared guard、已发布相机与 Scene/Undo/Simulation 隔离；
移速修改保留 render_revision、路径 generation 和像素。后台关闭 errors=0、warnings=0、liveAllocations=0。
正交光栅与 2 spp 路径 PPM 转 PNG 后人工查看，几何/背景完整；验收辅助图位于 out/m229-orthographic_console.png 和 out/m229-path_ipc.png。
check-spec 对 18 个变更 Markdown、1079 个本地链接、元数据/索引/测试入口的检查通过；git diff --check 通过。

基线使用 out/m229-baseline-source 和 out/build/m229-baseline，复用 windows-editor 已固定的 vcpkg 安装树及 RTXDI 源码；
VCPKG_MANIFEST_INSTALL=OFF 仅避免重复安装，没有切换版本/模块/编译器/validation 或修改验收门槛。baseline 与当前均在 GPU 连续 Orbit 开始阶段失败；
之前的 CPU 模拟导航/正交和独立视口步骤已运行，但不能把整个 Simulation 验收列为通过。

本阶段范围可按下面命令复现；实际分次证据见上表，不声称曾以这一个合并筛选重新全跑：

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug -Target @('dk_editor_tests','dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.(camera |viewport |interaction_gpu_validation$|viewport_commands_gpu_validation$|camera_commands_gpu_validation$|restir_pathtracing_gpu_validation$)' -Reason 'M22.9 camera commands and direct input/render regressions'
```

## 偏差与决策

沿用 M18 的相机数学；Walk 仍为固定高度无碰撞，不扩展角色模拟。相机设置与视口共用 guard，渲染修订号独立。

旧交互验收会在 Inspector 草稿存在时进行导航。首次服务接入将它阻断，依据现有 GUI 行为修订设计：显式 GUI 相机操作允许保留 Scene 草稿，外部命令继续拒绝全部草稿/手势冲突；Gizmo 始终阻止相机写入。CPU 同时验证草稿内容不变。
正交夹具为纯色平面，人工查看图像确认 3 种颜色对应几何和背景，非空像素断言改为至少 3 种；不拿无图像当通过。
Console 执行后文本框仍有焦点，验收驱动先点击空白视口释放文本输入，再按住右键，并在 IPC 读取期间保持合成指针/按钮。
这样验证真实导航状态下的命令拒绝；修正后相机渲染修订号由 Pan 的 8 变为 GUI Orbit 的 9，非仅按住但未发生相机变化。

## 遗留问题与下一步

M22.9 完成，下一阶段 M22.10 面板帮助/搜索。位置参数别名、自动 guard、Simulation 相机命令、持久化相机偏好均未新增。
既有 Simulation GPU 连续导航频率问题留待模拟预览性能专项定位；保留 1 秒至少 5 次、最大间隔小于 400 ms 的原断言。
本次不执行全量、Release 或其他平台矩阵；不对最低性能、跨设备、完整三方 SDK 作新的结论。

## 修改记录

- 2026-11-03T16:17:45+08:00：建立实施契约与记录。
- 2026-11-03T17:13:00+08:00：完成三入口相机服务、命令与定向验收；记录 GUI 草稿兼容修正和 Simulation 基线对照失败。
