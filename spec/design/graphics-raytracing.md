---
module: graphics-raytracing
created_at: "2026-10-14T08:18:00+08:00"
updated_at: "2026-10-14T08:22:56+08:00"
status: draft
---

# Vulkan 光线追踪底座设计

## 目标与当前边界

为[渐进式路径追踪](render-pathtracing.md)提供硬件三角形求交、加速结构、光追管线和 Graph 调度。
本页为 M16 的设计草案，接口名是拟议契约；实现状态和任务顺序仅由[Roadmap](../roadmap.md)维护。
沿用 Windows x64、Vulkan 1.4、Slang、Vulkan-Hpp RAII、VMA 和单提交队列，不新增跨 API RHI。

核对当前代码后的缺口：

| 当前入口 | 已有能力 | 本轮需要补充 |
| --- | --- | --- |
| [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp) | Vulkan 1.4、基本 feature、确定性选卡、同族共享设备 | 可选 RT feature/扩展协商、limits、拒绝原因 |
| [ShaderArtifact.hpp](../../engine/graphics/shader-types/include/dk/graphics/ShaderArtifact.hpp) | vertex/fragment/compute、五种 descriptor | raygen/miss/closest-hit/any-hit、AS descriptor、能力需求 |
| [Device](../../engine/graphics/device) | Buffer/Image、graphics/compute、提交保活 | BDA、BLAS/TLAS、SBT、RT pipeline/encoder |
| [Graph](../../engine/graphics/graph) | buffer/image 声明、同步和导入导出 | AS 内容依赖、build/trace 访问和类型化回调 |
| [GpuAssets.cpp](../../engine/render/resources/src/GpuAssets.cpp) | vertex/index/transfer usage | RT 专用构建输入和着色数据；旧 buffer 不能事后追加 usage |

Vulkan 版本满足不代表光追 feature 已启用。现有 CPU 几何 BVH 和云 ray marching 均不构成本页的硬件光追实现。
首版只做设备端 BUILD 和三角形实例，不含 UPDATE/refit、压缩、序列化、程序化 AABB、motion blur、
pipeline library、indirect trace、ray query、SER、异步 compute 或多 GPU。

## 模块与依赖

不另建一套 Vulkan 对象体系。设备能力、AS/SBT 对象和 encoder 扩展既有 `dk::graphics_device`；
纯 shader 产物扩展 `dk::graphics_shader_types`，编译/反射扩展 `dk::graphics_shaders`，图资源扩展 `dk::graphics_graph`。
公开头仍在各模块 `include/dk/graphics/`，实现放各自 `src/`。本设计不是预先创建空 target 的理由。

Device/Graph 不依赖 Render、Scene、Framework 或 Slang；Graph 继续消费 Device 的资源、访问校验和录制器。
Render 持有资产/场景到光追资源的映射，通过 Graph 录制，不借用原生 command buffer 绕过状态账本。
构建底座不强制设备具有 RT；非 RT 设备仍可运行原光栅/compute 路径。
本轮不升级三方依赖；先用当前固定 baseline 验证所需头文件、入口和 Slang 产物。
如确需升级，另按[三方库规范](../third-party-libraries.md)核验官方最新 port 并固定 baseline。

## 能力协商

拟扩展 `DeviceOptions` 为 disabled / if_available / required 的 RT 请求，默认 disabled。
能力快照分别报告 supported、enabled、未满足项和 limits；查询不得伪装成已经启用。
required 参与选卡筛选；显式 adapter 不满足则失败，不静默换卡。if_available 保留现有选卡顺序，
在选中卡上完整启用适用能力组，缺失时保持普通设备并提供原因；不改变用户选定 GPU。

核心能力组需要 `VK_KHR_acceleration_structure`、`VK_KHR_ray_tracing_pipeline`、
`VK_KHR_deferred_host_operations`，以及显式启用 `bufferDeviceAddress`、`accelerationStructure`、
`rayTracingPipeline`。Vulkan 1.4 已覆盖的核心依赖无需重复要求旧扩展名，feature 仍须逐项查询/启用。
VMA 仅在 BDA 已启用时设置对应 allocator 标志，地址 buffer 使用正确 usage 和内存分配标志。
依据：[KHR AS](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_acceleration_structure.html)、
[KHR RT pipeline](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_tracing_pipeline.html)、
[VMA BDA](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/enabling_buffer_device_address.html)。

