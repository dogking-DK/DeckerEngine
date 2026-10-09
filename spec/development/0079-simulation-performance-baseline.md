---
id: "0079"
created_at: "2026-10-09T14:34:00+08:00"
updated_at: "2026-10-09T15:04:00+08:00"
status: completed
design_refs:
  - ../design/foundation-profiling.md
  - ../design/physics-api.md
---

# 0079 M11.2 可复现性能与响应性基线

## 目标与设计依据

按[性能协议](../design/foundation-profiling.md#m112-模拟基线协议)，固定三种布片规模、seed42和300拍，
区分首次初始化、预热、稳态、直接API/GPU时间与真实IPC控制延迟，并形成后续响应性验收条件。
本阶段不改变模拟调度或生产命令，不新增依赖。

## 实际变更

- [SimulationBenchmark.cpp](../../tests/integration/SimulationBenchmark.cpp)新增可独立CPU-only构建的基准target，
  固定三规模/seed42/300拍/8拍批次/显式物理参数；划分冷首批、预热32拍、33个等长稳态批次及末尾4拍。
  直接计时CPU API、GPU设备/初始化/advance/wait/读回/零拍绘制；原生GPU时间按父提交和子类别保存。
  全量数组、数值指标、CPU参考误差、设备和编译开关写JSON，错误返回非零。
- [benchmark-simulation.py](../../scripts/benchmark-simulation.py)轮换规模和OFF/timestamp/Tracy顺序，
  实际capture并导出CPU/GPU CSV；保存源码/二进制hash、构建缓存、电源/硬件、原始每批样本，
  检查数组一致和完整capture，输出nearest-rank统计及配对采集开销。
- [控制夹具](../../scripts/benchmark-simulation-control.py)在busy请求已经写入Named Pipe后发ready信号，
  50ms后由SDK/dk-ctl发query/pause/stop/shutdown；冷热控制与退出分开计时，验证暂停稳定、Stop保护Edit。
  只清理夹具拥有的进程，保留失败日志；取消显式为unsupported，不修改生产命令或IPC协议。
- [聚合测试](../../tests/integration/SimulationBenchmarkTest.py)覆盖nearest-rank尾部、缺失/非有限数据、
  非连续/错误阶段步数、缺timestamp和构建配置不匹配。CMake与测试选择表同步。
- [报告](../benchmarks/2026-10-09-simulation.md)、[指南](../guides/simulation-benchmark.md)、设计/索引与Roadmap同步。
  后续请求p95≤100ms/max≤250ms，Stop/取消终态/退出p95≤250ms/max≤1000ms，GUI心跳p95≤50ms/max≤100ms；
  同时定义采样次数、冷/热条件及失败不能从统计中排除。

## 验证记录

Windows11 / MSVC19.51.36260.0 / RelWithDebInfo / Ryzen9 7940HX / RTX4070 Laptop / driver596.49。
使用[verify.ps1](../../scripts/verify.ps1)定向构建/测试；CPU-only目录关闭全部Graphics/Render/PHYSICS_GPU。
同一优化配置用于性能，GPU正确性单独启用required validation和同步检查。

| 范围 | 结果 | out/verify证据目录 |
| --- | --- | --- |
| OFF初轮基准与runner/ctl构建，CPU/非法参数/32×32 GPU300拍 | 3通过，0失败/跳过 | 20261009-143642-3e2b5291 |
| Tracy初轮同一数值基准 | 3通过，0失败/跳过 | 20261009-144101-a41632c4 |
| OFF显式固定输入、GPU同步验证与聚合回归 | 4通过，0失败/跳过 | 20261009-144551-e725c38e |
| Tracy显式固定输入、GPU同步验证与聚合回归 | 4通过，0失败/跳过 | 20261009-144717-b61ccbee |
| 最终CPU计时边界修正：OFF / Tracy / CPU-only | 每配置3通过，0失败/跳过 | 20261009-145434-68e6f1fd / 20261009-145434-c6961057 / 20261009-145434-73568427 |

最终仅修改CPU分支计时结束点，GPU代码未变，未重复扩大GPU验证范围。
聚合CTest内含6个Python用例。GPU诊断errors/warnings为0，仅单列已知AMD隐式层1.3/应用1.4 loader提示，Memory释放完成。
最终复算通过，源码/二进制hash与manifest一致，324个控制样本计数和summary SHA256复核通过。
check-spec校验171个Markdown、1803个本地链接、元数据/表格/索引/测试入口通过；git diff --check通过。
首轮文档检查发现测试选择表regex中的裸竖线拆列，改为候选测试前缀后检查通过。
状态复核：基准拥有独立heap/队列/子进程/输出目录，完成后关闭；失败保留日志，不承诺回滚已提交模拟。
原始manifest与聚合summary分别表示采集、汇总结果；复算失败不能沿用旧summary作通过证据。

执行入口：

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @('dk_simulation_benchmark','dk_run','dk_ctl') -TestRegex '^dk\.simulation\.benchmark_(cpu|invalid|aggregation|gpu)$' -Reason 'M11.2 baseline and real IPC fixtures'
cmake --preset windows-graphics-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics-profiling -Configuration RelWithDebInfo -Target dk_simulation_benchmark -TestRegex '^dk\.simulation\.benchmark_(cpu|invalid|aggregation|gpu)$' -Reason 'M11.2 Tracy fixed-input verification'
cmake --preset windows-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_simulation_benchmark -TestRegex '^dk\.simulation\.benchmark_(cpu|invalid|aggregation)$' -Reason 'M11.2 CPU-only and aggregation'
python scripts/benchmark-simulation.py
python scripts/benchmark-simulation.py --summarize out/benchmarks/simulation-20261009-145527-f2453c77
& ./scripts/check-spec.ps1
git diff --check
```

最终实测目录`out/benchmarks/simulation-20261009-145527-f2453c77/`：36次直接API运行、9份capture、
300744个GPU/5076个CPU区间、18套控制场景、72个runner和324个控制延迟样本，全部成功。
每次精确300拍，最大位置差1.058e-5m、速度差7.153e-5m/s，小于2e-3m/2e-2m/s；
每规模各采集模式/重复GPU数组相同，固定点位移/穿透为0。pause/Stop/退出实测检查通过。

首轮预跑与构建重叠，原夹具未保证busy已写入就启动shutdown，出现一次60秒等待失败；
保留failed manifest，未作为通过数据。改为预连接写请求后ready的有序夹具，独立smoke
`out/benchmarks/control-ready-smoke-f220aa99/`通过；完成一轮全矩阵后再收紧CPU计时边界，最后全部重采。
仅采用最终目录，不拼接先前最快样本；不据该夹具失败断言生产IPC丢回复。

## 结果、限制与下一步

三规模GPU无采集稳态8拍约8.16/8.36/8.25ms，CPU约0.135/0.963/3.824ms；
首批Graph编译约186–189ms，GPU初始化约453–468ms，设备创建另约337–358ms。
原生timestamp和实际Tracy的配对wall开销中位数约9.6–10.8%，不能把采集时间当零开销。
GPU冷start期间query中位数763–785ms，热query约30–31ms；冷首批query/pause约166–169ms。
基线完成不代表当前实现达到新响应性目标；下一项为M11.3。

未运行全引擎回归、Debug/Release双配置、GUI事件循环、呈现、长单次CPU/Luau任务中途控制、
取消、跨设备/跨平台或长期热节流测试；不新增或升级依赖、不实施性能优化。
CPU冷批次未与延迟50ms发出的控制重叠；样本数不足以确定可靠尾部，后续按设计增大样本并升级异步夹具。
