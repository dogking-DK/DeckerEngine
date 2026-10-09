---
module: graphics-graph
created_at: "2026-10-02T23:00:00+08:00"
updated_at: "2026-10-09T14:26:13+08:00"
status: accepted
---

# GPU Graph 设计

## 目标与本次范围

M6 用同一图组织上传、计算、绘制和读回。M6.1 提供 CPU 声明与结构校验：
transient/external buffer/image、Pass 访问与副作用、输出和显式依赖。
M6.2 发布独立 CPU 编译计划（依赖、排序、裁剪与逻辑生命周期）。
M6.3 接入状态导入/导出及单队列执行；M6.4 迁移样例并提供计划/同步诊断。阶段状态见 [Roadmap](../roadmap.md)。

## 模块边界和依赖方向

`engine/graphics/graph` 提供 `dk::graphics_graph`，公开入口 `dk/graphics/Graph.hpp`、`dk/graphics/GraphExecution.hpp`、`dk/graphics/GraphDiagnostics.hpp`，
命名空间 `dk::graphics::graph`。可选 `DK_BUILD_GRAPHICS_GRAPH` 默认 OFF，要求 device；
windows-graphics 预设显式开启，CPU runner 不链接 Graph。PUBLIC 依赖 device，以复用
BufferDesc/ImageDesc/AccessState、Memory 和 Vulkan 值类型；不依赖 Slang、SDL、Scene 或 Offscreen。
Graph 的 CPU 测试不初始化设备或 loader。

从 device 的已有录制验证提取纯函数 `ResourceValidation.hpp`：描述、stage/access/usage/layout、
范围检查由 Graph 与 CommandBatch 共用，避免两套相互偏离的规则；设备能力仍在实际创建时验证。
Graph 只消费 ResourceFactory、encoder、barrier、SubmissionQueue，不重建 Vulkan 所有权层。

## 接口与数据设计

- `Graph::create(resource)` 创建拥有型图；`declare_buffer/declare_image` 复制名字与描述。
  `Lifetime::transient/external` 是逻辑声明；external 在执行时绑定已有 Buffer/Image owner。
  external 的 `initialized` 表示导入时整个资源内容有效，transient 必须为 false。
  精细初始/最终状态及实际外部 owner 的绑定见执行契约。
- `BufferId/ImageId/PassId` 是不同类型，携带弱图身份与索引，不能手工伪造；默认、跨图、
  reset 前和已销毁图的引用都拒绝。move 转移身份，目标原有句柄失效。
- `Use` 携带类型化资源 ID 与 `AccessDescription`（stage/access/layout、buffer 字节范围、
  image aspect/mip/layer、full_overwrite）。`VK_WHOLE_SIZE` 归一为精确字节范围；image 要求显式范围。
  Pass 状态的 initialized 必须 false，内容有效性由图推导。
- `PassDesc` 包含名字、访问 span 和 side_effect。单队列首版不区分队列/Pass 类别，访问阶段表达用途。
  span/string_view 仅借用到调用返回，图复制所有持久字段。
  同一 Pass 的同 buffer 访问必须合并；image 不允许重叠子资源声明，非重叠允许。
  不支持 Pass 内隐藏 barrier；需拆成 Pass。Host 访问由上传/读回外围负责，不进入 GPU Pass。
- `add_dependency(before, after)` 表达执行先后；相同边幂等，自环立即拒绝。
  允许先加入多条边，再由 validate 检查组合循环；`remove_dependency` 支持修正，缺边为无变化。
  `mark_output(resource)` 表示需要保留的完整资源，重复标记幂等。空图有效，纯副作用 Pass 有效。
- 资源读写依赖按 Pass 声明顺序定义：RAW/WAR/WAW 保持该顺序，discard 写仍有 WAW/WAR；
  buffer 同步保守覆盖整个对象，与 M5 一致；image 按子资源相交，读读布局不同也产生依赖。
  显式反向边不能颠倒已有资源依赖，否则形成循环。编译可重排没有依赖的 Pass。
- 全部读取或保留内容的部分写，必须有 external 初始内容或前序 full_overwrite 覆盖访问范围。
  full_overwrite 是调用者完整写入声明范围的保证，不能同时声明读访问。
  buffer 按字节区间、image 按 mip/layer 检查覆盖；多个写入可以合并覆盖，输出要求整个资源有效。
  不检查 shader 的实际写入，调用者必须兑现声明。
