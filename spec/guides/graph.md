---
created_at: "2026-10-02T23:20:00+08:00"
updated_at: "2026-10-03T07:24:14+08:00"
---

# GPU Graph 声明与编译

[返回项目入口](../../README.md)。接口契约见 [Graph 设计](../design/graphics-graph.md)。
当前可声明资源、Pass、依赖和输出，在 CPU 上校验并编译；尚无 GPU 绑定或 execute 入口。

## 构建与验证

`windows-graphics` 开启 `DK_BUILD_GRAPHICS_GRAPH`，其他配置可显式开启，同时要求
`DK_BUILD_GRAPHICS_DEVICE` 和 Memory。消费者链接 `dk::graphics_graph`，
包含 `dk/graphics/Graph.hpp`；不要求 Slang、SDL3 或创建 Vulkan 设备。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_tests `
  -TestRegex '^dk\.graph\.' -Reason 'GPU Graph 声明、编译与生命周期'
```

## 声明顺序

1. 以开放的 Memory ResourceHandle 创建 `graph::Graph::create(resource)`。
2. `declare_buffer/declare_image` 返回类型化 ID；默认 transient 内容未初始化。
   external 目前只是逻辑导入声明，`initialized=true` 表示调用者保证整个资源内容有效。
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
分配表描述逻辑生命周期，不能据此在 GPU 完成前销毁资源；本阶段也不提供物理显存大小或 aliasing。

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

尚未支持实际 GPU 分配、barrier 规划、GPU 回调、实际 external owner 绑定、
状态导入导出或完成跟踪；依赖与后续步骤见 [Roadmap](../roadmap.md)。
