---
created_at: "2026-10-02T22:33:00+08:00"
updated_at: "2026-10-02T22:33:00+08:00"
---

# 窗口与呈现

[返回项目入口](../../README.md)。公开接口见 [Window](../../engine/platform/include/dk/platform/Window.hpp)、
[Presenter/Frame](../../engine/graphics/presentation/include/dk/graphics/Presentation.hpp)，
完整用法见 [窗口三角形示例](../../examples/presentation/src/main.cpp)。
底层对象与录制见 [Graphics 指南](graphics.md)，无窗口路径见 [离屏指南](offscreen.md)。

## 构建与运行

Windows x64，使用现有固定 vcpkg baseline 和 SDL3 3.4.16#1：

```powershell
cmake --preset windows-presentation
cmake --build out/build/windows-presentation --config Debug --target dk_presentation_demo
.\out\build\windows-presentation\bin\Debug\dk-presentation-demo.exe
# 必须启用验证层，呈现 120 帧后退出：
.\out\build\windows-presentation\bin\Debug\dk-presentation-demo.exe --validation --frames 120
```

默认持续运行至关闭窗口；支持拖动改变尺寸、最小化及恢复。--frames 取 1..1000000。
关闭窗口后排空渲染和呈现请求，再销毁交换链/设备/窗口。
shader 使用 common/triangle.slang 的 vertexMain/fragmentMain，Slang 编译为 SPIR-V 1.5；
管线在格式不变时复用，窗口尺寸通过动态 viewport/scissor 更新。

呈现要求 Vulkan 1.4、既有 device 特性、同一 graphics/compute/present 队列，
VK_KHR_swapchain 和 VK_EXT_swapchain_maintenance1 / swapchainMaintenance1。
不支持时返回具体能力诊断，不静默降级。验证层缺失时 --validation 失败；示例环境不支持可返回 77。
默认 if_available 可在无验证层环境运行，并报告未启用状态。
本机旧 AMD 隐式层与 Vulkan 1.4 的环境问题及进程内规避见 [0048](../development/0048-vulkan-14-baseline.md)，
不应通过关闭 Khronos 同步验证掩盖程序问题。

## 调用流程

在创建主线程上创建 Memory heap、Window 和 Presenter，随后循环：

1. `window.poll_events()` 更新所有 dk 窗口的事件；检查 close_requested。
2. `presenter.acquire()` 返回 ready/retry/suspended。后两者无 Frame，继续泵事件并适当等待。
3. 对 Frame 的 color 使用 `prepare`，再 `begin_rendering`、绑定管线、draw、end。
4. `presenter.present(std::move(frame))` 自动转换到 PresentSrc、等待 acquire、提交并呈现。

以下片段位于示例的循环中（`take` 负责检查 Result）：

```cpp
auto acquired = take(presenter.acquire());
if (acquired.status == dk::graphics::AcquireStatus::ready) {
    auto& frame = acquired.frame;
    const std::array uses{dk::graphics::image_use(frame.color(),
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eColorAttachmentRead | vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::ImageLayout::eColorAttachmentOptimal)};
    take(frame.commands().prepare(uses));
    dk::graphics::RenderingDesc rendering;
    rendering.color.view = &frame.color_view();
    auto encoder = take(frame.commands().begin_rendering(rendering));
    take(encoder.bind_pipeline(graphics));
    take(encoder.draw(3));
    take(encoder.end());
    auto presented = take(presenter.present(std::move(frame)));
    // presented.completion 仅表示渲染完成；呈现完成由 Presenter 内部维护。
}
```

`Presenter::queue()` 是借用，可用于 factory、独立上传与完成票据；不要移动/替换该队列。
Frame 的 commands、color、color_view 也是借用；不能移出 batch 或通过普通 queue.submit 呈现，
接口会拒绝绕过同步。Frame 必须在同一主线程销毁；未提交 Frame 析构会释放获取的图像。
每个 Presenter 同时只允许一个活动 Frame，可有 1..8 个已提交在途槽（默认 2）。

每帧丢弃上一帧像素，使用 clear 或完整写入；尚未初始化的图像不能呈现。
RenderEncoder 必须 end 后才能 present。跨帧保留的额外 view 只延长旧交换链寿命，
不授予下一帧访问权限；每次使用新的 Frame.color_view。
surface 提供 TransferSrc 时，Frame.color 的 usage 含该标志，可在 present 前通过原有 readback 读回；
格式由 Frame.format 决定，可能是 BGRA/RGBA sRGB，不能一律当作线性 RGBA 数据。

## 恢复与错误边界

像素尺寸变化自动触发重建，最小化返回 suspended。OUT_OF_DATE 返回 retry 或 needs_rebuild；
SUBOPTIMAL 允许本帧完成并在下一次 acquire 重建。`request_rebuild()` 可显式请求下一帧重建。
`acquire(timeout_ns)` 默认 16 ms，必须有限；超时是 retry。重建和 close 会排空旧代际，
所以该参数不限制整个 acquire 的总耗时。

参数/未结束录制等校验失败保留 Frame，可修正或放弃。有效帧开始提交后会被消费；
present 失败不回滚已提交渲染。获取或提交失败释放未呈现图像，信号量按真实完成状态回收。
候选交换链创建失败后不使用被退休的旧代际，下次重新尝试。
surface lost / device lost 是终态，关闭并重新创建窗口设备链；不自动更换设备或重建所有业务资源。
close 拒绝活动 Frame，正常完成后幂等；首次关闭 lost device 返回错误并标记 closed。
CPU OOM 沿用 std::bad_alloc 边界，已获取帧通过 RAII 清理。

首版为 FIFO、单 color attachment、sample=1、同队列呈现。无 HDR、独占全屏或多窗口共享设备调度。
当前验证平台为 Windows x64；Linux/macOS 尚未验收。

## 验证与依赖隔离

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-presentation `
  -Target @('dk_presentation_tests','dk_presentation_probe','dk_present_device_probe','dk_platform_probe','dk_presentation_demo') `
  -TestRegex '^dk\.(presentation\.|platform\.windows$)' -Reason '窗口与呈现定向验收'
```

CPU 策略测试不需 GPU；桌面/GPU 探针需要可用窗口系统，GPU 环境缺失的 77 为跳过，不算通过。
记录包括待呈现图像读回、重复创建/关闭、真实 resize/minimize/restore、失败注入、验证消息与 Memory/VMA 回收。
历史证据见 [0054](../development/0054-window-device.md)、[0055](../development/0055-swapchain-frames.md)、
[0056](../development/0056-presentation-recovery.md)。

`DK_BUILD_PLATFORM` 与 `DK_BUILD_GRAPHICS_PRESENTATION` 均默认 OFF。
Presentation 要求 PLATFORM/DEVICE；SDL3 的 platform feature 不包含 ImGui。
windows-graphics 仍为不依赖 SDL 的离屏配置，windows-dev 的 CPU runner 不依赖 SDL/Vulkan/Slang。
Presenter 本身不链接 Slang，示例与 shader 探针才显式链接编译器。
