---
created_at: "2026-10-09T14:20:00+08:00"
updated_at: "2026-10-09T14:26:13+08:00"
---

# GPU 性能观测

[返回项目入口](../../README.md)。M11.1 的观测只覆盖单队列；设计见
[性能分析](../design/foundation-profiling.md) 与 [GPU 提交](../design/graphics-resources.md)。

## 构建与验收

使用现有固定依赖，无须升级 vcpkg baseline。Tracy 工具须为 0.14.1，安装方法见
[工具说明](../../tools/profiling/README.md)。

```powershell
cmake --preset windows-graphics-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics-profiling -Configuration RelWithDebInfo -Target dk_gpu_profiling_probe -TestRegex '^dk\.profiling\.gpu_validation$' -Reason 'GPU timing and lifetime'
& ./scripts/capture-gpu-profiling.ps1
```

preset 开启 Tracy CPU/GPU 适配并关闭内存事件以减少干扰，使用 verify 的 RelWithDebInfo 配置。
采集脚本仅连接自己启动的回环探针，保存 gpu.tracy、GPU/CPU CSV、原始 GPU 计时 JSON、日志和 summary。
脚本验证 Pass 数量/耗时与原生查询匹配、CPU 提交编号、失败/放弃区间不出现；没有 GPU 返回77，不能当作通过。

无 Tracy 的 timestamp 路径：

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_gpu_profiling_probe -TestRegex '^dk\.profiling\.gpu_validation$' -Reason 'Native timestamps without Tracy'
```

探针使用 seed=42、8×8 布片、dt=10ms、4批×2步、96×96绘制，对比开关前后的粒子/速度/图像字节，
并验证 CPU 容差、两槽复用、显式 poll、超时、录制/提交/查询失败、容量耗尽、关闭/析构和设备丢失。
它是正确性/采集验收，不是 M11.2 的性能基线；不据此推导帧率或控制请求延迟。

## C++ 使用

对现有 SubmissionQueue 调用 `configure_gpu_profiling({true})`；默认为关闭，空闲时配置。
随后 Graph 自动记录整个提交和每个保留 Pass。通过 `execution.submission().gpu_profile()` 或
`frame.submission().gpu_profile()` 保存结果；仅在 queue.wait/poll 确认完成后 `status()==ready` 时读取 `timings()`。
票据包含 `submission_value`，可与 CPU graph.execute/graphics.submit/graphics.wait 的 zone value 对应。
结果只拥有 CPU 数据，可晚于队列销毁，但仍属于调用者的 Memory heap，释放结果后才能关闭该 heap。

`configure_gpu_profiling({false})` 在全部槽退休后释放查询池；旧计时仍有效。
`max_zones` 默认8192，合法1..32768，含提交区间；超过容量仅省略子区间并增加 dropped_zones，不影响求解。
未完成/禁用/查询不可用/设备丢失分别报告 pending/disabled/unavailable/device_lost；缺失数据不是零耗时。
手写 CommandBatch 可 begin_gpu_zone/end_gpu_zone，区间不嵌套，且开始/结束在动态 rendering 之外。

## 测量口径与限制

GPU 区间取同一 command buffer 的 BOTTOM_OF_PIPE 时间戳，包含管线执行及依赖停顿；
每个 Pass 的准备屏障在区间内，父提交区间与子区间不能相加。compute 当前对应 XPBD求解，draw对应布片绘制，
transfer单列复制/清除/读回。混合Pass按compute、graphics、transfer顺序归类，名称保留具体内容。
时间为 ns，按 timestampPeriod 换算并按 timestampValidBits 处理一次回绕；任一区间必须短于计数器回绕周期。
Tracy 绝对时间线还要求采集期间不跨越相对初始时钟的回绕周期；64位时间戳设备的常规实验不触及此限制。

首次开启 Tracy 适配做一次单时间戳提交并等待队列空闲；该初始化成本不属于求解。
alignment_window_ns 报告提交到完成采样的窗口，CPU/GPU绝对对齐存在该窗口误差及长期时钟漂移，
没有声称 calibrated timestamps，也不能用它测排队/请求响应延迟。CPU 录制时刻独立使用 steady_clock，非UTC。
Tracy连接代际变化会丢弃跨连接批次的Tracy事件，原生GPU计时不受影响。不开启profiling时无Tracy链接；
CPU-only配置不构建Device或Graph，不引入Vulkan。

M11.2 继续建立多规模、冷/热启动、采集开销和控制延迟的可复现基线。

原理依据：[Vulkan timestamp/query 规范](https://docs.vulkan.org/spec/latest/chapters/queries.html)；
私有事件适配参照固定 [Tracy 0.14.1 Vulkan 实现](https://github.com/wolfpld/tracy/blob/v0.14.1/public/tracy/TracyVulkan.hpp)，
采用同版本的非校准时钟对齐；query 生命周期由引擎提交槽管理。
