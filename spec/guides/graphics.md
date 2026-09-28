---
created_at: "2026-09-28T16:49:00+08:00"
updated_at: "2026-09-28T17:37:00+08:00"
---

# Vulkan 设备与诊断

[返回项目入口](../../README.md)。当前提供 M5.1 设备底座；尚无绘制、资源提交、shader 或窗口。
接口与生命周期见 [设备设计](../design/graphics-device.md)。

## 配置和运行

Windows x64 使用支持 Vulkan 1.3 的驱动。需要同一队列支持 graphics/compute，
以及 timelineSemaphore、synchronization2、dynamicRendering。
验证层测试还需要安装 Vulkan SDK 的 VK_LAYER_KHRONOS_validation 和 EXT_debug_utils。
vcpkg 提供编译头文件和 loader 包，不安装显卡驱动或验证层。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.' -Reason '验证 Vulkan 设备与诊断'
```

windows-graphics 继承 CPU 开发预设，额外开启 DK_BUILD_GRAPHICS_DEVICE；默认 windows-dev 保持 CPU-only。
设备模块需要 DK_BUILD_MEMORY。vulkan-device feature 安装 Vulkan、volk、vk-bootstrap 和 VMA；
不自动安装 Slang 或 SDL。volk 提供函数表，vk-bootstrap 构建 instance，VMA 管理设备内存分配。
Vulkan-Hpp 随 vulkan-headers 提供；使用 vulkan_raii.hpp 的 vk::raii 管理普通 Vulkan 对象。

探针可单独运行，打印设备/API/驱动/队列/验证状态与诊断计数：

```powershell
.\out\build\windows-graphics\bin\Debug\dk-device-probe.exe
.\out\build\windows-graphics\bin\Debug\dk-device-probe.exe --validation
```

每次探针执行 3 轮双设备交叠生命周期，并检查 VMA buffer 分配、映射、flush/invalidate、释放与零活 allocation；
同时通过公开 RAII device 创建/移动/销毁 fence 和 command pool，第一台设备销毁后继续使用第二台。
验证探针还通过 RAII instance 的 debug utils 接口投递 3 条 info 消息。
CTest 的两个探针标记 gpu，无必需设备或验证环境时返回 77 并显示 Skipped；跳过不代表 GPU 验证通过。
实际设备创建错误、诊断错误、消息未投递或未释放分配返回 1。
CPU 策略/模拟失败测试不加载 GPU，可单独筛选 `^dk\.device\.unit\.` 并仅构建 dk_device_tests。

## 嵌入模块

链接 dk::graphics_device 后，用 render 分类的 Memory heap 创建 Device：

```cpp
auto memory = dk::memory::MemorySystem::create();
if (!memory) return 1;
auto heap = memory->create_heap({"graphics", dk::memory::DomainCategory::render});
if (!heap) return 1;
dk::graphics::DeviceOptions options;
options.validation = dk::graphics::ValidationMode::required;
auto device = dk::graphics::Device::create(*heap, options);
if (!device) {
    // 记录 device.error().message 与 context。
    return 1;
}
```

自动选择满足要求的独显，其次集显等；adapter_index 可显式指定当次枚举索引，不满足要求就失败。
ValidationMode::if_available（默认）在环境缺失时报告原因并明确返回未启用状态；required 不降级。
默认 warning/error 输出到 stderr；可提供线程安全 noexcept DiagnosticSink 接收完整 severity/type/id/text。
sink 的 user data 必须活到创建失败或 Device 销毁返回。

`instance()`、`physical_device()`、`logical_device()`、`queue()` 返回只读 vk::raii 引用。
包含 Device.hpp 即可使用 Vulkan-Hpp；例如在上面的 device 作用域内创建临时子资源：

```cpp
try {
    vk::raii::Fence fence{device->logical_device(),
        vk::FenceCreateInfo{vk::FenceCreateFlagBits::eSignaled}};
    if (fence.getStatus() != vk::Result::eSuccess) return 1;
} catch (const vk::SystemError& error) {
    // Hpp 操作默认抛出 vk::SystemError；工厂仍返回 dk::Result。
    (void)error;
    return 1;
} // fence 先于 device 自动销毁。
```

RAII 引用和解引用后的 vk::* 句柄均借用；不得重新包装为另一拥有型对象。
需要 C 互操作时使用 `static_cast<VkInstance>(*device->instance())` 或 `device->native_device()`。
调用者保证线程同步，所有子资源及 GPU 工作先于 Device 结束；Device 移动不改变已借用对象的地址。
`device->allocator()` 返回借用的 VmaAllocator；调用 VMA API 创建的所有 allocation 必须先于 Device 释放，
不要自行销毁 allocator。VMA 创建的 buffer/image 由 VMA 配对释放，不能再交给 vk::raii::Buffer/Image 销毁。
引擎 target 传递 VK_NO_PROTOTYPES、VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0 和 VULKAN_HPP_NO_DEFAULT_DISPATCHER；
优先通过 RAII 方法调用，特殊 C 扩展可用 Device 的 proc 接口。不依赖 volk/Hpp 全局函数表，
不要覆盖这些编译定义。VMA 使用 Vulkan 1.2 API 路径，设备仍要求 Vulkan 1.3。
引擎对象/容器使用 Memory heap；Hpp dispatcher、vk-bootstrap/VMA 的内部 CPU 元数据使用三方默认分配器，
不计入该 heap 的用量。GPU allocation 由 VMA 单独统计。
本阶段不提供提交或等待封装，不能把析构当作隐式 wait-idle。
错误包含 Vulkan 操作名、VkResult 符号与数值。loader_path 留空使用系统 loader；
绝对路径可用于部署或复现缺失 loader，不能传相对路径。

初始设备验收见 [0042](../development/0042-vulkan-device.md)，三方接入和 VMA 结果见
[0043](../development/0043-vulkan-libraries.md)，Hpp RAII 迁移见 [0044](../development/0044-vulkan-hpp-raii.md)。
当前只交付设备和 allocator 接入，M5.2 资源封装/提交仍待实现。
