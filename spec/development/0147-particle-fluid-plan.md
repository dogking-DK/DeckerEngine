---
id: "0147"
created_at: "2026-10-23T18:16:00+08:00"
updated_at: "2026-10-23T18:27:00+08:00"
status: completed
design_refs:
  - ../design/physics-fluids.md
  - ../design/render-particles.md
  - ../design/physics-api.md
  - ../design/render-simulation.md
  - ../design/graphics-device.md
  - ../design/graphics-vulkan.md
  - ../design/graphics-shaders.md
  - ../design/architecture.md
---

# 0147 M21.1 原生点绘制与粒子流体设计落档

## 目标与设计依据

落实用户要求的原生点绘制、SPH/PBF CPU/GPU 双端计划，并提交本地。
此前 [调查](../research/2026-10-23-water-simulation.md) 与 [首期计划](../research/2026-10-23-sph-pbf-plan.md)
基于 `0c4df00`；本轮基于 `ebf89f3` 复核文档入口和已有阶段，将正式契约归入
[粒子流体](../design/physics-fluids.md)、[点云可视化](../design/render-particles.md) 与相关底座设计。
本轮仅做设计/计划落档，不实现功能；accepted 表示采用方案，completed 只表示本设计阶段完成。

## 实际变更

- Roadmap 新增 M21.1–9、依赖及独立验收；现有 M20 降噪编号与进度保持各自路线。
- 新建设计明确 CPU/GPU 数据边界、稳定 ID/SI/通道、动态邻域、WCSPH 子步、PBF Jacobi/邻域刷新，
  以及原生 point-list、largePoints、Slang 内建、深度/色标和 CPU/GPU 输出消费方式。
- 更新 Device/Vulkan/Shaders、Physics API、布片可视化和架构的 M21 扩展边界；
  保留旧 XPBD 路径，GPU 状态区分提交/完成/健康，旧代输出和图像有拥有型寿命。
- 同步设计/开发/总索引，将研究文档标为背景依据，正式状态指向 Roadmap，避免两份进度清单漂移。
- 统一 CPU-only 数值导出、四组合服务/GUI/脚本验收和版本化数据方案；
  MAC、场 ray marching、USD/VDB、检查点和真实水面后置。未新增依赖、命令或 CMake target。

## 验证记录

- `scripts/check-spec.ps1 -Path` 定向检查本次 15 份 Markdown：通过，945 个本地链接，
  以及全局元数据、表格、索引、测试入口与 JSON manifests 一致性通过。
  范围包括总索引/Roadmap、设计索引/八份模块设计、开发索引/本记录和两份研究文档。
- `git diff --check`：已跟踪差异通过。人工核对 M20 原有进度、M21.1–9 依赖、研究到正式设计链接，
  明确区分当前实现、拟新增接口和后续数值/设备验证；变更仅在 `spec/`。
- `git diff --cached --check`：包含新增文档的暂存差异通过；按暂存文件清单再次运行定向文档检查，
  仍为 15 份 Markdown / 945 个本地链接通过，暂存范围未包含代码或其他任务文件。

本轮无 C++、shader、CMake 或运行时协议变更，不运行引擎构建、CPU/GPU 数值测试或 GUI 测试；
上述功能验证均是 M21 后续验收条件，不能将文档检查当作功能通过。

## 偏差与决策

原计划 P0–P8 映射到 M21.1–9；M20 已由降噪路线使用，因此不复用该编号。
M21.1 只完成边界、方法和验收设计；实际核/边界离散参数、数值参考/容差在 CPU 阶段固定，
不能声称本次已经建立可执行数值基线。Slang 固定版本的原生点语义仍需 M21.2 实际验证。
不把 OpenUSD 作为首期依赖，也不删除其作为后续统一交换入口的调查结论。

## 遗留问题与下一步

流体路线下一步 M21.2：设备 feature/限制、point-list、PointSize/PointCoord 与真实离屏点绘制验收。
M21.3 再建立公共粒子输出、通用 renderer 和服务有类型分支；尚未实现的组合不进入命令能力发现。
本轮只要求本地提交，不推送远端。

## 修改记录

- 2026-10-23T18:16:00+08:00：建立 M21 正式设计、阶段和落档记录；验证待执行。
- 2026-10-23T18:27:00+08:00：15 份文档检查通过，M21.1 设计阶段完成；M21.2–9 保持待开始。
