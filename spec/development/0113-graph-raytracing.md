---
id: "0113"
created_at: "2026-10-14T13:38:57+08:00"
updated_at: "2026-10-14T14:04:00+08:00"
status: completed
design_refs:
  - ../design/graphics-graph.md
  - ../design/graphics-raytracing.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
---

# 0113 M16.5 Graph AS、构建与 trace

按[实施契约](../design/graphics-graph.md#m165-as-与-sbt-实施契约)将AS/SBT纳入逻辑资源、依赖、裁剪和单队列执行。
先更新设计，再拆分Device准备/录制并扩展Graph，最后验收CPU与required GPU。
保持现有依赖和生产target；成功submit为唯一状态发布点，失败保留旧账本，pending保活完整引用闭包。

## 实际实现

- [Graph.hpp](../../engine/graphics/graph/include/dk/graphics/Graph.hpp)新增AccelerationId/ShaderTableId、外部声明和
  build_input/acceleration_build/acceleration_read/shader_table_read访问辅助函数。
  BLAS候选记录外部输入buffer，TLAS记录BLAS集合；同Pass必须完整声明构建输入/子AS读，
  BUILD初始化且仅允许一次，AS/SBT作为一个不可分割单元参与内容依赖、RAW/WAR/WAW、输出和裁剪。
  编译计划复制依赖索引，输出即使无Pass也保留AS依赖闭包；格式化报告显示BLAS/TLAS/SBT及依赖。
- [AccelerationStructure.cpp](../../engine/graphics/device/src/AccelerationStructure.cpp)拆分prepare_build/build_prepared，
  原build保持便利包装。所有候选分配、输入/AS barrier在准备阶段完成；回调只录制已准备BUILD。
  每次准备独占scratch并保活至完成，不提供Graph共享scratch，也不将临时字节伪装为业务initialized。
  AS/SBT增加retain/state，AS增加只读准备和依赖owner核对，SBT增加share；Graph不访问Device私有实现。
  finish_pass/submit拒绝未消费BUILD，finish_pass清除trace准备，不能跨Pass沿用AS声明。
  普通buffer/image拒绝AS read/write，构建输入专用校验要求AS-build/ShaderRead及build-input usage。
- [GraphExecution.hpp](../../engine/graphics/graph/include/dk/graphics/GraphExecution.hpp)扩展外部AS/SBT绑定、
  AS expected_generation、导出generation和输出owner查询；执行前核对类型/kind、依赖身份、队列、物理别名与陈旧状态。
  PassContext::build/trace提供受限录制，每Pass前准备同步；AS/SBT退出用途仅只读。
  导出快照在submit前完成，成功后仅补首次BUILD generation及移动owner；后续提交不改写旧快照。
  同步诊断记录Graph逻辑资源准备，内部实例/scratch与Device重复输入准备不作为独立逻辑资源列出。
- 新增[Graph CPU测试](../../tests/unit/GraphRayTracingTests.cpp)和
  [required Graph RT探针](../../tests/integration/GraphRayTracingProbe.cpp)，复用M16.4 shader夹具；
  同步测试CMake、选择表、[使用指南](../guides/graph.md)、模块设计、架构和Roadmap。

## 验证与证据

环境为windows-graphics/RelWithDebInfo、MSVC 14.51、NVIDIA GeForce RTX 4070 Laptop GPU。
未变更baseline或依赖。日志位于out/verify下相应目录的summary.json、build.log、tests.log、results.xml。
GPU沿用0110–0112已核验的旧AMD隐式层兼容处理，仅在验证进程设置
`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并finally恢复；不改注册表/持久环境。
Khronos validation必须可用，CTest设置VK_LAYER_VALIDATE_SYNC与VK_VALIDATION_VALIDATE_SYNC，required RT探针不以77跳过。

| 运行编号（20261014） | 范围 | 结果 |
| --- | --- | --- |
| 134555-59cb6c7d | 原Graph CPU 38项、Device资源CPU 12项 | 50通过、0失败、0跳过 |
| 135037-427eed0d | 新Graph RT CPU 3项、Graph/Graph RT/AS/直接trace GPU 4项 | 4通过、3失败、0跳过；辅助函数继承默认image range导致两个CPU用例和Graph RT拒绝；显式设置空range及逻辑单元范围后修复 |
| 135333-876729ac | 3项Graph RT CPU及补齐身份/跨Pass负向用例后的Graph RT GPU | 4通过、0失败、0跳过；errors=0、warnings=0、liveAllocations=0 |

累计57个不同CTest条目最终通过，不把重复运行相加。原Graph buffer/image执行、AS构建和直接RT三个GPU回归
已在第二轮通过；修复只涉及Graph AS/SBT helper的范围默认值，第三轮重跑所有使用该helper的新用例。
未把中途失败记为通过，也未执行旧二进制来绕过构建。

复现命令（GPU部分在上述临时环境内执行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_graph_tests,dk_graphics_resource_tests `
  -TestRegex '^dk\.(graph\.graph |graphics\.unit\.)' -Reason 'M16.5 Graph与资源CPU回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_graph_tests','dk_graph_raytracing_probe','dk_graph_probe','dk_acceleration_structure_probe','dk_raytracing_probe'
) -TestRegex '^dk\.(graph\.(graph RT |rt_required_validation$|gpu_validation$)|acceleration\.required_validation$|raytracing\.required_validation$)' `
  -Reason 'M16.5 Graph AS内容依赖、required RT同步与原执行链回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_graph_tests,dk_graph_raytracing_probe `
  -TestRegex '^dk\.graph\.(graph RT |rt_required_validation$)' -Reason 'M16.5 修复范围并补齐身份与跨Pass拒绝'
```

CPU证明upload→BLAS→TLAS→trace内容边、无消费者BUILD裁剪、计划reset后快照独立、外部输出依赖闭包，
并拒绝跨图/过期ID、未初始化AS、漏输入/子AS、重复BUILD、写SBT与scratch误用。
真实GPU图完成upload→BLAS→TLAS→trace→readback，命中距离5、重心0.25/0.5、instance 7及miss标记，
primary/visibility数值按1e-4比较；捕获transfer→build、build→trace和trace→copy同步输入。
无消费者候选不绑定也不执行。导入已构建AS/SBT、只读退出同步与generation快照有效；
错误输入/子AS owner、kind、陈旧access/generation、AS/SBT物理别名和外来队列在提交前拒绝。
未消费prepare_build的finish/submit拒绝，构建回调不执行和trace回调失败、真实submit失败均保留旧账本。
前一Pass读AS不能授权后一Pass漏声明的trace。释放所有外部owner和Execution后timeout仍保活AS/SBT，
close完成最后读回并回收，VMA allocationCount与Memory liveAllocations均为0；validation错误/警告均为0。
定向check-spec通过：10份Markdown、688个本地链接、元数据/表格/索引/测试入口及JSON清单；git diff --check通过。

## 限制与下一步

AS/SBT仅external；首次BUILD、三角形、单提交域，未实现transient AS、UPDATE/refit、压缩或跨队列AS交接。
scratch每次构建独占，不提供复用/aliasing；BLAS输入按整buffer初始化与读取，已构建BLAS新图声明不再携带输入。
新增公开聚合字段置于末尾，保持原buffer/image调用方式。未增加Runtime命令或生产target依赖。
未运行全引擎、其他GPU/平台、Debug/Release矩阵、编辑器、路径积分或性能基线；不将其计作本阶段验收。
Roadmap标记M16.5完成，下一项M16.6为GPU无历史Lambert路径积分及HDR诊断。
