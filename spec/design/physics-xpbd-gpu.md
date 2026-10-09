---
module: physics-xpbd-gpu
created_at: "2026-10-09T10:59:13+08:00"
updated_at: "2026-10-09T15:30:00+08:00"
status: accepted
---

# GPU XPBD

## 范围与依赖

M10.3 将 [CPU XPBD](physics-xpbd.md) 的预测、分色距离约束、地面投影、速度重建映射为
[Graph](graphics-graph.md) Pass。`engine/physics/xpbd-gpu` 提供 `dk::physics_xpbd_gpu`，
PUBLIC 依赖 CPU XPBD/Graph，PRIVATE Shaders；不依赖 Render、Scene、Framework 或窗口。
可选 `DK_BUILD_PHYSICS_GPU` 默认关闭；windows-graphics 开启。CPU-only Runtime 仍使用 CPU 求解器；可选 [模拟服务](physics-api.md) 在 M10.4 接入 GPU 命令/脚本实验。渲染由独立 [布片 Pass](render-simulation.md) 消费。

## 数据和算法

初始化接收有效 CPU 求解器的拥有型快照，保留其约束顺序、颜色区间和初始退化方向；
位置/逆质量、速度、约束各为连续 16-byte 记录，方向为 float4。上传仅在初始化发生。
每色约束不共享端点，可无原子浮点写并行求解；颜色之间、每次迭代地面投影之间由 Graph 建立屏障。
每拍清零 lambda，拍内累计；dt 的 float 换算、阻尼和地面规则与 CPU 相同。
每个 dispatch 使用 64 线程并检查尾部边界。不实现自碰撞、弯曲或异步 compute。

`advance(queue, dt_ns, count, options, append)` 使用显式固定 dt（1..33.333333 ms），count 为 0..8；
总计算 Pass 不超过 4096。count=0 复制当前数据，可在暂停时绘制，步数不变。
初始位置、速度、约束和方向上传到设备；每批从已发布位置/速度复制到候选缓冲区。
预测临时位置、lambda 属于该次执行；新位置/速度为输出。步数仅在成功提交时增加。
不会默认读回粒子或等待 GPU。显式 readback 选项生成位置/速度读回，完成前读取返回错误。
快照补充初始方向，避免从已经推进过的 CPU 状态重算方向导致退化行为不同。

## 状态、提交与寿命

求解器拥有配置、静态数据和最新 GPU 输出；frame 拥有一次 Execution、提交票据和步数。
所有接口外部串行、单队列；不保存借用 queue 指针。参数、预算、录制、扩展 Pass 或提交失败均不发布候选，
原缓冲区/步数不变。所有分配在提交前完成；提交后仅移动 Execution 和分享输出所有权。
图执行保留所有在途输入、临时资源、descriptor 和 pipeline，销毁 solver/frame 不提前释放资源；queue.close 排空。
连续无消费者的相同步数/readback模式复用一个不可变编译计划；固定拓扑/配置保证声明相同，
dt只进入当次push constant，外部buffer和录制上下文每次重新绑定。缓存仅在提交成功后替换；含消费者的图独立编译。
提交成功不是 GPU 完成，也不是数值健康证明。设备丢失由 queue 报错；不承诺恢复。
显式诊断读回检查有限性及 CPU 相同的 1e6 数值边界，异常返回 invalid_state；GPU 数值异常不能撤销已提交批次，
调用者需停止该实验并重建。常规无读回路径不承诺同步报告数值发散，不沿用 CPU 整批数值回滚保证。

扩展回调仅在 advance 的同步建图期间借用 Graph、位置 BufferId/资源索引与粒子数，追加只读消费者，
回调数据须存活至 advance 返回；不得提交、改写物理资源或保留借用引用。共享图中先声明所有物理 buffer，
消费者追加 buffer/image 后编译一次、提交一次。诊断可捕获计划与实际同步列表。

## 验证

真实 Vulkan 同步验证；单粒子自由落体、柔性单边/质量权重、退化方向和地面，
seed=42 的 8×8 布片 300 拍、dt=10ms 对照同一 CPU 输入。
预先限定最大位置分量绝对差 ≤ 2e-3 m、速度 ≤ 2e-2 m/s，固定点与地面 ≤ 1e-6 m；
短解析例位置/速度 ≤ 2e-5。比较全量数组及独立计算的约束误差，不承诺跨 GPU 逐位一致。
验证 count=0、分批步数、非法参数、扩展录制失败、驱动提交失败、旧帧不变、待完成读回拒绝、在途销毁和关闭。
证据见 [0076](../development/0076-gpu-xpbd.md)。

## M11.1 性能观测

沿用队列显式开关，Graph 的 compute Pass 对应预测/分色约束/地面/速度求解，transfer 单列复制/读回，
布片消费者归 draw。增加 CPU 建图与初始化区间，shader 编译/Graph 编译/提交/等待单列。
观测不更改 dt、步数、分色、buffer、缓存键或数值发布点。采集失败仅影响观测，不撤销已提交状态。
独立 GPU profiling 探针比较相同输入的开关结果并导出区间；本节不新增模拟命令或改变同步服务。

## M11.3 有限任务的服务封装

私有 GpuSimulation 增加 submit/poll，submit 只推进并保留一个 pending frame；已有pending时拒绝再次提交。
poll 使用 frame.wait(queue, 0)，未完成返回false并继续保活；仅成功完成才退休frame并验证驱动诊断。
GPU提交计数由solver持有，完成数由任务单独记录；等待失败不能伪装回滚或完成。
有限任务的设备、MemorySystem、ThreadContext、queue和frame始终在同一专属worker创建/使用/销毁，
owner只读进度并发控制请求，不跨线程借用GPU状态。Stop排空后由owner移除Play；不detach在途worker。
原 step/read/capture 仍提供同步契约，有限任务显式诊断在稳定边界交给worker执行。
详见[任务状态契约](physics-api.md#m113-有界有限步任务)及[0080](../development/0080-bounded-simulation-tasks.md)。
