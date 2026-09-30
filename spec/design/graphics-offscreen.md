---
module: graphics-offscreen
created_at: "2026-09-30T09:00:00+08:00"
updated_at: "2026-09-30T10:18:48+08:00"
status: accepted
---

# 离屏绘制与计算

## 目标和边界

M5.4 将 [Device/资源提交](graphics-resources.md) 与 [Slang 产物](graphics-shaders.md) 连成
真实 draw/dispatch/readback 闭环。独立 `dk::graphics_offscreen`、`DK_BUILD_GRAPHICS_OFFSCREEN`，
要求 Device/Shaders 已启用；windows-graphics 开启，默认 CPU 与离线 Shader 配置不引入此模块。
复用现有固定依赖；不新增包、Runtime 命令、窗口、swapchain、Graph 或第二套通用渲染架构。
这是同步、单队列的底座验证入口；正式业务调度留给 M6/M7。

[M5.5 使用层设计稿](graphics-vulkan.md) 规划把本模块手写的 view/pipeline/descriptor、命令录制和
GPU 对象保活下沉至 device 使用层；本模块保留操作组合、结果验证和现有同步 API。
迁移安排在 M5.5.5，尚未实施；下文的 Work 所有权、每次建管线和 draw/dispatch 限制仍反映当前行为。

## 接口与数据

`OffscreenExecutor::create(resource, Device&&)` 消费设备，持有单槽 SubmissionQueue。
`draw(vertex, fragment, description)` 绘制无顶点缓冲/无 descriptor 的 triangle list：
动态 rendering、RGBA8_UNORM 单 color attachment、单采样、无深度/混合/剔除，动态 viewport/scissor；
参数为宽高、顶点数、clear RGBA，返回拥有型 RGBA8 字节（Vulkan 正高度 viewport 的行顺序）。
shader 必须使用 SV_VulkanVertexID 等内建输入，并有互相兼容的输出/输入；不支持本阶段之外的 graphics 布局。
示例采用 SV_VulkanVertexID，避免 Slang 的 SV_VertexID 生成需要额外 shaderDrawParameters 特性的 BaseVertex 访问。

`dispatch(shader, buffers, push_constants, group_count)` 接受 set=0、count=1 的 storage buffer 反射，
buffer 参数按 binding 匹配，每个 buffer 带初始字节；全部上传到 device memory，dispatch 后逐个读回。
返回与参数顺序相同的 binding/拥有型字节列表。支持一个 offset=0 的 push constant 范围，大小须严格匹配。
调用者提供 workgroup 数量并负责 shader 内部范围保护，不能把元素数误作组数。
检查设备 group/local-size/invocations、storage-buffer range/descriptor 数量、push constant、image/framebuffer/viewport 限制。
shader 来自受信任且未经修改的 compile_shader 产物；基本 SPIR-V 流、入口/阶段及本阶段布局检查不替代完整 SPIR-V 验证。
首版不启用额外设备特性，拒绝需要基础 Shader/Matrix 之外 capability 的产物。

## 所有权、提交点和失败

普通 Vulkan 对象全部用 vk::raii（ShaderModule、Pipeline/Layout、DescriptorSetLayout/Pool/Sets、ImageView）；
VMA Buffer/Image 复用既有所有者。控制块、保留列表及返回数据使用指定 Memory resource，
Hpp 内部短期容器使用上游分配器。公开参数均为借用，仅调用期间使用；返回值独立拥有字节。
调用者串行访问 executor，不公开其可变队列，单次最多一个 pending 操作；每次创建管线，不实现缓存。

输出 CPU 容器、所有资源与录制在 submit 前准备；提交后 pending work 持有整组管线/描述符/view/资源。
成功等待完成后才读回、发布返回值并释放 work。输入错误、分配/创建/录制/submit 失败不发布输出，
不会修改调用者输入。Memory resource 关闭后拒绝新操作，但仍允许 drain/析构回收；调用期间不能并发关闭资源。
可传播的 CPU 分配异常不伪装成 Result；提交前异常清除预留 work，提交后保持 pending。
不承诺 MSVC Debug STL 的 noexcept 容器构造发生 OOM 时可恢复。Vulkan/参数错误通过 Result 返回。
默认等待 10 秒，超时返回 conflict，其他 wait 错误保留原 Result；两者均保留 pending，禁止新操作。
`drain(timeout)` 等待并丢弃该失败调用的待处理结果，只有确认完成才释放；允许重试。
析构先让 SubmissionQueue 等待/处理 lost，再销毁 pending 的 Vulkan 子对象；资源持有设备寿命直至最后释放。
不伪造 GPU 完成，不因超时或 CPU 函数返回就释放对象。Device lost 沿用 M5.2 终态与析构保护。

## 实施与验证

先实现模块/参数策略，再加入 CPU 测试和独立 GPU probe。通过资源 copy 的 synchronization2 barrier
衔接 upload→compute→copy→host，color attachment 通过 tracked transition 转为 transfer source 后读回。
验证实际三角形背景/内部颜色、已知整数变换、非工作组整数倍数据、重复绘制/计算/销毁、零 VMA/Memory 残留，
并通过 wait timeout/错误、submit 失败注入验证生命周期。启用同步验证检查零相关 warning/error。
无必需设备/验证环境仅 probe 返回 77，不将跳过说成通过；CPU 参数测试不依赖 GPU。
定向复验资源提交、Shader、默认 CPU runner；默认不运行全量、Release、其他 GPU/平台或 Tracy GPU capture。
使用方式与完整示例见[离屏指南](../guides/offscreen.md)。

参考 [Khronos dynamic rendering](https://docs.vulkan.org/samples/latest/samples/extensions/dynamic_rendering/README.html)、
[同步示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html)。
开发记录：[0047](../development/0047-offscreen-execution.md)。
