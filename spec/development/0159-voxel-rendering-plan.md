---
id: "0159"
created_at: "2026-11-11T14:51:58+08:00"
updated_at: "2026-11-11T16:31:04+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/architecture.md
  - ../design/graphics-raytracing.md
  - ../design/graphics-shaders.md
  - ../design/render-pathtracing.md
  - ../design/render-pipeline.md
  - ../design/render-restir.md
  - ../design/render-denoising.md
  - ../design/assets-importers.md
---

# 0159 M23.1 体素渲染正式计划

## 目标与设计依据

将本会话中的七阶段体素渲染方案纳入实际开发计划，依据[体素设计](../design/render-voxels.md)
与[架构](../design/architecture.md)维护正式 Roadmap、模块边界和独立验收条件。
本次仅设计落档；accepted 表示采用设计，completed 仅用于本设计阶段，不能表示体素功能已经实现。
下列初始计划/验证对应提交 `c974fab`；用户随后要求原生 GPU 路径支持，本记录后段保存该扩展。
当前采用范围以主题设计和 Roadmap 为准，初始“路径后置/七阶段”不再代表当前计划。
后续追加 OpenVDB/NanoVDB 稀疏方案，取代此前“稠密生产网格/VDB 后置”；前两轮章节保留当时验证事实。

## 实际变更

- Roadmap 新增 M23.1–7：设计、CPU 网格/求交、GPU 遍历、混合场景合成、资产生命周期、Editor/自动化、质量性能交付。
  明确必要前置、首个离屏节点与使用闭环；M20/M21/M22 原有阶段状态和默认顺序不变。
- 新建 render-voxels 设计：规则稠密整数网格、空/实交界表面、独立 CPU oracle、DDA、透视/正交及世界距离一致性。
  明确 mesh 与体素的 D32/HDR 合成顺序，拒绝未支持的介质/路径渲染组合，不能静默漏画。
- 约定 Geometry/Assets/Render/Framework 的所有权、版本与提交边界，拟新增路径和 target 标为计划。
  文件 v1 和中间资源布局在对应实施阶段冻结；不添加依赖、CMake、源码、shader 或命令注册。
- 同步架构扩展边界、设计/开发索引及 spec 入口；体素表面与 M21 后置的密度场 ray marching 明确区分。

## 验证记录

```powershell
& ./scripts/check-spec.ps1 -Path @('spec/README.md','spec/roadmap.md','spec/design/README.md',
  'spec/design/architecture.md','spec/design/render-voxels.md','spec/development/README.md',
  'spec/development/0159-voxel-rendering-plan.md')
git diff --check
```

- 文档检查通过：7 份 Markdown、895 个本地链接，以及元数据、表格、全局索引、测试入口和 JSON 清单。
- `git diff --check` 通过；人工核对 M23.1–7 依赖、架构方向、设计/实现状态区分及 M20/M21/M22 状态未改变。
  新路径/target/API 均标为规划，不在当前测试选择表登记不存在的测试程序。

本轮不运行引擎构建、CPU/GPU 数值、GUI 或性能测试：尚无实现变更，设计中的测试均为未来验收计划。

## 偏差与决策

沿用用户确认的“规则网格中的实体体素表面”范围；没有扩展为烟雾介质、体素路径追踪、
网格体素化、SVO/LOD 或无限地形。M23 不等待 M20/M21，实际启动仍按用户指定路线和必要前置执行。
编号依据创建时最大开发记录 0158 分配；不预分配未来实现记录编号。

## 遗留问题与下一步

本路线下一实施阶段为 M23.2 CPU 网格与求交参考，须先固定 CPU 接口和独立 oracle 用例。
GPU 布局/精度、资产文件格式和性能优化分别在 M23.3、M23.5、M23.7 落实；不将其写为当前已实现。
本轮不开始 M23.2，也不改变其他路线的下一阶段。

## 修改记录

- 2026-11-11T14:51:58+08:00：创建正式计划、模块设计和七阶段 Roadmap；等待文档验收。
- 2026-11-11T14:58:14+08:00：文档检查与依赖审阅通过，完成 M23.1；后续六个实现阶段仍待开始。

## 2026-11-11：纳入原生 GPU 路径并明确光栅路线

### 范围与设计变更

按用户明确要求，将现有 GPU 路径追踪器的原生体素求交纳入 M23；继续只更新计划/设计，不实现代码。
保留 M23.1–7 编号，其中 M23.6/7 为光栅侧闭环/基线；追加 M23.8–13，父阶段须在两条路线全部验收后关闭。

