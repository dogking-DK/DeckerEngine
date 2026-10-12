---
id: "0095"
created_at: "2026-10-12T16:23:00+08:00"
updated_at: "2026-10-12T16:43:19+08:00"
status: completed
design_refs:
  - ../design/graphics-resources.md
  - ../design/graphics-vulkan.md
  - ../design/graphics-shaders.md
  - ../design/graphics-graph.md
  - ../design/render-pipeline.md
  - ../design/render-atmosphere.md
---

# 0095 M14.2 体积图像与深度采样底座

## 目标与设计依据

完成[Roadmap](../roadmap.md) M14.2；依据[资源契约](../design/graphics-resources.md)、
[反射](../design/graphics-shaders.md)、[Graph](../design/graphics-graph.md)补齐3D与深度采样基础。
不实现大气积分/LUT/合成，不新增三方依赖。

## 实际变更

- [ImageDesc](../../engine/graphics/device/include/dk/graphics/Resources.hpp)追加depth/type/require_linear_filter；
  支持原生3D且保留原2D聚合初始化含义。RGBA16F使用8字节texel，RGBA32F使用16字节。
  先校验三轴extent/mip/layer/usage/溢出，再按实际type和format/usage查询硬件上限；显式请求的线性过滤不支持则拒绝。
- [CommandTransfer](../../engine/graphics/device/src/CommandTransfer.cpp)补齐z/depth、row/slice pitch及乘加溢出；
  Graph共享image_copy_bytes。旧base-level copy覆盖所有z切片，便利readback输出紧密数据和depth/slice_pitch。
  局部写入不初始化整个mip，局部copy与full_overwrite矛盾时在原生命令前拒绝。
- [GpuObjects](../../engine/graphics/device/src/GpuObjects.cpp)核对2D/array/3D视图；
  [ShaderCompiler](../../engine/graphics/shaders/src/ShaderCompiler.cpp)提取ShaderImageShape，拒绝1D/cube/MSAA等无法表达形状。
  ShaderModule、布局合并/兼容/覆盖、BindingSet传递并校验维度；storage view只允许一个mip。
- shaderc反射JSON升级schema v2，每条binding含image_shape（none/2d/2d_array/3d）；更新工具参考、目录和指南，
  CLI进程验收3D输出及不支持形状失败时保留原SPIR-V。无需Runtime命令注册变更。
- [GraphExecution](../../engine/graphics/graph/src/GraphExecution.cpp)导入比较三轴类型和过滤请求，拷贝声明范围包含所有切片；
  诊断显示三轴extent/type。复用现有ledger产生storage→sample及depth attachment→sample屏障，无独立z切片状态。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)的D32增加Sampled usage，现有unlit Pass顺序和输出保持一致。
- 新增[体积GPU探针](../../tests/integration/VolumeImageProbe.cpp)和5个Slang夹具；扩充纯策略/反射及shaderc进程测试，
  同步CMake、测试选择表、主题设计、设计/开发索引和Roadmap。无新依赖、feature或CPU runner链接变化。

## 验证记录

环境：Windows x64、Visual Studio 2026/MSVC、windows-graphics Debug；
本轮GPU日志报告NVIDIA GeForce RTX 4070 Laptop GPU、驱动596.49、Vulkan 1.4.329、Slang 2026.18。
使用decker-build-verify选择直接影响链路；未启动GUI。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_graphics_resource_tests','dk_shader_tests','dk_shaderc','dk_graph_tests') `
  -TestRegex '^dk\.(graphics\.unit\.|shaders\.|shaderc\.process$|graph\.graph )' `
  -Reason 'M14.2 volume footprint, shape reflection, shaderc schema and Graph declaration regression'

& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_volume_image_probe `
  -TestRegex '^dk\.graphics\.volume_validation$' `
  -Reason 'M14.2 required real GPU volume/depth sampling, pitched transfers, Graph barriers and failure lifetime'

$previousLayerSetting = [Environment]::GetEnvironmentVariable('DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1', 'Process')
try {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_volume_image_probe','dk_shaderc','dk_graphics_resource_probe','dk_image_transfer_probe',
              'dk_graph_probe','dk_offscreen_probe','dk_render_pipeline_probe') `
    -TestRegex '^dk\.(graphics\.(volume_validation|gpu_resources_validation|image_transfer_validation)|graph\.gpu_validation|offscreen\.gpu_validation|render\.pipeline_gpu_validation|shaderc\.process)$' `
    -Reason 'M14.2 final volume full-overwrite and shaderc failure checks plus affected 2D GPU regression; disable incompatible AMD implicit loader layer only'
} finally {
  [Environment]::SetEnvironmentVariable('DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1', $previousLayerSetting, 'Process')
}
```