- `validate()` 返回首个可定位 Error，包含资源/Pass 名字和索引；循环包含闭合 Pass 路径。
  先检查内容，再检查资源边与显式边的合图。不缓存“有效”标记，compile 始终重新验证完整声明，包括将被裁剪的 Pass。
  查询返回只读借用 view；任何图修改、移动赋值、reset 或销毁后不得再使用 view。

## M6.2 编译计划

`Graph::compile()` 返回 move-only `CompiledGraph`，整个流程仅使用 CPU 与创建图的 Memory 域。
先验证完整图，再在临时候选中完成下列步骤；全部成功后才返回计划，失败不修改原图或先前计划。

### 依赖与裁剪

- 顺序约束复用 M6.1：显式边、RAW/WAR/WAW 及 image layout 转换；buffer 按整个对象，image 按相交子资源。
  诊断 `Dependency` 包含 before/after 的 Pass 声明索引、kind，以及资源的计划索引（显式边无资源）。
  多个原因独立保留，相同原因/资源/端点去重。
- 内容依赖单独计算：对读取或保留内容的访问，按声明顺序逆向找到覆盖对应字节/mip/layer 的最近写入者。
  完整覆盖写不依赖旧内容；保留写依赖旧内容并成为后续消费者的生产者。
  导入且 initialized 的剩余范围由外部提供，没有对应 Pass 边；部分写或多子资源生产者可能有多条内容边。
- 根为 side_effect Pass 和标记输出各部分的最终写入者。沿内容依赖与显式前驱保留闭包；
  RAW/WAR/WAW/layout 的纯顺序边不单独强制前驱存活。
  因此完整覆盖前的无用写可以裁剪，显式前置操作不能被意外删除。
  external 的未标记写入也可裁剪；调用者需用 mark_output 或 side_effect 声明外部可观察效果。
- 在保留 Pass 的诱导子图上排序，包含所有幸存的顺序、内容和显式边。
  每次选声明索引最小的就绪 Pass，输出确定性拓扑顺序；无根图可裁为零 Pass。
  即使死分支存在未初始化读取或循环，编译也拒绝，不能利用裁剪隐藏非法声明。

### 数据、生命周期与分配

- `CompiledGraph::passes()` 保留全部 Pass 的名字、副作用、归一化访问、retained 和可选 order_index；
  `order()` 只含保留 Pass 的声明索引；`dependencies()` 只含两端均保留的边。
- `resources()` 顺序固定为 buffer 声明序列后接 image 声明序列；`PlannedUse::resource` 和依赖/分配条目
  使用此快照内的连续索引，不能作为另一计划或 Graph 的 ID。
  资源条目复制名称、类型化描述、原同类型声明索引、lifetime/initialized/output、retained、
  可选 first_use/last_use（均为 order 的位置）及 allocation_index。
- retained 资源为保留 Pass 使用的资源或标记输出。external 只登记，不列入 transient 分配；
  未使用的 initialized external 输出 retained=true、无 first/last_use，也不制造空 Pass。
- 每个活跃 transient 资源有独立 `TransientAllocation`，不做 aliasing 或物理块复用。
  create_before 为首次使用位置；普通资源 release_after 为末次使用位置。
  标记输出的 release_after 为空，表示需要向结果所有者保留，不能在末次 Pass 后当作临时量丢弃。
  分配条目按 create_before、资源索引排序。这里只规划逻辑生命周期；物理大小/对齐和显存类型仍由
  M6.3 的 ResourceFactory/VMA 决定，GPU pending 保活仍由 M5 完成跟踪保障，不能按 CPU 位置提前销毁。
- 名字、描述、访问和索引全部复制到计划；不保存原图 ID 或原图对象。
  原图修改/reset/销毁不改变计划，原 Graph ID 仍按原规则失效；重复 compile 返回各自独立快照。
  计划的只读 span/string_view 借用到计划被覆盖或销毁，Memory 关闭后仍允许读取已发布快照。
  默认/移后 CompiledGraph 的 bool 为 false，所有只读列表为空；合法空图计划 bool 为 true。

### 验证与限制

覆盖真实 upload→compute→draw→readback 计划、确定性排序、死分支、旧写覆盖、显式前驱、WAR/WAW、
子范围/部分保留/多 mip/layer 生产者、external 输出、生命周期、快照独立性、预算失败与关闭回收。
复用 dk_graph_tests；本阶段不改 device 录制，默认不重复 GPU/窗口/全量回归。
编译沿用成对依赖检查与区间分割，优先明确语义；不承诺大图性能。GPU barrier/执行见下文。

## 单队列执行契约（M6.3）

