---
id: "0112"
created_at: "2026-10-14T11:50:46+08:00"
updated_at: "2026-10-14T12:18:26+08:00"
status: completed
design_refs:
  - ../design/graphics-raytracing.md
  - ../design/graphics-shaders.md
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
---

# 0112 M16.4 RT shader、管线、SBT与trace

目标：按[实施契约](../design/graphics-raytracing.md#m164实施契约)交付四阶段shader、AS descriptor、
拥有型RT pipeline/SBT和显式trace，验证实际命中与visibility。沿用固定依赖和既有生产target。
不接入Graph AS、GPU路径积分或编辑器；Runtime命令未变，独立shaderc增加四个阶段和schema v3。

## 实际实现

- [ShaderArtifact.hpp](../../engine/graphics/shader-types/include/dk/graphics/ShaderArtifact.hpp)和
  [RayInterface.cpp](../../engine/graphics/shader-types/src/RayInterface.cpp)增加RT阶段、AS类型、结构payload与静态TraceRay反射。
  SPIR-V校验按阶段限定RayTracingKHR/extension，重算接口以拒绝伪造metadata；不开放其他可选capability。
  IncomingRayPayload不要求Location，callee按结构签名匹配；相同字节数的float/uint不兼容。
- [ShaderCompiler.cpp](../../engine/graphics/shaders/src/ShaderCompiler.cpp)映射Slang四阶段及AS布局，
  允许payload/hit attribute入口参数，生成schema v3和编译器/profile/入口/宏/依赖路径及内容FNV-1a指纹。
  [shaderc](../../tools/shaderc/src/main.cpp)、[参考](../commands/shaderc.md)和[指南](../guides/shaders.md)同步；
  跨进程RT编译和阶段/动态选择器失败时保留旧SPIR-V，沿用单文件原子发布。
- [RayTracing.hpp](../../engine/graphics/device/include/dk/graphics/RayTracing.hpp)和
  [RayTracing.cpp](../../engine/graphics/device/src/RayTracing.cpp)提供move-only pipeline/SBT、三角hit groups，
  创建前验证stage/owner/layout/payload/选择器。单raygen、ray type交错hit、每类型一个miss，native递归深度1。
  SBT查询本pipeline handles，分别满足handle/base alignment、stride和区域边界；raygen size=stride，callable全零。
  SBT保留pipeline/layout及upload buffer，不公开可写buffer或接受裸地址SBT。
- Pipeline.cpp合并重复RT阶段，拒绝混合基线阶段；AS descriptor参与普通及per-stage设备限额。
  Bindings.cpp使用稳定的AS write pNext/handle存储，绑定仅接受本提交域TLAS并保留BLAS引用链；完整构造后发布。
- [CommandRayTracing.cpp](../../engine/graphics/device/src/CommandRayTracing.cpp)提供显式prepare_trace和generation借用encoder。
  SBT host-write→shader-read、BLAS/TLAS build-write→RT AS-read走现有提交账本；普通buffer/image仍须prepare。
  trace在任何native录制前验证资源访问、descriptor、push、dispatch及实例/geometry/ray-type索引。
  AS完成后保留geometry count/实例SBT范围用于验证，BUILD generation不随trace改变。
  成功submit才发布账本，失败/放弃回滚预约；pending保留所有依赖，timeout不释放，close等待并发布读回。
- [RT策略测试](../../tests/unit/RayTracingTests.cpp)、[Slang测试](../../tests/unit/ShaderCompilerTests.cpp)、
  [CLI进程测试](../../tests/integration/ShadercProcessTests.cmake)、[required GPU探针](../../tests/integration/RayTracingProbe.cpp)
  和[shader夹具](../../tests/fixtures/shaders/raytracing.slang)覆盖正负路径；同步CMake及测试选择表。

## 验证与证据

windows-graphics/RelWithDebInfo，MSVC 14.51；未修改baseline或安装新依赖。
日志位于out/verify各目录的summary.json、build.log、tests.log、results.xml。
GPU测试沿用0110/0111核验的旧AMD隐式层规避，仅在验证进程设
`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并finally恢复。Khronos validation为required，
CTest同时设置VK_LAYER_VALIDATE_SYNC和VK_VALIDATION_VALIDATE_SYNC；无77跳过，不修改注册表/持久环境。

| 运行编号（20261014） | 范围 | 结果 |
| --- | --- | --- |
| 115518-1770e279 / 115717-32333dba | shader测试及shaderc构建 | 构建失败，分别补齐span头、修复Catch INFO条件表达式；未执行旧二进制测试 |
| 115809-c80d834e | Slang及shaderc 13条目 | 12通过、1失败、0跳过；发现incoming payload无Location，修正提取规则 |
| 115904-d794d7ba | 四RT阶段编译与产物校验 | 1通过、0失败、0跳过 |
| 120308-3d1659d2 | Device及策略测试构建 | Vk region花括号赋值歧义导致构建失败；显式使用vk::StridedDeviceAddressRegionKHR后修复 |
| 120540-79dc0f96 | 4 RT策略、12原资源策略 | 16通过、0失败、0跳过 |
| 121204-9b9f772a | 首次真实RT管线/SBT/trace | 1通过、0失败、0跳过；零validation诊断和残留分配 |
| 121400-3727a12e | 12 Slang、2 shaderc、3离屏策略及RT/离屏/AS 3个GPU探针 | 20通过、0失败、0跳过 |
| 121637-1c16dae4 | 最终RT disabled、外来队列、布局缺阶段及完整GPU数值/寿命 | 1通过、0失败、0跳过；errors=0、warnings=0、liveAllocations=0 |

复现命令（GPU部分在上述临时环境内执行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_raytracing_tests','dk_graphics_resource_tests'
) -TestRegex '^dk\.(raytracing\.unit\.|graphics\.unit\.)' -Reason 'M16.4 RT and resource policies'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_shader_tests','dk_shaderc','dk_offscreen_tests','dk_offscreen_probe','dk_acceleration_structure_probe','dk_raytracing_probe'
) -TestRegex '^dk\.(shaders\.|shaderc\.|offscreen\.unit\.|offscreen\.gpu_validation$|acceleration\.required_validation$|raytracing\.required_validation$)' -Reason 'M16.4 compiler CLI and affected GPU paths'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target dk_raytracing_probe `
  -TestRegex '^dk\.raytracing\.required_validation$' -Reason 'M16.4 final RT capability owner and lifetime checks'
```

增量共36个不同CTest条目通过，未把重复运行相加。最后仅重跑新增负向用例所在的RT探针。
GPU为NVIDIA GeForce RTX 4070 Laptop GPU，SBT handle size/alignment=32/32、base alignment=64。
11条射线分别执行primary与visibility：命中/未命中、距离/重心（容差1e-4）、primitive/geometry/custom instance，
mask拒绝、TMin/TMax、背面、非均匀缩放和位移、any-hit IgnoreHit及force-opaque。
两个geometry和非零instance SBT offset选择不同hit入口，实际读回区分三个hit group输出，证明handle/index布局可用。
同批BLAS→TLAS→trace→readback通过同步验证；错误pipeline/SBT、越界实例、未构建/未准备、缺绑定、超限dispatch和过期encoder拒绝。
创建失败无残留；真实submit失败保持TLAS和输出已提交账本；外部owner释放后timeout仍保活，close完成读回并释放。
最终VMA allocationCount与Memory liveAllocations均为0，GPU errors/warnings均为0。
定向check-spec通过：12份Markdown、682个本地链接、元数据/表格/索引/测试入口及JSON清单；git diff --check通过。

## 限制与下一步

首版仅triangle hit groups，closest-hit必需、any-hit可选；ray type为1..15、hit groups最多120、layout入口最多256、
push constant最多一个共享范围。TraceRay仅raygen且SBT选择器静态，payload限32-bit scalar/vector/struct/fixed array，
hit attribute为float2；不支持callee递归、callable、intersection、local SBT参数、非一致descriptor索引。
未验证其他GPU/平台、配置矩阵、全引擎、编辑器、CPU路径积分回归或性能目标。
文档与Roadmap更新为M16.4完成；下一项M16.5接入Graph AS、类型化build/trace和图同步。
