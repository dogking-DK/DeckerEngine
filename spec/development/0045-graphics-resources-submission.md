---
id: "0045"
created_at: "2026-09-28T18:19:00+08:00"
updated_at: "2026-09-28T18:40:00+08:00"
status: completed
design_refs:
  - ../design/graphics-resources.md
  - ../design/graphics-device.md
---

# 0045 M5.2 资源与提交生命周期

## 目标

按 [资源与提交设计](../design/graphics-resources.md) 完成 VMA Buffer/Image、上传/读回、单队列
timeline 完成跟踪、固定槽复用和延迟释放；继续 Vulkan-Hpp RAII 与 CPU-only 边界。
Tracy GPU 接入边界在 graphics 内定义，不宣称 GPU capture 已交付。

## 决策与实际实现

[Resources.hpp](../../engine/graphics/device/include/dk/graphics/Resources.hpp) 提供 move-only Buffer/Image、
CommandBatch、SubmissionQueue 和带 owner 身份的 Submission。普通 Vulkan 对象使用 vk::raii；
VMA allocation 只经 vmaDestroyBuffer/Image 释放，所有引擎控制块和保留列表使用 Memory resource。
Vulkan-Hpp、volk、vk-bootstrap、VMA 均复用现有固定版本，无依赖新增/升级。

[Resources.cpp](../../engine/graphics/device/src/Resources.cpp) 在同 dk::graphics_device target 内实现：

- 单独共享设备寿命，资源可晚于队列销毁；pending 持有资源但资源不反向持有队列，无所有权环。
- upload/readback 映射与 flush/invalidate，设备内存角色、范围/usage/对齐/溢出检查；
  单 mip/layer 二维 4 字节 color image，格式/extent 能力由物理设备检查。
- 1–64 固定提交槽、RAII command pool/buffer 和 timeline semaphore；提交成功才发布票据、布局与 pending。
  提交点后只做 noexcept 所有权转移，不再分配 CPU 控制块/列表。
- GPU 完成前保留 allocation，CPU read/write 被录制/pending 引用保护；wait timeout 不回收，poll/wait 确认后复用。
- image 独占录制预约和局部 predicted layout；放弃/失败不改全局布局，成功提交后允许同队列后续批次使用。
- synchronization2 保守 barrier，buffer/image 上传读回；跨 owner 资源/票据拒绝。
- close/最终析构排空 pending，Device lost 进入终态；其他等待错误保留状态以供重试。
  析构 wait/queue idle 均失败时 fail-stop，不释放无法确认已停止使用的资源。

增加资源描述/操作单元测试、GPU 数据/寿命探针与内部 driver failure seam，更新 CMake 和测试选择表。
同步 README、架构/设备/资源/Profiling 设计、Graphics 指南、三方说明、索引与 Roadmap。
仅加入 CPU profiling zones；Tracy GPU context、query 与 slot 的接入寿命契约已定义，未启用 GPU timestamp/capture。

## 验证记录

环境：Windows x64，MSVC 19.51.36257，Debug /W4 /WX，动态 CRT，DK_ENABLE_PROFILING=OFF。
RTX 4070 Laptop、NVIDIA 596.49、Vulkan API 1.4.329；系统 loader / SDK 验证层 1.4.321。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_graphics_resource_tests', 'dk_graphics_resource_probe') -TestRegex '^dk\.graphics\.' -Reason '验证 M5.2 资源与提交'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.' -Reason '复验设备链路'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_roundtrip)$' -Reason '确认 CPU-only 边界'
```

| 检查 | 结果与证据 |
| --- | --- |
| 资源 CPU + GPU | 7/7 通过、0 跳过，`out/verify/20260928-183320-b909251d` |
| 新增提交点关闭 Memory 分配检查后的 GPU 复验 | 2/2 通过、0 跳过，`out/verify/20260928-183629-36ac5e9b`；CPU 策略代码未改变 |
| 设备回归 | 16/16 通过、0 跳过，`out/verify/20260928-182309-f190dfe3`；Device 实现未改变 |
| CPU-only runner | 2/2 通过、0 跳过，`out/verify/20260928-182756-f9d77c7f` |
| 二进制依赖 | dumpbin /DEPENDENTS 检查资源 probe 与 CPU runner，均无 Vulkan DLL 静态导入 |
| 开发中失败 | 探针 helper 最初仅支持 dk::Error，无法接收 Memory AllocationError；构建停止且未运行测试，添加专用重载后由上述运行复验通过 |

每个资源 GPU probe 16 轮 buffer/image 数据往返、5 种格式；主队列 completed=33、pending=0、VMA allocations=0。
验证层 errors=0、warnings=0，全部资源最终释放后 Memory liveAllocations=0。
覆盖 submit OOM/lost 注入、timeout/等待错误保留与重试、poll 回收、slot 饱和/复用、
放弃与移动 batch、布局回滚、跨队列拒绝、资源提前/延后销毁、析构排空和 CPU 分配失败保护。
测试还在 Memory begin_close 之后成功提交已准备 batch、等待并读回，确认提交点不依赖新引擎 CPU 分配。

同步验证通过 CTest 环境开启；核验本机 SDK 对应
[同步验证说明](https://github.com/KhronosGroup/Vulkan-ValidationLayers/blob/vulkan-sdk-1.4.321.0/docs/syncval_usage.md)
使用 VK_LAYER_VALIDATE_SYNC，新版本前缀 VK_VALIDATION_VALIDATE_SYNC 同时配置兼容。
`scripts/check-spec.ps1` 通过（95 个 Markdown、942 个本地链接及元数据/表格/索引/测试入口/JSON），
`git diff --check` 通过。

## 限制与下一项

未运行全量、Release、Linux/其他 GPU、profiling ON 或 Tracy GPU capture；未主动触发真实硬件 device lost/OOM。
失败注入不会在真实 pending GPU 工作仍运行时伪造 lost 并释放它；不是硬件恢复验收。
仅单队列、调用者串行访问、单 mip/layer 四字节 color 格式；barrier 保守，未做性能优化。
GPU timestamp zone/capture 尚未启用，M5.2 只交付其接入边界。下一项为 M5.3 Slang 编译工具。
