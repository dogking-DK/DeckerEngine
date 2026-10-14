---
module: graphics-raytracing
created_at: "2026-10-14T08:18:00+08:00"
updated_at: "2026-10-14T13:58:31+08:00"
status: accepted
---

# Vulkan 光线追踪底座设计

## 目标与当前边界

为[渐进式路径追踪](render-pathtracing.md)提供硬件三角形求交、加速结构、光追管线和 Graph 调度。
M16.2–5已提供能力、AS、管线/SBT与Graph构建/trace；实施契约见下文，阶段状态和任务顺序由[Roadmap](../roadmap.md)维护。
沿用 Windows x64、Vulkan 1.4、Slang、Vulkan-Hpp RAII、VMA 和单提交队列，不新增跨 API RHI。

核对当前代码后的缺口：

| 当前入口 | 已有能力 | 本轮需要补充 |
| --- | --- | --- |
| [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp) | Vulkan 1.4、确定性选卡、同族共享设备；RT三态请求、支持/启用/缺项与limits | 后续按材质需要扩展独立非一致索引能力组 |
| [ShaderArtifact.hpp](../../engine/graphics/shader-types/include/dk/graphics/ShaderArtifact.hpp) | graphics/compute、四种RT阶段、AS descriptor与payload/SBT需求 | 后续按材质需求扩展能力组 |
| [Device](../../engine/graphics/device) | Buffer/Image、graphics/compute、提交保活；BDA/VMA、拥有型BLAS/TLAS、SBT与RT pipeline/encoder | 后续按Render需求扩展受支持子集 |
| [Graph](../../engine/graphics/graph) | buffer/image/AS/SBT声明、构建/trace内容依赖、同步、导入导出与类型化回调 | GPU路径积分作为Graph客户端接入 |
| [GpuAssets.cpp](../../engine/render/resources/src/GpuAssets.cpp) | vertex/index/transfer usage | RT 专用构建输入和着色数据；旧 buffer 不能事后追加 usage |

Vulkan 版本满足不代表光追 feature 已启用。现有 CPU 几何 BVH 和云 ray marching 均不构成本页的硬件光追实现。
首版只做设备端 BUILD 和三角形实例，不含 UPDATE/refit、压缩、序列化、程序化 AABB、motion blur、
pipeline library、indirect trace、ray query、SER、异步 compute 或多 GPU。

## M16.2设备能力与BDA实施契约

`DeviceOptions::ray_tracing`使用RayTracingMode，默认disabled；required参与select_adapter筛选，
显式索引不满足就拒绝。if_available沿既有排序选卡，在该卡完整启用或保持非RT，不能为RT偏好换卡。
无效枚举在创建loader前拒绝。能力查询对disabled同样执行，但不启用可选feature/扩展/VMA BDA标志。

`AdapterInfo::ray_tracing`保存三个扩展与三个feature的独立支持位；supported()要求六项全满足，
missing()返回固定六项字符串数组，满足项为空。属性快照分别保存AS/RT pipeline原生limits，pNext清空；
不支持的扩展不查询对应结构且limits为零。Vulkan 1.4基线和队列要求仍由普通选卡契约校验。
`Device::ray_tracing_enabled()`只表示实际启用的整组；BDA在本阶段随RT整组启用，不提供单独BDA请求。
纹理非一致索引能力组仍留待M16.7.1，M16.2不顺带启用无关shader/descriptor feature。

逻辑设备仅增加AS、RT pipeline、deferred host operations三个扩展及bufferDeviceAddress、
accelerationStructure、rayTracingPipeline三个feature。WSI扩展/feature链与RT合并，
不重复请求已由Vulkan 1.4覆盖的旧扩展名。共享queue view沿用同一启用状态与limits。
创建后检查本Device的BDA和后续设备端BUILD/trace所需入口；缺项为初始化错误，释放候选，
if_available不在驱动创建失败后静默重试普通设备。VMA仅在BDA启用时设置BUFFER_DEVICE_ADDRESS标志。

