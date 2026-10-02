---
id: "0058"
created_at: "2026-10-03T07:08:39+08:00"
updated_at: "2026-10-03T07:28:09+08:00"
status: completed
design_refs:
  - ../design/graphics-graph.md
  - ../design/architecture.md
---

# 0058 M6.2 依赖编译与资源生命周期

## 目标与设计依据

按 [Graph 设计](../design/graphics-graph.md)实现独立编译快照、依赖诊断、拓扑排序、裁剪与 transient 逻辑分配计划。
本次只推进 M6.2，不创建/提交 GPU 工作。

## 实际变更

- [Graph.hpp](../../engine/graphics/graph/include/dk/graphics/Graph.hpp) 增加 Graph::compile、CompiledGraph，
  以及只读 Pass/资源/访问/依赖/分配条目。计划复制全部名称、描述和归一化访问，不保存 Graph ID 或原图 owner。
- [GraphInternal.hpp](../../engine/graphics/graph/src/GraphInternal.hpp) 提取内部声明状态；
  Graph.cpp 的共同 analyze 保留 M6.1 内容与循环校验，并生成带资源和原因的顺序约束。
  validate/compile 使用相同分析，不因裁剪而放过非法死分支。
- [GraphCompile.cpp](../../engine/graphics/graph/src/GraphCompile.cpp) 逆向追踪最近字节/mip/layer 写入者，
  将内容依赖与纯顺序约束分开；以最终输出生产者和副作用 Pass 为根，沿内容和显式前驱保留闭包。
  被完整覆盖的无用写和独立死分支被裁剪；保留写追踪旧内容。surviving RAW/WAR/WAW/layout 边维持顺序。
- 在保留子图上使用声明索引作为就绪优先级，生成确定性拓扑顺序；依赖诊断去重并排序。
  计算实际 order 位置上的首末次使用，逐个规划活跃 transient 的独立分配；external/未使用资源不分配。
  输出条目不在末次 Pass 后逻辑释放，物理 GPU 保活仍由后续执行及 M5 完成跟踪负责。
- 计划候选全部成功后返回；原图修改/reset/销毁不影响已有快照，默认/移后计划返回空视图。
  预算失败保留原图与先前计划，Memory 关闭后仍可读取发布的快照，最终释放无残留。
- 沿用 M6.1 的 Memory UniquePtr 记录与 count=0 容器构造，避免 MSVC Debug proxy 的 noexcept 分配陷阱。
  新增 GraphCompileTests.cpp，复用 dk_graph_tests；同步 CMake、架构/索引、[使用指南](../guides/graph.md)和 Roadmap。
  没有新增三方依赖或构建开关。

## 验证记录

Windows x64 Debug，MSVC 19.51，warnings-as-errors 开启；复用 out/build/windows-graphics。
MSBuild/CMake 使用已授权的外部工具链与 vcpkg 缓存访问，配置随受影响 target 构建自动更新。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_tests `
  -TestRegex '^dk\.graph\.' -Reason 'M6.2 graph compilation culling dependencies and resource lifetime contracts'
& ./scripts/check-spec.ps1
git diff --check
```

| out/verify 运行 | 结果 |
| --- | --- |
| 20261003-072006-a64e679a | 构建通过；34/34 用例通过，0 失败、0 跳过 |

18 个 M6.1 用例及 16 个编译用例覆盖：

- upload→compute→draw→readback 的完整 CPU 编译计划，归一化范围、依赖原因与分配位置；
- 确定性就绪排序，无根/空图，完整覆盖裁剪、显式前驱、side_effect 与幸存 RAW/WAR/WAW；
- 分裂字节范围、保留写、多个 mip/layer 的最近生产者及未用读取的 layout 边裁剪；
- imported output 无额外 Pass/分配，排序位置与声明索引区分，输出保留及独立 transient 条目；
- 非法死分支拒绝，快照在原图修改/reset/销毁/move 后独立有效；
- 预算耗尽与逐步释放预算的编译失败 sweep，包含已成功分配多项候选后的失败，原图/旧计划不变且无残留；
- 域关闭时拒绝新编译，已发布计划仍可读，释放后 live_allocations=0 并成功关闭。

文档检查、git diff --check 通过：119 个 Markdown、1167 个本地链接，元数据/索引/测试入口均通过。
未运行全量、Release、GPU、窗口或 CPU runner 进程回归：本次变更仅涉及 Graph CPU 声明分析与编译，
未修改 device 录制、资源创建、Shader、呈现或 Runtime 链路。结果不代表 Graph GPU 执行已经实现或验收。

## 偏差与决策

无验收失败或范围扩张。M6.2 的分配计划是逻辑生命周期表，不是显存字节大小预测或 aliasing 分配器。
显式依赖表示必须保留的前置操作；单纯资源顺序约束只在两端均存活时生效，避免错误保留被覆盖的工作。
external 写入的可观察性由调用者通过 output/side_effect 声明，不自动把所有外部写都视为根。

## 下一步

M6.3：实际资源导入/导出、单队列 barrier/layout 计划与执行、提交完成及 GPU 验证。

## 修改记录

- 2026-10-03T07:08:39+08:00：更新设计并创建记录。
- 2026-10-03T07:26:05+08:00：完成实现、34 项定向验收与文档/阶段状态同步。