- [体素设计](../design/render-voxels.md)明确共享网格/材质/表面语义，光栅 compute DDA 与路径 AABB/intersection DDA 的职责。
  DDA 按网格边界逐格遍历，不是固定步长的密度积分；光栅合成不代表体素已经参与路径反弹/阴影。
- [RT 底座](../design/graphics-raytracing.md)与[shader 设计](../design/graphics-shaders.md)增加拟实施的 AABB BUILD、
  intersection 编译/反射、自定义属性、procedural 命中组、SBT/Graph 与资源寿命契约。
  核对 Khronos 文档：intersection 通过属性传值、不访问 ray payload；真实交点才可触发遮挡，不能用 AABB 代替实体。
- [路径设计](../design/render-pathtracing.md)规划通用 SurfaceHit、voxel-only/混合输入、CPU oracle 与 Classic Lambert/GGX/NEE/MIS/RR。
  明确次级射线不能套用相机 far，块边界不能产生假面，GPU 身份不能经过有损 float 转换；不承诺硬件同 t 决胜顺序。
- [光栅设计](../design/render-pipeline.md)明确 compute 命中 → 片段深度/HDR 合成 → tone 的顺序和能力范围。
  光栅不要求 RT，不新增表面网格化、每体素 draw 或完整 PBR/阴影系统。
- [ReSTIR](../design/render-restir.md)与[信号](../design/render-denoising.md)规划类型化来源、占用/材质变化失效、
  接收面/visibility 和 fresh/guide/reference 适配；已有空气/云路径组合另阶段验收，M20 滤波器与介质信号边界不变。
