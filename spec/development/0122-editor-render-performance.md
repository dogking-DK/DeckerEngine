---
id: "0122"
created_at: "2026-10-16T09:49:00+08:00"
updated_at: "2026-10-16T10:30:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/render-pipeline.md
  - ../design/render-pathtracing.md
  - ../design/render-clouds.md
---

# 0122 Sponza 渲染性能与云预览

## 目标与设计依据

用户反馈Sponza大气/路径追踪慢，云启用后报错保留旧图。依据[编辑器](../design/editor.md)和[管线](../design/render-pipeline.md)设计，
测量实际默认场景，优化天空漫射、worker调度和云渐进发布，同时覆盖失败后的恢复。

## 实际变更

- [天空漫射](../../engine/render/pipeline/src/SkyDiffuse.hpp)：将每片段256次Sky采样移至每个Sky纹理代一次的129×65法线方向表；
  片段只做四点插值。仍使用原球面积分方向、太阳单位响应、法线/alpha/双面/地面语义；缓存保活原Sky及在途buffer。
- [空气透视](../../shaders/render/aerial.slang)：光学薄、相对密度尺度很短且远离太阳地平线的光程使用8段，其余128段和512段参考保持。
  保留近地面直接积分与行星遮挡，不能用跨地面滤波替代；补上1.8m相机高度和1/10/50m距离的独立CPU及GPU参考。
- [路径worker](../../engine/editor/src/PathPreview.cpp)：删除每tile固定1ms sleep，改为≤1ms GPU完成票据等待。
  Windows调度粒度原先将等待放大，1280×720的240个tile约耗3.76秒。新调度保持tile、spp、反弹数、随机样本和1GiB预算不变。
- [环境worker](../../engine/editor/src/EnvironmentPreview.cpp)：云先发布完整首帧，再每4次更新发布，到32次停止；只保留一个结果槽。
  [Viewport](../../engine/editor/src/Viewport.cpp)在首帧前合并视角输入，完成后请求最新视角，避免每次拖动都取消上一帧。
  较早视角保留真实camera/size身份，拾取和Gizmo仍要求当前图；不同document/revision/参数的图不能覆盖当前内容。
- 相机进入地下给出可操作错误，错误画面仍能导航/Home恢复；正常大视口禁用4096像素参考模式的选择，保留Temporal。
  Release/RelWithDebInfo普通启动默认关闭验证层，Debug仍if_available，显式--validation不变；验收继续required validation。
- [EditorDesktopRenderTest](../../tests/integration/EditorDesktopRenderTest.py)通过真实ImGui和同Console后端的IPC命令，
  覆盖默认云单独启用、两段连续拖动、sparse、云影支持域失败/保留旧图/命令恢复、地下相机/Home恢复和路径往返。
  云一致性驱动等待32次细化后再和capture逐字节对照；没有用首个低历史帧替代原质量验收。
- 新[worker探针](../../tests/integration/EditorRenderBenchmark.cpp)记录完整帧/首帧、逐spp耗时，并提供独立GPU timestamp/VMA诊断；
  抬头相机另输出clear/overcast，已查看实际云层图像。测试中瞬间active状态可能短于Windows轮询周期，生命周期测试改为接受已观察到的活动或完整完成状态。

## 验证记录

Windows/windows-editor/Release，NVIDIA GeForce RTX 4070 Laptop GPU / 596.49。没有升级依赖。
自动窗口均--background，使用项目副本；环境变量DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1只作用于验证子进程。

最终13个不同CTest名称通过，零跳过：6项Editor（worker、环境一致性、云一致性、Sponza worker、路径GUI、默认Sponza GUI），
4项日光（normals/direct/diffuse/failures），3项空气透视（numeric/composition/failures）。
required Vulkan同步validation无错误/警告，关闭后零live allocations；已有环境/云GUI与capture仍逐字节一致。
文档检查通过9份Markdown、219条本地链接及元数据/索引/测试入口/JSON；git diff --check通过。

证据均位于out/verify，实际命令、目标、提交、配置和测试名以各summary.json为准：

| 目录 | 实际结果 |
| --- | --- |
| 20261016-095321-b7aaec28 | 新探针MemorySystem结果类型/非默认快照构造编译失败；修正后重新构建，未使用旧程序验收。 |
| 20261016-095413-e0a3c5ab | 优化前Sponza worker baseline验证1/1通过。 |
| 20261016-095952-0f6a40ca | Submission不是bool，编译失败；改用有效票据value。 |
| 20261016-100106-afee9268 | 新worker和默认GUI通过；生命周期测试未观察到短暂active而失败，修正观察竞争。 |
| 20261016-100756-b58275f7 | 3/9通过；发现关闭天空漫射的占位buffer未初始化、近裁剪测试比值越界、测试拖动转到纯黑墙；分别修正。diffuse数值及aerial失败保护通过。 |
| 20261016-101107-16c7381f | 日光normals/direct/failures与aerial numeric/composition共5项通过；GUI将故意注入错误计为非预期，修正驱动期望计数。 |
| 20261016-101546-247bdd8a | 测试动作数组CTAD尺寸不一致编译失败；拆分声明后重建。 |
| 20261016-101657-4d51b741 | 最终6项Editor全部通过，包含11阶段真实窗口/IPC测试。 |
| 20261016-102023-c3b4b684 | 最终dk-editor-render-benchmark命名及含天空视点的worker回归1/1通过。 |

