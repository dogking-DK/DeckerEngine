---
id: "0162"
created_at: "2026-11-11T19:00:00+08:00"
updated_at: "2026-11-11T19:38:00+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/render-pipeline.md
---

# 0162 M23.4 mesh/voxel 深度与 HDR 合成

## 目标与设计依据

实现 [M23.4](../roadmap.md#m23实体体素渲染)，契约见[体素合成](../design/render-voxels.md#m234-合成实施契约)与[ScenePipeline](../design/render-pipeline.md#m23-光栅侧体素合成)。
复用 M23.3 的 NanoVDB/共享 HDDA，真实 GPU 验证双向遮挡、投影/裁剪、Unlit/方向光与既有曝光/tone，保持旧 mesh 行为。

## 实际变更

- [ScenePipeline](../../engine/render/pipeline/include/dk/render/ScenePipeline.hpp) 增加 `VoxelInstance`、palette、Unlit/方向光设置和 `render_voxels`；可选 PRIVATE 消费 render_voxels，未启用时明确拒绝。程序化实例与已有 mesh RenderView 合成，不新增持久协议。
- [VoxelComposition](../../engine/render/pipeline/src/VoxelComposition.cpp) 在 CPU 验证整个 palette/最大值、唯一 ID、变换、相机和额度，保守八角视锥剔除；按稳定 ID 排序，复制 palette 并保活不可变网格。最多 64 实例、1 Mi 像素；附加 payload 默认 256 MiB。
- [体素 shader](../../shaders/render/voxel-raster.slang) 在 GPU 生成射线，直接复用 PNanoVDB/共享 HDDA，逐实例累积 64-byte 最近表面记录；[全屏合成](../../shaders/render/voxel-composite.slang) 用 fragment D32 Less/test/write 合入 HDR，精确同深度保留 mesh，体素同 t 保留较小 ID。无主机逐像素渲染或读回后合成。
- [方向光 shader](../../shaders/render/directional.slang) 与 voxel 共用 Lambert 公式，mesh 保留基色纹理、alpha mask 和 emissive；法线采用逆转置，拒绝缺法线/金属 mesh、金属 voxel 和大气/云组合。沿用既有曝光、Reinhard/sRGB。
- Graph 在同次提交中完成 clear → mesh depth/opaque → HDDA → HDR/D32 composite → 诊断 → tone/readback。新增最终 D32 sampled-compute 诊断，与线性 HDR 读回均限制 65536 像素，不扩大 Device 深度传输接口。
- GPU 状态按错误码位图汇总；工作量、地址、非有限数值和射线下溢错误阻止 wait/read/color 发布，读回失败不修改调用者输出。输入失败/submit 失败不改变旧帧、Scene、GpuAssets 或 VoxelGpuCache。shader 程序懒缓存可在输入失败后保留。
- CPU SparseGrid 新增 `maximum_value` 元数据，覆盖 inactive 非零值，避免不可见非法 palette 索引逃逸。修复 CPU/GPU 共享遍历的外露面法线选择，增加独立解析回归。
- 新增 `windows-voxels-raster` 与 [required GPU 探针](../../tests/integration/VoxelRasterProbe.cpp)，同步测试选择表、构建/调用指南和阶段文档；依赖版本、baseline、NanoVDB ABI 不变，无新增 CUDA/RT。

## 验证记录

环境：Windows、MSVC 19.51 / VS 2026、Debug、`DK_WARNINGS_AS_ERRORS=ON`，Vulkan SDK 1.4.363.0、Slang 2026.18，
NVIDIA GeForce RTX 4070 Laptop GPU / driver 596.49。新探针显式确认 RT 未启用，Khronos validation 与同步验证开启，缺设备/层直接失败。

运行前固定门槛：每个像素 RGB ≤1 byte，HDR ≤2e-4×max(1,abs(reference))，最终 D32 ≤2e-6；alpha 精确匹配，无跳过边缘像素/图像遮罩。
参考使用 CPU double 反投影、解析 quad/实体盒求交及独立 Lambert/Reinhard/sRGB，不调用被测 HDDA。

```powershell
cmake --preset windows-voxels-raster -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels-raster `
    -Target @('dk_voxel_raster_probe','dk_voxel_tests','dk_voxel_sparse_tests','dk_voxel_gpu_probe','dk_render_pipeline_tests') `
    -TestRegex '^dk\.(voxels\.|render\.pipeline\.)' -Reason 'M23.4 共享遍历与合成直接回归' -Parallel 4
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels-raster -Target dk_voxel_raster_probe `
    -TestRegex '^dk\.voxels\.raster_.*_required$' -Reason 'M23.4 最终 GPU 合成与寿命验收' -Parallel 4
```

- `out/verify/20261111-192352-811d2247`：30 项中 29 通过、1 失败、0 跳过。14 项 CPU 稠密/稀疏、9 项 Pipeline CPU、M23.3 Slang/SPIR-V/3 项 required GPU 全通过；剩余合成失败为 CPU 参考的正交擦边舍入，随后修正。
- `out/verify/20261111-192934-668f6c13`：合成 1/1 通过，68.68 s；修正参考后保持原像素/HDR/深度门槛。
- `out/verify/20261111-193256-c86f5f69`：最终合成/寿命 2/2 通过、0 失败、0 跳过，分别 106.41 / 3.38 s；补齐射线下溢失败门禁。两项均 validation errors=0、warnings=1、known=1，最终 live=0/GPU allocations=0。耗时包含 CPU 全像素参考且同时有另一目录构建，不作性能结论。
- `out/verify/20261111-193122-7e64e8a0`：旧 mesh 配置 10/10 通过、0 失败、0 跳过，包含 9 项 Pipeline CPU 和真实 GPU 回归；GPU=RTX 4070 Laptop / 596.49，errors=0、warnings=0、liveAllocations=0、pending=0、allocations=0。覆盖原曝光/空场景/resize、正交尺寸和 near/far 四张全图、HDR 传输、预算/提交/关闭及在途释放。
- 此前通过的共享 CPU/GPU 回归代码未再改动；最终补丁只变更 raster shader 的失败保护、夹具和接口注释，因此只复跑两项 raster 门禁。
- `check-spec.ps1 -Path` 定向检查本次 11 份 Markdown，通过 1080 个本地链接、元数据、表格、索引、测试入口与 JSON manifests；`git diff --check` 通过。

合成覆盖：两投影×Unlit/方向光×曝光 0.5/2 的 96×64 HDR/D32 全像素；64³/640×480 双投影全像素；
双向遮挡、同深度 mesh、同 t 多实例/反序、alpha mask、空列表与旧入口逐字节相同；
80×60 双投影的内部相机、near 穿过实体、far 剔除、负缩放/剪切及 Y 翻转/透视偏移。不同分辨率连续提交验证 resize/重复帧。
寿命覆盖：坏 ID/palette/变换/投影/组合/额度拒绝、不可见 palette 最大值、注入 submit 失败/timeout、提交后 palette 修改、
GPU 工作量/射线下溢失败禁止发布、在途释放 grid/cache/pipeline、closing 与最终 GPU/Memory 分配归零。

旧 mesh 使用 `windows-graphics`（`DK_BUILD_RENDER_VOXELS=OFF`、`DK_BUILD_VOXEL_VDB=OFF`）：

```powershell
$previousAmdLayer = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
        -Target @('dk_render_pipeline_probe','dk_render_pipeline_tests') `
        -TestRegex '^dk\.render\.(pipeline\.|pipeline_gpu_validation$)' `
        -Reason 'M23.4 未启用体素配置的旧 mesh CPU/GPU 回归' -Parallel 4
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previousAmdLayer }
```

只用 AMD manifest 自身的开关暂时隔离已知旧隐式层，执行后恢复环境；不关闭 Khronos/sync，依据见 [0048](0048-vulkan-14-baseline.md)。
新体素门禁保留精确 allowlist：AMD layer API 1.3 / application 1.4 的单条 Loader warning 单列为 known，其他 warning 均失败。

产物：`out/build/windows-voxels-raster/tests/integration/voxel-artifacts/` 下的两张 `voxel-mixed-*.ppm` 和 `voxel-composition-plan.txt`。
已转换为 PNG 做人工查看，与自动全像素结果一致；截图只作辅助证据。

## 偏差与决策

1. 初次构建发现夹具 allocator 不兼容和混合类型 auto 声明，已修复；未使用旧二进制作为构建通过证据。
2. D32 shader 的 `Texture.GetDimensions` 引入未开放的 ImageQuery capability；改为显式 push extent，保留原设备能力边界。随后修正 ComputeEncoder push constants 的阶段参数。
3. 分轴 NDC 除法使应为相同斜率的射线出现 float 差异，导致精确角擦边误命中；改为 CPU 预计算像素步长、GPU 中心像素偏移，未修改夹具或放宽门槛。
4. 方向光独立解析参考暴露旧共享遍历法线问题：同时越过外表面与内部 cell 平面时，原 XYZ 优先可能取内部面。CPU/GPU 现在优先选择相邻为空的真正外露面，退化多面交点再按 XYZ；补 dense/sparse/GPU 解析回归，M23.2/3 直接链路通过。
5. 独立 double oracle 在零长度角擦边/正交平行外平面有反投影舍入；只按 double epsilon 修复解析边界判定，未遮罩像素或改变 GPU RGB/HDR/D32 容差。
6. 寿命夹具最初被本地拒绝测试向量额外保活网格，释放这些测试所有者后核验真实帧在途持有与回收。

上述早期失败保留在 `out/verify/` 下以 `20261111-` 开头的目录，后缀为 `191113-8ce97969`、`191218-ca049303`、`191343-58ec0859`、
`191536-f2c3a224`、`191802-180728ff`、`191957-c2597de1`；均不计为通过。

## 遗留问题与下一步

本阶段仅程序化不透明表面合成；没有正式 `.vdb`/Scene 资产、Editor/命令/capture、几何阴影、完整 PBR 或大气/云组合。
没有执行 Release/全引擎测试、跨厂商 GPU、128³/256³ 性能矩阵；本阶段不承诺 FPS 或加速倍数。
下一步 M23.5 文件资产；原生 GPU 路径支持依次由 M23.8/9 实现，本合成验收不替代 intersection/SBT/反弹验收。

## 修改记录

- 2026-11-11T19:00:00+08:00：固定合成与数值/状态门槛，开始实现。
- 2026-11-11T19:38:00+08:00：完成 M23.4 与真实 GPU/旧 mesh 验收，记录边界修正及失败保护，下一步 M23.5。
