---
id: "0129"
created_at: "2026-10-16T17:06:00+08:00"
updated_at: "2026-10-16T17:25:00+08:00"
status: completed
design_refs:
  - ../design/editor-interaction.md
  - ../design/editor.md
  - ../design/foundation-math.md
  - ../design/render-capture.md
---

# 0129 M18.1 相机镜头与导航设置

## 目标与设计依据

实现 [M18.1](../roadmap.md#m18相机操作模式与投影扩展)：可调透视 FOV/裁剪、导航偏好、共用投影计算与截图一致性。
依据 [交互设计](../design/editor-interaction.md)、[Math](../design/foundation-math.md)、[截图](../design/render-capture.md)。
Fly/Walk 与正交不在本次范围。保留已有 spec/README.md 和 ai-documentation-workflow.md 工作区修改。

## 实际变更

- [Projection](../../engine/foundation/math/include/dk/math/Projection.hpp) / [实现](../../engine/foundation/math/src/Projection.cpp)：共享双精度透视矩阵与局部射线，有限参数和可表示性校验。Editor 与 Capture 共用矩阵，公开依赖显式链接 dk::math；无新库/构建开关。
- [Camera](../../engine/editor/src/Camera.cpp)：姿态改为 eye/yaw/pitch/focus distance，镜头/导航偏好分离；原子设置、有效镜头版本、同值/纯偏好不失效、Home 保留设置、恢复默认与失败保护。
- FOV 同时进入 ray/project/pan/frame/units_per_pixel。Frame 按包围盒角点求可行距离并检查近远裁剪，不能完整框选时拒绝并保留姿态；模拟固定初始视向不随用户灵敏度/反转设置漂移。
- [CameraSettingsUi](../../engine/editor/src/CameraSettingsUi.cpp)：Edit/Simulation 共用弹层但独立草稿和相机，提供 FOV/Auto 与 Manual 裁剪、Orbit/平移/滚轮和后续 Fly/Walk 偏好；Apply/Cancel/Restore defaults，非法输入保留旧值及草稿。活动 Gizmo/文本/弹层阻止导航。
- 光栅环境 Auto near=1，其余视图 near=.05，far=10000；手动值不被上下文改写。Viewport 和 worker 校验实际 aspect/环境域，失败不提交新图、保留旧图和恢复入口；相机改变不触发几何重建。
- [CaptureService](../../engine/framework/services/src/CaptureService.cpp) 复用透视计算，命令字段/默认值不变。ConsistencyDriver 与 PathSmoke 记录真实 FOV，删除固定 65°；新增真实 GUI FOV/非法值/速度输入，模拟分别应用 55°/75°并确认 Edit 独立。
- 更新使用指南、模块设计与 Roadmap。Fly/Walk、正交、跨启动偏好保存尚未实现，不新增 Scene Camera 或 UI 相机 Runtime 命令。

## 验证记录

Windows / Release，GPU 为 NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49。使用既有 windows-editor 和
windows-dev（Editor/Platform/Graphics/Render GPU 关闭）配置；CPU-only 另配置用于验证依赖隔离，没有 Debug/Release 双配置或全量回归。
自动窗口均 `--background`、独立工程副本、required Vulkan/sync validation；仅验证子进程设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1。

| 证据目录（out/verify） | 范围与结果 |
| --- | --- |
| 20261016-171355-7d8bac4b | windows-editor 构建 math/editor tests、Editor、runner、ctl；7 项相机/透视 CPU 用例通过，无失败/跳过。 |
| 20261016-171714-d78dcc66 | 构建 Editor、runner、ctl；7 项真实窗口/GPU 用例通过，无失败/跳过。覆盖 interaction、consistency、environment/cloud consistency、simulation、pathtracing、media_pathtracing。 |
| 20261016-172001-a2fd7b3f | windows-dev 的 math/editor tests 构建成功；CTest discovery 失败，测试未运行。六个 9 月旧二进制为 Catch2 3.13，而当前固定依赖的发现脚本为 3.16；报 json::verbosity=quiet 无法解析。 |
| 20261016-172315-93684951 | 重建上述六个过期测试程序后发现恢复；相同 7 项相机用例通过，无失败/跳过。没有修改依赖 baseline、清缓存或执行额外用例。 |

实际命令均使用 scripts/verify.ps1，完整参数/选择列表保存在对应 summary.json：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_math_tests','dk_editor_tests','dk_editor_app','dk_run','dk_ctl') `
  -TestRegex '^dk\.(math\.perspective |editor\.(camera |environment camera ))' -Reason 'M18.1 透视与相机模型'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_editor_app','dk_run','dk_ctl') `
  -TestRegex '^dk\.editor\.(interaction|consistency|environment_consistency|cloud_consistency|simulation|pathtracing|media_pathtracing)_gpu_validation$' `
  -Reason 'M18.1 真实GUI、镜头与截图一致性'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration Release `
  -Target @('dk_math_tests','dk_editor_tests','dk_commands_tests','dk_core_tests','dk_io_tests','dk_protocol_tests','dk_scene_tests','dk_service_tests') `
  -TestRegex '^dk\.(math\.perspective |editor\.(camera |environment camera ))' -Reason 'CPU-only边界与过期Catch2程序恢复'
```

CPU 用例覆盖 FOV/aspect 射线与近远平面、角点完整框选、Gizmo 80px 尺度、pan 屏幕位移、非法/非有限输入、
环境域/resize 拒绝、手动裁剪、Home/默认值、缩放饱和和无变化版本。真实窗口原有拾取/Gizmo、取消、保存重载继续通过。
55° 路径 GUI 在 path/resumed/camera 三个检查点与固定截图线性最大绝对误差均为 0（每组 320896 像素）；
Scene media 七组回归均为 0（每组 96×96）。速度改为 9m/s 后已完成 spp、camera revision 与 generation 均保持，非法 FOV=0 被拒绝且草稿保留。
大气/云 GUI 与固定捕获仍逐字节一致；相关记录末尾 validation errors/warnings/liveAllocations 均为 0。
没有运行跨设备、全量、完整性能基线或未来导航/正交测试。
最终为 14 个不同用例通过，另在 CPU-only 配置复验 7 项；未将首次发现失败计为测试通过。
文档检查与 git diff --check 通过；已验证的代码没有后续修改。

## 遗留问题与下一步

M18.1 无剩余实现项。下一阶段 M18.2 接入 Fly/固定高度 Walk 和连续输入，复用本阶段设置；正交按 M18.3–5 推进。

## 修改记录

- 2026-10-16T17:06:00+08:00：建立共享投影、镜头/偏好、失败和验证契约。
- 2026-10-16T17:25:00+08:00：完成实现、GUI/截图与 CPU-only 定向验收；保留旧 Catch2 发现失败及恢复证据。
