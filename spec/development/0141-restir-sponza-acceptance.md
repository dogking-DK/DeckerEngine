---
id: "0141"
created_at: "2026-10-19T18:28:00+08:00"
updated_at: "2026-10-20T08:57:58+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# M19.8 Sponza 质量、性能与默认策略

按用户要求，以真实 Sponza 作为画质/性能场景；程序灯不再作为本阶段收益验收。设计见 [ReSTIR](../design/render-restir.md#m198-sponza-验收实施契约)。保留既定 0.5% 参考认证、2% 误差、三轮计时与 100ms/8ms 交互目标，不以降质换通过。

## 实施与验证

已扩展既有 [介质基准](../../tests/integration/PathMediaBenchmark.cpp)，保持旧默认入口，增加四种模式、实际配置/历史来源、完整线性图像、长链 GPU/wall 计时、640×360 相机移动与停动恢复。独立 [采集/聚合脚本](../../scripts/benchmark-restir-sponza.py)核对冻结输入、三轮身份、8 seed 链、缺失/非有限时间与图像；参考未认证、阶梯未达、交互未达均单列。新机完整验收已完成，最终策略保持 Classic MIS，结果见下文及[报告](../benchmarks/2026-10-19-restir-sponza.md)。

首次沙箱构建停在 MSBuild 启动阶段，已取消；沙箱外构建成功。`20261019-183532-a036e836` 中旧聚合 1 项通过，4 个 GPU 烟测因本机 AMD switchable layer API 版本 warning 失败；按既有进程级兼容变量复验，不降低诊断要求。新聚合 Python 单元 6 项独立运行通过。随后 20261019-183928-52520022 的 13 项定向检查通过；20261019-184421-53bb0988 中 6 项通过，新增质量烟测发现 seed 结束与场景结束重复 release_view。修复后仅重跑失败用例，20261019-184553-03221839 通过。合计 14 个唯一定向 CTest 通过，无最终失败/跳过；旧机正式三轮性能已完成，随后按下述交接状态暂存质量链。

未运行的检查不视为通过。工作区已有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 的用户修改不纳入本阶段提交。

质量等待方式补充：微小图像逐次阻塞唤醒开销明显，独立质量基准改为最多 1ms 主动 poll 后回退 wait；产品/全尺寸性能等待不变。20261019-190055-46a361ae 的 required 质量烟测通过，8 条空间 64 spp 链前后像素、二阶矩、transport 和历史统计逐值一致。短对照采样累计 wall 为 1084.3032/954.4268ms，仅作采集方式验证，不是算法性能收益。旧阻塞采集保留于 out/m19-8-sponza；正式数据整体使用最终程序重采于 out/m19-8-sponza-poll，避免混用二进制。


## 换机前进度提交（历史）

用户在 2026-10-19 19:58 要求先提交当前进度、换电脑继续。已停止本次采集进程；正在运行的 temporal/cloud-inside 不计入已完成证据。该次提交时阶段保持 in-progress，未将完整验收或最终归档标为通过；新机完成状态见后文。

- 基准、采集/严格聚合、来源与完整图像校验、4 个新 GPU smoke 和报告拒绝测试已实现；没有改变生产 shader、物理模型或默认算法。
- 正式同来源数据：12/12 性能进程完成，28/40 质量进程完成。质量为 classic 10、initial 10、temporal 8、spatial 0。
- 当时尚缺 temporal 的 cloud-inside/cloud-above，以及 spatial 全部十组；完整 summary、等误差/等时间与均值结论、最终归档复算尚未执行。
- 当时已完成的数据和 120 幅全尺寸图像随提交 `3202043` 保存并转移；当前目录已按后述精简规则清除原始快照，历史证据可从该提交恢复。
- 旧机三轮全尺寸结果的[历史结论](../benchmarks/2026-10-19-restir-sponza.md#旧机历史结论与保留范围)已合并到正式报告。预览/取消/tile 三项同时达标的场景数依次为 Classic 10/10、initial 10/10、temporal 1/10、spatial 0/10；owned peak 均低于 1GiB。它不是最终质量验收结论。
- Classic 参考仅 cloud-inside/cloud-above 通过 0.5% 认证，其余八组不得生成认证等误差时间。4×4 质量与 640×360 画质严格区分；低尺寸首表面匹配率低，不能外推全尺寸时空复用收益。

最终报告拒绝测试补充实际 transport/seed、各档 pass 提交数和分别非负的 wall/GPU 增量。7 个 Python 单元通过；`20261019-195954-a22b1281` 再次构建基准并通过新旧 2 项聚合 CTest（0 失败/0 跳过）。与前述 GPU 检查合计仍为 14 个唯一定向 CTest，不重复累加重跑次数。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
    -Target dk_pathmedia_benchmark `
    -TestRegex '^dk\.pathtracing\.(restir_sponza_aggregation|media_benchmark_aggregation)$' `
    -Reason 'Validate M19.8 progress handoff and report rejection rules'
```

该次未运行：完整 52 进程聚合/最终归档复算、全工程测试、Debug 矩阵、CPU-only 独立配置及本阶段 GUI 回归。已有 M19.7 GUI/capture 证据不替代这些未执行检查。

进度归档校验通过：40 份已完成进程报告、12 份缺失任务显式列出，120 幅图像按内容去重为 119 份，总量约 86 MiB；逐文件 hash、图像尺寸/有限性和等待对照通过。`check-spec.ps1 -Path` 定向检查 8 个 Markdown / 745 个本地链接及全局索引/测试入口通过，`git diff --check` 通过。

## 新电脑接续（2026-10-19）

当前 CPU Ryzen 9 9950X3D，GPU NVIDIA GeForce RTX 5090 D v2 / 610.88，VS2026 18.9.1、MSVC 19.51.36256.0（工具目录 14.51.36231）、CMake 4.2.1、Vulkan SDK 1.4.328.1。系统 Python 仅有未安装解释器的启动器，使用本机 Python 3.12.14，并通过忽略的 CMakeUserPresets.json / windows-editor-local 显式指定。

vcpkg 最初缺少项目 baseline；仅 fetch 后旧 checkout 仍没有所需版本数据库。已确认无本地修改，再快进到固定 33d78c1e 并 bootstrap，24 个依赖安装与 configure 成功，未修改依赖清单或版本。Sponza 从原公开来源恢复 74 文件 / 53,606,207 字节，与旧归档逐文件 SHA-256 一致，70 个外部 URI 完整；HDR/天空盒不被本阶段使用，未恢复。具体操作补入[构建指南](../guides/build.md#新机器接续)与[素材说明](../../projects/demo/assets/README.md)。

本机 vulkaninfo 检出 OBS 捕获层 API 版本警告，按其 JSON 的 disable_environment 在子进程设置 DISABLE_VULKAN_OBS_CAPTURE=1 后警告消失；未设置旧机 AMD 兼容变量，required Khronos/sync 保留。Release 基准与 probe 已构建成功，`out/verify/20261019-220424-f1bdb0b1` 的 14 项检查全部通过（12 GPU、2 聚合；0 失败/0 跳过，CTest 550.26s）。其中介质支持域 CPU oracle/GPU 长链用例 371.92s，通过后才开始正式采集。额外独立运行的 7 个报告拒绝 Python 单元通过；首轮文档检查 6 文件 / 86 本地链接通过。

正式采集使用新目录 `out/restir-sponza-5090`，于 22:15:54–23:35:26 完成 12 个性能进程与 40 个质量进程，无失败、重试或删除轮次；严格聚合成功。`--archive spec/benchmarks/2026-10-19-restir-sponza-5090` 随后生成并立即离线复算成功。保存 52 份报告、120 幅完整线性图像（119 份去重文件），连同环境和验证证据约 87.2 MiB，原始证据保存于提交 `f1e5734`，附加证据哈希另行校验通过。当前仅保留[关键数据](../benchmarks/2026-10-19-restir-sponza-5090/README.md)，旧机历史结论并入正式报告，计时没有混用。

本机个人预设为 `windows-editor-local`；`out/enter-decker.ps1` 可在 PowerShell 中 dot-source，临时提供 Python PATH 和本机 OBS 兼容变量，已验证 Python 3.12.14。它们属于忽略的本地配置，不写入共享路径或修改系统 PATH。绘图依赖仅安装在 out/plot-deps，生成报告图表并人工检查，无引擎依赖变化。

## 最终结果与交付范围

- 仅 cloud-inside/cloud-above 的 N/4N 参考通过 0.5%：0.3738% / 0.3716%。四模式均在 16384 spp 达到 2%，ReSTIR 等误差加速比 0.357–0.581×，未达 2×；其余八组保持未认证和 null 时间/加速比。
- 40 个模式/场景的 4096→16384 误差均下降、RGB 均值均满足两项界限；未认证参考下只作描述性检查，不证明全尺寸质量。共同实测预算下 ReSTIR 误差均高于 Classic。
- 三项交互同时达标为 Classic 6/10、initial 7/10、temporal 2/10、spatial 0/10；tile 与取消均达标。Classic 第二轮 surface 和 spatial 第一轮等存在明显预览波动，原样保留，不将原因归给未经定位的组件。
- owned peak 最大 897.342 MiB，满足 1GiB；所有进程 CPU/VMA 活分配为 0、诊断错误/警告为 0。运动 epoch 连续，停动恢复到 16 spp，历史界限与来源通过聚合。
- M19.8 与 M19 测量流程完成，默认仍为 Classic MIS，ReSTIR 显式可选。没有修改生产 shader、物理模型、采样门槛或默认算法；下一步为参考收敛和成本归因，不提前宣称 GI/PT/DLSS 已实现。

新机 required 验证使用 Release，目标 dk_pathmedia_benchmark / dk_pathtracing_probe，14 项筛选与复现命令完整保存在[验证摘要](../benchmarks/2026-10-19-restir-sponza-5090/validation/summary.json)和[报告](../benchmarks/2026-10-19-restir-sponza.md#复现)。未运行全工程、Debug 矩阵、独立 CPU-only 或本阶段 GUI/capture 回归；本轮未改生产代码。最终文档检查通过 13 份 Markdown、851 个本地链接及全局索引/测试入口/JSON；归档说明补充后 2 份 / 24 链接复验通过。首次暂存检查发现原始工具输出的行尾空白，改为 gzip 保留原始字节；其余附加日志使用不被忽略的 txt，摘要文本通过 attributes 固定 LF，避免跨机换行转换破坏哈希。修正后 git diff --cached --check 通过，直接读取 Git 暂存 blob 的 181 个核心/附加文件 SHA-256 全部匹配；2 份文档 / 24 链接复验通过。

## 2026-10-20 基准资料精简

按用户要求，只保留结论和关键文件；沿用 0141 记录，不新增阶段。旧机交接清单和原始进度证据可从提交 `3202043` 恢复，新机完整验收归档可从 `f1e5734` 恢复。

保留正式 Sponza 报告与概览图、机器可读 summary、manifest、environment、验证 summary，以及小型 CPU 基线报告/JSON。清除旧机进度目录、重复的 4070 草稿/预览图，以及新机逐进程报告、RGB32F 图像、构建/采集明细日志和失效归档索引。旧机关键结论合并到正式报告；摘要目录不再宣称能直接离线复算。后续原始数据和可选完整归档留在 out，基准程序与采集/聚合功能不变。

ReSTIR 专用基准文件从 354 个 / 174.19 MiB 精简为 9 个 / 约 0.83 MiB，移除 345 个文件；不重写 Git 历史。保留的 6 个数据/图表文件 SHA256 与清理前逐字节一致，结论数值未改。素材说明改指保留的 manifest；新旧完整工程清单因 HDR/天空盒未恢复及 README 等差异不相同，但其中 74 个 Sponza 文件的哈希全部一致。

清理前已执行 `python scripts/benchmark-restir-sponza.py --directory spec/benchmarks/2026-10-19-restir-sponza-5090 --verify-archive`，全部哈希、图像与 summary 重算通过。清理后 `check-spec.ps1 -Path` 检查 5 个 Markdown、63 个本地链接及全局索引/测试入口通过，已删除路径引用扫描无残留，`git diff --check` 通过。本次只整理文档/数据，未重跑引擎构建或 GPU 基准。