- `GraphExecution.hpp` 的 `execute(plan, queue, desc)` 每次创建独立资源实例、录制一个 batch 并提交一次。
  绑定和回调使用当前计划索引。每个 retained external 必须有唯一绑定，每个 retained Pass 必须有唯一回调；
  裁剪 Pass 不调用，裁剪资源不分配。绑定拒绝错类型/描述、空 owner、重复物理资源、跨设备、正在其他 batch
  录制或未在当前 batch 获取的 WSI image；本阶段只支持同队列自有 buffer/image。
- external 通过显式 `share()` 共享已有 owner，无原生重复所有权。描述精确匹配。
  默认从 owner 的上次成功提交状态导入，可提供 expected initial states（buffer 一项；image layer-major/mip-minor）
  作全量陈旧状态断言；不允许伪造状态覆盖账本。声明 initialized=true 要求所有导入子资源已初始化。
  绑定引用及回调 user_data 只借用到 execute 返回；回调外部 CPU 副作用不保证回滚。
- `PassContext` 提供当前 Pass 声明资源的借用查询，以及 typed copy/fill/clear、compute/render encoder。
  不暴露可移动/提交/重新 prepare 的 CommandBatch。调用者必须遵守声明的范围、绑定和 full_overwrite 承诺；
  不保存上下文、资源引用或 encoder 到回调之外。回调返回 Result；异常转为执行错误。
  每 Pass 前用 `CommandBatch::prepare` 生成 synchronization2 barrier；buffer 整体追踪，image 按 mip/layer。
  每 Pass 后 `finish_pass` 要求没有活动 rendering 或尚未执行的声明写入，并清除准备状态、使 encoder 失效。
  不推断 shader 内的实际访问与完整覆盖，额外未声明的资源访问仍属于调用者违约。
- transient 在 create_before 通过队列工厂创建；非输出在 release_after 释放执行层引用，
  batch/pending slot 仍保活实际 GPU owner。输出由 `Execution` 持有，可在完成后读取，或作为下次图的外部绑定。
  `Execution` 只读导出 external/输出的最终状态快照；后续提交不修改旧快照。
- 可对 retained external/输出声明 final access（不得 full_overwrite 或伪造 initialized），
  在所有 Pass 后统一 prepare。允许不重叠 image 子资源项，同 buffer 最多一项。
  final access 只建立下一使用者所需同步/layout，不初始化内容。
  buffer initialized 沿用整资源保守账本；部分范围写入不应将整个 buffer 标为已初始化，
  即使多个范围累计覆盖全体也可保持 false。图内部的内容依赖仍按精确字节范围证明。
- 所有输出 owner、状态快照、回调目录和临时元数据在提交前构建；唯一提交点为成功的队列 submit。
  参数/分配/回调/提交失败时销毁未提交 batch 和候选结果，不发布全局资源状态或完成票据；先前执行不变。
  成功后仅做无分配的 owner 移动/释放，返回已有 Submission；通过 queue.wait/poll 跟踪，超时保留 pending。
  结果提前销毁不取消已提交 GPU 工作。queue 及其资源仍须外部串行访问。
- 计划/执行持久元数据使用计划的 Memory resource；资源和 batch 使用队列的 Memory resource。
  关闭计划 Memory 后可读已有快照，但拒绝新 execute。无队列并发、aliasing、WSI 提交或 hidden wait。

验证：复用 CPU 图用例，新增 GPU probe 验证实际 barrier/layout、重复执行、导入断言/导出快照、
子资源、裁剪、输出保活、录制/提交/分配失败、超时/完成/设备丢失。同步回归直接变更的资源层。

## 生命周期、并发和错误处理

Graph 是 move-only，调用者串行访问。持久字段及校验临时容器使用创建时的 Memory resource；
Error 延续 Core 的 std::string 边界。弱句柄不会保活图数据，但会保留身份控制块直到句柄释放。
关闭 Memory 域后拒绝声明、查询和校验；counts 仍可观察数量，销毁仍可回收；不承诺 OS OOM 下 Error 分配必定成功。

资源/Pass 记录通过 Memory UniquePtr 独立拥有，发布和目录扩容只移动指针。
MSVC Debug 的 allocator-only 空容器构造及 string/vector move 可能在 noexcept 内分配 proxy；
图使用可抛异常的零 count 构造，不移动包含这些容器的记录，使预算失败能到达 Result 边界。
声明先校验并构建候选，再一次 append 发布。无效参数或可捕获分配失败不改变计数、内容、
依赖、输出或已有句柄。reset 先创建空候选，再替换身份；失败保留原图。
validate/compile 只读，失败不修改图，不存在 GPU 状态提交点。执行的提交点见单队列执行契约。