数值比较：新增近地短程CPU/512段GPU对照与原门槛通过；天空漫射对32768方向收敛参考通过，覆盖斜法线、极点和方位接缝。
固定Sponza三种环境优化前后RGBA8的RGB最大差为1色阶，RMSE为0.0286/0.0332/0.0346色阶（0–255范围）；没有改曝光或减少场景几何。

## 性能数据与复现

原始数据在out/benchmarks/sponza-0122，summary.json包含报告、程序及关键源码SHA256。
baseline为fb4fe64实现加测量探针，采用独立运行before-isolated；before-1/2探索运行发生重叠，明确排除。
优化后final-1/2/3为串行独立进程，GPU timestamp关闭、validation关闭。每种环境一次首次请求+三次小幅相机变化；
路径同为1280×720、4反弹、1spp/batch、64tile、seed=1，报告第2–6样本间隔；不以冷启动混入稳态。
下表旧值是单轮中位数，新值是三轮各自中位数的范围，不宣称旧版也有三轮；耗时包含worker/消费者观察和环境读回，非GPU耗时或GUI帧率。
观察器的1ms睡眠受Windows调度量化影响，样本较少且GPU时钟不锁定，不作为硬实时上限。

| 工作负载 | 优化前ms | 优化后三轮p50范围ms |
| --- | --- | --- |
| 大气完整新视角 | 64.612 | 30.175–45.039 |
| sparse云首个完整新视角 | 404.326 | 14.259–29.629 |
| overcast云首个完整新视角 | 402.940 | 20.413–30.187 |
| 路径追踪每个完整样本 | 3756.781 | 109.263–125.831 |

云完整32次细化仍约0.31–0.42秒，首帧提前显示不冒充收敛质量提速。冷启动大气含导入/编译约3.2–3.6秒，
首次带云约1.2–1.7秒到首帧；路径含加载/编译/构建约2.9–3.5秒到首样本。未消除首次初始化成本。
新增抬头视角的sky-overcast完成32次更新约0.48–0.62秒，确认正常Sponza资产下确有云层像素，而非只检查命令成功。

真实编辑器另跑三轮--performance（validation关闭），使用正常736×405视口和2秒连续右键往复拖动：
大气实际图像更新30.0–31.5次/秒，云28.5–29.5次/秒；GUI呈现165.0–165.5次/秒，两种计数分开，不把GUI刷新当渲染帧率。
全部11阶段成功，原始数据在gui/desktop-render-122643c01d7b40e3b195eca9faac0245、
desktop-render-ad9781f78afb473da55978fdec406552、desktop-render-60be8d859f7d48efbf905af5efd179fc。
两次沙箱内尝试分别被临时文件权限和本机管道ACL拒绝，失败报告保留且不计入性能轮次；在授权的正常本机权限下完成测试。

profile为单独诊断进程，不混入上述计时：大气帧GPU约2.75–2.77ms（AP体积约1.59ms、表面约0.20ms），
庭院云约2.8–3.0ms，抬头云约6.69ms；只包含该Frame submission，不包含前置LUT/场/云影prepare，子Pass不能再与父区间相加。
完成安全点VMA device-local allocation约109MiB（大气）/134MiB（云），路径总device allocation约554MiB；CPU heap backing峰值约360MiB。
这是分配安全点采样，不是系统总显存或精确在途峰值；两个VMA口径不混加。

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_editor_app','dk_editor_render_benchmark','dk_ctl','dk_environment_preview_tests','dk_run') `
  -TestRegex '^dk\.editor\.(desktop_render|render_worker|environment_worker|cloud_consistency|environment_consistency|pathtracing)_gpu_validation$' `
  -Reason 'Sponza交互、云恢复与原截图/路径一致性'
& ./out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/benchmarks/sponza-rerun
& ./out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/benchmarks/sponza-profile --profile
```

## 范围与限制

用户现场未提供原始错误全文；已通过实际Editor命令覆盖默认启用，并复现参考尺寸、云影支持域和地下相机三种拒绝路径。
保留物理/API支持域检查，改进可选择设置、错误信息和恢复操作，不将所有未知现场错误归为同一原因。
未运行全量、其他配置或跨设备测试；更新的是用户所用Release程序。没有新增去噪、完整PBR纹理、体积路径追踪或扩大1GiB额度。
既有spec/README.md与ai-documentation-workflow.md修改不属于此任务，不纳入提交。

## 修改记录

- 2026-10-16T09:49:00+08:00：创建设计与测量计划。
- 2026-10-16T10:30:00+08:00：完成实现、13项独立定向检查、三轮worker/GUI复测及GPU/显存诊断。
