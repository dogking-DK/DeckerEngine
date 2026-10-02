---
id: "0057"
created_at: "2026-10-02T23:00:00+08:00"
updated_at: "2026-10-02T23:38:04+08:00"
status: completed
design_refs:
  - ../design/graphics-graph.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
---

# 0057 M6.1 图声明与结构校验

## 目标与设计依据

按 [Graph 设计](../design/graphics-graph.md)完成资源/Pass 声明和提交前结构校验。
只推进 M6.1；排序计划、裁剪、分配与 GPU 执行不在本次范围。

## 实际变更

- [Graph.hpp](../../engine/graphics/graph/include/dk/graphics/Graph.hpp)、
  [Graph.cpp](../../engine/graphics/graph/src/Graph.cpp) 新增 move-only 图、类型化弱身份 ID、
  transient/external 描述、访问范围/内容保留、Pass 副作用、输出和可修正的显式依赖。
- 声明先校验并构建拥有型候选，再发布；名称/访问列表由 Memory 域复制。
  非法引用、重叠声明、未初始化读取/保留和未完整写入的输出被拒绝。
  内容覆盖按 buffer 字节区间及 image mip/layer 计算，不按图像尺寸分配临时数组。
- 资源 RAW/WAR/WAW 保持声明顺序；image 布局变化也产生依赖，buffer 保守按整对象。
  与显式边合并后通过迭代 DFS 检查循环，错误给出闭合 Pass 路径；不发布或缓存执行顺序。
- [ResourceValidation.hpp](../../engine/graphics/device/include/dk/graphics/ResourceValidation.hpp)
  从 M5 录制层提取描述与访问纯验证函数。CommandBatch 与 Graph 共用规则，Graph 不包含 device/src。
  同时修正未知 stage/access 原始位被 Vulkan-Hpp flags 取反掩掉的问题。
- MSVC Debug 的 allocator-only vector 构造与 string/vector move 虽为 noexcept，仍可能分配 iterator proxy。
  使用可抛异常的 count=0 构造；资源/Pass 记录以 Memory UniquePtr 持有，扩容和发布只移动指针，
  避免预算失败穿过 noexcept 终止进程。未改全局 Memory allocator 或其他模块行为。
- 新增默认 OFF 的 DK_BUILD_GRAPHICS_GRAPH、dk::graphics_graph、dk_graph_tests；
  windows-graphics 显式启用，要求 device，无新增三方依赖或 baseline 变更。
  同步架构、设计索引、测试选择表与 [Graph 使用指南](../guides/graph.md)。

## 验证记录

Windows x64 Debug，MSVC 19.51，既有 windows-graphics，warnings-as-errors 开启。
配置最初因 sandbox 无法写既有 vcpkg 缓存失败；授权后同一预设配置通过，没有换工具链或关闭模块。
MSBuild 首次同样因 FileTracker 访问受限失败；之后以获准权限执行，保留失败摘要。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_graph_tests','dk_graphics_resource_tests','dk_graphics_resource_probe','dk_offscreen_probe') `
  -TestRegex '^dk\.(graph\.|graphics\.unit\.|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)' `
  -Reason 'M6.1 declarations plus shared access policy fix and affected GPU recording regressions'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_tests `
  -TestRegex '^dk\.graph\.' -Reason 'M6.1 graph final validation after MSVC Debug allocation-failure fix'
& ./scripts/check-spec.ps1
git diff --check
```

GPU 回归仅在该进程设 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复原值；
沿用 [0048](0048-vulkan-14-baseline.md) 的旧 AMD 隐式层兼容处理，Khronos 同步验证保持开启。

| out/verify 运行 | 结果 |
| --- | --- |
| 20261002-231537-8b809f48 | MSBuild FileTracker 访问受限；未运行测试 |
| 20261002-231606-704a7a09 | 22/23 通过，未知 access 位回归失败；定位并修正原始掩码检查 |
| 20261002-231847-0bbf153b | 26/26 通过，零跳过；16 Graph + 8 资源 CPU + 2 GPU |
| 20261002-232229-467d119f | 新增 image 部分内容检查通过；预算用例因 MSVC Debug noexcept proxy 分配终止而超时 |
| 20261002-232710-6b2e86d8 | 修正拥有型记录和容器构造后，全部 18 Graph 用例通过、零跳过 |

合计 **28 个不同用例通过**（18 Graph、8 资源 CPU、2 GPU）；后续 Graph 内部修正不改变
已验收的 device 纯校验和 GPU 录制代码，因此没有重复 GPU 回归。
预算 sweep 覆盖创建、候选字段、发布、校验临时分配等不同失败位置，失败后无残留分配；
默认/跨图/销毁/reset/move、区间缺口、部分 mip/layer、显式及资源混合循环均有用例。

GPU：RTX 4070 Laptop，NVIDIA 596.49，API 1.4.329。
Offscreen：12 draw + 12 compute、3 生命周期，pending/VMA/Memory=0，errors/warnings=0。
Resources：16 roundtrips、5 formats、completed=34，pending/allocations/liveAllocations=0，errors/warnings=0。
dumpbin /DEPENDENTS 检查 Graph 测试仅直接导入 mimalloc 和系统/CRT DLL，无 Slang、SDL、Vulkan DLL；
结合 Graph 代码无 loader/设备创建调用确认其 CPU 声明边界，单独的 DLL 列表不作为动态加载证明。

文档检查和 git diff --check 通过；文档检查覆盖 118 Markdown，最终 1156 个本地链接。
未运行全量、Release、CPU runner 进程回归、窗口恢复、其他 GPU/平台；
本次选择共享访问校验直接影响的资源与离屏链路做 GPU 回归。
Graph 本阶段无 GPU 执行，所以不能将既有 GPU 回归视为 Graph 执行验收。

## 偏差与决策

M6.1 先验证资源访问形成的顺序约束，但不提供可消费排序或生命周期计划，维持 M6.2 边界。
external 仅声明逻辑资源与整资源内容有效性；实际 owner、精细初始/最终状态在 M6.3 绑定。
内存失败测试揭示的 Debug 构造约束已合入 [Graph 生命周期设计](../design/graphics-graph.md)。

## 下一步

M6.2：依赖编译、拓扑排序、输出/副作用根裁剪、transient 生命周期与分配计划诊断。

## 修改记录

- 2026-10-02T23:00:00+08:00：创建设计与记录。
- 2026-10-02T23:31:00+08:00：完成实现、两项失败修复、定向验收和状态同步。