BufferDesc允许ShaderDeviceAddress usage；创建在VMA分配前要求Device已启用BDA。
`Buffer::device_address()`拒绝空buffer、lost device和未声明usage；成功返回当前buffer的非零地址。
查询不修改资源访问/初始化状态、不提交GPU工作，普通buffer不能事后追加usage。
地址只是借用数值，Buffer及Device寿命仍靠已有owner/share/提交保活；禁止把地址当所有权。
AS storage/build input usage在M16.3接入；SBT和trace在M16.4接入。

验证分层：纯策略遍历六项缺失及required/if_available/disabled、显式选卡与WSI组合；
fake dispatch验证查询结构、启用链、VMA flags、缺入口/创建失败/取消逆序回收与共享寿命；
真实required RT+validation probe核对limits、创建BDA buffer、非零稳定地址、普通buffer拒绝与关闭回收。
required probe缺设备/feature/validation一律失败，不以77跳过计为通过。普通Device/资源回归和CPU-only命令独立验证。

## M16.3 BLAS/TLAS实施契约

`AccelerationStructure.hpp`提供move/share拥有型AS、三角几何与实例描述、构建尺寸和状态快照。
`ResourceFactory::create_blas/create_tlas`创建独立候选，`CommandBatch::build`仅允许首次BUILD。
BLAS支持多几何、R32G32B32Sfloat、非索引或uint16/uint32索引；顶点stride/offset、索引offset、
primitive/vertex count、缓冲范围、设备geometry/primitive上限在查询尺寸前校验。
索引内容由调用者保证小于vertex_count，设备私有buffer不做隐式CPU读回。
TLAS输入CPU实例描述，复制行主序3x4变换及24-bit custom index/SBT offset、8-bit mask和基本flags；
拒绝非有限/不可逆变换、非法flags与上限，内部生成拥有型实例buffer，不接受裸BLAS地址。
空几何/实例拒绝；调用者为空场景选择环境路径。所有对象和输入必须属于同一提交域。

每个AS拥有原生句柄、独立VMA storage和Device；析构先销毁AS再释放storage。
候选拥有构建输入，TLAS在整个寿命内拥有其BLAS；构建完成后释放输入，scratch仅由录制/提交批次持有。
使用驱动查询的storage/scratch尺寸；每次录制独立scratch，检查size+alignment溢出，按实际BDA地址向上对齐，
允许驱动返回零scratch需求。AS storage offset固定0，不做suballocation。

AS使用独立类型账本，记录访问、candidate/submitted/completed和generation；generation为首次成功构建提交的timeline值，
在同一提交域内解释（同批次AS可共享该代）。录制只修改本地账本/保留资源；同批BLAS先构建后TLAS可读取，
跨批次使用已成功提交BLAS。queue submit成功才发布代和访问；poll/wait/close确认后才标completed。
录制放弃、参数/创建/提交失败保持旧代及候选状态，释放预约后可重试候选；已提交对象不能重建。
timeout保留pending引用，device lost状态覆盖普通状态。队列关闭/卸载走既有等待和保活路径。
创建/录制不自动替换调用者持有的旧代；调用者仅在submit成功后交换拥有句柄。

类型化build内部完成输入到AS-build shader-read屏障、BLAS build-write到TLAS build-read屏障；
scratch为专用临时工作区，不读取旧内容也不将其记为业务初始化。输入要求账本已有完整初始化，
可由本批upload或host write建立；只读重复输入合并。AS访问不伪装成普通storage-buffer访问。
M16.5 Graph AS声明/trace复用此拥有关系和提交账本，拆分prepare_build与build_prepared以把同步放在Pass外。

