---
created_at: "2026-09-28T16:49:00+08:00"
updated_at: "2026-10-02T21:26:00+08:00"
---

# Vulkan 使用层：资源、管线、录制与提交

[返回项目入口](../../README.md)。当前提供资源工厂、管线/绑定、类型化录制、显式同步、单队列提交与异步读回；
离线 shader 编译见 [Shader 指南](shaders.md)，真实 draw/dispatch/readback 见[离屏指南](offscreen.md)。
窗口呈现尚未实现。接口与生命周期见 [设备设计](../design/graphics-device.md) 和[资源设计](../design/graphics-resources.md)。

## 配置和运行

Windows x64 使用支持 Vulkan 1.4 的 loader 和显卡驱动。需要同一队列支持 graphics/compute，
以及 timelineSemaphore、synchronization2、dynamicRendering、maintenance4。
创建 instance 与 VMA 均使用 VK_API_VERSION_1_4；1.3 及更低版本明确拒绝，不自动降级。
1.4.0 即满足 API 版本门槛，headers/SDK 的 patch 版本不作为额外最低驱动要求。
验证层测试还需要安装 Vulkan SDK 的 VK_LAYER_KHRONOS_validation 和 EXT_debug_utils。
vcpkg 提供编译头文件和 loader 包，不安装显卡驱动或验证层。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.' -Reason '验证 Vulkan 设备与诊断'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_graphics_resource_tests', 'dk_graphics_resource_probe') -TestRegex '^dk\.graphics\.' -Reason '验证资源、提交和数据往返'
```

windows-graphics 继承 CPU 开发预设，额外开启 DK_BUILD_GRAPHICS_DEVICE、DK_BUILD_GRAPHICS_SHADERS 和 DK_BUILD_GRAPHICS_OFFSCREEN；默认 windows-dev 保持 CPU-only。
设备模块需要 DK_BUILD_MEMORY。vulkan-device feature 安装 Vulkan、volk、vk-bootstrap 和 VMA；
独立 shaders feature 安装 Slang，SDL 尚未启用。volk 提供函数表，vk-bootstrap 构建 instance，VMA 管理设备内存分配。
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

若本机出现 `VK_LAYER_AMD_switchable_graphics uses API version 1.3`，这是旧 AMD 隐式层的声明与 1.4 应用不匹配。
本机已核实该层的 manifest 提供 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`；可仅在验证进程内使用，
Khronos 显式验证层及同步验证仍保持开启。测试仍统计全部诊断，不过滤该警告：

```powershell
$previousAmdLayer = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_probe', 'dk_graphics_resource_probe', 'dk_offscreen_probe') -TestRegex '^dk\.(device\.gpu_validation|graphics\.gpu_resources_validation|offscreen\.gpu_validation)$' -Reason '隔离旧 AMD 隐式层并验证 Vulkan 1.4'
} finally {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previousAmdLayer
}
```

该开关是本机驱动 manifest 的约定；其他机器先核对自身环境。这里不修改系统注册表、持久环境变量、SDK 或驱动。
Loader 通用的 `VK_LOADER_LAYERS_DISABLE` 在本机还会产生 forced-disabled warning，因此没有用它作为零诊断验收环境。

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
以下是专门的底层互操作示例；常规资源与命令使用后面的 ResourceFactory/encoder。
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
不要覆盖这些编译定义。VMA 使用 Vulkan 1.4 API 路径，maintenance4 显式启用，核心内存需求查询由 volk table 注入。
引擎对象/容器使用 Memory heap；Hpp dispatcher、vk-bootstrap/VMA 的内部 CPU 元数据使用三方默认分配器，
不计入该 heap 的用量。GPU allocation 由 VMA 单独统计。
底层 Device 不提供提交或隐式等待；以下 SubmissionQueue 提供提交与等待，且会在最终析构时排空自身工作。
错误包含 Vulkan 操作名、VkResult 符号与数值。loader_path 留空使用系统 loader；
绝对路径可用于部署或复现缺失 loader，不能传相对路径。

初始设备验收见 [0042](../development/0042-vulkan-device.md)，三方接入和 VMA 结果见
[0043](../development/0043-vulkan-libraries.md)，Hpp RAII 迁移见 [0044](../development/0044-vulkan-hpp-raii.md)。
M5.2 的资源与提交验收见 [0045](../development/0045-graphics-resources-submission.md)。
Vulkan 1.4 基线与 VMA 查询验证见 [0048](../development/0048-vulkan-14-baseline.md)。

## 上传与读回

