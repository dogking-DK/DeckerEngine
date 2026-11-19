# DeckerEngine 当前工作

## 最近完成

- 已修复 Sponza 转向时的介质步长倒退与 ReSTIR 权重下溢。开启稀疏云和大气，原生、640、192 三档复测 9 次全部完成，更新率中位数为 3.88、21.28、67.18 FPS。九张同机位运动帧的显示像素与修复前一致，原生 400 姿态和低太阳角 128 spp 也通过。[修复前后对照](spec/benchmarks/2026-11-19-media-motion-fix.md)

- Scene media 可在保留体积云和大气的同时使用首表面 ReSTIR v2，运动预览默认原尺寸，长边可在 Path tracing 面板调整。[0185 实现与验收](spec/development/0185-media-restir-preview.md)
- 路径追踪探针的 ReSTIR、介质与体素用例已独立编译。修改直接光 ReSTIR 用例时只重编对应对象，Release 增量构建中位数从 18.70 秒降到 11.43 秒。[测量与验收](spec/development/0184-build-iteration.md#release-探针增量构建)
- Windows 命令行开发可使用与 windows-dev 功能对齐的 Ninja 预设。VS 多目标定向验证共用一次 MSBuild，三目标无改动构建中位数从 2.14 秒降到 1.38 秒。[0184 验证记录](spec/development/0184-build-iteration.md)
- RTK 项目 hook 已验证自动精简。本工作副本已固定恢复库，根目录搜索、下一次调用从子目录取回折叠行均已通过。[项目导航 skill](.agents/skills/decker-code-navigation/SKILL.md#跨命令取回折叠结果)
- 开发工作台、精简 benchmark、离线复算样本与本地代码索引的实施和验证见 [0183](spec/development/0183-development-workflow.md)。
- 完整 RGB 表面直接光 ReSTIR v2 已接入 Editor 和 capture，Sponza 运动误差下降；原生尺寸更新率相对 v1 下降 4.1%。[0182 结果](spec/benchmarks/2026-11-17-restir-direct.md)
- Scene media 的 RGB 预览降低天空开口花斑，受光地面与间接光仍有噪声。[0181 结果](spec/benchmarks/2026-11-17-media-preview.md)

## 当前工作

Scene media 的首表面直接光和运动预览设置已接入 Editor。介质采样与后续反弹仍使用现有体积积分器。

## 下一步

- 降噪路线停在 M20.2 已验收。候选 M20.3 的目标是独立 À-trous 滤波选项，以及 Editor / capture 的最小接通。[降噪设计](spec/design/render-denoising.md)
- 粒子流体路线已完成设计。候选 M21.2 的目标是原生点绘制，为后续流体输出提供显示路径。[点绘制设计](spec/design/render-particles.md)

## 待决定

若继续当前渲染研究，候选问题包括介质散射噪声和动态几何重投影，目前尚未排期。[0185 记录](spec/development/0185-media-restir-preview.md)
