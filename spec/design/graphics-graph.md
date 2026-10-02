---
module: graphics-graph
created_at: "2026-10-02T23:00:00+08:00"
updated_at: "2026-10-03T07:24:14+08:00"
status: accepted
---

# GPU Graph 设计

## 目标与本次范围

M6 用同一图组织上传、计算、绘制和读回。M6.1 提供 CPU 声明与结构校验：
transient/external buffer/image、Pass 访问与副作用、输出和显式依赖。
本次 M6.2 发布 CPU 编译计划（依赖、排序、裁剪与逻辑生命周期），不创建 GPU 对象，不录制、不提交；
M6.3 接入状态导入/导出及单队列执行，M6.4 迁移样例。阶段状态见 [Roadmap](../roadmap.md)。

## 模块边界和依赖方向

`engine/graphics/graph` 提供 `dk::graphics_graph`，公开入口 `dk/graphics/Graph.hpp`，
命名空间 `dk::graphics::graph`。可选 `DK_BUILD_GRAPHICS_GRAPH` 默认 OFF，要求 device；
windows-graphics 预设显式开启，CPU runner 不链接 Graph。PUBLIC 依赖 device，以复用
BufferDesc/ImageDesc/AccessState、Memory 和 Vulkan 值类型；不依赖 Slang、SDL、Scene 或 Offscreen。
Graph 的 CPU 测试不初始化设备或 loader。

从 device 的已有录制验证提取纯函数 `ResourceValidation.hpp`：描述、stage/access/usage/layout、
范围检查由 Graph 与 CommandBatch 共用，避免两套相互偏离的规则；设备能力仍在实际创建时验证。
后续 Graph 只能消费 ResourceFactory、encoder、barrier、SubmissionQueue，不重建 Vulkan 所有权层。

## 接口与数据设计

- `Graph::create(resource)` 创建拥有型图；`declare_buffer/declare_image` 复制名字与描述。
  `Lifetime::transient/external` 是逻辑声明；external 目前不绑定原生对象。
  external 的 `initialized` 表示导入时整个资源内容有效，transient 必须为 false。
  精细初始/最终状态及实际外部 owner 的绑定在 M6.3 实现。
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
编译沿用成对依赖检查与区间分割，优先明确语义；不承诺大图性能。GPU barrier/执行仍属 M6.3。

## 生命周期、并发和错误处理

Graph 是 move-only，调用者串行访问。持久字段及校验临时容器使用创建时的 Memory resource；
Error 延续 Core 的 std::string 边界。弱句柄不会保活图数据，但会保留身份控制块直到句柄释放。
关闭 Memory 域后拒绝声明、查询和校验；counts 仍可观察数量，销毁仍可回收；不承诺 OS OOM 下 Error 分配必定成功。

资源/Pass 记录通过 Memory UniquePtr 独立拥有，发布和目录扩容只移动指针。
MSVC Debug 的 allocator-only 空容器构造及 string/vector move 可能在 noexcept 内分配 proxy；
图使用可抛异常的零 count 构造，不移动包含这些容器的记录，使预算失败能到达 Result 边界。
声明先校验并构建候选，再一次 append 发布。无效参数或可捕获分配失败不改变计数、内容、
依赖、输出或已有句柄。reset 先创建空候选，再替换身份；失败保留原图。
validate/compile 只读，失败不修改图，不存在 GPU 状态提交点。本阶段没有执行回调或 GPU owner。

## 声明层实施与验证依据（M6.1）

1. 提取 M5 纯验证接口，建立可选 Graph target 与 CPU 测试入口。
2. 实现声明、归一化、只读查询、输出、依赖、reset 和失败保护。
3. 校验未初始化读取/保留、范围覆盖、显式/资源循环和句柄寿命。
4. 测试 upload→compute→draw→readback 声明、深度/多 mip/layer、跨图/移后/reset、
   溢出与空范围、非法状态、重复声明、资源与显式依赖混合循环、Memory 预算失败和关闭。
5. 定向回归 device CPU 资源验证与受影响的 GPU 资源/离屏 probe；不运行全量或多配置矩阵。

## 决策与限制

先用清晰的成对依赖检查和迭代 DFS 验证循环，不声称大图编译性能；没有递归深度限制。
硬件格式/extent 限制不在纯 CPU 校验中证明。暂不提供版本化 SSA 资源、多队列、aliasing、
历史图句柄、并发构建、执行闭包或复杂导入状态；这些能力不能从已有声明接口推断。

## 相关记录

[架构](architecture.md)、[资源](graphics-resources.md)、[Vulkan 使用层](graphics-vulkan.md)、
[0057 声明记录](../development/0057-graph-declarations.md)、[0058 编译记录](../development/0058-graph-compilation.md)。