纹理材质增加单独的有界数组能力组：`shaderSampledImageArrayNonUniformIndexing`，以及实际使用的
sampled-image/sampler/per-stage/descriptor-set limits。使用固定大小、全部初始化的 descriptor 数组；
不因此要求 runtimeDescriptorArray、partiallyBound 或 updateAfterBind。核心 RT probe 不以纹理能力为前提，
请求带纹理的 renderer 时能力不够须明确拒绝。Slang 实际生成的额外 capability 需审核后纳入请求，不能靠 validation 报错补救。

记录 AS geometry/primitive/instance 数量上限、scratch alignment、SBT handle size/alignment/base alignment、
maxShaderGroupStride、maxRayRecursionDepth、maxRayDispatchInvocationCount 和 relevant descriptor 限制。
尺寸乘法、索引、offset 和地址对齐先做溢出检查。所有 pipeline 用到的扩展入口从当前 Device dispatch 装载。
CPU-only 模块和 RT 关闭设备不得调用这些入口；共享 Device view 沿用创建时已启用的能力，不能运行中补开 feature。

## 资源、构建与所有权

拟提供拥有型 `AccelerationStructure`、构建尺寸/描述、`ShaderBindingTable` 和 `RayTracingPipeline`；
AS 区分 bottom/top level，并具有提交域身份、内容有效性、最后访问与构建代次。
AS owner 保留 storage buffer 和 Device，析构先释放 AS 句柄再释放 VMA buffer；禁止双重拥有同一 Vulkan buffer。
公开地址仅供内部描述组装，不能把整数地址作为寿命凭据。

| 资源 | 需要的用途 | 寿命约束 |
| --- | --- | --- |
| 顶点/索引/实例构建输入 | AS build input read-only、shader device address；需着色读取的 buffer 另加 storage | 录制及构建提交保活；着色用数据继续由快照保留 |
| BLAS/TLAS storage | AS storage；地址需求按创建路径声明 | AS owner 独占此代 storage；旧代在所有消费者完成前存活 |
| Scratch | storage、shader device address，满足设备 scratch alignment | 查询 build size 后分配；首版每个 build 独立范围，避免隐藏复用冲突 |
| SBT | shader binding table、shader device address、必要 transfer usage | 与创建它的 pipeline 代次绑定，trace 提交保活 |
| 材质/几何表、输出 | storage / sampled / transfer 按实际声明 | 绑定集、图执行和提交闭包保活 |

初版按 primitive 建不可变 BLAS；相同资产内容/上传代次和构建选项复用，变换变化只重建 TLAS。
TLAS owner 保留全部引用的 BLAS owner，描述中的 device address 不代替此引用。
几何变化重建相关 BLAS，拓扑/实例集合或变换变化重建 TLAS；材质值变化只更新着色表，
opaque/mask 或单/双面影响构建/instance flags 时，按实际 key 失效相应 AS。
空场景采用独立环境输出路径，不提交非法空构建。不可逆变换、实例索引超限和不支持的格式在提交前拒绝。

构建遵循 candidate → submitted → completed。Graph 提交成功后才发布 submitted 资源状态和代次；
同队列后续使用可依靠已声明依赖接续，CPU 不可将 submitted 报作完成。独立失败不污染旧快照。
首版不在旧 AS 上原地 rebuild/update，避免新旧帧同时引用时破坏不可变契约。

## Shader、管线与 SBT

