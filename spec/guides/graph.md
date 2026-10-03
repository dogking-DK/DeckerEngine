---
created_at: "2026-10-02T23:20:00+08:00"
updated_at: "2026-10-03T14:45:24+08:00"
---

# GPU Graph 声明、编译与执行

[返回项目入口](../../README.md)。接口契约见 [Graph 设计](../design/graphics-graph.md)。
资源、Pass、依赖与输出在 CPU 上声明和编译；执行时绑定实际资源并使用单队列提交。

## 构建与验证

`windows-graphics` 开启 `DK_BUILD_GRAPHICS_GRAPH`，其他配置可显式开启，同时要求
`DK_BUILD_GRAPHICS_DEVICE` 和 Memory。消费者链接 `dk::graphics_graph`，
包含 `dk/graphics/Graph.hpp`；CPU 声明/编译无需设备。执行还需包含 `dk/graphics/GraphExecution.hpp`
并传入 SubmissionQueue；模块不依赖 Slang 或 SDL3。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_tests `
  -TestRegex '^dk\.graph\.graph ' -Reason 'GPU Graph CPU 声明、编译与生命周期'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_probe `
  -TestRegex '^dk\.graph\.gpu_validation$' -Reason 'GPU Graph 单队列执行与同步验证'
```

## 声明顺序

1. 以开放的 Memory ResourceHandle 创建 `graph::Graph::create(resource)`。
2. `declare_buffer/declare_image` 返回类型化 ID；默认 transient 内容未初始化。
   external 在 execute 时绑定 Buffer/Image；`initialized=true` 要求导入账本中整个资源内容有效。
3. `add_pass(PassDesc)` 复制名字和访问列表；每项 `Use` 包含资源 ID 及 AccessDescription。
   使用 M5 的 stage/access/layout 枚举。buffer 指定 offset/size，image 指定 aspect/mip/layer。
4. 完整写入访问范围时设置 `full_overwrite=true`；其他访问保留内容，需要已初始化数据。
   声明完整写入时不得同时带读访问。每个 Pass 的同 buffer 访问合并，image 子资源不能重叠。
5. `mark_output(id)` 标记需要保留的完整资源；外部副作用通过 PassDesc.side_effect 表达。
   `add_dependency(before, after)` 增加显式先后关系，`remove_dependency` 可修正依赖。
6. `validate()` 检查内容、资源依赖和显式边；失败通过 Error 的 message/context 定位。
   它不缓存成功状态，每次修改后需要重新校验。

读写依赖保持 Pass 声明顺序。buffer 同步按整个对象，image 按子资源相交；
覆盖写仍然需要等待先前读写。显式边可以重排互不依赖的 Pass，反转已有资源依赖会报告循环。
一个 Pass 内不支持隐藏的状态切换，需拆成多个 Pass。

[GraphTests.cpp](../../tests/unit/GraphTests.cpp) 的
`graph declares upload compute draw and readback without a device` 是完整公共接口例子：
导入 upload → 写 transient mesh → compute 更新 → draw 写 color → transfer 到 external readback。
验证整个声明链不触发 loader、设备、窗口、命令录制或 GPU 提交。

## 编译与检查计划

`graph.compile()` 已包含完整声明校验，成功返回拥有型 `CompiledGraph`。
计划保持独立快照，后续修改、reset 或销毁原图不会改变它。所有索引仅在当前计划内有效。

| 查询 | 内容 |
| --- | --- |
| `passes()` | 全部声明的名称/访问；retained 表示是否保留，order_index 为实际排序位置 |
| `order()` | 保留 Pass 的声明索引，按拓扑顺序排列，同一就绪集合优先较早声明者 |
| `dependencies()` | 保留 Pass 间的显式、RAW/WAR/WAW、layout 和 contents 依赖及资源索引 |
| `resources()` | 先 buffer 后 image 的完整声明快照、output/retained、first_use/last_use 与 allocation_index |
| `allocations()` | 每个活跃 transient 的独立分配条目：resource、create_before、release_after |

`first_use/last_use/create_before/release_after` 都是 `order()` 中的位置，不是 Pass 声明索引。
output 的 release_after 为空，表示向结果所有者保留；external 不进入分配表。
分配表描述逻辑生命周期；执行器与 pending slot 保证 GPU 完成前资源仍存活，不提供显存 aliasing。

例如，同一 transient buffer 连续被两个完整写覆盖，只有最终内容被标记输出时，第一个写可裁剪。
如果两次写之间有一个带 side_effect 的读取，则旧写、读取和新写全部保留，执行顺序不变。
显式前驱会随其被保留的后继一起保留；纯顺序边不把无用前驱强行保留。
external 的可观察写入也必须通过 mark_output 或 side_effect 声明，否则可以被裁剪。

`contents` 追踪实际被使用的最近写入者，与保守 buffer 同步范围区分；
部分字节或多个 mip/layer 输出可能保留多个生产者。即使无输出的分支非法，compile 也会拒绝。
完整的声明→计划用法见 [GraphCompileTests.cpp](../../tests/unit/GraphCompileTests.cpp) 中
`graph compile plans upload compute draw and readback`，无需创建 GPU 设备。

## 句柄与失败

不要将临时图 ID 作为持久资源身份。跨图、默认和 reset 前的 ID 会被拒绝；
move 保留转入图的身份。reset 成功后所有旧 ID 失效；失败则原图保留。
查询得到的 name/span 是只读借用，图发生修改、移动赋值或销毁后不要继续使用。
声明参数或分配失败不会发布半个 Pass，不会改变已存在的资源和依赖。
编译失败同样不修改图，也不影响先前已发布的计划；默认/移后计划 bool 为 false，查询返回空列表。
计划的只读视图借用到计划被覆盖或销毁；关闭 Memory 域后仍可读取已有计划，释放计划后可完成域回收。

执行的失败与所有权规则见下节；阶段依赖与后续步骤见 [Roadmap](../roadmap.md)。

## 单队列执行

1. `ExternalBinding` 用计划 resources() 索引绑定同队列的现有 Buffer/Image，描述必须完全相同。
   每个 retained external 必须绑定，不能把同一物理资源绑定为多个逻辑资源；transient 由图创建。
   expected_states 留空时读取当前账本，提供时做精确校验；image 按 layer-major/mip-minor 排列。
2. `PassCallback` 用 passes() 声明索引绑定函数指针和 user_data。每个 retained Pass 都需回调；
   裁剪 Pass 可提供回调但不会执行。回调只借用到 execute 返回，不是保存在计划里的闭包。
3. `PassContext` 按计划索引查询当前 Pass 的 buffer/image；copy/fill/clear 直接接受索引，
   compute/rendering 返回现有 typed encoder。管线/绑定仍由 M5 工厂创建，shader 的绑定和访问范围必须与声明一致。
   回调必须返回失败 Result，不能保存上下文/资源引用或 encoder，也不能重入修改当前计划或队列。
   每个 Pass 后会清除准备状态并使 encoder 失效，重复写或内部状态切换需拆分 Pass。
4. 可用 `FinalAccess` 设置输出或 external 资源的退出 stage/access/layout，例如 buffer 的 HostRead。
   同一 image 可给不重叠的 mip/layer 范围；退出访问只建立同步，不初始化内容。
5. `execute(plan, queue, desc)` 完成 CPU 录制并提交，返回 Execution。
   `queue.wait(execution.submission())` 或 poll 确认完成；状态发布不表示 GPU 已完成，未完成时不能 CPU 读取。
6. Execution::buffer/image 只返回标记输出的 owner；需要跨结果保存时调用 share()。
   导入的输出同样持有显式共享 owner。states() 保存 external/输出的最终状态快照，后续执行不会改写。
   非输出 transient 在逻辑末次使用后释放执行器引用，实际 GPU 生命周期由 pending slot 保证。

以下例子创建 transient readback 输出并填充；为突出调用顺序用 value()，实际调用应处理各步 Result。
`heap` 和 `queue` 分别是开放的 Memory resource 和现有 SubmissionQueue。

```cpp
using namespace dk::graphics;
using namespace dk::graphics::graph;
auto graph = Graph::create(heap).value();
const auto output = graph.declare_buffer("readback",
    {64, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}).value();
