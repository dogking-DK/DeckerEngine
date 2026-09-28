---
id: "0042"
created_at: "2026-09-28T16:38:00+08:00"
updated_at: "2026-09-28T16:56:45+08:00"
status: completed
design_refs:
  - ../design/graphics-device.md
---

# 0042 M5.1 Vulkan 设备与诊断

## 目标与设计依据

完成 [graphics-device](../design/graphics-device.md) 的设备创建、能力选择、验证诊断和失败清理；
保持 CPU-only，无窗口、无资源提交。按 Roadmap 只推进 M5.1。

## 实际变更

先建立模块设计后实现 [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp)、
[Device.cpp](../../engine/graphics/device/src/Device.cpp) 和纯策略选择函数。
实例独立动态装载 Vulkan；创建 instance、验证 messenger、logical device 和单个 graphics/compute 队列，
只启用 timelineSemaphore、synchronization2、dynamicRendering，要求 Vulkan 1.3。
自动优先合格独显；显式枚举索引不回退。无设备或缺少能力时返回可复现的诊断。

ValidationMode 支持 disabled/if_available/required，缺少必需验证环境不降级；
消息 sink 保留 severity/type/id/text，覆盖创建与销毁，默认 warning/error 输出 stderr。
CPU 拥有对象和枚举容器使用调用者 Memory resource；分配异常与中途 Vulkan 失败均逆序清理。
公开 Device 包装可移动，内部 callback 状态地址固定；不提供 GPU 提交或隐式 wait-idle。

[CMake 设备入口](../../engine/graphics/device/CMakeLists.txt) PUBLIC 依赖 Vulkan::Headers/Core/Memory；
新增 DK_BUILD_GRAPHICS_DEVICE、vulkan-device feature 和 windows-graphics 预设，windows-dev 保持 CPU-only。
官方 vulkan/headers/loader 版本核验与现有 baseline 一致，保留固定基线。
vulkan 2023-12-17，headers/loader 1.4.357.0；未接入 VMA/Slang/SDL。

新增 [设备单元测试](../../tests/unit/DeviceTests.cpp)、[GPU 探针](../../tests/integration/DeviceProbe.cpp)，
同步测试选择表、[Graphics 指南](../guides/graphics.md)、构建/依赖文档、设计索引与 Roadmap。

## 验证记录

环境：Windows x64 / Windows SDK 10.0.26100.0，VS2026 MSVC 19.51.36257，Debug 动态 CRT。
vcpkg 库由 MSVC 14.51 toolchain 解析并成功实际链接；头文件来自固定 baseline 的 1.4.357.0。
系统 loader 文件版本 1.4.321.1，Vulkan SDK/Khronos validation 1.4.321.1。
vulkaninfoSDK --summary 枚举 RTX 4070 Laptop（NVIDIA 596.49，API 1.4.329）和
AMD 610M（24.10.36，API 1.3.287）。模块自动选择 RTX 4070，queue family 0。

| 验证 | 结果 | 证据目录 |
| --- | --- | --- |
| windows-graphics，dk_device_tests + dk_device_probe，`^dk\.device\.` | 13/13，通过；11 CPU + 2 GPU，无跳过 | out/verify/20260928-164740-a6c83939 |
| windows-graphics /WX，两个 target 构建，`^dk\.device\.unit\.` | 清除测试 strcpy 警告后 11/11；编译零 warning/error | out/verify/20260928-165144-7f2c323c |
| windows-graphics /WX，dk_device_probe，`^dk\.device\.gpu_` | 最终 dk-device-probe 命名及 CTest 入口，2/2，无跳过 | out/verify/20260928-165508-5d681562 |
| windows-dev /WX，dk_run，`^dk\.(bootstrap\.version\|runtime\.batch_roundtrip)$` | 2/2，CPU 版本与场景保存/重载通过 | out/verify/20260928-164936-bd5a320a |

以上脚本为 [verify.ps1](../../scripts/verify.ps1)，目标、筛选、工作区状态与结果见各目录 summary.json，
GPU 输出保存在 results.xml。关闭/开启验证各重复创建和销毁 3 次，均 errors=0、warnings=0、
liveAllocations=0；验证路径 routed=3，确认消息经真实 debug messenger 投递且销毁后无错误。
单元测试覆盖能力与显式选择、单队列、验证策略、缺失 loader/设备、创建各阶段失败、
枚举 VK_INCOMPLETE 重试/上限、移动后 callback、分配异常逆序清理；这些不需要 GPU。
dumpbin /DEPENDENTS 核对 CPU dk-run 无 Vulkan/窗口库，CPU CMakeCache 的设备开关为 OFF。
`scripts/check-spec.ps1` 通过：91 个 Markdown、901 条本地链接及元数据/表格/索引/测试入口/JSON；
首次检查发现测试选择表 regex 的未转义竖线，简化 GPU 前缀后通过。`git diff --check` 通过。

失败记录：首次配置受 vcpkg 缓存写权限限制，获得工具权限后按原预设成功；
首次编译泛型回调的 nullptr 类型推导失败，改为明确类型。
MSVC 随后在原泛型算法/回调组合处异常退出 -1073741819；使用显式参数回调及简单遍历后编译通过，
未更换工具链或关闭模块。一次沙箱 MSBuild FileTracker 权限失败，使用已授权构建环境解决。
失败构建均由 verify 停止，未用旧二进制记为通过。

## 偏差与决策

使用动态 loader 和每实例函数表，使 loader 缺失也能返回结构化错误；不新增 volk/vk-bootstrap。
首版固定 Vulkan 1.3 所需 feature，避免在 M5.2 才发现设备不支持同步基础。
Windows 默认加载系统 loader，不将 vcpkg loader 的依赖安装误记为该二进制的实际运行验收。

## 遗留问题与下一步

本阶段范围无未解决问题。下一项为 M5.2：VMA Buffer/Image、提交完成跟踪、延迟销毁和 Tracy GPU 边界。
未运行全量、Release、Linux/macOS 或其他 GPU；AMD 只枚举，未进行设备创建验收。
未实现/验证 GPU 资源与提交、shader、离屏绘制、呈现；未进行 Tracy 性能采集。

## 修改记录

- 2026-09-28T16:38:00+08:00：建立设计和开发记录。
- 2026-09-28T16:55:00+08:00：完成 M5.1 实现、定向验收和 CPU-only 回归，更新阶段状态。
