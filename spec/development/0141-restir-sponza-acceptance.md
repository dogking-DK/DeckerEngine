---
id: "0141"
created_at: "2026-10-19T18:28:00+08:00"
updated_at: "2026-10-19T20:02:34+08:00"
status: in-progress
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# M19.8 Sponza 质量、性能与默认策略

按用户要求，以真实 Sponza 作为画质/性能场景；程序灯不再作为本阶段收益验收。设计见 [ReSTIR](../design/render-restir.md#m198-sponza-验收实施契约)。保留既定 0.5% 参考认证、2% 误差、三轮计时与 100ms/8ms 交互目标，不以降质换通过。

## 实施与验证

已扩展既有 [介质基准](../../tests/integration/PathMediaBenchmark.cpp)，保持旧默认入口，增加四种模式、实际配置/历史来源、完整线性图像、长链 GPU/wall 计时、640×360 相机移动与停动恢复。独立 [采集/聚合脚本](../../scripts/benchmark-restir-sponza.py)核对冻结输入、三轮身份、8 seed 链、缺失/非有限时间与图像；参考未认证、阶梯未达、交互未达均单列。默认仍为 Classic MIS；最终验收与策略结论待剩余质量数据完成。

首次沙箱构建停在 MSBuild 启动阶段，已取消；沙箱外构建成功。`20261019-183532-a036e836` 中旧聚合 1 项通过，4 个 GPU 烟测因本机 AMD switchable layer API 版本 warning 失败；按既有进程级兼容变量复验，不降低诊断要求。新聚合 Python 单元 6 项独立运行通过。随后 20261019-183928-52520022 的 13 项定向检查通过；20261019-184421-53bb0988 中 6 项通过，新增质量烟测发现 seed 结束与场景结束重复 release_view。修复后仅重跑失败用例，20261019-184553-03221839 通过。合计 14 个唯一定向 CTest 通过，无最终失败/跳过；正式三轮性能已完成；质量链按下述交接状态暂存。

未运行的检查不视为通过。工作区已有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 的用户修改不纳入本阶段提交。

质量等待方式补充：微小图像逐次阻塞唤醒开销明显，独立质量基准改为最多 1ms 主动 poll 后回退 wait；产品/全尺寸性能等待不变。20261019-190055-46a361ae 的 required 质量烟测通过，8 条空间 64 spp 链前后像素、二阶矩、transport 和历史统计逐值一致。短对照采样累计 wall 为 1084.3032/954.4268ms，仅作采集方式验证，不是算法性能收益。旧阻塞采集保留于 out/m19-8-sponza；正式数据整体使用最终程序重采于 out/m19-8-sponza-poll，避免混用二进制。


## 换机前进度提交（尚未完成 M19.8）

用户在 2026-10-19 19:58 要求先提交当前进度、换电脑继续。已停止本次采集进程；正在运行的 temporal/cloud-inside 不计入已完成证据。阶段保持 in-progress，不将完整验收或最终归档标为通过。

- 基准、采集/严格聚合、来源与完整图像校验、4 个新 GPU smoke 和报告拒绝测试已实现；没有改变生产 shader、物理模型或默认算法。
- 正式同来源数据：12/12 性能进程完成，28/40 质量进程完成。质量为 classic 10、initial 10、temporal 8、spatial 0。
- 尚缺 temporal 的 cloud-inside/cloud-above，以及 spatial 全部十组；完整 summary、等误差/等时间与均值结论、最终归档复算尚未执行。
- 已完成数据和 120 幅全尺寸图像保存于[进度归档](../benchmarks/2026-10-19-restir-sponza-progress/README.md)，可随 Git 转移；不依赖当前机器 out 目录中的图像绝对路径。
- [性能报告草稿](../benchmarks/2026-10-19-restir-sponza.md)已有三轮全尺寸结果。预览/取消/tile 三项同时达标的场景数依次为 Classic 10/10、initial 10/10、temporal 1/10、spatial 0/10；owned peak 均低于 1GiB。它不是最终质量验收结论。
- Classic 参考仅 cloud-inside/cloud-above 通过 0.5% 认证，其余八组不得生成认证等误差时间。4×4 质量与 640×360 画质严格区分；低尺寸首表面匹配率低，不能外推全尺寸时空复用收益。

最终报告拒绝测试补充实际 transport/seed、各档 pass 提交数和分别非负的 wall/GPU 增量。7 个 Python 单元通过；`20261019-195954-a22b1281` 再次构建基准并通过新旧 2 项聚合 CTest（0 失败/0 跳过）。与前述 GPU 检查合计仍为 14 个唯一定向 CTest，不重复累加重跑次数。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
    -Target dk_pathmedia_benchmark `
    -TestRegex '^dk\.pathtracing\.(restir_sponza_aggregation|media_benchmark_aggregation)$' `
    -Reason 'Validate M19.8 progress handoff and report rejection rules'
```

未运行：完整 52 进程聚合/最终归档复算、全工程测试、Debug 矩阵、CPU-only 独立配置及本阶段 GUI 回归。已有 M19.7 GUI/capture 证据不替代这些未执行检查。

进度归档校验通过：40 份已完成进程报告、12 份缺失任务显式列出，120 幅图像按内容去重为 119 份，总量约 86 MiB；逐文件 hash、图像尺寸/有限性和等待对照通过。`check-spec.ps1 -Path` 定向检查 8 个 Markdown / 745 个本地链接及全局索引/测试入口通过，`git diff --check` 通过。

## 新电脑接续顺序

1. 读取本记录、[ReSTIR 实施契约](../design/render-restir.md#m198-sponza-验收实施契约)、[性能草稿](../benchmarks/2026-10-19-restir-sponza.md)及进度归档 README。继续使用 0141，不新建重复记录。
2. 按[构建指南](../guides/build.md)准备 VS2026/CMake/vcpkg/Vulkan 环境，配置 `cmake --preset windows-editor`；构建 `dk_pathmedia_benchmark` 和定向验证所需的 `dk_pathtracing_probe`，运行受影响 smoke/数值回归。AMD switchable layer 的进程兼容变量仅在该机需要时设置，Khronos/sync 验证仍必须保留。
3. 换硬件、驱动或重建程序后，使用全新输出目录，重新采集完整三轮性能和四模式质量。不要把本机已完成报告拼接成新机器的一套计时；manifest 二进制和设备/驱动来源必须一致。同机同二进制原目录才可使用 `--resume`。
4. 用草稿中的命令完成采集，执行 `--summarize`，核对参考认证、独立链均值、4096→16384 趋势、相机停动与有限历史。未达保持未达，未认证保持 null；不要为了完成阶段降低阈值或关闭 pass。
5. 使用 `--archive spec/benchmarks/<日期>-restir-sponza` 生成完整可复算归档；当前 progress 目录是历史快照，不能传给 `--verify-archive` 冒充完整 52 进程归档。更新质量表、设计/指南/路线图，文档与聚合验证完成后再标记 M19.8/0141 completed，并独立提交阶段收尾。
