---
module: platform
created_at: "2026-10-02T21:40:00+08:00"
updated_at: "2026-10-02T22:46:00+08:00"
status: accepted
---

# SDL3 窗口设计

## 目标与边界

M5.6 提供可选的 `engine/platform`、`dk::platform`，公开头位于 include/dk/platform。
依赖 Core/Memory，PRIVATE SDL3；不依赖 Graphics、Slang、Scene 或 Runtime。
`DK_BUILD_PLATFORM` 默认关闭，选择独立 platform feature（SDL3[vulkan]），不拉入 ImGui。
本次只实现窗口与事件泵；完整键鼠输入映射、编辑器及多窗口呈现调度留给后续模块。

## 接口与寿命

Window 是共享窗口寿命的值句柄，create 接收 Memory resource 和 WindowDesc；
提供 poll_events、status、resize、minimize、restore。WindowStatus 返回实际像素尺寸、
最小化与关闭请求；事件泵处理进程内所有本模块窗口，SDL_QUIT 置全部窗口关闭请求。
WindowDesc 用 UTF-8 标题、逻辑尺寸、可调整尺寸/Vulkan/隐藏开关。
最小化输出零绘制尺寸，恢复后重新读取像素尺寸；关闭事件只标记请求，不隐式销毁。

SDL video 子系统按共享 lease 成对 InitSubSystem/QuitSubSystem；最后一个窗口及设备引用释放后退出。
所有窗口调用、最终销毁及呈现对象销毁都在创建它们的主线程执行，外部串行。
内部 WindowAccess 提供 SDL 句柄和寿命 token 给 Presentation，不在公开头暴露 SDL/Vulkan 类型。
控制块使用调用者 Memory；SDL 内部临时/系统分配沿用 SDL 默认分配器。

## 状态与失败

创建先校验参数和 Memory 状态，成功后发布完整窗口；中途失败释放已初始化资源。
尺寸请求由窗口系统异步应用，resize 成功只表示 SDL 接受请求，status 是实际状态。
空对象/错误线程/非法尺寸返回 Result 错误，不更新状态。OOM 沿用 Memory 的异常边界。
不接管外部 SDL 窗口；调用者不得同时另设 SDL 事件消费者吞掉本模块事件。

## 验证

参数、错误线程、双窗口事件隔离、重复创建/销毁、实际像素 resize/minimize/restore 与 Memory 回收。
平台探针不需要 Vulkan 设备；窗口初始化失败明确报错，不将缺失桌面当作通过。首先验收 Windows x64。

相关：[架构](architecture.md)、[呈现](graphics-presentation.md)、[0054](../development/0054-window-device.md)。
