---
id: "0159"
created_at: "2026-11-11T14:51:58+08:00"
updated_at: "2026-11-11T14:58:14+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/architecture.md
---

# 0159 M23.1 体素渲染正式计划

## 目标与设计依据

将本会话中的七阶段体素渲染方案纳入实际开发计划，依据[体素设计](../design/render-voxels.md)
与[架构](../design/architecture.md)维护正式 Roadmap、模块边界和独立验收条件。
本次仅设计落档；accepted 表示采用设计，completed 仅用于本设计阶段，不能表示体素功能已经实现。

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
