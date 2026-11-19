# DeckerEngine 当前工作

## 最近完成

- Windows 命令行开发可使用与 windows-dev 功能对齐的 Ninja 预设。Scene 的头文件重编与两轮耗时见 [0184](spec/development/0184-build-iteration.md)。
- RTK 项目 hook 已验证自动精简。本工作副本已固定恢复库，根目录搜索、下一次调用从子目录取回折叠行均已通过。[项目导航 skill](.agents/skills/decker-code-navigation/SKILL.md#跨命令取回折叠结果)
- 开发工作台、精简 benchmark、离线复算样本与本地代码索引的实施和验证见 [0183](spec/development/0183-development-workflow.md)。
- 完整 RGB 表面直接光 ReSTIR v2 已接入 Editor 和 capture，Sponza 运动误差下降；原生尺寸更新率相对 v1 下降 4.1%。[0182 结果](spec/benchmarks/2026-11-17-restir-direct.md)
- Scene media 的 RGB 预览降低天空开口花斑，受光地面与间接光仍有噪声。[0181 结果](spec/benchmarks/2026-11-17-media-preview.md)

## 当前工作

正在优化 VS 多目标定向验证，比较一次调度多个目标与原入口的耗时。

## 下一步

- 降噪路线停在 M20.2 已验收。候选 M20.3 的目标是独立 À-trous 滤波选项，以及 Editor / capture 的最小接通。[降噪设计](spec/design/render-denoising.md)
- 粒子流体路线已完成设计。候选 M21.2 的目标是原生点绘制，为后续流体输出提供显示路径。[点绘制设计](spec/design/render-particles.md)
- 路径介质路线有一项独立诊断候选。复现并定位既有 128 spp 数值域拒绝，验收时与已通过的 20 spp 吞吐测量分别记录。[0180 记录](spec/development/0180-path-medium-majorants.md#发现及处理)

## 待决定

选择下一条开发路线。若继续当前渲染研究，0182 的后续问题是介质散射噪声、动态几何重投影及更广的运动样本；目前尚未排期。[0182 边界](spec/development/0182-restir-surface-direct.md#遗留问题与下一步)
