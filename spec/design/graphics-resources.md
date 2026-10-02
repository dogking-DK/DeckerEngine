---
module: graphics-resources
created_at: "2026-09-28T18:19:00+08:00"
updated_at: "2026-10-02T21:07:00+08:00"
status: accepted
---

# Vulkan 资源与提交生命周期

## 目标、边界与依赖

M5.2 在 [设备底座](graphics-device.md) 上交付 VMA Buffer/Image、上传/读回、单队列提交、
完成票据、固定提交槽和延迟回收。实现位于 engine/graphics/device，继续使用 dk::graphics_device；
公开 Resources.hpp，不新增依赖或 feature。普通 Vulkan 所有权使用 vk::raii；VMA 资源配对释放。
设备使用层现在覆盖对象、管线、绑定和录制；不负责 shader 编译、窗口、Graph 调度、多队列或后台提交线程。
[离屏模块](graphics-offscreen.md) 负责特定绘制/计算输入输出，逐步迁移到同一批次保留机制。

[M5.5 使用层设计](graphics-vulkan.md) 扩展全部 GPU 对象保留和常用操作。
M5.5.1 的 GpuObjects.hpp 提供 ResourceFactory、ImageView/Sampler/ShaderModule，
工厂以 weak queue 借用身份拒绝失效/关闭调用，普通对象独立保活 device，view 保活 image。
ShaderModule 复制入口/阶段/反射，原始编译产物可在工厂返回后释放；不在设备模块链接 Slang。
M5.5.2 增加 Pipeline.hpp/Bindings.hpp：跨阶段布局合并、可复用 graphics/compute 管线、
不可变 BindingSet，支持多个 set、固定数组、uniform/storage buffer、sampled/storage image 和 sampler。
管线持有布局、绑定持有布局/池/资源；这些寿命引用不将 Buffer 标为 recording/pending 忙状态。
M5.5.3 的 CommandEncoder.hpp 接入类型化绘制/计算、显式 prepare/barrier 和对象闭包保留。
Buffer 按整对象、Image 按 mip/layer 跟踪阶段/访问/布局与内容有效性。
Transfer.hpp 提供 ReadbackRequest，upload/readback 便利方法直接位于 CommandBatch。

## 接口与数据

SubmissionQueue::create(resource, Device&&, slot_count) 消费设备并拥有一个独占提交域；
slot_count 为 1–64。创建中途失败会销毁已消费设备；调用者不得继续使用移后 Device。
Buffer/Image 由队列工厂创建，move-only，可为空；内部资源控制块和容器归入调用者 Memory resource。
BufferDesc 指定大小、Vulkan usage、device/upload/readback 内存用途；CPU write/read 检查角色、范围、忙状态，
通过 VMA map、flush/invalidate、unmap 配对处理，设备用途不能直接访问。
map/范围/角色失败不写用户数据；read 的 invalidate 失败不修改输出；write 的 flush 失败时
host 内存可能已写入，不保证字节回滚，也不会因此自动提交 GPU 工作。
ImageDesc 提供多 mip/layer、sample=1 的二维 image，view 支持 2D/2D array。color 支持 RGBA8 UNORM/SRGB、BGRA8 UNORM、
R32_UINT/R32_SFLOAT，均为每像素 4 字节；D32_SFLOAT 只支持 depth attachment/sampling，拒绝 byte copy。
检查 extent、mip/layer、字节数溢出、usage 和设备 format 能力；Image::state(mip,layer) 替代单值 layout 查询。
Buffer/Image 的 vk::* 句柄只借用，不得额外销毁；VMA allocation 不公开。

begin() 获取空闲槽并开始 one-time command buffer。prepare(ResourceUse) 根据局部账本发 barrier；
barrier(ResourceBarrier) 校验显式 before 状态后使用同一实现，供未来 Graph 直接规划同步。
copy_buffer、带 ImageCopyRegion 的双向 copy、fill/clear 只执行已准备访问，缺少同步在原生命令前拒绝。
ImageCopyRegion 指定 mip/layer、像素区域、buffer offset/row length/image height；按 footprint 验证范围和溢出。
写访问消费 prepare 声明，后续依赖需要重新 prepare；连续只读保留声明并累积读取阶段。
Buffer 范围用于验证，同步保守覆盖整对象；image 按子资源独立处理。
Undefined 只作为初始布局，read/load 必须有已初始化内容；局部写入不证明整子资源有效。
shader 整图写入可在 ResourceUse.full_overwrite 声明保证，真实性由调用者负责。

