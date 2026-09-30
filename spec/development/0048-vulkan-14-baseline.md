---
id: "0048"
created_at: "2026-09-30T09:37:00+08:00"
updated_at: "2026-09-30T09:44:00+08:00"
status: completed
design_refs:
  - ../design/graphics-device.md
---

# 0048 Vulkan 1.4 运行基线

## 目标与实现

按用户要求与[设备设计](../design/graphics-device.md)，将 instance、loader/显卡要求、VMA API 统一为 1.4，
显式启用 maintenance4 并接入核心内存需求查询。普通对象继续使用 vk::raii；此变更不推进 M5.5。

- [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp) 的 device_api_version 改为 VK_API_VERSION_1_4，
  AdapterInfo 增加 maintenance4；[Selection](../../engine/graphics/device/src/Selection.cpp) 拒绝版本不足或特性缺失并报告原因。
- [设备工厂](../../engine/graphics/device/src/Device.cpp) 在创建 instance 前拒绝 1.3 loader，
  vk-bootstrap 使用同一 1.4 常量；旧显卡不接收新 feature 结构查询，也不创建 logical device。
  版本 variant 非零同样拒绝。API 1.4.0 可接受，不将 header patch 当作最低设备 patch。
- 显式查询/启用 Vulkan13Features::maintenance4，继续只启用所需特性。
  将 volk table 的 vkGetDeviceBufferMemoryRequirements / vkGetDeviceImageMemoryRequirements 注入 VMA，
  allocator_info.vulkanApiVersion 与 instance 同为 1.4；缺少查询入口时返回错误，按既有所有权逆序清理。
- CMake find_package(VulkanHeaders 1.4 CONFIG REQUIRED)；已固定的 vulkan-headers 1.4.357.0、VMA 3.4.0 满足要求。
  本次核验官方 [headers port](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-headers/vcpkg.json) 和
  [VMA port](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-memory-allocator/vcpkg.json)，版本与 baseline 一致且无修订。
  不修改 `33d78c1ed898a06938f31312167c7abefd229455` baseline，不升级本机 SDK/驱动。
- [单元测试](../../tests/unit/DeviceTests.cpp) 检查真实传入的 Instance/VMA 版本、特性/函数指针、
  1.3.4095 拒绝、1.4.0/1.4.1 接受、variant/maintenance4 缺失及入口缺失后的回收。
  [GPU probe](../../tests/integration/DeviceProbe.cpp) 输出 configuredAPI=1.4，实际执行 VMA buffer/image 预创建内存类型查询。
  同步使用指南、依赖说明与设计索引；旧阶段记录保留当时环境和结果。

参考 [Khronos 版本指南](https://docs.vulkan.org/guide/latest/versions.html) 和
[VMA 配置](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/struct_vma_allocator_create_info.html)。
Vulkan 1.4 接受 SPIR-V 1.6 及以下，已有 SPIR-V 1.5 编译契约无需调整。

## 定向验证

Windows x64、MSVC 19.51.36257、Debug /W4 /WX，RTX 4070 Laptop、NVIDIA 596.49、GPU API 1.4.329，
Khronos 验证层来自 Vulkan SDK 1.4.321.1；Slang 2026.18。没有运行全量、Release、其他平台/显卡或 profiling ON。
CPU runner 的链接/配置未改，本次不重复其上一阶段验证。

```powershell
cmake --preset windows-graphics -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe', 'dk_graphics_resource_probe', 'dk_offscreen_probe') -TestRegex '^dk\.(device\.|graphics\.gpu_|offscreen\.gpu_)' -Reason '验证 Vulkan 1.4 版本门槛、maintenance4/VMA 配置和资源离屏 GPU 链路'
```

配置/构建成功。首轮 `out/verify/20260930-093929-442ee49e`：21 项中 19 通过、2 失败、0 跳过。
15 项 CPU 全通过，三个非验证 GPU 用例与 device 验证通过；offscreen 和 resources 的严格零警告断言失败。
两者数据结果、寿命与零活分配断言通过，errors=0，分别 warnings=4/7，全部来自
`VK_LAYER_AMD_switchable_graphics uses API version 1.3 ... application ... 1.4`。
本机 AMD manifest 的 layer/ICD 声明均为 1.3.287。

使用 Loader 通用 `VK_LOADER_LAYERS_DISABLE=VK_LAYER_AMD_switchable_graphics` 隔离后，
`out/verify/20260930-094138-2705ff0a` 仍为 1/3 通过：同一层产生 forced-disabled warning，
不将这个结果作为零警告通过证据，也不修改诊断统计来忽略消息。

最终使用已读取的 AMD manifest 自身 `disable_environment` 开关，仅对子进程设置，执行后恢复原值：

```powershell
$previousAmdLayer = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_probe', 'dk_graphics_resource_probe', 'dk_offscreen_probe') -TestRegex '^dk\.(device\.gpu_validation|graphics\.gpu_resources_validation|offscreen\.gpu_validation)$' -Reason '使用 AMD manifest 声明的停用开关隔离旧隐式层，保留 Vulkan 1.4 Khronos/同步验证'
} finally {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previousAmdLayer
}
```

最终 `out/verify/20260930-094231-6b3e3660`：3/3 通过、0 跳过；全部 errors=0 warnings=0。
Khronos 显式验证层保持 required，resources/offscreen 同步验证保持开启；未修改系统注册表、持久环境或第三方文件。
跨上述有效结果覆盖 21 个不同测试；不重复计数重跑项。设备 probe 3 轮双设备及 VMA/RAII 查询销毁通过；
资源 probe 16 轮、5 种格式、33 次完成、零 pending/allocation；离屏 12 组绘制/计算、3 轮寿命及故障注入通过，
pending/VMA/Memory 均清零。默认系统环境的旧 AMD 层警告仍存在，使用限制与复现命令已写入[指南](../guides/graphics.md)。

`scripts/check-spec.ps1` 通过：103 个 Markdown、1024 个本地链接、元数据/表格/索引/测试入口/JSON；
`git diff --check` 通过。

## 交接

Vulkan 1.4 配置与验收完成，最低设备能力的提高是本次有意的兼容性变化。
没有启用全部 Vulkan 1.4 可选 feature，也没有验证物理 GPU 丢失或跨平台行为。
后续 M5.5 窗口与呈现继续按 Roadmap 独立实施。
