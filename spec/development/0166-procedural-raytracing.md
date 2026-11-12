---
id: "0166"
created_at: "2026-11-12T11:42:00+08:00"
updated_at: "2026-11-12T11:56:00+08:00"
status: completed
design_refs:
  - ../design/graphics-raytracing.md
  - ../design/render-voxels.md
  - ../design/graphics-shaders.md
---

# 0166 M23.8 原生 AABB 与 NanoVDB intersection 门禁

## 目标与设计依据

实施 [M23.8](../roadmap.md#m23实体体素渲染) 与 [RT 实施契约](../design/graphics-raytracing.md#m238-实施契约)。
补齐通用 AABB BUILD、intersection 编译/反射、procedural hit group、混合 SBT 和 Graph；
使用现有 PNanoVDB 版本验证独立 intersection 阶段，不提前实现 M23.9 体素路径积分。

## 状态与失败保护

创建只产生候选；校验先于 native 命令，成功 submit 才发布资源状态。BUILD 后保留类型/映射；
TLAS/SBT/descriptor 独立保活，NanoVDB shader 数据不能随 build input 提前释放。失败及 pending/close 纳入验收。

## 验证计划

CPU：AABB 参数/范围/有限性、属性结构/限额/阶段、SBT kind、shaderc schema 与旧 triangle。
GPU：required RT + validation/sync；混合实例、两类射线、PNanoVDB 查值/属性、Graph upload/build/trace/readback、坏输入拒绝、失败和寿命。

## 实际变更

- Graphics Device：受检 CPU AABB 上传、原始 GPU AABB 描述/BUILD；TLAS 保留 kind/count/offset，trace 拒绝范围正确但种类错误的 SBT。
- Shader Types/Shaders/CLI：intersection 第五阶段、SPIR-V execution model 5314、hit_attribute 结构、ReportHit 阶段限制；intersection 禁用 payload/TraceRay；schema 4，旧阶段枚举值与 triangle 调用方式保留。
- Pipeline：procedural group 与 intersection 入口、属性生产/消费结构与 maxRayHitAttributeSize、intersection descriptor stage/限额；保持 static SBT selectors、native recursion 1。
- 新增 required NanoVDB intersection 探针：既有 PNanoVDB 32.7.0 窄适配和 32-bit uint buffer；17 条解析射线 × radiance/visibility，叶值 37 与负坐标 inactive constant tile 值 53；三角/球体最近交点、不同 geometry/primitive/instance、非均匀缩放、内部起点、TMin/TMax、mask/any-hit 和包围盒内空区域。
- 复用 Graph 的 AS/SBT 通用路径，无需新增 Graph 资源类型：Nano transfer→intersection、AABB/triangle BUILD→TLAS→trace→readback；callback/submit 失败保护、BUILD 完成释放 bounds、Nano shader buffer 保活至 pending 结束、close 回收。
- 同步 RT/Shader/体素设计、依赖说明、shaderc 参数与 schema、Roadmap/索引/测试选择表；无新增或升级依赖，无 CUDA 接入。

## 验证记录

Windows x64，Debug，固定 Slang 2026.18、PNanoVDB 32.7.0；NVIDIA GeForce RTX 4070 Laptop GPU。
required Vulkan validation + sync；禁用无关的 AMD switchable 隐式层，保留 Khronos 验证。

| 证据目录（out/verify 下） | 结果与范围 |
| --- | --- |
| 20261112-114419-62c12129 | 首轮 CPU 28 项：27 通过、1 失败；AABB、属性/SBT、旧 shader、intersection 与 shaderc schema 4 通过 |
| 20261112-114800-e6cd0bf4 | 7 项中 6 通过；NanoVDB intersection 编译/SPIR-V/required GPU 与旧 triangle AS/pipeline/Graph 三个 GPU 回归通过；旧内存注入用例再次失败 |
| 20261112-115221-6e7037a3 | 修正后 7 项全部通过：内存注入、intersection/伪造产物、shaderc、Nano 编译/SPIR-V/GPU；GPU 覆盖最终禁用 RT 拒绝及 AABB input 提前释放检查 |
| 20261112-115444-e04eba5b | 最终 RT CPU 6 项全部通过，包含 intersection per-stage 可见性/限额和属性/SBT 策略 |

失败原因与修正：既有 shader 内存注入测试的 128 字节预算不足以容纳 MSVC Debug 空容器的 iterator proxies，触发 noexcept 构造终止而非可捕获的增长分配失败。
预算改为 512 字节，保留 `bad_alloc`、failure_count 增加和 live_allocations=0 的断言；该用例复验通过。未改变生产分配器或隐藏失败。

最终有效范围共 34 个不同 CTests（30 CPU、4 required GPU），全部有通过证据，无跳过；各 GPU 均 0 错误、0 警告、0 残留 CPU/VMA 分配。
Nano intersection SPIR-V 9520 words，4 个全局 descriptor、1 个 8-byte `s(i32;f32;)` hit attribute；SBT handle/alignment/base 为 32/32/64。
构建失败次数为 0。后续验证保留先前通过项，只重跑实际改变的拒绝/寿命策略和失败项。

主要复验命令（各轮准确命令见对应 summary.json）：

```powershell
$env:VCPKG_ROOT = 'C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Debug -Target dk_acceleration_structure_tests,dk_raytracing_tests,dk_shader_tests,dk_shaderc -TestRegex '^dk\.(acceleration\.unit\.|raytracing\.unit\.|shaders\.|shaderc\.process$)' -Reason 'M23.8 CPU contracts' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Debug -Target dk_voxel_intersection_probe,dk_raytracing_probe,dk_graph_raytracing_probe,dk_acceleration_structure_probe -TestRegex '^dk\.(voxels\.(slang_intersection|intersection_spirv_validation|intersection_gpu_required)|raytracing\.required_validation|graph\.rt_required_validation|acceleration\.required_validation)$' -Reason 'M23.8 required GPU gates' -Parallel 4
```

文档检查：12 份 Markdown、1063 条本地链接及元数据/表格/索引/测试入口/JSON 通过；`git diff --check` 通过。索引初次新增行曾因空行断表被检查器拒绝，修正后复验通过。

## 边界与下一步

M23.8 完成，M23 保持开放，下一步 M23.9 将实际体素块/共享 HDDA 接入已有多次反弹与阴影积分。
解析球体/属性门禁不构成原生体素路径图像、性能或 Editor/capture 的交付。
本阶段未运行 Release、跨厂商、完整路径图像/Editor/光栅全量；没有改动 compute HDDA 或既有路径积分器。
原始 GPU AABB buffer 的内容（及后续修改）由调用方保证有限/有序；受检 CPU 上传入口提供边界验证。
本地独立提交；未重试此前被自动审批拒绝的公共远端推送。


## 修改记录

- 2026-11-12T11:42:00+08:00：固定实施边界与验收口径。
- 2026-11-12T11:56:00+08:00：完成 AABB/intersection/SBT 与 34 项定向验收，关闭 M23.8，保留 M23.9 边界。
