---
module: graphics-graph
created_at: "2026-10-02T23:00:00+08:00"
updated_at: "2026-10-02T23:30:11+08:00"
status: accepted
---

# GPU Graph 设计

## 目标与本次范围

M6 用同一图组织上传、计算、绘制和读回。本次 M6.1 实现 CPU 声明与结构校验：
transient/external buffer/image、Pass 访问与副作用、输出和显式依赖。
不创建 GPU 对象，不录制、不提交，也不提供可执行计划；M6.2 才发布排序、裁剪和生命周期计划，
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
  显式反向边不能颠倒已有资源依赖，否则形成循环。未来编译可重排没有依赖的 Pass。
- 全部读取或保留内容的部分写，必须有 external 初始内容或前序 full_overwrite 覆盖访问范围。
  full_overwrite 是调用者完整写入声明范围的保证，不能同时声明读访问。
  buffer 按字节区间、image 按 mip/layer 检查覆盖；多个写入可以合并覆盖，输出要求整个资源有效。
  不检查 shader 的实际写入，调用者必须兑现声明。
- `validate()` 返回首个可定位 Error，包含资源/Pass 名字和索引；循环包含闭合 Pass 路径。
  先检查内容，再检查资源边与显式边的合图。不缓存“有效”标记，未来 compile 必须重新验证。
  查询返回只读借用 view；任何图修改、移动赋值、reset 或销毁后不得再使用 view。

## 生命周期、并发和错误处理

Graph 是 move-only，调用者串行访问。持久字段及校验临时容器使用创建时的 Memory resource；
Error 延续 Core 的 std::string 边界。弱句柄不会保活图数据，但会保留身份控制块直到句柄释放。
关闭 Memory 域后拒绝声明、查询和校验；counts 仍可观察数量，销毁仍可回收；不承诺 OS OOM 下 Error 分配必定成功。

资源/Pass 记录通过 Memory UniquePtr 独立拥有，发布和目录扩容只移动指针。
MSVC Debug 的 allocator-only 空容器构造及 string/vector move 可能在 noexcept 内分配 proxy；
图使用可抛异常的零 count 构造，不移动包含这些容器的记录，使预算失败能到达 Result 边界。
声明先校验并构建候选，再一次 append 发布。无效参数或可捕获分配失败不改变计数、内容、
依赖、输出或已有句柄。reset 先创建空候选，再替换身份；失败保留原图。
validate 只读，失败不修改图，不存在 GPU 状态提交点。本阶段没有执行回调或 GPU owner。

## 实施步骤和验证计划

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
[0057 开发记录](../development/0057-graph-declarations.md)。
