---
id: "0030"
created_at: "2026-09-28T09:36:00+08:00"
updated_at: "2026-09-28T10:19:40+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
---

# 0030 M1.7.7 重复工作负载、定向集成与性能基线

基线 0767666，初始工作区干净。应用 spec-workflow、state-contracts、build-verify。
先细化 [Memory 测量契约](../design/foundation-memory.md)，新增可重复负载和三配置运行/报告工具。
不新增依赖，不提前推进 M4.1.1；性能结论限定为当前机器与合成 CPU 工作负载。

## 范围与实现

heap/PMR/arena/local/shared pool，三组尺寸/对齐，1/2/4/N 线程及合法跨线程回收。
定向集成覆盖重复 scratch/pool 临时对象和持久结果交接；所有 case 校验数据和最终零存活。
同一优化级别下 OFF/CPU-only/Memory 连接采集，保存吞吐/延迟分布、峰值/保留量和开销对照。
原始证据保存在 out；提交可复核的基线报告、完整聚合表、设计与 Roadmap 状态。

## 行为与失败处理

已新增 [MemoryBenchmark.cpp](../../tests/integration/MemoryBenchmark.cpp)、
[运行脚本](../../scripts/benchmark-memory.ps1) 与 inspector benchmark 模式；不改生产内存 API/默认参数。
三项定向检查覆盖快速矩阵/管线、非法参数、64 字节预算下部分分配失败的清理（不能挂住同步参与者）。
错误测试由 [CMake 夹具](../../tests/integration/MemoryBenchmarkFailure.cmake) 同时验证退出码 1、指定诊断和无成功 summary，
不把任意崩溃误作通过。worker 失败后仍经过约定 barrier，释放已申请块再报告；线程启动失败会减少同步参与数。
管线的诊断数组在线程启动前分配，避免主线程分配异常时让 producer 永久等待。

每个 case 创建独立系统/heap，记录 payload/checksum、每 worker batch 分位数、墙钟吞吐、backing 峰值/保留量、
局部 logical peak；allocator/metadata 初始化和预热在计时外。管线使用真实 token/context cache/scratch/ObjectPool/owning 容器，
验证缓存稳定、线程退出清空以及 weak 控制块延迟关闭，不宣称实现 M4 Jobs 或资产导入器。

脚本检查三配置编译选项/运行时开关和实际连接状态，三次轮换顺序运行；不自动更改构建配置。
全部数据/资源检查及 trace 读回通过后才发布成功状态。失败记录错误、终止本次启动的剩余子进程并保留日志；
报告生成失败也保持 failed。哈希与机器/构建元数据、逐次 JSONL/capture/CSV 放在 out，
版本化[报告](../benchmarks/2026-09-28-memory.md)、[完整 264 行聚合表](../benchmarks/2026-09-28-memory.csv)及
[证据摘要](../benchmarks/2026-09-28-memory-evidence.json)供后续比较。

## 调整与失败记录

第一次小规模真实采集 `out/benchmarks/20260928-094732-edcf2f42` 失败：进程退出前未完整排空
Windows DLL 中的 Tracy client，CPU trace 无负载区间，被计数校验拒绝。
改为全部计时与资源/zone 结束后，在独立工具内 RequestShutdown 并限时等待 HasShutdownFinished；
未改 Runtime/MemorySystem 的 profiler 生命周期。小规模三配置重验 `20260928-095715-84a7325a` 全部通过。

首轮完整矩阵 `out/benchmarks/20260928-095808-cbe026c7` 为 64 测量轮/8 预热轮，9 次运行、264 个聚合行全部通过；
短用例存在最高约 3.5 倍的三次吞吐范围差异。为延长采样，默认改为 512 测量轮/32 预热轮，
完整重验 `20260928-100008-05415094` 通过。最终修正计时外诊断数组分配顺序后，
以交付代码重新生成正式基线 `20260928-101009-1c340ec3`，所有原始结果保留，不按吞吐选最快运行。
最终 benchmark/driver/inspector 的 SHA256 与该次正式采集一致。

