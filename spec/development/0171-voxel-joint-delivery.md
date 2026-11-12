---
id: "0171"
created_at: "2026-11-12T17:06:00+08:00"
updated_at: "2026-11-12T18:29:18+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/render-pathtracing.md
---

# 0171 M23.13 稀疏体素联合交付

## 目标与契约

实现 [M23.13](../roadmap.md#m23实体体素渲染) 与[联合交付契约](../design/render-voxels.md#m2313-联合交付实施契约)：同输入质量/资源/性能，RT 块尺度评估，独立统计 oracle，Slang 双阶段及 CPU-only/非 RT/RT 证据。沿用 spec-workflow、build-verify 和 state-contracts；默认布局和资产协议保持。

## 实施与验收

已实现：默认保持 8 的显式 32/128 保守块合并、只读 NanoVDB/块/palette/AABB/AS/SBT 统计、复用 M23.7 输入与光栅 oracle 的联合基准，以及严格三轮聚合/源码与二进制来源记录。

已获得的证据：

- `20261112-171323-af70faf7`：两项新增 CPU 块策略/不可变状态测试通过；GPU 首轮因基准 CPU 参考未传入 pixel 维度失败。
- `20261112-172335-2f921baa`：无 Vulkan/CUDA 的 CPU-only 配置 10 项稀疏/稠密 oracle、转换、并发和拒绝回归全部通过。
- `20261112-172436-4e360d15`：required GPU 联合门禁与聚合 2 项通过（271.29 s / 0.37 s）；6 输入×2 投影×3 布局，全图布局差异为 0，8 seed Lambert/GGX 独立参考最大均值差 6.4487e-5，资源完全回收。
- 迭代修正的基准问题还包括：光栅实例补全匹配 Scene 的 entity/asset；内部起点按 compute 几何外法线、PathTraceHit 朝向法线分别比较；缓存命中的 AS 不重复记录旧 BUILD GPU 时间。没有放宽像素、统计、生命周期门槛。

- 初轮正式 Release 在 sparse64 / 1280×720 / 11 spp 发现原块入口重定位引起布局图像差异。尝试统一原点但保留块裁剪后，solid256 仍暴露表面遗漏；一 ULP t 区间放宽和网格入口重定位也未通过。全部候选退出正式统计，未放宽数值/图像阈值。
- 最终统一网格 HDDA 前缀 + 严格 cell 归属 + 保守 AABB；相同浮点 t 允许跨整数格面，仍受区域预算。`canonical-prefix.json` 完整 12 输入、4 视图、3 布局图像差异 0，回收/诊断通过。代价是可能重复遍历前缀，保留实际性能数据，不宣称块内裁剪优化成功。
- required 联合质量增加 sparse128/sparse256/solid256，以及这四个稀疏/实心输入的两分辨率 11 spp 长采样回归；共享 HDDA 影响的 Classic、四 DI、信号、Scene media/光栅重新验证通过。
- `20261112-180549-cc22a872` 未设置进程级 AMD implicit layer 禁用，收到系统层 API 版本警告，按零警告门槛失败并中止；未作为通过证据。后续保持 required Khronos/sync，仅禁用该无关层复验。

- `20261112-181030-e0dc2a28`：最终实现 19 项全部通过，4 CPU Slang/SPIR-V 与 15 required GPU，包含 Classic、四 DI、信号、Scene media、光栅/HDDA/intersection 和后台 RT 禁用入口，222.17 s。
- `20261112-173345-2b25574c`：无 VDB 配置旧 mesh analytic/progressive state 两项 required GPU 通过；严格编译通过。

- `20261112-181502-aafc720d`：最终 Release 构建与聚合 CTest 通过（内含 5 项缺证据/混源/统计/资源/慢样本保留单元检查）。
- 同源码/同 Release 程序的 enhanced required quality（9 输入 + 24 长采样视图）及 3 轮正式矩阵通过：12 输入×4 视图×3 布局，完整图像差异 0，逐 seed 均值最大差 6.4487e-5，零 validation 错误/警告，资源完全回收。
- 保留默认 8。32 在 sparse256 的候选空区导致明显退化；128 有特定输入收益，但无稳定全面优势。完整协议、来源、压缩原始数据、CPU/GPU/AS/在途统计、可复算图与能力矩阵见[联合报告](../benchmarks/2026-11-12-voxel-joint.md)。

## 交付边界

M23.1–13 已全部完成，关闭 M23 父阶段。没有物理非 RT 卡、跨厂商/跨平台或完整引擎回归认证；RT 禁用使用同一真实 GPU 的显式禁用路径。
本次复验 11 项体素路径 GPU 专项（Classic 3、ReSTIR/信号 4、Scene media 4）；历史 CPU 长测及完整 Editor/capture 证据沿用 M23.10–12，未重复声称执行。
不包含 VDB 密度/SDF、透明/发光体素、流式世界、编辑笔刷、raster sequence/介质或 M20 降噪滤波器。浮点/地址/预算拒绝、转换峰值采样下界与固定顺序/未锁频限制见报告。

## 最终核对

- `scripts/check-spec.ps1 -Path` 定向检查 11 个 Markdown、1130 个本地链接、元数据/表格/索引/测试入口与 JSON manifests 通过。
- 由归档 gzip 执行 `scripts/benchmark-voxel-joint.py --aggregate ... --plot` 通过；再生 summary JSON/Markdown 按仓库 LF 归一后与交付文件逐字节一致，SVG/PNG 已检查可读性。
- 全部归档 SHA-256、514 个实测源码文件及 Release 程序 SHA-256 核对通过；数据压缩无损。纯文本归档遵循仓库 LF；源码摘要保留实测工作区字节（含换行），压缩原始 JSON 未改写。原始样本和失败候选诊断分别归档，不混入最终统计。
- `git diff --check` 通过；本阶段使用独立详细本地提交。
