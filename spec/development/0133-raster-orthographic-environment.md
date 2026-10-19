---
id: "0133"
created_at: "2026-10-19T12:21:27+08:00"
updated_at: "2026-10-19T12:40:31+08:00"
status: completed
design_refs:
  - ../design/render-pipeline.md
  - ../design/render-atmosphere.md
  - ../design/render-clouds.md
  - ../design/render-capture.md
  - ../design/editor-interaction.md
  - ../design/editor.md
---

# 0133 M18.5 光栅大气与云正交

## 目标与设计依据

按[管线](../design/render-pipeline.md)、[大气](../design/render-atmosphere.md)和[云](../design/render-clouds.md)设计，
实现每像素近面起点、平行方向、线性深度、AP/天空/云和历史重投影；通过独立数值参考后开放 GUI/capture。
保留原视图/作业候选发布点，失败不改 Scene、旧完成图、历史源或已存在输出文件。

## 实现与验证进度

- 已先明确支持域与独立 CPU 参考。PipelinePolicy 分别校验有限透视/居中正交；AerialParameters 加入近面中心位移，三条大气/云 shader 共享 RasterCamera 与 CloudCamera。
- 正交 AP 的每个 XY 体素具有独立起点，深度按 far-near 映射；正交天空直接128段积分并查 T/MS，诊断512段。径向高度用有理化平方差，近面跨行星内部像素输出黑。
- 云纹理相位、球壳和太阳角均随起点移动；历史代表点包含近面位移，旧正交深度按旧前向投影减 near 检查。投影/高度变化沿用历史重置，世界云影不改缓存键。
- Editor 开放环境正交与 Raster/Path/Unlit 切换，保留镜头域候选验证；Capture 移除旧类型拒绝，域错误仍沿 worker 失败保护，schema 不变。
- 新增正交 AP/云独立 CPU 图像、near 光程、静态/移动/反遮挡历史、世界云影、GUI/capture 与固定序列重放。
- 计划定向验证 CPU 投影/相机/capture，required GPU 大气/云数值、时域/云影及真实后台 GUI/capture。
- 首轮 `out/verify/20261019-122419-4cb4f14e`：Release 三个目标、11项全部通过（9个管线CPU、正交大气GPU、正交云GPU），无跳过。
- 第二轮 `out/verify/20261019-122739-d27f6dba`：32项中29通过、3失败。新增正交世界云影与两个后台环境 GUI/capture 全部通过；原透视大气、云数值/历史/寿命及介质路径 GUI 通过。
- 三处关联回归：旧 composition 用例显式路径无云影，共用入口默认有云影且接收面在云层上方；修正用例为明确相同开关。旧 capture 用例错误限定 quality 枚举仅4种，现需允许已存在的路径枚举。Python render_cases 尚无 projection/ortho_height 来源规范化，导致旧透视 sequence 报告也被拒绝；已同步有效相机字段并新增来源篡改/含糊输入回归。
- 第三轮 `out/verify/20261019-123310-335f5ef6`：6项全部通过，包含上面三项复验、100km正交跨度、近面行星内云像素和 Python 用例校验。
- 最终审查增加旧近面反遮挡回归。`123550-3049bb93` 的长云壳先被距离门槛拒绝，不能单独证明近面检查；改短云壳后 `123652-87633e16` 真实复现25个半尺寸像素中16个错误接受旧历史。
- 已增加代表点位于旧正交近面前的拒绝，以及几何负深度不冒充背景的保护；`123743-bd5f53a3` 的正交、透视时域、固定序列和云 GUI 4项全部通过。

## 验证范围与支持限制

所有构建/CTest 使用 scripts/verify.ps1，现有 out/build/windows-editor / Release；完整 targets、regex、所选名称和结果见各轮 summary.json。
GPU 为 RTX 4070 Laptop GPU / 596.49，要求真实 Vulkan validation/synchronization，GUI 全部 --background。
设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 排除无关系统切换层告警，没有放宽实际验证断言。

- 正交 AP 与 GPU reference 图像及独立 CPU 世界射线对照，覆盖低空/高空/100km可见跨度、旋转/平移、行星遮挡；RGB T最大绝对误差 0.000088064，阈值0.002。
- 云下/内/上三张独立 CPU 图像的云 T 最大误差0.000144486、混合 T 0.000123208；reference/optimized 与原5%/12%/0.02阈值比较通过。静态32预热+120帧、风/相机连续移动、alpha-mask反遮挡与失败旧图保护通过。
- 真实环境和云 GUI 各6个 capture 对照点逐字节相同，新增正交/改高度/返回透视均为736×240；云端32次预热，投影变化时固定世界云影代号保持。Path trace→Raster→Path trace 正交切换也通过原介质GUI流程。
- 当前 runner 实际 commands.list/commands.describe 核验 render.capture/sequence effect、schema与来源；正交reference捕获/序列像素一致，temporal孤立重放一致，高度与投影切换重置历史，超域失败保留旧JSON，Python/Luau现有示例通过。
- 未执行全量、第二配置或性能基准。本阶段不声称改善既有大气/云性能目标；正交天空按像素积分，表面天空漫射仍保留已记录的眼点闭合近似。

支持有限刚体 forward-Z 居中正交，near>0、far<=1000000m、可见宽高各<=100000m；沿用大气/云各自参数支持域和固定云影覆盖限制。
正交介质从近面开始、far只裁剪几何；光栅眼点在地面以上，行星内近面像素不透光。无新依赖，无场景格式或命令 schema 变更。

去重取每项最新结果，共44项通过、0失败、0跳过；聚合见 out/m18.5-verification.json。定向文档检查与 git diff --check 通过。

## 遗留与交付

本阶段无未完成项。M18.1–5 全部完成；后续里程碑另行确定。用户已有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 修改保持原样，不纳入提交。

## 修改记录

- 2026-10-19T12:40:31+08:00：完成定向实现、数值/GUI/命令复验、历史边界修复与设计/Roadmap同步。