## 验证命令与结果

最终定向构建/测试均使用 `/W4 /WX`，覆盖不同 profiling 条件编译和优化配置；未扩大到全引擎回归。

```powershell
& ./scripts/verify.ps1 -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'M1.7.7 管线与错误清理'
& ./scripts/verify.ps1 -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'M1.7.7 OFF 优化路径'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'M1.7.7 CPU-only 路径'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'M1.7.7 Memory 路径'
cmake --build out/profiling-tools/inspector --config Release --target dk_memory_trace_inspect --parallel 4
pwsh -NoProfile -File scripts/benchmark-memory.ps1 -Rounds 512 -Warmup 32 -Batch 16 -MaxThreads 32 -Repetitions 3 -Port 18140
```

| 检查 | 结果 | 本地证据 |
| --- | --- | --- |
| 最终 Debug：smoke/非法参数/部分分配失败 | 3/3 通过 | `out/verify/20260928-100908-dd40c126` |
| 最终 RelWithDebInfo OFF | 3/3 通过 | `out/verify/20260928-100911-1d638a3e` |
| 最终 RelWithDebInfo CPU-only | 3/3 通过 | `out/verify/20260928-100914-6abf1764` |
| 最终 RelWithDebInfo Memory | 3/3 通过 | `out/verify/20260928-100918-2ddb9f56` |
| 正式全矩阵、定向管线、三配置真实采集和报告 | 9 进程 × 88 场景＝792 次，264 个聚合行；全部通过 | `out/benchmarks/20260928-101009-1c340ec3` |
| 错误构建目录配置拒绝 | 指定 `-OffBuildDir out/build/windows-profiling` 后 exit 1、summary failed、0 次工作负载 | `out/benchmarks/20260928-101447-46a2e173` |
| 更新 inspector 后读回已有 context/pool 文件 | 均通过，11/21 次配对与原曲线一致 | `out/profiling/20260928-092247-750c0acb`、`20260924-101851-38a72924` |

正式三个 Memory trace 分别有 4,049,708 / 4,049,720 / 4,049,701 次 backing 申请，全部配对；
异线程 free 为 2,011,282 / 2,011,298 / 2,011,268。独立 heap 累计计数逐次吻合，12 条局部曲线通过。
CPU-only 三次均为 0 内存事件/曲线。每次 CPU CSV 都核对固定的 87 Case / 504,288 Batch / 1 Pipeline / 544 Import，
并检查其他内部 zone 有闭合时长及源码位置。细节及完整 CPU zone 计数见报告。
新 inspector 对旧 context/pool 的验证是读取旧客户端产物，不是重新采集旧工作负载。

收尾检查通过：`check-spec.ps1 -Path` 限定本次 13 个 Markdown，验证 346 个本地链接、时间戳、开发编号与 JSON 清单；
PowerShell 语法解析、版本化 evidence JSON、CSV/三份源码 SHA256 对照及 `git diff --check` 均通过。

## 限制与交接

本机固定合成输入的初始基线，不是稳定排名：OFF 最宽三次吞吐相差约 3.04 倍；
256/64、32 worker arena 的 Memory/OFF 耗时比 75.710，提示采样同步是后续诊断重点，未作因果隔离实验。
min/max、profiling 耗时比与方法限制已写入报告，耗时比低于 1 不解释为确定提速。
没有新增/升级依赖、生产分配策略或命令。未运行全引擎测试、其他平台、ASan/TSan、真实 M4 导入/Jobs、GPU、
固定亲和性/温度/频率的长期稳态基准或 Tracy GUI 人工验收。

M1.7.7 完成，M1.7 全部子阶段已验收；下一项 **M4.1.1 元数据与身份目录**，下一可用开发编号 **0031**（开工前重查）。
