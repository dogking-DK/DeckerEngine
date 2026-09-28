---
created_at: "2026-09-28T16:49:00+08:00"
updated_at: "2026-09-28T18:34:00+08:00"
---

# Vulkan 设备、资源与提交

[返回项目入口](../../README.md)。当前提供设备、VMA Buffer/Image、单队列提交、上传/读回与延迟释放；
尚无绘制、shader 或窗口。接口与生命周期见 [设备设计](../design/graphics-device.md) 和[资源设计](../design/graphics-resources.md)。

## 配置和运行

Windows x64 使用支持 Vulkan 1.3 的驱动。需要同一队列支持 graphics/compute，
以及 timelineSemaphore、synchronization2、dynamicRendering。
验证层测试还需要安装 Vulkan SDK 的 VK_LAYER_KHRONOS_validation 和 EXT_debug_utils。
vcpkg 提供编译头文件和 loader 包，不安装显卡驱动或验证层。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.' -Reason '验证 Vulkan 设备与诊断'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_graphics_resource_tests', 'dk_graphics_resource_probe') -TestRegex '^dk\.graphics\.' -Reason '验证资源、提交和数据往返'
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
底层 Device 不提供提交或隐式等待；以下 SubmissionQueue 提供提交与等待，且会在最终析构时排空自身工作。
错误包含 Vulkan 操作名、VkResult 符号与数值。loader_path 留空使用系统 loader；
绝对路径可用于部署或复现缺失 loader，不能传相对路径。

初始设备验收见 [0042](../development/0042-vulkan-device.md)，三方接入和 VMA 结果见
[0043](../development/0043-vulkan-libraries.md)，Hpp RAII 迁移见 [0044](../development/0044-vulkan-hpp-raii.md)。
M5.2 的资源与提交验收见 [0045](../development/0045-graphics-resources-submission.md)。

## 上传与读回

文件包含 `<dk/graphics/Resources.hpp>` 和 `<array>`，在上述 Device 创建成功后消费设备：

```cpp
auto queue = dk::graphics::SubmissionQueue::create(*heap, std::move(*device), 3);
if (!queue) return 1;
std::array<std::byte, 64> input{}, output{};
input.fill(std::byte{0x5a});
using dk::graphics::BufferMemory;
const auto usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
auto upload = queue->create_buffer({input.size(), usage, BufferMemory::upload});
auto gpu = queue->create_buffer({input.size(), usage, BufferMemory::device});
auto readback = queue->create_buffer({input.size(), usage, BufferMemory::readback});
if (!upload || !gpu || !readback || !upload->write(0, input)) return 1;
auto batch = queue->begin();
if (!batch) return 1;
if (!batch->copy(*upload, *gpu, input.size()) || !batch->copy(*gpu, *readback, input.size())) return 1;
auto ticket = queue->submit(std::move(*batch));
if (!ticket) return 1;
auto completed = queue->wait(*ticket);
if (!completed || !*completed || !readback->read(0, output) || output != input) return 1;
if (!queue->close()) return 1;
```

创建队列后不再使用移后的 Device；queue.device() 只借用设备。factory 预检失败不消费设备，
开始创建后发生失败则销毁已消费设备。队列、资源和相关 Vulkan 访问由调用者串行执行，
不要绕过队列提交/重置内部 command buffer 或 signal 内部 timeline。

Image 使用 `create_image({width, height})`，默认 RGBA8、transfer src/dst + sampled。
`batch.copy_to_image(upload, image)` 与 `copy_to_buffer(image, readback)` 复制整图，缓冲至少 width×height×4 字节，
自动处理 transfer layout；`transition(image, vk::ImageLayout::eShaderReadOnlyOptimal)` 可将其准备为只读布局。
支持单 mip/layer 二维 RGBA8 UNORM/SRGB、BGRA8 UNORM、R32_UINT/FLOAT；其他格式和复杂 subresource 待扩展。

begin() 在固定槽（1–64）用尽时返回 conflict；调用 poll() 或 wait() 确认完成后才复用。
wait 的超时单位为 ns，返回 false 时资源仍在使用；默认等待无超时。票据不能跨队列使用。
Buffer 的 CPU 访问在录制和 pending 期间被拒绝，wait/poll 回收引用后才允许访问。
VMA 负责 non-coherent flush/invalidate；flush 失败不保证已写 host 字节回滚。

丢弃 batch 会取消未提交录制；提交失败不更新票据或 image 全局布局。一个 image 同时只能由一个 batch 录制，
成功提交后即可在同队列下一批继续使用。包装提前销毁时队列保留 allocation 到 GPU 完成；
buffer/image 可晚于 queue 销毁，它们会继续持有设备，因此诊断 sink 和 Memory 系统也须保持存活。
close() 遇录制中的 batch 返回 conflict；成功后拒绝新工作。队列最终析构等待 pending 工作，
device lost 为终态；其他等待失败保留资源以供重试。API 的 bad_alloc 不转换为 GPU 成功/完成。

内置同步以正确性为先，采用保守 barrier；目前未做异步传输队列或 barrier 合并优化。
手工录制可借用 batch.command_buffer()，必须先 retain 所有使用资源，并经 transition 更新 image 布局。
该路径不追踪手工创建的 image view/pipeline 等子对象，调用者须将它们保持到票据完成。

资源 GPU 探针每次验证 16 轮 buffer/image 往返、5 种格式、提交失败/超时/等待错误、跨队列拒绝、
延迟释放和关闭。验证用例通过环境设置开启 Khronos 同步验证（兼容旧 SDK 的 VK_LAYER_VALIDATE_SYNC
和新版本的 VK_VALIDATION_VALIDATE_SYNC），无环境返回 77；不将跳过当通过。
Tracy 当前标记 CPU 创建/submit/wait/collect；GPU query/context 的寿命契约已确定，
GPU timestamp zone/capture 尚未启用，见[接入边界](../design/graphics-resources.md#tracy-gpu-接入边界)。
