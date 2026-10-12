---
id: "0093"
created_at: "2026-10-12T14:48:00+08:00"
updated_at: "2026-10-12T14:48:32+08:00"
status: completed
design_refs:
  - ../design/render-atmosphere.md
  - ../design/render-clouds.md
  - ../design/architecture.md
  - ../design/render-pipeline.md
---

# 0093 大气渲染与体积云开发任务规划

## 目标与设计依据

按用户要求设计大气渲染和体积云的开发任务计划。基于代码提交`744e737`，
新增[大气草案](../design/render-atmosphere.md)与[体积云草案](../design/render-clouds.md)，
将后续工作纳入[Roadmap](../roadmap.md)。本记录完成仅代表规划交付，M14/M15仍未开始实现。

## 实际变更

- Roadmap新增M14.1–7、M15.1–7，共14个有前置和独立验收条件的小阶段；下一项为M14.1。
  保留M0–M13验收结果，区分未来交付节点与当前能力。
- 大气草案选择Hillaire分层LUT方向，规定CPU环境状态、单位/坐标、场景格式迁移、
  太阳漫反射、天空/空气透视、缓存提交与失败保护；参考来源链接保存在设计中。
- 体积云草案参考Nubis，先建立无历史高采样参考，再推进合成、半分辨率/时域重建和云影；
  明确同一射线上空气/云的区间组合、每视图历史、显式时间/seed及资源额度。
- 架构和管线设计新增规划入口；设计索引以draft登记新模块，开发索引记录本次规划。
  未创建任何新CMake target、源文件、shader、命令、场景字段或测试入口，未调整三方依赖。

基线核对发现：当前ImageDesc/创建/视图限定2D，场景深度没有sampled usage，opaque仍为unlit；
SceneSnapshot没有环境状态，ScenePipeline没有稳定ViewId或时域历史。
因此3D资源/深度采样安排在M14.2，最小日光接收安排在M14.4，不能将云影当作独立贴图功能验收。
现有M13实验格式绑定XPBD/cloth，渲染用例清单另行版本化，不声称已有通用云实验执行器。

## 验证记录

- 已只读核对ScenePipeline、资源描述/创建/视图、RenderView、SceneSnapshot、Graph与capture设计、
  Python实验格式，确认上述缺口及依赖方向。
- 已查阅作者项目/论文索引、Epic官方说明、Bruneton参考文档及Nubis原始课程/作者资料。
  没有移植外部源码或素材，也没有以演示性能作为本机结果。
- 文档检查通过：8份Markdown、562个本地链接，元数据、表格、索引、测试入口及JSON清单校验通过。
  `git diff --check`通过；未把现有用户文档改动纳入本次提交。
- 未运行引擎构建、CTest、GPU图像和性能采样：本次只修改文档；草案中的质量/预算是待验证候选目标。

实际文档检查命令（仓库根目录，PowerShell）：

```powershell
& ./scripts/check-spec.ps1 -Path @(
  'spec/design/render-atmosphere.md', 'spec/design/render-clouds.md',
  'spec/design/architecture.md', 'spec/design/render-pipeline.md',
  'spec/design/README.md', 'spec/roadmap.md',
  'spec/development/README.md', 'spec/development/0093-atmosphere-cloud-plan.md'
)
git diff --check
```

## 偏差与决策

默认范围为Windows/Vulkan、地球尺度地表至高空、单太阳/单云层、普通场景视口与离屏截图。
没有人员/日历预算，因此不给工期；完整PBR/IBL、几何阴影、天气流体和布料专用视口环境接入另行规划。
候选画质阈值先由参考路径确认，性能由实际GPU/驱动/构建和固定序列测量后冻结；
记录原始数据、动态误差、冷启动/稳态和失败，不能事后悄悄放宽以获得通过。
设计草案仅定边界与研究方向；每阶段开工时细化实际接口和测试，过大阶段继续拆分。

## 遗留问题与下一步

规划完成后从M14.1开始：确定CPU环境类型、米制/RGB能量标度和参数约束，设计场景格式升级，
经统一命令实现事务/撤销/保存，并验证CPU-only隔离。后续实现须另建相应开发记录。
工作区已有的spec/README.md修改和spec/guides/ai-documentation-workflow.md不属于本次任务，不纳入提交。

## 修改记录

- 2026-10-12T14:48:00+08:00：创建规划记录，完成两份草案、路线与索引整理；等待文档检查。
- 2026-10-12T14:48:32+08:00：文档与差异检查通过，规划交付完成；M14/M15实现状态仍为未开始。