- `out/verify/20261012-162815-48e8e44b`：60/60通过，0失败/跳过。38项Graph、10项Slang、11项资源策略、1项shaderc进程。
  三轴mip/零尺寸/非法类型/层数/usage、对齐/offset/row/slice溢出、shape冲突/数组区分覆盖。
- 探针初次`20261012-163252-fe2ade13`失败：夹具误把图像当作编译资源0；Graph资源按buffer后image排列。
  修正后`20261012-163407-9e0d241d`失败：深度附件需要现有encoder的read/write契约。
  增加初始化clear Pass并声明附件read/write；`20261012-163520-5272d3d4`为1/1通过，0跳过。
  两次失败均是新探针声明错误，未降低设备访问校验；故障注入断言也要求实际到达callback/submit位置。
- `out/verify/20261012-163618-f2ac430f`：二维回归2通过/3失败，0跳过；全部数值/生命周期断言通过，
  三个旧探针因AMD隐式层API1.3与应用1.4不兼容告警失败，errors=0。按既有0062验证流程仅在子进程环境禁用该层，
  Khronos required及同步验证保持开启；不修改系统驱动或全局环境。
- `out/verify/20261012-163915-b70d7d95`：最终7/7通过，0失败/跳过，包含新探针、shaderc进程及5条二维GPU链路。
  新探针已加入局部copy伪造full_overwrite拒绝和external depth/过滤请求不匹配拒绝，产物是最后实现构建结果。
  GPU验证均errors=0/warnings=0；pending、VMA和Memory分配回收断言通过。
- 共有66个不同测试通过，无未解决失败。体积GPU测试要求真实设备及validation可用，未配置skip return code。
  RGBA16F/32F的4×3×5逐体素storage结果精确；采样覆盖两角、三轴非整数插值、z clamp/repeat，绝对容差0.002；
  带offset/row/slice padding上传、紧密读回、局部z覆盖、3D mip、legacy双向copy逐字节一致。
  深度值先clear为0.875再经附件clear为0.375，16点nearest采样全部精确为0.375；检查实际Graph屏障前后状态。
  硬件3D extent越界、R32_UINT线性过滤能力、错误view/binding、未初始化读取和Graph欠声明z切片拒绝。
  录制/提交失败保留状态，取消/未提交/在途readback不写用户输出，提交后释放image/view/pipeline/binding仍能完成读回。

未运行Release、引擎全量、编辑器GUI、性能/跨厂商验证、CPU runner重建：本次只改变Graphics及其ScenePipeline直接消费链路，
不改CPU环境/持久化/命令注册或依赖配置。测试不代表大气LUT、日光或空气透视已实现；不承诺真实GPU OOM或device loss恢复。

## 偏差与决策

保留原聚合字段顺序，新增字段放末尾；线性过滤由ImageDesc显式请求，不从独立sampler猜测shader采样配对。
不请求该能力的调用者仍须自行保证采样操作兼容；不做隐式格式降级。3D首版仅color sampled/storage/transfer，
无3D附件、二维切片view、cube/MSAA或深度byte copy。D32仅提供nearest采样验证。
反射仍是结构验证，不承诺识别手工伪造反射与SPIR-V的全部差异。shaderc消费者须按schema v2更新并重新编译产物。

## 遗留问题与下一步

M14.2范围无遗留；下一项M14.3实现大气积分参考及Transmittance/Multiple Scattering/Sky View LUT和离屏天空样例。
用户已有spec/README.md及AI文档迁移指南不纳入提交。

## 修改记录

- 2026-10-12T16:23:00+08:00：先更新设计，创建记录。
- 2026-10-12T16:42:03+08:00：实现及66项不同定向测试完成，登记探针修正/隐式层告警和最终验证结果；关闭M14.2。

- 2026-10-12T16:43:19+08:00：文档检查初次发现0095索引行误放在表格尾注之后，移回表格后通过14份Markdown/614个本地链接及元数据、表格、索引、测试入口、JSON清单；git diff --check通过。

最终文档命令：

```powershell
& ./scripts/check-spec.ps1 -Path @(
  '.agents/skills/decker-build-verify/references/test-selection.md',
  'spec/commands/README.md','spec/commands/shaderc.md','spec/guides/shaders.md','spec/design/README.md',
  'spec/design/graphics-resources.md','spec/design/graphics-vulkan.md','spec/design/graphics-shaders.md',
  'spec/design/graphics-graph.md','spec/design/render-pipeline.md','spec/design/render-atmosphere.md',
  'spec/development/0095-volume-images.md','spec/development/README.md','spec/roadmap.md'
)
git diff --check
```
