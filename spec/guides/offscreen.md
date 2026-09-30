---
created_at: "2026-09-30T09:26:00+08:00"
updated_at: "2026-09-30T09:26:00+08:00"
---

# 离屏绘制、计算与读回

[返回项目入口](../../README.md)。`dk::graphics_offscreen` 将 [Slang 编译](shaders.md) 和
[Vulkan 资源提交](graphics.md) 接成同步 GPU 执行入口，无窗口、无 swapchain。
这是底座验证用途：图形支持无顶点缓冲/descriptor 的 triangle list，计算支持 set 0 的 storage buffer 与 push constant。
完整限制和寿命约定见 [graphics-offscreen](../design/graphics-offscreen.md)。

## 构建与运行

需要同一 Vulkan 1.3 队列支持 graphics/compute，以及 timelineSemaphore、synchronization2、dynamicRendering。
验证用例需要 Vulkan SDK 的 Khronos 验证层。默认 CPU 和独立离线 Shader 预设不启用此模块。

```powershell
cmake --preset windows-graphics -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_offscreen_tests', 'dk_offscreen_probe') -TestRegex '^dk\.offscreen\.' -Reason '验证离屏绘制计算与同步寿命'
```

预设开启 `DK_BUILD_GRAPHICS_OFFSCREEN`，要求 Device/Shaders 同时开启。自定义程序链接
`dk::graphics_offscreen`，并调用 `dk_deploy_slang(target)` 部署编译器依赖。
CPU 参数检查可仅选择 `dk_offscreen_tests` / `^dk\.offscreen\.unit\.`，不需要 GPU。

GPU 探针每次编译三个入口，在三轮设备生命周期内验证 12 组 draw/compute，另验证失败、超时、恢复与带 pending 析构。
图像使用逐像素背景/三角形内部插值校验；compute 对 1/64/257/1031 个元素执行已知整数变换并检查尾部哨兵。
同时要求 VMA/Memory 无残留、零验证 warning/error。无必需环境返回 77（Skipped），不代表验收通过。
验证用例自动开启同步验证，兼容 `VK_LAYER_VALIDATE_SYNC` / `VK_VALIDATION_VALIDATE_SYNC`。

CTest 在 `out/build/windows-graphics/tests/integration/` 输出 `offscreen-triangle.ppm` 和
`offscreen-triangle-validation.ppm`，均为 64×64 RGB PPM。也可直接运行
`out/build/windows-graphics/bin/Debug/dk-offscreen-probe.exe --validation`，此时图像输出到当前目录。
日志打印实际 GPU/驱动/API、Slang 版本/目标和诊断计数；验证记录见 [0047](../development/0047-offscreen-execution.md)。

## C++ 调用

在仓库根运行下面程序，MemorySystem 必须晚于所有返回字节、shader 和 executor 销毁：

```cpp
#include <dk/graphics/Offscreen.hpp>
#include <dk/memory/MemorySystem.hpp>

int main()
{
    using namespace dk::graphics;
    auto system = dk::memory::MemorySystem::create();
    if (!system) return 1;
    auto heap = system->create_heap({"offscreen", dk::memory::DomainCategory::render});
    if (!heap) return 1;
    auto device = Device::create(*heap);
    if (!device) return 1;
    auto executor = OffscreenExecutor::create(*heap, std::move(*device));
    if (!executor) return 1;
    auto vertex = compile_shader({"shaders/common/triangle.slang", "vertexMain", ShaderStage::vertex}, *heap);
    auto fragment = compile_shader({"shaders/common/triangle.slang", "fragmentMain", ShaderStage::fragment}, *heap);
    if (!vertex || !fragment) return 1;
    auto image = executor->draw(*vertex, *fragment); // 默认 64×64 RGBA8_UNORM。
    if (!image) return 1; // 记录 error().message；超时仍保留 GPU 对象。
    // image->rgba8 是连续 width * height * 4 字节；正高度 viewport 的行顺序。

    auto compute = compile_shader({"shaders/common/transform.slang", "computeMain", ShaderStage::compute}, *heap);
    if (!compute) return 1;
    const std::array<std::uint32_t, 4> input{1, 2, 3, 4}, initial{};
    const std::array<std::uint32_t, 4> push{4, 3, 7, 0}; // count, multiplier, bias, reserved。
    const std::array<ComputeBufferInput, 2> buffers{{
        {0, std::as_bytes(std::span{input})}, {1, std::as_bytes(std::span{initial})}}};
    auto output = executor->dispatch(*compute, buffers, std::as_bytes(std::span{push}), {1, 1, 1});
    if (!output) return 1;
    // output[1].bytes 为 uint32 10, 13, 16, 19；用 memcpy 读取，避免对齐/对象寿命假设。
    return 0;
}
```

输入采用未经修改的受信任 `compile_shader` 产物。graphics 两阶段接口必须兼容；示例的
`SV_VulkanVertexID` 不需要额外 shaderDrawParameters 特性。compute 按反射 binding 匹配，返回顺序与输入一致；
每个 storage buffer 完整上传并读回，调用者 CPU 输入不修改。dispatch 参数是工作组数量，线程越界由 shader 保护。

默认等待 10 秒，超时返回 conflict；等待错误和超时均保留所有 GPU 所有者，新操作被拒绝。
调用 `drain(timeout_ns)` 确认完成并丢弃失败调用的结果；false 表示仍 pending，错误也可重试。
析构会等待自身工作。Memory 关闭后禁止新 draw/dispatch，仍允许 drain；操作期间需串行访问并保持资源 open。
普通 Vulkan 对象全部采用 `vk::raii`，VMA 独占其 Buffer/Image。尚无管线缓存、场景渲染、窗口或 Tracy GPU capture。