文件包含 `<dk/graphics/Transfer.hpp>` 和 `<array>`，在上述 Device 创建成功后消费设备：

```cpp
auto queue = dk::graphics::SubmissionQueue::create(*heap, std::move(*device), 3);
if (!queue) return 1;
auto factory = queue->resources();
std::array<std::byte, 64> input{}, output{};
input.fill(std::byte{0x5a});
const auto usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst;
auto gpu = factory.create_buffer({input.size(), usage});
if (!gpu) return 1;
auto batch = queue->begin();
if (!batch || !batch->upload(*gpu, input)) return 1;
auto request = batch->readback(*gpu);
if (!request) return 1;
auto ticket = queue->submit(std::move(*batch));
if (!ticket) return 1;
auto completed = queue->wait(*ticket);
if (!completed || !*completed) return 1;
auto read = request->try_read(output);
if (!read || !*read || output != input || !queue->close()) return 1;
```

多次 upload/readback 复用同一个 batch，只在显式 submit 时提交。输入 byte span 在 upload 返回后可释放。
ReadbackRequest 在未提交/未完成时 try_read 返回 false 且不改输出，放弃或失败后返回取消错误；
poll/wait/close 推进完成状态。请求不强持有 queue，正常队列析构排空后仍可读取；设备丢失不返回成功数据。

创建队列后不再使用移后的 Device；queue.device() 只借用设备。factory 预检失败不消费设备，
开始创建后发生失败则销毁已消费设备。队列、资源和相关 Vulkan 访问由调用者串行执行，
不要绕过队列提交/重置内部 command buffer 或 signal 内部 timeline。

Image 使用 `factory.create_image({width, height})`，默认 RGBA8、transfer src/dst + sampled。
ImageDesc 末尾可指定 mip_levels/array_layers，ImageViewDesc 选择 2D/2D array 和子资源范围。
`batch.upload(image, bytes, {mip, layer, x, y, width, height})` 上传选定区域，
`batch.readback(image, region)` 返回该区域的紧密排列字节。上传可指定 buffer offset/row length/image height，
长度须匹配实际 footprint；高层读回这三个字段须为零，description() 返回实际行字节数。
color 支持 RGBA8 UNORM/SRGB、BGRA8 UNORM、R32_UINT/FLOAT；D32_SFLOAT 支持深度附件和采样，暂不支持 byte 传输。
新 image 必须先 clear/完整上传或声明完整 shader 覆盖才能读；局部写入不会证明整 mip/layer 内容有效。

begin() 在固定槽（1–64）用尽时返回 conflict；调用 poll() 或 wait() 确认完成后才复用。
wait 的超时单位为 ns，返回 false 时资源仍在使用；默认等待无超时。票据不能跨队列使用。
Buffer 的 CPU 访问在录制和 pending 期间被拒绝，wait/poll 回收引用后才允许访问。
VMA 负责 non-coherent flush/invalidate；flush 失败不保证已写 host 字节回滚。

丢弃 batch 会取消未提交录制；提交失败不更新票据或资源全局状态。一个 Buffer/Image 同时只能由一个未提交 batch 预约，
成功提交后即可在同队列下一批继续使用。包装提前销毁时队列保留 allocation 到 GPU 完成；
buffer/image 可晚于 queue 销毁，它们会继续持有设备，因此诊断 sink 和 Memory 系统也须保持存活。
close() 遇录制中的 batch 返回 conflict；成功后拒绝新工作。队列最终析构等待 pending 工作，
device lost 为终态；其他等待失败保留资源以供重试。API 的 bad_alloc 不转换为 GPU 成功/完成。

内置同步以正确性为先，采用保守 barrier；目前未做异步传输队列或 barrier 合并优化。
原生互操作使用 unsafe_record(before, after, callback)，先用 retain 的对象重载声明额外对象，
资源状态前后一一对应；回调不得 end/reset/submit 或 signal 内部 timeline。回调退出使已有 encoder 失效，
异常使批次 invalid。command_buffer() 已弃用，仅保留兼容；常规调用使用下面的 encoder。