compute()/begin_rendering() 返回借用同一批次的 encoder；代次检查拒绝过期、提交后或 batch 移动后的调用。
RenderEncoder 默认设置覆盖 render area 的 viewport/scissor，支持单 color、可选 D32 depth、vertex/index、多实例和 push constants。
rendering 不可嵌套，scope 内禁止 prepare/copy/compute；end 或有效 encoder 析构结束 scope。
在打开 rendering 时移动 batch 会将其标记 invalid，禁止提交；放弃时释放资源与预约。
绘制前检查 attachment 格式、全部所需绑定、常量、顶点和索引字节范围。
索引内容决定的实际 vertex 地址不能在 CPU 侧推断，调用者保证索引值与 vertex_offset 不越界。
图形 storage 写入/attachment feedback 暂不支持。load/store discard 使内容失效，局部 clear 不证明全图初始化。

对象绑定自动保留资源闭包至 GPU 完成；单纯创建 BindingSet 不增加资源忙引用。
unsafe_record(before,after,callback) 是原生互操作入口；对象用 retain 重载声明，资源范围前后一一对应。
回调负责声明真实性，不得 end/reset/submit 或 signal 内部 timeline；退出使 encoder 失效，异常使 batch invalid。
旧无 region 的 copy/transition 和 command_buffer 暂保留给待迁移消费者及专门底层 probe；
其保守同步不属于 Graph 路径。零长度 copy 拒绝；CPU 零字节 read/write 是合法无变化。
支持布局 Undefined（仅初始）、TransferSrc/Dst、ShaderReadOnly、General、ColorAttachment、DepthStencilAttachment。

submit(CommandBatch&&) 返回 Submission（弱 owner 身份和单调值）；poll() 查询完成并回收，
wait(ticket, timeout_ns) 返回 bool（false 为超时），close() 等待并停止新录制。
Stats 区分 recording/pending/free 槽、提交值、已完成值和 pending 保留资源引用数。

upload 接受 buffer slice 或 color image region 与输入 byte span，返回前复制输入至独立 staging；
输入内存可立即释放。多次 upload/readback 只录制到当前 batch，不隐式提交或等待。
image upload 支持行步长及前缀 offset，要求输入字节数等于 footprint；image readback 返回紧密排列的区域，
region 的 buffer_offset/row_length/image_height 必须为零，低层 copy 仍可指定目的步长。
组合操作首条命令前的错误保留有效 batch；已发命令后的异常将 batch 标记 invalid，禁止提交。

ReadbackRequest 为 move-only，description 返回大小/格式/区域尺寸/行字节数；status 为
unsubmitted/pending/ready/cancelled/device_lost。try_read 在 unsubmitted/pending 返回 false 且不修改输出，
cancelled/lost 返回错误；ready 时经 VMA invalidate 读取，输出 span 必须精确匹配大小。
请求不持有 queue，只持有独立完成记录及 staging；queue 的 poll/wait/close 或正常析构更新完成状态。
submit 失败或 batch 放弃时取消请求，成功时以预分配列表绑定 pending；用户丢弃请求不会取消 GPU 工作。
queue 正常析构排空后，外部 request 仍可读；lost 是共享设备域终态，不返回成功数据。

## 所有权、提交点与失败

设备单独放入共享 lifetime，资源控制块和队列都持有它；资源可以晚于队列销毁。
队列 pending 槽持有资源，但资源不反向持有队列，避免环。录制 batch 持有队列，确保 command pool
在 batch 放弃前不销毁；票据不延长队列寿命。诊断 sink 的 user data 必须晚于所有资源的最终销毁。
所有 API、相关资源和借用队列由调用者串行访问；本阶段不提供线程安全提交。

槽状态为 free→recording→pending→free。未提交 batch 析构释放录制保留和资源预约，不提交 GPU 工作；
下次获取 free 槽才重置 pool。Buffer 被录制或 pending 引用时拒绝 CPU read/write，避免覆盖 GPU 数据。
Buffer/Image 每次只允许一个未提交 batch 预约，记录局部 AccessState；成功提交才发布状态，
失败或放弃不改变全局状态。已提交资源可在同队列后续 batch 继续使用，barrier 按队列顺序生效。

所有 CPU 分配/保留列表准备在 vkQueueSubmit2 之前完成。仅 VK_SUCCESS 是提交点：发布票据、
递增 timeline、转移预分配资源/对象列表、提交局部资源状态，这些步骤不得分配或抛出。
失败不发布票据/值/状态，batch 被消费且释放录制引用，资源仍由调用者持有；std::bad_alloc 保持标准异常约定。
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
