# DeckerEngine 当前工作

## 最近完成

- Sponza 开启稀疏体积云和大气后，Scene media + ReSTIR v2 在 RTX 4070 Laptop 上的实际新视角更新率为原生 3.81、640 长边 19.61、192 长边 60.62 帧/秒。12 次性能尝试中有 3 次 GPU 路径错误，同设置重跑完成。[数据、同机位画质和复测方法](spec/benchmarks/2026-11-19-media-resolution.md)

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
- 路径介质路线有一项独立诊断候选。复现并定位既有 128 spp 数值域拒绝，验收时与已通过的 20 spp 吞吐测量分别记录。[0180 记录](spec/development/0180-path-medium-majorants.md#发现及处理)

## 待决定

若继续当前渲染研究，候选问题包括介质散射噪声、动态几何重投影和不同运动预览分辨率的性能测量，目前尚未排期。[0185 记录](spec/development/0185-media-restir-preview.md)
