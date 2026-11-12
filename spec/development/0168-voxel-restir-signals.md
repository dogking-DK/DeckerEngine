---
id: "0168"
created_at: "2026-11-12T14:15:00+08:00"
updated_at: "2026-11-12T15:11:34+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
  - ../design/render-denoising.md
---

# 0168 M23.10 体素 ReSTIR 与表面信号

## 目标与状态契约

实施 [M23.10](../roadmap.md#m23实体体素渲染) 和 [实施契约](../design/render-voxels.md#m2310-restir-与信号实施契约)。
将类型化稳定身份、Surface DI 四种模式与 fresh/guide/reference 信号接入已有稀疏体素路径；不开放 M23.11 介质组合或新增滤波器。
冻结来源、私有候选和完整 epoch 发布保持一致；内容替换/失败/取消不得污染旧 completed owner。

## 验证计划

CPU 内容版本/解析身份/initial RIS 对独立三角化；required GPU 四种 DI 均值/支持、遮挡/显露/内容失效、信号分量/guide/透视正交 reference、预算/失败/取消及旧 mesh 回归。默认 Debug、定向 target/regex，无需重复 M23.9 已完成的长时介质统计。

## 实施与验证记录

- CPU `estimate_restir_reference` 开放原生体素，共用既有材质/偏移/visibility，遍历错误转成 Result；内容版本 create 分配、rebind 共享，不回绕。公开 `PathSurfaceIdentity` 解析代表信号样本，区别于 sample0 诊断。
- GPU ReSTIR 管线增加与 Classic 相同的 procedural radiance/shadow hit groups；首表面保存/重建完整 voxel payload。最终阴影与后续反弹使用混合几何，移除 voxel 索引误作 triangle 寻址的路径。
- surface v2 为 96 B/像素，temporal 比较 kind/instance/cell/face/value 后检查位置/法线；spatial 保留跨表面/材质来源与支持域校正，使用来源自己的 BSDF。temporal completed raw/surface/reservoir 为256 B；非 temporal 完成后仍只留128 B raw。
- signal/source v2 为368 B/像素，新增整数 key/signed cell。完整 epoch 同时发布 raw、reservoir 与信号；新增解析身份与 cell/palette 校验。reference 私有持有冻结 CPU geometry owner，不保活 GPU 历史；v1、无 token、篡改公开 hash/version 的旧 owner 均拒绝。
- 接入四个 required GPU 模式 `voxel_restir_numeric/reuse/state` 和 `voxel_signals`，CPU 增加 initial RIS/独立外露面、内容版本及预算断言；同步旧 M23.9 能力拒绝断言、mesh 信号版本和测试选择表。

### 验证结果

最终39项配置/测试组合获得通过证据：16 CPU、23 required GPU（VDB配置26项+无VDB CPU 11项+无VDB GPU 2项）。初次回归的两项超时和v1预算夹具失败均经下述定向复验关闭；不重复已通过的无关检查。全部已完成GPU项为RTX 4070 Laptop、required validation/sync，errors=0、warnings=0、CPU/VMA owner无泄漏。

- 初始门禁 `out/verify/20261112-141854-b1ceb549`：Debug 构建及旧 mesh `restir_numeric_gpu_required` 通过。
- 首轮 `out/verify/20261112-142234-e388c810`：6/9 通过，三项夹具/断言失败。独立三角化不得比较局部 primitive 索引；直接命中不保证空区域遍历计数非零；原透视夹具无 mesh 边缘覆盖。修正为线性颜色/二阶矩对照、实际命中 kind 与更宽视野，未放宽数值阈值。
- 修正后 `out/verify/20261112-142533-30938e16`：5 CPU +4 required GPU 全通过。RTX 4070 Laptop、Khronos validation/sync，errors=0、warnings=0、CPU/VMA owner 无泄漏；后续补入 GGX 跨材质支持与相机显露，并在最终回归重跑专项。
- 独立无 VDB/无 Vulkan CPU 配置 `out/verify/20261112-143001-35af1163`：11/11 通过；覆盖 RIS reservoir/merge/support/随机域、灯域、预算与输入拒绝。
- 最终 VDB GPU 回归 `out/verify/20261112-142846-77245a20`：23/26 通过；两个旧长测 `restir_spatial_support/materials` 触及240秒，`restir_spatial_budget` 沿用v1应成功尺寸而拒绝。后续只复验这三项。预算复验 `out/verify/20261112-145145-8d69285d` 已1/1通过。长测复验 `out/verify/20261112-145512-db0b8ae3` 的空间支持域已在490.87秒通过，材质矩阵在445.60秒通过，长测复验2/2通过。预算夹具按96 B surface将30²/56²作为应成功尺寸，保持1 MiB额度、64²拒绝、外持owner失败及释放恢复断言。支持域长测改600秒；64候选×65536路径×8链的低roughness GGX矩阵改1200秒，采样数和数值门槛保持。temporal支持域已在237.68秒通过，同步留600秒执行上限。
- 无 VDB GPU 配置：严格引擎构建通过；测试目标初次被旧 `PathTextureCases.hpp` C4456 遮蔽警告阻断，局部循环变量改名后完整构建通过。`out/verify/20261112-145307-6a8ebc0d` 的 mesh Classic/四种 DI 信号 v2 与纹理回归2/2通过。

四种 DI 各4条独立 seed 链、每条128 spp/4×4完整图像为一个观测；解析 Lambert/environment 的红通道0.42，实测 initial/temporal/spatial/combined 分别0.418900/0.419810/0.418107/0.420770，RGB 绝对门槛0.025。初始/空间另对独立外露面；GGX 跨面/材质空间支持对独立 CPU 来源支持矩阵。固定射线2048 spp 比较 CPU 原生、独立三角化与 GPU 的 mean/second moment，覆盖 area、RR、双向混合命中和薄体素/mesh 互相遮挡。

Classic 与四种 DI、透视/正交均验证信号开关 raw/second moment 逐值相同、fresh 分量重组、signed cell/face/value 与 entity/asset/content_version、albedo/normal/roughness/metallic、主射线距离、motion 符号/单位/jitter；外持旧信号逐字节不变。四阶段注入提交失败、取消/释放、占用/材质/实例替换、v1/伪造来源和实际 GPU 预算失败均保留最后完整 epoch。

### 复现命令

使用既有配置，`VCPKG_ROOT=C:/code/third_lib/vcpkg`；GPU 设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`。CTest required 条目固定启用 sync validation，不接受 skip。最终命令如下（PowerShell）：

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_voxel_path_tests,dk_pathtracing_probe -TestRegex '^(dk\.voxels\.path_cpu\..*|dk\.pathtracing\.(voxel_restir_numeric|voxel_restir_reuse|voxel_signals|voxel_restir_state|voxel_numeric|voxel_lighting|voxel_state|signal_numeric|signal_geometry|signal_textures|signal_state|restir_materials|restir_textures|restir_temporal_numeric|restir_temporal_support|restir_spatial_numeric|restir_spatial_support|restir_spatial_budget|restir_medium_numeric|restir_medium_reuse|allocations)_gpu_required)$' -Reason 'M23.10 final acceptance and direct regressions' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(signal_numeric|texture_numeric)_gpu_required$' -Reason 'M23.10 no-VDB GPU compatibility' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.RESTIR (counter|light measures|merge|reservoir|CPU (initial oracle|rejects budgets|black background))' -Reason 'M23.10 no-VDB CPU compatibility' -Parallel 4
```

### 实际边界

仅表面 DI/信号生产与底层 API。体素 Scene media 仍明确拒绝，待 M23.11；Editor/Console/IPC/capture 路径装配在 M23.12；未新增滤波器、纹理体素材质或 voxel 面积灯。文档检查通过8份Markdown/733个本地链接、metadata/table/index/test entrypoints/JSON；`git diff --check`通过。未运行 Release、全量测试、长时 CPU 介质统计、全分辨率质量/性能基线；M23.13 单独承担联合交付。未改依赖基线，不引入 CUDA。为通过已有 `/WX` 严格配置，仅重命名旧纹理夹具中遮蔽坐标的循环变量；不改变纹理数据或断言。

## 修改记录

- 2026-11-12T14:15:00+08:00：冻结类型化 GPU surface v2、信号/source v2 与内容 owner 验证、预算及验收边界。

- 2026-11-12T15:11:34+08:00：完成类型化表面/信号v2、原生体素四种DI与失败保护；39项定向配置/测试组合完成验收，M23.10关闭。
