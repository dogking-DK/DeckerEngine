---
id: "0108"
created_at: "2026-10-14T08:20:00+08:00"
updated_at: "2026-10-14T08:22:56+08:00"
status: completed
design_refs:
  - ../design/graphics-raytracing.md
  - ../design/render-pathtracing.md
  - ../design/architecture.md
  - ../design/render-pipeline.md
---

# 0108 渐进式路径追踪任务规划

## 目标与设计依据

响应“设计光线追踪管线任务”；用户明确选择“渐进式路径追踪：先做正确性参考，再接入编辑器与截图”。
代码基线为 `fc7238a`，当前M0–M15质量与基线已交付；本次只交付任务设计，不实现renderer。
规划完成不作为M16功能验收证据；默认下一项为M16.1。

## 实际变更

- 新增[光追底座草案](../design/graphics-raytracing.md)：能力协商、BDA/AS、Slang阶段/反射、
  RT pipeline/SBT、Graph依赖/同步、提交保活与失败契约，扩展现有Graphics模块。
- 新增[管线草案](../design/render-pathtracing.md)：CPU参考、积分/材质/光源范围、随机样本、
  不可变场景与每视图历史、工作/显存额度、后台交互、精确spp捕获和质量/性能协议。
- [Roadmap](../roadmap.md)新增M16.1–11，材质拆为M16.7.1/2，共12个独立验收任务；
  明确前置、交付节点F和下一项。全部实现状态为未开始。
- 架构/现有管线增加规划入口，设计索引登记draft，开发索引登记本次记录。
  未创建target、源文件、shader、命令、场景字段或测试；未调整vcpkg依赖。

## 基线核对与关键决策

当前Device未协商RT/BDA，shader产物只有vertex/fragment/compute，Graph只管理buffer/image，
GpuAssets只分配vertex/index/transfer用途，CPU BVH也没有材质alpha遍历。
因此先显式补齐底座，不能把Vulkan 1.4、依赖头文件或云ray marching当作已经实现光追。
现有材质确有metallic/roughness，默认metallic=1；Lambert参考显式选材质，后续再接GGX子集。

选择KHR RT pipeline与SBT、raygen迭代和完整BUILD，不同时维护ray-query/wavefront/去噪后端。
首轮固定常量环境、显式方向太阳和发光三角形；M14/M15视点相关介质不直接用于任意反弹，
启用介质的场景必须显式选择表面模式并记录边界。无GPU支持时不静默回退。
样本数只在GPU完成且代次仍匹配后推进；文件在owner最终原子替换时发布，两个提交点分别验收。
现有PPM与有界线性报告足够本轮验证，不新增EXR/降噪依赖，不虚构实时性能或开发工期。

## 验证记录

- 已只读核对Device、ShaderArtifact、CpuData、GpuAssets、Geometry和相关设计/最近记录，确认边界与缺口。
- 已查阅Khronos Vulkan规范/指南、Slang官方文档、VMA官方说明和PBRT原书；引用位于两份设计的对应主题。
  未移植外部源码/素材，没有以公开演示作为本机能力或性能证据。
- 文档初检与补充后的复验通过：8份Markdown、638个本地链接，元数据/表格/索引/测试入口/JSON清单均通过；
  `git diff --check`通过。复验覆盖scratch同步/初始化边界、微表面依据与最终索引状态。
- 未运行引擎构建、CTest、GPU探测/图像或性能采样：本次为纯文档规划，按decker-build-verify仅执行文档检查。

本次文档检查入口（仓库根目录PowerShell）：

```powershell
& ./scripts/check-spec.ps1 -Path @(
  'spec/design/graphics-raytracing.md', 'spec/design/render-pathtracing.md',
  'spec/design/architecture.md', 'spec/design/render-pipeline.md',
  'spec/design/README.md', 'spec/roadmap.md',
  'spec/development/README.md', 'spec/development/0108-pathtracing-plan.md'
)
git diff --check
```

## 遗留问题与下一步

M16.1实施时冻结CPU模型、随机维度、具体相机/反弹约定、解析夹具和误差/工作预算；
M16.2真实探测硬件/驱动能力，后续逐阶段细化草案与独立记录/提交。草案质量和额度均为待验证候选。
现有 `spec/README.md` 修改及 `spec/guides/ai-documentation-workflow.md` 属用户工作区内容，不纳入本次提交。

## 修改记录

- 2026-10-14T08:20:00+08:00：创建两份草案及12项任务，完成相关入口和索引；等待文档验证。
- 2026-10-14T08:22:56+08:00：初检通过，补充Graph scratch特殊内容语义与材质来源；规划交付完成，M16实现仍全部未开始。