- [Roadmap](../roadmap.md#m23实体体素渲染)追加 RT 底座、Classic 路径、ReSTIR/信号、介质路径、消费者及联合质量性能验收。
  M23.8 满足前置后可独立于光栅性能优化推进；M20/M21/M22 原有状态与默认执行顺序不变。
- 同步架构、设计/开发索引与 spec 入口。体素材质明确 baseColor/metallic/roughness，emission=0；
  不新增体素灯采样、透明体素或密度场求解，原生文件 v1 在 M23.5 包含两后端共用材质。

### 本次扩展验证

```powershell
$voxelPlanDocs = @('spec/README.md','spec/roadmap.md','spec/design/README.md',
  'spec/design/architecture.md','spec/design/render-voxels.md','spec/design/graphics-raytracing.md',
  'spec/design/graphics-shaders.md','spec/design/render-pathtracing.md','spec/design/render-pipeline.md',
  'spec/design/render-restir.md','spec/design/render-denoising.md','spec/development/README.md',
  'spec/development/0159-voxel-rendering-plan.md')
& ./scripts/check-spec.ps1 -Path $voxelPlanDocs
git diff --check
```

- 文档检查通过：13 份 Markdown、1074 个本地链接，以及元数据、表格、索引、测试入口和 JSON 清单。
- `git diff --check` 通过；人工核对阶段依赖、M23.7 不关闭父阶段、非 RT 能力分离、混合几何身份与光照范围。
  现行设计已移除“体素路径追踪不在 M23”的约束；记录前半保留带版本说明的历史七阶段事实。

未运行引擎构建、shader 编译、GPU/CPU 数值、GUI 或性能测试；所有新增功能和验收均仍是计划。
不因原生 RT 机制在 Vulkan 标准中存在而声称本引擎已经实现；不新增 CMake target、测试程序、依赖或命令注册。

### 当前下一步

下一实施阶段仍为 M23.2 CPU 网格与独立求交参考，后续状态由 Roadmap 维护。
M23.8/9 冻结 shader 产物/属性/SBT、块布局/通用命中，M23.10 冻结身份/信号版本；
M23.12 接通两后端消费者，M23.13 联合交付。当前只完成这次设计扩展，不启动任何实现阶段。

- 2026-11-11T15:32:31+08:00：原生路径及光栅说明扩展完成，文档验收通过；M23.2–13 均待开始。

## 2026-11-11：采用 OpenVDB/NanoVDB 稀疏网格

### 本次范围和决策

按用户要求更新正式设计、Roadmap 和分阶段验收；本轮只改文档，不安装依赖或开始功能实现。
保留 M23.1–13 编号，所有实现阶段仍待开始，稀疏生产路线不能由稠密参考验收代替。

- OpenVDB 用于可选 CPU 构建/`.vdb` 导入，NanoVDB 连续快照用于 CPU/GPU 查询；稠密 DDA 与独立外露面 oracle 保留为正确性参考。
- 明确 Int32 占用/palette、零背景、active/inactive/tile 规范化、负坐标、有限域、采样中心/单元角半格转换及可逆仿射 grid/entity 组合。
  SDF/fog/速度/点网格不是本轮实体输入，不能按 active mask 或密度阈值猜测占用；不按稀疏全域分配稠密纹理。
- 光栅 compute 与 RT intersection 共用 PNanoVDB/Slang accessor + HDDA；前者做深度/HDR 合成，后者通过块 AABB/BLAS/TLAS 参与主射线、反弹和阴影。
  代理覆盖非零 tile，tree/leaf/块边界不产生假面；稳定身份使用有符号 cell 坐标而非节点或字节偏移。
- M23.2 增加依赖锁定/无 CUDA CPU 构建与转换、稀疏查询对 oracle；M23.3/8 分别设 compute/intersection Slang 编译、反射和 required GPU 门禁。
  M23.5 完成 `.vdb` 描述及 NanoVDB 产物/缓存协议；M23.7/13 增加大跨度低占用、常量 tile、转换/AS/在途内存与性能矩阵。
- 同步架构、导入器、光栅/路径、RT/Shader、ReSTIR/信号设计和索引；复用不可变候选/提交/保活契约，明确 GPU buffer 同步与 CPU 转换峰值。

### 依赖核验与已知接入问题

2026-11-11 经 GitHub API 实时查询官方 vcpkg master 为 `f38855d514df2b21c2e911a6c6865703abb8da15`，
该固定提交的 openvdb port/版本历史为 `12.0.1#0`，许可证 Apache-2.0；选型证据链接见[三方库说明](../third-party-libraries.md#m23-openvdbnanovdb-稀疏网格选型)。
上游同版本 PNanoVDB 提供 HLSL 分支/Int32 布局，尚未在工程 Slang 编译或 GPU 上执行。
官方 `openvdb[nanovdb]` 当前引入 CUDA，portfile 强制启用 CUDA；上游 NanoVDB 数据结构本身不依赖 CUDA。
因此 M23.2 必须完成无 CUDA 打包验证；必要时只对同版本固定来源做最小 overlay，记录补丁与退出条件，不能默认为官方 feature 已满足要求。
本次 manifest、builtin-baseline、CMake 和锁定的 Slang 均未改变；实施时重新核验官方最新 port 后固定构建基线。

### 本次验证

```powershell
$vdbPlanDocs = @('spec/README.md','spec/roadmap.md','spec/third-party-libraries.md',
  'spec/design/README.md','spec/design/architecture.md','spec/design/assets-importers.md',
  'spec/design/render-voxels.md','spec/design/graphics-raytracing.md','spec/design/graphics-shaders.md',
  'spec/design/render-pathtracing.md','spec/design/render-pipeline.md','spec/design/render-restir.md',
  'spec/design/render-denoising.md','spec/development/README.md','spec/development/0159-voxel-rendering-plan.md')
& ./scripts/check-spec.ps1 -Path $vdbPlanDocs
git diff --check
```

- 文档检查通过：15 份 Markdown、1165 个本地链接，以及元数据、表格、索引、测试入口和 JSON 清单。
- `git diff --check` 通过；人工核对现行稀疏/稠密职责、M23.2/3/8 依赖及门禁、两后端入口、阶段状态和旧计划的历史标注。
- CPU 转换、Slang 编译、Vulkan GPU/RT、功能/性能测试均未运行，本次没有实现代码；官网/port 核验不代表这些测试通过。

### 当前下一步

M23.2：先保留独立稠密 oracle，再接入固定依赖与 CPU OpenVDB→NanoVDB 转换/查询，验收无 CUDA、数据/坐标/失败契约。
M23.3 以实际 Slang compute 兼容性为门禁；M23.8 的 NanoVDB intersection 门禁依赖其已固定的 accessor/布局，通用 RT 底座可提前推进。
任一门禁失败保留未通过状态并修复适配，不以 dense/mesh 回退宣称稀疏能力完成。父 M23 仍需全部阶段证据才能关闭。

- 2026-11-11T16:31:04+08:00：稀疏方案、依赖选型及 Slang 双阶段验收计划落档，文档检查通过；依赖和 M23.2–13 功能仍未实施。
