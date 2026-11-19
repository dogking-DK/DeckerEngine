---
id: "0185"
created_at: "2026-11-19T14:00:00+08:00"
updated_at: "2026-11-19T15:05:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
  - ../design/editor.md
---

# 0185 Scene media 首表面 ReSTIR v2 和运动预览尺寸

## 目标与设计依据

让 GPU 介质路径在保留空气、云和后续反弹的同时复用首表面直接光。Editor 会话公开运动预览长边上限，支持原尺寸。

## 实际变更

- GPU 渐进模式在候选准备、时间合并和空间合并阶段检查当前接收点及来源的几何可见性。介质阴影共用同一射线构造，支持有限太阳、环境与面积灯，分别处理行星地面、网格和体素。最终着色复用本轮几何可见性并重新估计空气/云透射。
- `Scene media + ReSTIR DI` 在 Editor 中自动选择 v2，候选数、两个复用开关和反弹数取当前面板值。独立 capture 使用 `pathtrace.version=3` 与 `restir.version=2`，省略后者的旧请求保留 v1。单通道介质采样和碰撞后的体积光路保持原模型。
- 视口服务、GUI 与 `editor.viewport.set/query` 共享 `motion_preview_long_edge`。0 使用实际视口的宽高，正值仅对运动中的介质预览按比例设上限。变更沿现有视口 guard 提交，图像与 reservoir 按实际渲染尺寸判定能否沿用。
- 运动探针与桌面性能夹具显式指定原测量所用的 192 像素长边。0181 的新旧颜色采样对照显式固定 ReSTIR v1。交互验收另覆盖原尺寸和自定义长边的实际运动帧。

## 验证记录

- Windows Release，构建树 `out/build/windows-voxel-editor`，NVIDIA GeForce RTX 4070 Laptop。首次沙箱内 CMake 再生成无法读取本机 vcpkg 目录，随后使用同一构建树与工具链完成构建。
- `out/verify/20261119-140217-41c10cd9/tests.log`：9 个视口命令/状态用例和体素编辑器与独立捕获对照通过。表面 v2 的数值断言通过，旧 AMD 隐式层的 Vulkan 版本警告导致严格验证失败，中止该轮。后续命令仅为验证进程设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，继续使用 Khronos/sync 验证。
- `out/verify/20261119-141416-2bf0794b`：介质 v2 数值通过。比较了带颜色的大气和云对太阳的透射、几何投影，以及没有首表面时多反弹介质路径与 Classic 的逐样本一致性。两个 GUI 测试在云时间编辑处失败，新控件把原有输入框挤出测试窗口的可见区域。调整面板布局，并让 ReSTIR 运动用例通过实际输入框设置长边。
- [五项 GPU 复验](../../out/verify/20261119-141753-33acf318/summary.json)全部通过，涵盖表面 v2、介质历史、独立 capture 和两条后台 GUI 链路。带云场的 4 反弹 capture 准备 654 个首表面样本，实际应用 652 个，记录了时间及空间复用。240×240 测试视口在默认设置下交付 116 次原尺寸运动更新。在面板录入 160 后，Scene media v2 交付 103 次 160×160 运动更新，停稳后恢复 240×240。计数仅用于确认交付，未测量原生桌面分辨率的帧率。

- [表面遮挡能量与 Sponza 两项运动回归](../../out/verify/20261119-142710-cdc4904b/summary.json)通过 3/3，[最终视口命令复验](../../out/verify/20261119-144146-cffc527a/summary.json)通过 9/9。本次共通过 19 个不同的定向用例。未运行全量、Debug、跨 GPU 矩阵或新模式的等时间性能对比。

以下命令可在同一构建树复验介质 v2 数值与实际 GUI 行为。其余各组实际执行的目标和测试名见上述日志。

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target @('dk_pathtracing_probe','dk_editor_app','dk_ctl') -TestRegex '^dk\.(pathtracing\.restir_medium_v2_gpu_required|editor\.(restir|media)_pathtracing_gpu_validation)$' -Reason '介质 v2 与运动预览'
```

## 使用边界

ReSTIR 复用到达首表面的灯光样本。云内和大气内的散射仍依赖介质采样。原尺寸运动预览的成本随实际视口、云密度和反弹数变化，测试未给出统一的交互帧率。

## Sponza 原生视口复测

扩展独立探针，以同一 Sponza 稀疏云场复测运动长边 0、640 和 192。性能通过后台 GUI 注入连续 SDL 转向，按已提交的新相机计时。画质通过编辑器重放相同姿态序列，保存实际交付的 3 spp 图、相机矩阵、镜头、几何和云场哈希。旧版 v1 对照仍保留原配置。[基准报告与复测入口](../benchmarks/2026-11-19-media-resolution.md)

[Release 编辑器和测量工具构建与旧探针回归](../../out/verify/20261119-144937-5ecc65cc/summary.json)通过。参考相机首次误用了光栅大气的裁剪限制，改用编辑器路径追踪的镜头设置后，[重建与回归](../../out/verify/20261119-150018-0ae65c60/summary.json)通过。三组 48/96 spp 参考均保留同一稀疏云场，重放实测也通过 Vulkan 同步验证。12 次性能尝试中有三次 GPU 路径错误，所有日志已保留，三档各完成三轮。此次未修改积分器，未验证其他 GPU 或天气预设。