const Use write{output, {{vk::PipelineStageFlagBits2::eClear,
    vk::AccessFlagBits2::eTransferWrite}, 0, VK_WHOLE_SIZE, {}, true}};
graph.add_pass({"fill", std::span{&write, 1}}).value();
graph.mark_output(output).value();
const auto plan = graph.compile().value();
const std::array callbacks{PassCallback{0, [](PassContext& pass, void*) {
    return pass.fill(0, 0x12345678u);
}}};
const auto execution = execute(plan, queue, {{}, callbacks, {}}).value();
std::array<std::uint32_t, 16> pixels{};
if (queue.wait(execution.submission()).value()) {
    execution.buffer(0).value()->read(0, std::as_writable_bytes(std::span{pixels})).value();
}
```

完整可运行的复制、图片转换、外部绑定、多 mip/layer 与失败用例见
[GraphProbe.cpp](../../tests/integration/GraphProbe.cpp)。GPU 缺失返回 77（跳过），不计作通过。
已有 AMD 隐式层兼容问题及进程级环境处理见 [0048](../development/0048-vulkan-14-baseline.md)。

参数、分配、录制或提交失败时不发布候选 Execution、全局资源状态或完成票据；回调自身 CPU 副作用不回滚。
成功提交后提前销毁结果不会取消 GPU 工作。等待超时保持 pending 和 owner；设备丢失沿用队列终态。
关闭计划 Memory 后已有计划/结果仍可读，但拒绝新 execute。

buffer 的 initialized 只保守描述整 buffer：部分写不使其变为 true，即使多个写累计覆盖了全部字节。
图内部按字节范围证明的内容仍能使用；下一次声明 initialized=true 导入时要求账本确实为 true。
本阶段为同队列自有资源执行，不支持 WSI acquire/present、多队列或 aliasing；
Offscreen 已使用 Graph 运行 draw/dispatch，完整组合用例见下文。


## 计划和同步诊断

包含 `dk/graphics/GraphDiagnostics.hpp`，调用 `format_plan(plan)` 得到拥有型 PlanReport，
通过 text() 取得只读文本。报告显示 retained/culled Pass、访问范围、资源首末次使用与独立分配、
拓扑顺序及 explicit/RAW/WAR/WAW/layout/contents 依赖；阶段/访问掩码为 Vulkan 十六进制值，layout 为枚举数值。
名字会转义换行和引号；结果不借用原计划，可在计划销毁或 Memory 关闭后读取。
格式是人类诊断信息，不作为持久化协议。

执行时将 ExecutionDesc::capture_synchronization 设为 true，Execution::synchronization() 就会记录
Pass 前及最终访问的逐资源 barrier 输入。每项有 Pass 索引（final 为空）、资源索引、mip/layer、
before 与 target；buffer 同步仍为整对象。barrier 不改变 initialized，target 不是 shader 执行后的内容状态。
连续只读时 before 可能包含累计访问，target 是本次请求；最终账本快照用 states() 查询。
默认不捕获，同步诊断没有 GPU 计时或性能含义。诊断分配失败仍放弃候选执行，不发布 GPU 状态。

执行错误保留原 Error code/message/context，再加 Pass 名字/索引、资源名字/索引或提交阶段。
打印错误时应同时输出 context，按最内层到最外层阅读。

## 完整 upload→compute→draw→readback 样例

[GraphPipeline.cpp](../../tests/integration/GraphPipeline.cpp) 是仅使用公共 Graph/Device/Shader API 的完整示例，
随 dk_offscreen_probe 构建运行；[graph-transform.slang](../../shaders/common/graph-transform.slang) 读取上传顶点并缩放。

1. CPU 写入 upload buffer，图中的 upload Pass 复制到 transient 顶点和索引 buffer。
2. compute 通过 storage binding 更新顶点，图生成 compute-write→vertex-read barrier。
3. clear 初始化 color image；draw 以 Load 使用已有内容并执行索引绘制。
4. readback 复制 color 到标记输出的 transient buffer，最后转到 HostRead。
5. 同一计划执行四次，每次与 M5 原有 indexed triangle 的全部像素比较，末尾没有 transient/pending 残留。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_offscreen_probe `
  -TestRegex '^dk\.offscreen\.gpu_validation$' -Reason 'Graph 完整管线与 M5 样例迁移回归'
```

CTest 在 `out/build/windows-graphics/tests/integration/graph-pipeline-validation.txt` 输出计划与首轮实际同步记录；
无验证层版本测试输出 graph-pipeline.txt。同一 probe 继续验证 12 次 Offscreen draw、12 次 dispatch、
原有低层绑定/深度校验及超时、失败、drain、析构行为；具体环境与证据见
[0060](../development/0060-graph-integration.md)。进程级 AMD 层兼容设置按前述 Graphics 指南处理。
