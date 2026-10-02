---
id: "0056"
created_at: "2026-10-02T22:23:00+08:00"
updated_at: "2026-10-02T22:46:00+08:00"
status: completed
design_refs:
  - ../design/graphics-presentation.md
  - ../design/platform.md
  - ../design/graphics-device.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
---

# 0056 M5.6.3 呈现恢复与集成验收

## 范围

依据 [Presentation](../design/graphics-presentation.md)，覆盖实际 resize/minimize/restore、
out-of-date/suboptimal、获取/提交/呈现/候选构造失败、surface/device lost、Memory 关闭与代际回收。
提供窗口三角形示例，完成指南与 CPU/离屏隔离回归；M5.6 全部子阶段通过后关闭 M5。

## 验证计划

复用 private API seam 作一次性失败注入；已入队的 present 错误先调用真实 present，保证 fence 信号行为真实。
device lost 只在没有在途 GPU/呈现工作时模拟，不将真实仍在使用的资源当作 lost 后强制销毁。
测试超时/错误后的资源保留与重试、创建候选后失败以及仍被用户 view 保留的退休代际。
窗口呈现同步验证、可选实际图像读回、CPU 策略、离屏 GPU 与独立 CPU runner 分别记录结果。

## 实际变更

- 新增 recovery probe：实际三轮 resize/minimize/restore；TIMEOUT/NOT_READY、acquire OOM、suboptimal/out-of-date、
  present fence 超时/错误、退休代际仍有 view、native swapchain 创建后失败、获取后 view 创建失败、submit OOM、
  present OOM、surface lost、无在途工作的 device lost 与获取后 Memory 关闭/分配异常。
- 将实例依赖补齐/去重放入 Device，避免外部 SurfaceSource 漏掉 maintenance1 的实例依赖；
  create_present_device 先拒绝非法/Closing Memory，Presenter.stats 反映共享设备 lost，close 如实报告 lost 终态。
- 新增 dk-presentation-demo，使用公开 Window/Presenter/factory/encoder 接口；无原始 Vulkan 创建、录制或提交调用。
  支持持续窗口运行、--validation、--frames N；缺少验证层时默认 if_available 不妨碍正常结束。
- 设备窗口 probe 的诊断计数改为原子，满足回调线程安全契约。
- 同步 Presentation/Platform/Device/Resources/架构、README、构建/Graphics/呈现指南、测试选择表及 Roadmap。

## 实际验证

全部为 Windows x64 Debug。GPU：RTX 4070 Laptop，NVIDIA 596.49，API 1.4.329。
窗口/离屏 shader 均复用 Slang SPIR-V 1.5；窗口 vertexMain/fragmentMain 来自 common/triangle.slang。
Khronos validation 和同步验证开启。沿用 0048 的进程内 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，
结束恢复；没有关闭 Khronos validation，也没有把跳过记为通过。

| out/verify 证据 | 范围 | 结果 |
| --- | --- | --- |
| 20261002-222610-44024ce5 | Device 策略 + 首轮恢复 | 16 通过、1 失败；夹具未等到 acquire 注入点，见下文 |
| 20261002-223159-9eb53127 | recovery_validation、公开 example_smoke | 2/2 通过，零跳过 |
| 20261002-223618-319d9257 | 独立 windows-graphics：Offscreen CPU 策略与 GPU 同步验证 | 4/4 通过，零跳过 |
| 20261002-223625-3182ce8f | 独立 windows-dev：版本、batch 正常/错误 | 3/3 通过，零跳过 |
| 20261002-224042-a2dc0fae | 最终 4 项 Presentation CPU、线程安全 Device probe、普通 144 帧路径 | 6/6 通过，零跳过 |

首轮恢复夹具错误：在途帧槽可能先超时，尚未进入 acquire API，夹具就断言注入的 OOM 必须返回。
改为确认一次性 acquire hook 已被消费后断言，不依赖驱动时序或额外固定 sleep。
该项在后续 recovery 通过；未修改实现以绕开失败。文档检查曾发现表内 regex 的竖线未转义，修正后通过。

恢复 probe 观测 13 个交换链代际、27 次 render submit、23 次成功/次优呈现、6 次放弃；
图像/候选构造失败无状态发布，present OOM 不回滚已提交渲染；每次可恢复错误后的正常帧再次成功。
另验证无实际在途工作时的 device lost，以及获取后分配异常清理。
示例呈现 8 帧。普通 probe 重跑 3 次窗口生命周期共 144 帧并读回首尾图像。
窗口和离屏均零验证错误/警告，Memory/VMA 无残留。

实际命令范围：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-presentation `
  -Target @('dk_presentation_probe','dk_presentation_demo') `
  -TestRegex '^dk\.presentation\.(recovery_validation|example_smoke)$' -Reason 'M5.6.3 recovery and example'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_offscreen_tests','dk_offscreen_probe') `
  -TestRegex '^dk\.offscreen\.(unit\.|gpu_validation$)' -Reason 'M5.6 independent headless regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run `
  -TestRegex '^dk\.(bootstrap\.version$|runtime\.batch_(roundtrip|errors)$)' -Reason 'M5.6 CPU isolation'
& ./scripts/verify.ps1 -BuildDir out/build/windows-presentation `
  -Target @('dk_presentation_tests','dk_present_device_probe','dk_presentation_probe') `
  -TestRegex '^dk\.presentation\.(unit\.|device_validation$|frames_validation$)' -Reason 'M5.6 final affected probes'
```

结合 [0054](0054-window-device.md) 的 18 项与 [0055](0055-swapchain-frames.md) 的 12 项，
M5.6 共 **40 个不同用例通过**，重复回归不重复计数。
版本化 verify 脚本保存每次 targets、regex、工作区状态和 JUnit；本次额外聚合为 out/m56-verification.json。

隔离证据：windows-dev 的 DEVICE/SHADERS/OFFSCREEN/PRESENTATION/PLATFORM 均 OFF；
windows-graphics 的 PLATFORM/PRESENTATION OFF。MSVC dumpbin /dependents 确认 CPU runner 无 SDL/Vulkan/Slang 直接导入，
离屏 probe 无 SDL 导入，present-device probe 有 SDL 而无 Slang 导入；同时核对 target 链接依赖。
文档检查通过（115 个 Markdown，最终链接数以末次检查为准）；git diff --check 通过。
示例源码扫描无 vk::raii/CreateInfo/unsafe_record/command_buffer/原始 acquire/present。

## 限制与交接

M5.6.1–3 全部完成，M5 关闭；下一项 M6.1 图声明与结构校验，开工前建立 graphics-graph 设计。
未运行全量、Release、其他 GPU/平台或 Tracy GPU capture。第一版仅同 graphics/compute/present 队列、FIFO、
sample=1，要求 EXT swapchainMaintenance1；不支持 HDR、独占全屏或多窗口共享设备调度。
重建/close 阻塞排空；未知驱动状态无法证明完成时 fail-stop，不强行销毁同步对象。
没有新增包版本或变更固定 builtin-baseline。
