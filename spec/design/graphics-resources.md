---
module: graphics-resources
created_at: "2026-09-28T18:19:00+08:00"
updated_at: "2026-09-30T10:18:48+08:00"
status: accepted
---

# Vulkan 资源与提交生命周期

## 目标、边界与依赖

M5.2 在 [设备底座](graphics-device.md) 上交付 VMA Buffer/Image、上传/读回、单队列提交、
完成票据、固定提交槽和延迟回收。实现位于 engine/graphics/device，继续使用 dk::graphics_device；
公开 Resources.hpp，不新增依赖或 feature。普通 Vulkan 所有权使用 vk::raii；VMA 资源配对释放。
该模块不涉及 shader、绘制、窗口、Graph、多队列或后台提交线程。
[离屏模块](graphics-offscreen.md) 在此之上管理管线、描述符和 image view 至提交完成。

[M5.5 使用层设计稿](graphics-vulkan.md) 规划将保留机制扩展到全部 GPU 对象，增加工厂/encoder、
资源访问状态与子资源范围、staging 和完成后读回接口；这些扩展尚未实现。
本页单 mip/layer、全图布局、手工 retain 和保守 copy barrier 仍是当前契约；
各 M5.5.x 实施时逐节更新本页，保持唯一提交点、timeout 保活和无资源→队列引用环的约束。

## 接口与数据

SubmissionQueue::create(resource, Device&&, slot_count) 消费设备并拥有一个独占提交域；
slot_count 为 1–64。创建中途失败会销毁已消费设备；调用者不得继续使用移后 Device。
Buffer/Image 由队列工厂创建，move-only，可为空；内部资源控制块和容器归入调用者 Memory resource。
BufferDesc 指定大小、Vulkan usage、device/upload/readback 内存用途；CPU write/read 检查角色、范围、忙状态，
通过 VMA map、flush/invalidate、unmap 配对处理，设备用途不能直接访问。
map/范围/角色失败不写用户数据；read 的 invalidate 失败不修改输出；write 的 flush 失败时
host 内存可能已写入，不保证字节回滚，也不会因此自动提交 GPU 工作。
ImageDesc 提供单 mip、单 layer、sample=1 的二维 color image；首版支持 RGBA8 UNORM/SRGB、BGRA8 UNORM、
R32_UINT/R32_SFLOAT，均为每像素 4 字节。检查 extent、字节数溢出、usage 和设备 format/extent 能力。
Buffer/Image 的 vk::* 句柄只借用，不得额外销毁；VMA allocation 不公开。

begin() 获取空闲槽并开始 one-time command buffer。CommandBatch 提供 buffer copy、整图 buffer/image
双向 copy、image transition，以及 retain(buffer/image) 和借用 RAII command_buffer() 扩展点。
手工录制须先 retain 全部使用资源，手工 image barrier 不得绕过 transition 的布局账本。
内置 copy 检查 owner、usage、范围、4 字节对齐、同 buffer 重叠和 image 尺寸；使用 synchronization2
建立保守 memory barrier 和 transfer/host 可见性。零长度 copy 拒绝；CPU 零字节 read/write 是合法无变化。
支持布局 Undefined（仅初始）、TransferSrc/Dst、ShaderReadOnly、General、ColorAttachment；非初始目标不能为 Undefined。

submit(CommandBatch&&) 返回 Submission（弱 owner 身份和单调值）；poll() 查询完成并回收，
wait(ticket, timeout_ns) 返回 bool（false 为超时），close() 等待并停止新录制。
Stats 区分 recording/pending/free 槽、提交值、已完成值和 pending 保留资源引用数。

## 所有权、提交点与失败

设备单独放入共享 lifetime，资源控制块和队列都持有它；资源可以晚于队列销毁。
队列 pending 槽持有资源，但资源不反向持有队列，避免环。录制 batch 持有队列，确保 command pool
在 batch 放弃前不销毁；票据不延长队列寿命。诊断 sink 的 user data 必须晚于所有资源的最终销毁。
所有 API、相关资源和借用队列由调用者串行访问；本阶段不提供线程安全提交。

槽状态为 free→recording→pending→free。未提交 batch 析构释放录制保留和 image 预约，不提交 GPU 工作；
下次获取 free 槽才重置 pool。Buffer 被录制或 pending 引用时拒绝 CPU read/write，避免覆盖 GPU 数据。
Image 每次只允许一个活跃 batch 预约，记录局部 predicted layout；成功提交才发布布局，
失败或放弃不改变全局布局。已提交 image 可在同队列后续 batch 继续使用，barrier 按队列顺序生效。

所有 CPU 分配/保留列表准备在 vkQueueSubmit2 之前完成。仅 VK_SUCCESS 是提交点：发布票据、
递增 timeline、转移预分配资源列表、提交局部 image 布局，这些步骤不得分配或抛出。
失败不发布票据/值/布局，batch 被消费且释放录制引用，资源仍由调用者持有；std::bad_alloc 保持标准异常约定。
GPU 完成由 timeline 值确认；timeout 不释放 pending、不复用槽。token 必须属于同一队列且已提交。
资源包装提前销毁只释放用户引用，pending 引用到完成时才释放 VMA allocation。
不以 CPU 下一帧或引用计数为 GPU 完成依据，不允许 host signal 内部 timeline。

close() 遇活跃 batch 返回 conflict 且不关闭。正常析构必须排空自身 pending 工作；
wait/submit 的 device lost 标记终态，停止新工作，关闭时允许按 lost 设备规则回收，不伪造成功完成值。
其他等待错误保留 pending，允许重试；析构中等待失败则尝试 queue idle，仍无法确认安全回收时 fail-stop，
不能静默销毁可能仍在使用的资源。此队列析构等待区别于底层 Device 的无隐式等待契约。

## Tracy GPU 接入边界

本阶段用现有 DK_PROFILE_ZONE 标记资源创建、录制、submit、wait、collect 的 CPU 成本，
不把它们当成 GPU 时间。后续 GPU context 必须归此单队列、晚于 slot query/zone 结果回收才销毁；
每槽的 timestamp query 只能在完成票据确认后读取/重置，begin/end 同一 command buffer，
检查 timestampValidBits/timestampPeriod，禁用 profiling 不建 query/context。
Tracy Vulkan adapter 留在 graphics 私有实现，不使 foundation/profiling 依赖 Vulkan，也不借用 volk 全局表。
M5.2 交付该生命周期边界；GPU timestamp zone/capture 待实际 Pass（M5.4–M7）专项验收。

## 验证

CPU 测试验证描述、范围/溢出/对齐、usage/layout；GPU probe 验证多轮 buffer 和 image 数据往返、
槽用尽、timeout、跨队列票据/资源拒绝、布局回滚、pending 延迟回收、移动/放弃、关闭与资源延长设备寿命。
内部测试 seam 注入 submit 失败与 device lost，不提供生产公开故障开关。
Memory 关闭新分配后：录制 retain 分配失败不发布工作，已准备好的 batch 仍可提交/等待/释放，
验证提交点无引擎 CPU 分配；poll 和等待错误后的重试分别覆盖。
启用同步验证的 probe 检查零错误/警告，另有无验证层探针；环境缺失返回 77，失败不能跳过。
复验 M5.1、CPU-only runner 与静态导入边界，不默认跑全量或双配置。

参考 Khronos [同步示例](https://docs.vulkan.org/guide/latest/synchronization_examples.html)、
[timeline semaphore](https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html) 和
VMA [映射规范](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/memory_mapping.html)。
开发记录：[0045](../development/0045-graphics-resources-submission.md)。