验证：纯策略覆盖计数/格式/跨度/地址溢出、scratch对齐、变换和字段边界；required RT GPU probe执行
多几何BLAS、多个BLAS的多实例TLAS、同批和跨批依赖，检查代/地址、失败保持旧代、放弃重试、
pending外部释放/timeout/关闭、VMA与Memory回收及validation/sync零诊断。已知射线命中仍属M16.4。
规范依据：[构建命令](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBuildAccelerationStructuresKHR.html)、
[尺寸查询](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetAccelerationStructureBuildSizesKHR.html)、
[三角输入](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureGeometryTrianglesDataKHR.html)、
[实例布局](https://docs.vulkan.org/refpages/latest/refpages/source/VkAccelerationStructureInstanceKHR.html)。

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

`DeviceOptions::ray_tracing`提供disabled / if_available / required请求，默认disabled。
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

已提供拥有型 `AccelerationStructure`和构建尺寸/描述，以及`ShaderBindingTable`与`RayTracingPipeline`。
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

后续Render集成按primitive建不可变BLAS并按资产内容/上传代次及构建选项复用；Device允许单个BLAS含多个三角几何。
变换变化只重建TLAS，资产缓存策略不在Device层实现。
TLAS owner 保留全部引用的 BLAS owner，描述中的 device address 不代替此引用。
几何变化重建相关 BLAS，拓扑/实例集合或变换变化重建 TLAS；材质值变化只更新着色表，
opaque/mask 或单/双面影响构建/instance flags 时，按实际 key 失效相应 AS。
空场景采用独立环境输出路径，不提交非法空构建。不可逆变换、实例索引超限和不支持的格式在提交前拒绝。

构建遵循 candidate → submitted → completed。SubmissionQueue提交成功后才发布submitted资源状态和代次；
同队列后续使用可依靠已声明依赖接续，CPU 不可将 submitted 报作完成。独立失败不污染旧快照。
首版不在旧 AS 上原地 rebuild/update，避免新旧帧同时引用时破坏不可变契约。

## Shader、管线与 SBT

### M16.4实施契约

ShaderStage增加raygeneration/miss/closesthit/anyhit，descriptor增加acceleration_structure。
SPIR-V 1.5能力白名单按RT阶段允许RayTracingKHR；Device在native shader/layout创建前检查实际RT启用。
Slang入口允许对应payload/hit-attribute参数类别，拒绝其他入口资源/uniform；全局AS绑定继续使用固定数组和set/binding。
产物schema v3记录RT需求、profile、源/依赖内容指纹，以及从SPIR-V提取的payload结构和静态TraceRay SBT选择参数。
payload首版限32-bit标量/向量、固定数组和结构体；按结构类型匹配，不能仅比较总字节数；三角hit attribute为float2。
不支持的接口和动态SBT选择明确拒绝，不通过手填元数据绕过原生SPIR-V检查。

ResourceFactory创建RayTracingPipeline和与其一一绑定的ShaderBindingTable。管线含一个raygen、每射线类型一个miss、
以ray-type交错排列的triangle hit groups（closest-hit必选、any-hit可选），允许同阶段多个入口；禁止混入graphics/compute。
首版TraceRay只由raygen发出，native递归深度1，路径反弹在raygen循环实现；callable/intersection/local SBT参数暂不支持。
首版ray_type_count为1..15、hit groups最多120、布局入口最多256；push constant最多一个共享范围。
pipeline创建前核对每个TraceRay的offset/stride/miss index及payload与对应miss/hit组；布局必须覆盖所有资源/阶段/push范围。
TLAS保留实例SBT offset和BLAS geometry count，trace验证offset+(geometryCount-1)*rayTypeCount+rayType在hit区域内。

SBT使用当前pipeline返回的handles，每区起点按base alignment、record stride按handle alignment，raygen size=stride。
分配前校验乘加/地址溢出、stride与dispatch设备上限；host upload buffer只由SBT owner暴露借用region，不允许写裸地址SBT。
SBT保留pipeline/layout，BindingSet保留TLAS/BLAS，成功提交保留整个依赖闭包；创建/录制/submit失败不发布半套对象或资源状态。
沿M16.3 AS账本扩展trace-read，显式CommandBatch::prepare_trace准备SBT及TLAS/BLAS同步；普通buffer/image仍由prepare声明。
RayTracingEncoder是带generation的批次借用，trace不隐式prepare；拒绝过期encoder、未准备/未构建AS、外来对象及不匹配SBT。

验收包括四阶段编译/反射、CLI v3与失败保留旧输出、payload/布局/阶段/SBT范围拒绝，及required RT validation/sync真实射线：
命中/未命中、primitive/instance custom index、重心/距离、mask、any-hit忽略与visibility两类射线；覆盖提交失败和pending释放。
不在本阶段实现Graph AS资源声明、路径积分或编辑器功能。

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
Scratch 在 AS build 阶段需要 AS read/write 同步，但其旧字节不是算法输入。
M16.5采用每次prepare_build独占scratch，不提供图内共享/复用scratch，因此无共享范围的WAR/WAW；
专用prepared-build状态保活内部临时用途，禁止shader读取未定义scratch内容，
不把scratch变成可初始化/导出的业务buffer。具体声明、依赖身份和准备/录制拆分见[Graph实施契约](graphics-graph.md#m165-as-与-sbt-实施契约)。

| 依赖 | stage/access 语义 |
| --- | --- |
| 上传 → AS build 输入 | transfer write → AS build / shader read |
| AS build scratch | AS build / AS read 与 AS write；新 scratch 无旧内容依赖 |
| BLAS build → TLAS build | AS build/write → AS build/read |
| TLAS/BLAS build → trace | AS build/write → ray tracing shader / AS read |
| SBT 上传 → trace | 当前host write → ray tracing shader / shader read（基础扩展路径） |
| 材质/几何上传 → hit shader | transfer write → ray tracing shader / storage 或 sampled read |
| trace 输出 → 累积/tone/readback | ray tracing shader / storage write → 实际 compute/fragment/transfer read |

统一扩展 ResourceValidation、CommandBatch ledger、Graph 编译和导出；AS barrier 由执行层生成，
Pass 内不藏 prepare/barrier。SBT 专用 access bit 只有额外启用对应 maintenance 扩展时才能用，
基础方案按 Khronos [RT 同步指南](https://docs.vulkan.org/guide/latest/extensions/ray_tracing.html)使用兼容的 shader read。
`PassContext::build/trace`提供受限类型化录制，不暴露submit/prepare，也不能保留回调借用到函数外。
每次 trace 的完整依赖闭包包含 pipeline/layout、SBT、绑定、TLAS/BLAS、着色数据与输出。

队列成功 submit 为资源状态提交点。CPU 分配、校验、录制和 submit 失败不发布账本；
timeout 保留 pending，销毁 Frame/cache 不取消 GPU 工作。取消停止后续提交，已提交内容等待安全回收；
device_lost 使队列失效，不承诺旧 GPU 画面仍可用。跨队列仅使用现有完成图像交接，不传递可变 AS。

## 验证与实施边界

按 Roadmap 逐阶段验收，先纯 CPU 再 required GPU，每次只回归受影响链路：

- 能力策略：缺单个扩展/feature、显式选卡、disabled 不访问 RT、纹理能力不足、limit/溢出拒绝。
- 资源/编译：BDA usage、AS 尺寸/对齐、独立对象寿命、shader 阶段/AS 反射/布局冲突、SBT 索引与对齐。
- 真实求交：已知三角形 miss/hit/重心/距离，多 BLAS 实例和负缩放/剪切，visibility ray 与 alpha mask。
- Graph：构建输入→BLAS→TLAS→trace→readback 顺序、裁剪、未初始化 AS、旧状态、跨图/队列拒绝。
- 失败：候选分配/编译/录制/提交失败保留旧资源；pending 卸载、取消、超时和关闭后无资源泄漏。
- required GPU 必须启用 validation 和同步验证；设备或层缺失不能记为通过。非 RT 光栅回归与 CPU-only 隔离分别验收。

设备能力/BDA已提供CPU策略与required RT探针，入口和实测见[0110](../development/0110-raytracing-device.md)。
AS构建实测见[0111](../development/0111-acceleration-structures.md)，四阶段编译、RT pipeline/SBT、
真实命中与visibility及失败寿命见[0112](../development/0112-raytracing-pipeline.md)。Graph AS构建/trace、导入导出、
失败和pending寿命见[0113](../development/0113-graph-raytracing.md)；GPU路径积分待M16.6。

## 相关设计与记录

[设备](graphics-device.md)、[资源](graphics-resources.md)、[使用层](graphics-vulkan.md)、
[shader](graphics-shaders.md)、[Graph](graphics-graph.md)、[路径追踪](render-pathtracing.md)、
[0108 规划记录](../development/0108-pathtracing-plan.md)。
