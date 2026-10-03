---
id: "0059"
created_at: "2026-10-03T07:49:53+08:00"
updated_at: "2026-10-03T08:14:35+08:00"
status: completed
design_refs:
  - ../design/graphics-graph.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
---

# 0059 M6.3 单队列同步与执行

## 目标与设计依据

按 [Graph 设计](../design/graphics-graph.md)和[资源设计](../design/graphics-resources.md)
实现实际 owner 导入导出、逐 Pass 同步录制、单次提交、完成跟踪与失败保护。本次不推进 M6.4 样例迁移。

## 实际变更

- [GraphExecution.hpp](../../engine/graphics/graph/include/dk/graphics/GraphExecution.hpp) 增加 ExternalBinding、
  PassCallback、FinalAccess、PassContext、Execution 和 execute；索引属于单个编译计划，回调只在执行期间借用。
- [GraphExecution.cpp](../../engine/graphics/graph/src/GraphExecution.cpp) 按计划创建 transient、绑定 external、
  在每 Pass 前 prepare，同步由 M5 synchronization2 层生成；typed copy/fill/clear 校验声明范围，
  compute/render encoder 沿用使用层。裁剪资源/Pass 不创建/调用，输出 owner 与状态快照由结果持有。
- 外部描述、状态断言、初始化和唯一物理绑定在录制前校验；batch retain 校验设备/录制保留/WSI 归属。
  可独立设置 image mip/layer 退出 layout 与 buffer HostRead。快照记录提交状态，wait/poll 确认完成。
- 所有结果元数据与快照在提交前构建；成功提交后仅无分配地移动/释放 owner。
  录制 Result 错误、异常、GPU 创建失败、Memory 预算失败、submit 错误均放弃候选，不发布票据/全局状态。
  结果提前销毁仍由 pending slot 保活，超时不回收，设备丢失沿用队列终态。
- 提取 [GraphPlanInternal.hpp](../../engine/graphics/graph/src/GraphPlanInternal.hpp)，不修改 M6.2 编译语义。
  Graph 不包含 device 私有头，不调用 Vulkan；只有 GPU 测试使用原有私有提交错误注入接口。
- [Resources.hpp](../../engine/graphics/device/include/dk/graphics/Resources.hpp) 增加 Buffer::description、显式 share、
  batch 局部 state 与 finish_pass。结束 Pass 时拒绝未执行写入/活动 rendering，清除准备并使旧 encoder 失效。
  修正 typed prepare 与 unsafe_record 的部分 buffer full_overwrite 不得将整个对象标为初始化；
  原生部分填充的局部/全局状态及读回通过专门 GPU 回归，精确字节内容证明仍属于 Graph。
- 新增 [GraphProbe.cpp](../../tests/integration/GraphProbe.cpp)，扩展空资源 CPU 检查，
  同步 CMake、测试选择表、模块/开发索引、架构、README、[使用指南](../guides/graph.md)和 Roadmap。
  没有新增三方依赖、开关或命令。

## 验证记录

Windows x64 Debug、MSVC 19.51、warnings-as-errors；既有 out/build/windows-graphics。
GPU 为 NVIDIA GeForce RTX 4070 Laptop GPU，driver 596.49、Vulkan 1.4.329。
GPU 执行过程开启 Khronos validation 及 synchronization validation，按 [0048](0048-vulkan-14-baseline.md)
仅在子进程环境设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复；未禁用 Khronos 层。

```powershell
$priorAmdLayer=$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
      -Target @('dk_graph_tests','dk_graph_probe','dk_graphics_resource_tests','dk_graphics_resource_probe') `
      -TestRegex '^dk\.(graph\.(graph |gpu_validation$)|graphics\.(unit\.|gpu_resources_validation$))' `
      -Reason 'M6.3 graph and resource CPU contracts plus actual GPU synchronization validation'
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=$priorAmdLayer }
```

| out/verify 运行 | 结果 |
| --- | --- |
| 20261003-080130-916a3279 | 首次 probe 构建失败：生成源码的字符串换行不合法；已修复，未运行测试 |
| 20261003-080244-4a252a04 | 构建通过；CTest 不支持负向前瞻筛选，发现阶段失败，未运行测试；已换兼容表达式 |
| 20261003-080406-79c2a561 | 44/44 通过：34 Graph CPU、8 资源 CPU、Graph GPU 与资源 GPU；0 失败、0 跳过 |
| 20261003-080620-03572e90 | 补充边界后 2/2 通过：Graph GPU、empty resource/batch CPU；0 失败、0 跳过 |
| 20261003-081248-d9d24420 | unsafe_record 部分 buffer 写修正后资源 GPU 1/1 通过；0 失败、0 跳过 |

边界补充项仅重建 dk_graph_probe、dk_graphics_resource_tests，TestRegex 为
`^(dk\.graph\.gpu_validation|dk\.graphics\.unit\.empty resource and batch operations reject without native calls)$`。
最后的原生入口修正仅构建 dk_graphics_resource_probe 并运行 `^dk\.graphics\.gpu_resources_validation$`。
各次只重跑直接受影响检查，未重复全套。

GPU Graph probe 覆盖：

- 四次 upload buffer→transient buffer→image→readback 字节一致，未用 Pass 回调不调用；
- RAW/WAR/WAW、layout 切换、多 mip/layer 写入/读回，分裂 buffer 字节生产者及越界录制拒绝；
- 真正外部 image 的四个子资源初始断言、不同最终 layout、layer-major 导出与账本一致；
- 描述/类型/缺失/重复/物理别名、陈旧状态、未初始化导入、跨设备、竞争录制保留的拒绝；
- 回调错误/异常/声明写未执行、原生 submit 错误、首次资源创建后硬件 extent 拒绝时，无发布/泄漏；
- Memory 预算逐步释放 sweep 覆盖部分候选已分配后的失败，原计划不变、GPU/CPU 临时分配全部回收；
- native submit 内关闭图元数据域仍成功发布，之后拒绝新执行但已有快照可读；
- 结果 move、外部/内部输出显式 share、提前销毁结果、超时与最终完成释放；
- 下次执行不修改先前状态快照；无 Pass 的外部输出、空计划、设备丢失终态；
- 跨 Pass 保存的 compute encoder 在下一回调里报 expired，不会录制命令。

最终 Graph probe：pending=0、VMA allocations=0、Memory liveAllocations=0、validation errors=0、warnings=0。
资源 GPU 回归：16 轮/5 格式，pending=0、VMA/Memory 残留=0、validation errors/warnings=0。
文档检查通过：120 Markdown、1183 本地链接及元数据/索引/测试入口；git diff --check 通过。未运行 Release、全量、窗口、CPU runner 或离屏样例迁移；
本阶段不改变管线、呈现或 Runtime，现有样例的完整图迁移验收属于 M6.4。

## 遗留问题与下一步

M6.3 已完成；下一项 M6.4 将 upload/compute/draw/readback 样例全部纳入 Graph 并完善诊断。
当前不支持 WSI acquire/present、多队列和 aliasing。Shader 真实覆盖范围仍由声明者保证。
部分 buffer 写入的整资源 initialized 保持保守 false，即使图内多个范围合起来已有效；
这不会妨碍当前图的精确内容证明，但后续 initialized=true 导入需要整资源初始化账本。

## 修改记录

- 2026-10-03T07:49:53+08:00：设计先行，创建记录。
- 2026-10-03T08:10:35+08:00：完成实现、GPU/CPU 定向验收和阶段文档同步。