## 声明层实施与验证依据（M6.1）

1. 提取 M5 纯验证接口，建立可选 Graph target 与 CPU 测试入口。
2. 实现声明、归一化、只读查询、输出、依赖、reset 和失败保护。
3. 校验未初始化读取/保留、范围覆盖、显式/资源循环和句柄寿命。
4. 测试 upload→compute→draw→readback 声明、深度/多 mip/layer、跨图/移后/reset、
   溢出与空范围、非法状态、重复声明、资源与显式依赖混合循环、Memory 预算失败和关闭。
5. 定向回归 device CPU 资源验证与受影响的 GPU 资源/离屏 probe；不运行全量或多配置矩阵。

## 决策与限制

使用成对依赖检查和迭代 DFS 验证循环，没有递归深度限制。
M10.3 的多拍 XPBD 暴露逐边线性去重和每节点扫描全部边的成本：依赖改为批量排序去重，
DFS/调度只遍历对应 before 区间；裁剪使用仅含 contents/显式边的反向索引。
依赖原因、范围覆盖、确定性拓扑次序和最终诊断顺序保持不变；仍是成对分析，不声称任意大图线性扩展。
真实性能/回归证据见 [0076](../development/0076-gpu-xpbd.md)。
硬件格式/extent 限制不在纯 CPU 校验中证明。暂不提供版本化 SSA 资源、多队列、aliasing、
历史图句柄、并发构建或跨队列导入状态；这些能力不能从已有声明接口推断。

## 相关记录

[架构](architecture.md)、[资源](graphics-resources.md)、[Vulkan 使用层](graphics-vulkan.md)、
[0057 声明记录](../development/0057-graph-declarations.md)、[0058 编译记录](../development/0058-graph-compilation.md)、[0059 执行记录](../development/0059-graph-execution.md)、[0060 集成记录](../development/0060-graph-integration.md)。

## 样例与诊断（M6.4）

Offscreen 的 draw/dispatch 改为 Graph 客户端，GPU 上传、计算、绘制、读回均为显式 Pass。
清屏采用独立 transfer clear Pass（完整覆盖），draw 使用 Load 和保留读写；避免把 attachment 读访问
伪装成不读取旧内容的 full_overwrite。组合 GPU 样例用上传的初始顶点经过 compute 后作为 vertex buffer 绘制，
最后读回；与 M5 图像/计算基线及重复运行结果比较。管线/绑定仍来自 Device 工厂，Graph 不依赖 Shader 编译器。

`format_plan(plan)` 返回拥有型 PlanReport，其 text() 借用内部 Memory String：按声明索引列出 retained/culled Pass、访问范围、
资源类型/寿命/首末使用/分配，以及拓扑顺序和依赖原因；名字转义换行与引号，文本稳定、无地址或设备句柄。
格式为人类诊断，不承诺持久化协议；默认/关闭的计划域拒绝生成，已返回文本仍独立有效。

`ExecutionDesc::capture_synchronization` 默认 false；启用后 Execution::synchronization() 返回实际录制的
每个资源 barrier 输入（Pass 索引或 final、资源索引、mip/layer、before 和目标 state）。
buffer 为整对象，image 逐子资源；目标 state 的 initialized 沿用 before（barrier 不创造内容），
read/read 时目标访问是本次请求，可能与账本累计的访问不同。顺序与 prepare 的发出顺序一致。
计划诊断只在执行候选中分配，失败仍不发布/提交半个结果；该文本不含运行计时，GPU 观测见 M11.1。
错误补充执行阶段、Pass 名字/索引或资源名字/索引上下文，保留原 Error code/message/context。

## M7.2 颜色 footprint 与深度清除

PassContext 的 image copy 按 Device 的 color_texel_bytes 计算实际 buffer 范围（含 RGBA32F），
不得用四字节假设放过欠声明范围。clear_depth 只访问已声明 depth 子资源并委托 typed Device 操作，
完整写入的 content/lifetime/submit 规则保持不变。关联 [0062](../development/0062-render-pipeline.md)。

## M11.1 Pass 观测

execute 在启用队列观测时围绕每个保留 Pass 的 prepare/record/finish 记录区间，裁剪 Pass 不发事件。
分类从声明的访问阶段得出：compute 优先、graphics 次之、transfer 次之，其余 other；名称为拥有型副本。
整个提交区间另包含导入/最终屏障等 GPU 工作；CPU 建图/编译/录制分别埋点。观测不进入编译计划或缓存键。
失败放弃的图不发布 GPU 时间；完整结果由 Execution.submission().gpu_profile() 查询。