资源 GPU 探针每次验证 16 轮 buffer/image 往返、5 种格式、提交失败/超时/等待错误、跨队列拒绝、
延迟释放和关闭。验证用例通过环境设置开启 Khronos 同步验证（兼容旧 SDK 的 VK_LAYER_VALIDATE_SYNC
和新版本的 VK_VALIDATION_VALIDATE_SYNC），无环境返回 77；不将跳过当通过。
Tracy 当前标记 CPU 创建/submit/wait/collect；GPU query/context 的寿命契约已确定，
GPU timestamp zone/capture 尚未启用，见[接入边界](../design/graphics-resources.md#tracy-gpu-接入边界)。


## 管线、绑定与计算

包含 `<dk/graphics/Transfer.hpp>`；使用已有 queue、factory 与受信任的 CompiledShader。
以下片段假定 compute 产物只声明 set=0/binding=0 的 storage buffer，且无 push constants；
input/output 为相同长度的四字节对齐 byte span，groups 为合法工作组数量，shader 负责访问边界。

```cpp
using namespace dk::graphics;
auto module = factory.create_shader(compiled);
if (!module) return 1;
const std::array shaders{&*module};
auto layout = factory.create_pipeline_layout(shaders);
if (!layout) return 1;
auto pipeline = factory.create_compute_pipeline({&*module, &*layout});
auto storage = factory.create_buffer({input.size(), vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst});
if (!pipeline || !storage) return 1;
const std::array<BindingWrite, 1> writes{{{0, 0, BufferBinding{&*storage}}}};
auto bindings = factory.create_bindings(*layout, 0, writes);
if (!bindings) return 1;
// layout/pipeline/bindings 可保留，并用于后续多个批次。
auto batch = queue->begin();
if (!batch || !batch->upload(*storage, input)) return 1;
const std::array uses{buffer_use(*storage, vk::PipelineStageFlagBits2::eComputeShader,
    vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite)};
if (!batch->prepare(uses)) return 1;
const std::array sets{&*bindings};
if (!batch->dispatch(*pipeline, sets, {}, groups)) return 1;
auto request = batch->readback(*storage);
if (!request) return 1;
auto ticket = queue->submit(std::move(*batch));
if (!ticket) return 1;
auto done = queue->wait(*ticket);
if (!done || !*done) return 1;
auto read = request->try_read(output);
if (!read || !*read) return 1;
```

多次 dispatch 可借用 `batch.compute()`，依次 bind_pipeline/bind_sets/push_constants/dispatch。
写后读/写必须先再次 prepare，即使 layout 不变；descriptor 本身不推断 shader 的读写意图。
Bindings 创建后不可变，更新绑定生成新对象；旧绑定与其资源自动保活到 GPU 完成。
多个 set、固定数组、uniform/storage buffer、sampled/storage image 与 sampler 例子见
[GPU probe 的 pipeline_bindings](../../tests/integration/OffscreenProbe.cpp)。

## 绘制与显式同步

`prepare(输入/附件) → begin_rendering(RenderingDesc) → bind/draw → end → readback` 是基本顺序。
RenderingDesc 指定 color view 与可选 depth view、load/store/clear 和区域，默认 viewport/scissor 覆盖区域。
Color 的声明包含 ColorAttachmentRead/Write 与 ColorAttachmentOutput；depth 包含 Early/LateFragmentTests、
DepthStencilAttachmentRead/Write。已开启的 rendering scope 内不能 prepare、copy、compute 或嵌套 rendering。
GraphicsPipelineDesc 指定 vertex/fragment、布局、顶点 binding/attribute、格式、深度和混合设置；
RenderEncoder 提供 vertex_buffer、index_buffer、draw/draw_indexed。索引值决定的实际顶点范围由调用者保证。
完整的 compute→vertex→indexed draw、D32 深度与对象提前释放示例见上述 probe 的 indexed_depth。

ResourceUse 的 buffer_use/image_use 明确 stage/access、layout 和范围，prepare 自动依据账本发 barrier。
需要外部规划时传 `ResourceBarrier{use, before}` 给 batch.barrier，不再重复 prepare；
它和 encoder/copy 共用一份状态。Buffer::state() / Image::state(mip,layer) 是上次成功提交的状态，不能据此判断 GPU 完成。
buffer 按整对象保守跟踪，image 按 mip/layer；ResourceUse.full_overwrite 表示下一次 shader 写入完整初始化声明的子资源，
调用者必须保证真实覆盖。低层 copy_buffer/带 region 的 copy/fill/clear 要求已 prepare，不隐式加第二份同步。

end、提交或 batch 移动后，旧 encoder 返回 invalid_state；移动打开 rendering 的 batch 会使批次 invalid。
单次 dispatch 与传输组合方法在已经发出命令后失败也使批次 invalid，应放弃后新建。普通 bind/retain 负责保活，不能代替同步。
接口仍是 Vulkan 专用：保留格式、usage 和 stage/access 值类型；不包含窗口呈现、Graph 调度、多队列、MSAA 或 bindless。
