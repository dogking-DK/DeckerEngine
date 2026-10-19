# M17.6 介质路径测试与测量

入口是 `dk-pathmedia-benchmark` 与 `scripts/benchmark-path-media.py`。需要现有 Release、RT/非一致纹理索引设备及 demo Sponza 资产；没有适用设备应记录未运行。
测试选择见 [选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)，算法与预算见 [设计](../design/render-pathtracing.md#m176-测量与优化实施契约)。

## 固定输入与门槛

性能固定640×360、4个逐批1 spp、tile64；每场景12个96×54新相机首完整显示及在途取消。CPU场景/AS准备与冷编译单列，不并入热帧。
真实worker补测192×108的完整新相机显示、环境变更、取消至排空；后台GUI持续拖动2秒，检查低尺寸期间持续产生新来源，停动恢复原尺寸。
临时预览与完整图使用相同介质模型，尺寸改变重置历史；不能用预览时间证明640×360或生产尺寸达到固定质量。

0142 的表面预览调度对照允许显式 `--tile 256`；历史 M17/M19.8 固定基准仍用64，不能混合聚合。
例如 `dk-pathmedia-benchmark --root projects/demo --case surface --algorithm initial --tile 256 --full-preview --output out/preview/initial.json`。
默认场景实际 worker 入口为 `dk-editor-render-benchmark projects/demo out/preview/desktop --desktop-path`，
在1600×1200和2048²下依次切换 Classic/initial/temporal/spatial，记录连续采样、显存和取消恢复。
加 `--validation` 启用 required validation；对应 CTest 为 `dk.editor.restir_desktop_gpu_validation`，需要 demo 素材和足够的设备额度。
该 worker 使用真实默认相机与 Editor 预算/调度，但没有窗口合成，不作为 GUI 帧率；有验证层的结果不作为关闭验证层的性能数据。

冻结4反弹/RR3、model4/sampler2、时间17、曝光0、种子101+7919*i（i=0..7）；surface是model3/sampler1的独立开销对照。
负载涵盖庭院、天空、clear/sparse/overcast、低太阳、雾、云内、云上方。原始JSON保存环境、相机、几何hash和字段hash。
优化前后另存32×18/4 spp线性RGB及二阶矩，绝对差不得超过1e-6。

质量使用四个小场景CPU N=16384/4N=65536参考，8 seed均值N/4N误差≤0.5%，GPU偏差≤2%并处于5联合标准误差+1e-4内。
Sponza采用4×4的独立8 seed GPU N/4N参考；参考未达0.5%时不能认证质量耗时。候选64/256/1024/4096/16384 spp的跨seed线性nRMSE目标为2%，另报上/下半ROI。
达到时间是小图实际渐进测量，包含先前档位的采样；质量运行启用验证层，该时间不混入关闭验证的性能基线。未达填null，不估算后续样本数或全尺寸耗时。

## 执行

先构建目标，再运行；不得拿旧二进制覆盖构建失败。以下为优化版三轮与质量采集，两个命令串行执行：

```powershell
python scripts/benchmark-path-media.py --directory out/benchmarks/media --executable out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --shaders engine/render/pathtracing/shaders --collect optimized
python scripts/benchmark-path-media.py --directory out/benchmarks/media --executable out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --shaders engine/render/pathtracing/shaders --collect quality
```

原版基线用相同探针、冻结二进制与shader目录执行 `--collect baseline`；优化版的唯一探针差异是启用已验证的CPU重绑。
不要在同一GPU并发跑其他probe/Editor/capture；驱动、设备、资产、shader和二进制SHA保存在manifest。已知AMD隐式层冲突只在测量子shell临时处理，详见[构建指南](build.md)。

```powershell
python scripts/benchmark-path-media.py --directory out/benchmarks/media --summarize
python scripts/benchmark-path-media.py --directory out/benchmarks/media --archive spec/benchmarks/2026-10-16-path-media
python scripts/benchmark-path-media.py --directory spec/benchmarks/2026-10-16-path-media --verify-archive
```

归档包含全部原始逐tile时间、线性/二阶矩、参考和输入摘要；校验重算门槛与三轮汇总，不把归档回放当作本机GPU验证。
`dk.editor.media_worker_gpu_validation` 检查worker来源/取消/资源释放，`dk.editor.desktop_render_gpu_validation` 包含真实Sponza空气/云路径持续拖动及完整尺寸恢复。
性能worker可运行 `dk-editor-render-benchmark.exe projects/demo out/benchmarks/media-worker --media --profile`；须串行独立进程三轮。自动Editor始终使用 `--background`。

## 边界

本轮不更改majorant、随机维度、散射模型、spp/反弹，也不加入去噪或wavefront。每路径工作硬界保持；每路径结束合并tile计数，任何超额拒绝整个候选，已完成历史继续有效。
总额度是成功结果的工作上限，超额时并发路径可能已多执行部分步骤，仍受逐路径硬界约束。
输入指纹v3用于CPU几何复用，刷新资产必须重新导入/构建。Capture仍保持小场景额度，Sponza视口能力不等于Sponza离屏捕获能力。

本机阶段结果与未达候选见[2026-10-16实测](../benchmarks/2026-10-16-path-media.md)。归档如包含editor.json，还校验三轮worker与真实GUI的100ms候选及连续运动/完整尺寸恢复。

## M19.8 ReSTIR 四模式 Sponza 验收

使用同一 `dk-pathmedia-benchmark` 与独立 `scripts/benchmark-restir-sponza.py`，比较 classic / initial / temporal / spatial。
本入口的性能与热相机均为 640×360；质量仍为 4×4、8 条独立历史链和五档 spp，不能外推全尺寸质量。
质量显式关闭 validation 并逐提交记录 wall/GPU 时间；required Khronos/sync 烟测与数值回归另行执行。
参考 0.5%、候选 2%、收益 2 倍及预览/取消 100ms、tile 8ms 门槛固定，未认证或有限阶梯未达均保留。

新机先按[构建指南](build.md#新机器接续)恢复环境与 Sponza。构建目标为 `dk_pathmedia_benchmark` / `dk_pathtracing_probe`，
CTest 筛选与完整采集命令见[M19.8 报告](../benchmarks/2026-10-19-restir-sponza.md#复现)。
在全新证据目录依次执行 `--collect performance`、`--collect quality`、`--summarize`；四模式三轮性能共 12 进程，
十场景四模式质量共 40 进程。`--archive` 保存完整线性图像与所有报告，`--verify-archive` 离线重算并核对字节摘要。
仅同设备/驱动、同二进制和同输入可用 `--resume`；改变来源后完整重采，不拼接旧机器计时。
参考与候选从各自空历史开始，模式共享参考数据，不共享 reservoir；保留全部 pass 与当前可见性校正。
GPU tracking 步数未暴露时标为不可观测，不以预算代替实测。默认策略以报告证据为准，不由测量流程自动开启 ReSTIR。