扩展阶段为 raygeneration、miss、closesthit、anyhit；intersection/callable 留待实际需求。
增加 AS descriptor 的类型化反射/绑定，跨阶段合并仍拒绝 binding/type/count 冲突。
编译产物保存 feature/profile/源依赖指纹，离线 JSON schema 在实施时版本化；同步 shaderc 参考和发现接口。
保持逐入口编译和现有固定 SPIR-V 1.5 策略，显式核验 `SPV_KHR_ray_tracing` 产物及 payload 布局。
各阶段共享 payload 定义，采用全局 descriptor 和 push constants，首版无 SBT local parameters。
Slang 阶段和 capability 依据：[入口与参数](https://docs.shader-slang.org/en/stable/external/slang/docs/user-guide/02-conventional-features.html)、
[capability](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a3-02-reference-capability-atoms.html)。

SBT 以 pipeline 返回的 group handles 构造，不缓存跨 pipeline/device 的 handle 字节。
分别计算 raygen/miss/hit 区域，对齐、stride、size 和总 buffer 上界严格校验；raygen size=stride，
未用 callable 区域置零，命中索引通过实例/geometry/ray-type 的明确映射验证。
基础方案含 radiance 与 visibility 两类射线；hit shader 仅返回命中信息/遮挡，raygen 循环执行反弹和阴影查询。
因此 Vulkan 递归深度可为 1，路径最大反弹数独立设定，不能误将两者绑定。
依据：[SBT 与 trace 规范](https://docs.vulkan.org/spec/latest/chapters/raytracing.html)。

固定数组的每项都绑定有效默认或实际纹理，材质索引做 CPU/GPU 双侧范围保护，
非一致索引用 Slang 对应标记；所有可能被命中 shader 访问的资源均须声明和保留。
pipeline/绑定/SBT 构造全部完成才发布；失败保留旧程序和布局，不返回半套对象。

## Graph 同步与执行

AS 是一等类型化逻辑资源，不能只声明 storage buffer 或把 build 标为 side_effect 来规避依赖。
首版支持外部 AS owner 导入，由调用方先查询/分配候选，Graph 负责 GPU 构建；不实现 transient AS aliasing。
增加带图身份的 AS ID、build-read/build-write/trace-read 与有效性规则；首次完整 BUILD 是初始化写。
TLAS 构建显式依赖引用的 BLAS，trace 显式读取 TLAS 及其 BLAS 集合；无消费者的候选 build 可裁剪。
编译计划复制映射/依赖，执行验证绑定身份、同设备/队列及预期状态，跨图 ID、重复物理绑定均拒绝。
Scratch 在 AS build 阶段需要 AS read/write 同步，但其旧字节不是算法输入；
用专用 build-scratch 用途表达此区别，仅供类型化 build encoder 使用，不伪造普通 buffer 已初始化。
该用途参与资源保活和 WAR/WAW，禁止 shader 将未定义 scratch 内容当作业务输出读取；
新建、连续构建和误用 scratch 的图编译/同步案例分别验收。

| 依赖 | stage/access 语义 |
| --- | --- |
| 上传 → AS build 输入 | transfer write → AS build / shader read |
| AS build scratch | AS build / AS read 与 AS write；新 scratch 无旧内容依赖 |
| BLAS build → TLAS build | AS build/write → AS build/read |
| TLAS/BLAS build → trace | AS build/write → ray tracing shader / AS read |
| SBT 上传 → trace | transfer write → ray tracing shader / shader read（基础扩展路径） |
| 材质/几何上传 → hit shader | transfer write → ray tracing shader / storage 或 sampled read |
| trace 输出 → 累积/tone/readback | ray tracing shader / storage write → 实际 compute/fragment/transfer read |

统一扩展 ResourceValidation、CommandBatch ledger、Graph 编译和导出；AS barrier 由执行层生成，
Pass 内不藏 prepare/barrier。SBT 专用 access bit 只有额外启用对应 maintenance 扩展时才能用，
基础方案按 Khronos [RT 同步指南](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)使用兼容的 shader read。
`PassContext` 拟增加受限 build/trace encoder，不暴露 submit，也不能保留回调借用到函数外。
每次 trace 的完整依赖闭包包含 pipeline/layout、SBT、绑定、TLAS/BLAS、着色数据与输出。

队列成功 submit 为资源状态提交点。CPU 分配、校验、录制和 submit 失败不发布账本；
timeout 保留 pending，销毁 Frame/cache 不取消 GPU 工作。取消停止后续提交，已提交内容等待安全回收；
device_lost 使队列失效，不承诺旧 GPU 画面仍可用。跨队列仅使用现有完成图像交接，不传递可变 AS。

## 验证与实施边界

按 Roadmap 逐阶段落实本草案，先纯 CPU 再 required GPU，每次只回归受影响链路：

- 能力策略：缺单个扩展/feature、显式选卡、disabled 不访问 RT、纹理能力不足、limit/溢出拒绝。
- 资源/编译：BDA usage、AS 尺寸/对齐、独立对象寿命、shader 阶段/AS 反射/布局冲突、SBT 索引与对齐。
- 真实求交：已知三角形 miss/hit/重心/距离，多 BLAS 实例和负缩放/剪切，visibility ray 与 alpha mask。
- Graph：构建输入→BLAS→TLAS→trace→readback 顺序、裁剪、未初始化 AS、旧状态、跨图/队列拒绝。
- 失败：候选分配/编译/录制/提交失败保留旧资源；pending 卸载、取消、超时和关闭后无资源泄漏。
- required GPU 必须启用 validation 和同步验证；设备或层缺失不能记为通过。非 RT 光栅回归与 CPU-only 隔离分别验收。

测试程序和 CMake/选择表在实现对应任务时才创建。本次未探测本机 RT 能力，也没有 GPU 验收结果。

## 相关设计与记录

[设备](graphics-device.md)、[资源](graphics-resources.md)、[使用层](graphics-vulkan.md)、
[shader](graphics-shaders.md)、[Graph](graphics-graph.md)、[路径追踪](render-pathtracing.md)、
[0108 规划记录](../development/0108-pathtracing-plan.md)。
