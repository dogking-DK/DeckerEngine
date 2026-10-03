---
id: "0060"
created_at: "2026-10-03T14:28:57+08:00"
updated_at: "2026-10-03T14:50:13+08:00"
status: completed
design_refs:
  - ../design/graphics-graph.md
  - ../design/graphics-offscreen.md
  - ../design/architecture.md
---

# 0060 M6.4 样例迁移与诊断

## 目标与设计依据

按 [Graph 设计](../design/graphics-graph.md)与[离屏设计](../design/graphics-offscreen.md)
迁移现有同步样例，验证完整 upload→compute→draw→readback 图，并补齐计划/执行同步诊断。
本轮仅推进 M6.4；M6 全部验收后下一项为 M7.1。

## 实际变更

- [Offscreen.cpp](../../engine/graphics/offscreen/src/Offscreen.cpp) 改为 Graph 客户端：draw 使用
  clear→draw→readback，dispatch 使用 upload→compute→readback。CPU 输入只写 upload buffer，GPU 传输均进入图。
  storage/color/readback 由图创建；view/binding 在回调内创建并由 encoder/batch 保活。
- 保持公共同步 draw/dispatch、返回顺序、尾部保留、超时与 drain 的可观察状态约定。
  Work 预留 Graph Execution，成功提交后无分配地接收结果；读回只在确认完成后发布。
  Graph 内可捕获的异常返回 internal_error；外围准备仍沿用原分配异常约定。
- [Offscreen CMake](../../engine/graphics/offscreen/CMakeLists.txt) 显式要求 GRAPH=ON，并以 PRIVATE 依赖 Graph。
  windows-graphics 已开启，不改已有预设、外部依赖版本或 baseline。
- [GraphDiagnostics.hpp](../../engine/graphics/graph/include/dk/graphics/GraphDiagnostics.hpp) / GraphDiagnostics.cpp
  增加 format_plan 和拥有型 PlanReport：按稳定索引输出排序、裁剪、访问范围、资源首末使用、分配及依赖原因。
  文本名字转义，无 native 句柄/地址。报告使用 Memory，独立于原计划并可在域关闭后读取。
- [GraphExecution.hpp](../../engine/graphics/graph/include/dk/graphics/GraphExecution.hpp) 增加可选同步捕获，
  Execution::synchronization 提供每次 Pass 前/final 的 before/target、资源和子资源索引。
  记录对应实际 prepare 的 barrier 输入；在提交前分配，失败仍放弃候选。barrier 不创造 initialized 内容。
- 执行错误保留原 Error code/message/context，追加 Pass 名字与索引、资源名字与索引或提交阶段；
  回调异常带 Pass 上下文，避免旧实现把嵌套 context 丢掉。
- [GraphPipeline.cpp](../../tests/integration/GraphPipeline.cpp) 增加完整公开 API 组合例子：
  upload 顶点/索引→compute 缩放顶点→clear/draw→readback，同一计划运行四次，逐像素对照 M5 indexed triangle。
  [graph-transform.slang](../../shaders/common/graph-transform.slang) 确实读取并更新上传数据，不绕过上传依赖。
  同步诊断核对 compute-write→vertex-read；无用 Pass/资源裁剪，输出计划及首轮 barrier 文本。
- 新增 GraphDiagnosticsTests，扩充 GraphProbe 的同步诊断、预算失败和嵌套错误上下文回归；
  OffscreenProbe 原有低层非法绑定/深度/encoder 用例保留作为 M5 基线。
  同步设计、架构、构建与使用指南、测试表、README、依赖说明和 Roadmap。

## 验证记录

Windows x64 Debug，MSVC 19.51，warnings-as-errors；既有 out/build/windows-graphics。
RTX 4070 Laptop GPU、NVIDIA 596.49、Vulkan 1.4.329；Slang 2026.18 / SPIR-V 1.5 / row-major。
GPU 测试保持 Khronos validation + synchronization validation 开启。
按 [0048](0048-vulkan-14-baseline.md) 临时设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 并在结束恢复，未禁用 Khronos 层。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_graph_tests','dk_graph_probe','dk_offscreen_tests','dk_offscreen_probe') `
  -TestRegex '^dk\.(graph\.graph |offscreen\.unit\.)' `
  -Reason 'M6.4 Graph diagnostics and Offscreen migration CPU contracts'
$priorAmdLayer=$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
      -Target @('dk_graph_probe','dk_offscreen_probe') `
      -TestRegex '^dk\.(graph|offscreen)\.gpu_validation$' `
      -Reason 'M6.4 complete graph pipeline and migrated M5 image compute and lifetime regression'
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=$priorAmdLayer }
& ./scripts/check-spec.ps1
git diff --check
```

| out/verify 运行 | 结果 |
| --- | --- |
| 20261003-143728-cfc109c5 | 构建通过；40/40 CPU 用例通过：37 Graph、3 Offscreen；0 失败、0 跳过 |
| 20261003-144145-b96fb15f | 新组合例子接入后构建通过；Graph/Offscreen GPU 验证 2/2 通过；0 失败、0 跳过 |

本轮无失败构建/测试。CPU 检查验证既有声明/编译、报告裁剪/依赖/范围与确定性、
报告在计划销毁/关闭后的独立寿命、预算部分失败不改写原计划/报告且无 Memory 残留。
GPU 检查包括完整四次管线与 M5 全图像逐像素相等、12 次 draw 插值/背景、12 次整数计算及尾部哨兵，
以及原有多 set/纹理/深度/encoder、提交/分配故障、wait 超时/错误、drain/移后/关闭/pending 析构。
Graph 原有导入导出、子资源、裁剪、失败/完成/设备丢失检查继续通过，预算 sweep 已开启同步捕获。
最终两项 GPU 探针 validation errors=0、warnings=0、pending=0、VMA=0、Memory liveAllocations=0。

可检查的产物位于 `out/build/windows-graphics/tests/integration/`：

- graph-pipeline-validation.txt：6 个声明 Pass、5 个 retained、7 个资源、分配/依赖及 12 项同步记录；
- offscreen-triangle-validation.ppm：保留原 64×64 三角形输出。

文档检查通过：121 Markdown、1205 本地链接及元数据/表格/索引/测试入口；git diff --check 通过。未运行 Release、全量、窗口/呈现、CPU runner、独立离线 shader 配置或 GPU 性能测试：
本轮不修改这些链路，Graph 不新增 Slang 依赖，只有 Offscreen 在既有私有依赖中引入 Graph。
已通过测试之后只做文档/注释调整，不重复构建。

## 遗留问题与下一步

M6.4 已完成，M6 的四个小阶段均已验收。下一项 M7.1：场景/视图只读提取、GpuMesh/纹理上传与卸载寿命。
仍不包含多队列、WSI 图提交、显存 aliasing、GPU timestamp/Tracy capture 或管线缓存。
诊断格式不是持久化协议，shader 真实访问/完整覆盖依旧由调用者声明保证。

## 偏差与决策

没有将“清屏并读写附件”错误声明成带读访问的 full_overwrite；使用独立 clear Pass，保持 M6.1 内容证明语义。
PlanReport 采用 shared owner 包含 String，避免直接返回 String 在 MSVC Debug 的 noexcept move 中分配代理，
保证图元数据预算失败可以进入 Result 边界。Graph/Offscreen 的常规路径不直接录制 Vulkan。

## 修改记录

- 2026-10-03T14:28:57+08:00：设计先行，创建记录。
- 2026-10-03T14:48:07+08:00：完成迁移、诊断、40 CPU/2 GPU 验收及 M6 状态同步。
