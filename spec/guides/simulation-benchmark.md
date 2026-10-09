# 模拟性能与控制延迟基线

协议见[性能设计](../design/foundation-profiling.md#m112-模拟基线协议)，
实测结果见[M11.2报告](../benchmarks/2026-10-09-simulation.md)。
后续响应试验与对照见[M11.4报告](../benchmarks/2026-10-09-simulation-response.md)。
这些工具测量M11.2同步入口基线。M11.3新增有限任务后，旧IPC控制采集器会检测cancel命令并主动拒绝采样，
防止套用旧完成/暂停假设；M11.4使用下述独立响应夹具。独立求解/数值基准仍可运行。
有限任务的功能验收见[0080](../development/0080-bounded-simulation-tasks.md)，用法见[模拟指南](simulation.md)。

## 构建与正确性

Windows、现有VS工具链和Vulkan设备；固定依赖baseline，不新增第三方包。
使用两个既有构建目录，同为RelWithDebInfo，Tracy配置关闭内存事件和调用栈。
不要在正式采样期间同时构建、运行其他GPU测试或另一个capture。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @('dk_simulation_benchmark','dk_run','dk_ctl') -TestRegex '^dk\.simulation\.benchmark_(cpu|invalid|aggregation|gpu)$' -Reason 'Simulation baseline and IPC fixtures'
cmake --preset windows-graphics-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics-profiling -Configuration RelWithDebInfo -Target dk_simulation_benchmark -TestRegex '^dk\.simulation\.benchmark_(cpu|invalid|aggregation|gpu)$' -Reason 'Tracy baseline correctness'
```

GPU正确性用required validation和同步检查；必须实际通过，不将设备不可用当作性能基线通过。
CPU-only可用windows-profiling构建同一target并筛选cpu、invalid、aggregation，完全不需要Graphics。
benchmark_invalid检查非法规模；aggregation检查空/非有限计时、缺失timestamp、步数/阶段错误及配置不匹配。

## 正式采样

先按[GPU观测指南](gpu-profiling.md)准备匹配的Tracy0.14.1命令行工具，再运行：

```powershell
$env:PYTHONDONTWRITEBYTECODE='1'
python scripts/benchmark-simulation.py --skip-control
```

支持`--off-dir`、`--tracy-dir`、`--tools-dir`、`--port`和`--repetitions`（3..10）；
路径默认从仓库根解析。唯一输出目录为`out/benchmarks/simulation-时间-随机后缀/`，不覆盖旧数据。
OFF、timestamps、Tracy三种GPU模式与规模按轮次轮换；CPU参考使用OFF程序。
全程一批完成再开始下一批，无额外GPU并发。固定300拍中，前32拍供首次/预热观测，
之后33批×8拍作为稳态，最后4拍另列。GPU求解不默认读回；读回和256×256的count=0绘制单列。
直接API性能显式关闭validation，真实服务保留if_available策略，不能直接相减推导IPC纯开销。

每次运行保存manifest、构建缓存、二进制和基准源码SHA256、设备驱动、当前电源计划、CPU型号、
全部批次/数值数组、capture及CSV、子进程日志。工作负载或控制失败返回非零，manifest保留failed。
manifest只描述采集阶段；汇总另行验证原始数据，成功才生成summary。复算失败返回非零，不能沿用旧summary作为本次通过证据。
采集进程限时180秒，客户端/服务探针限时60秒；超时不代表任务取消，夹具仅终止自己创建的进程。
`--skip-control`只采直接API数据，需要配合下述响应夹具完成M11.4；
复现历史M11.2控制数据须使用其历史提交，不带该参数。不能用Debug或开validation数据冒充正式性能数据。

IPC覆盖每种CPU/GPU规模：空闲query、冷start期间query、首批8拍期间query/pause/stop/shutdown，
以及32拍预热后自动pump期间的query/pause/stop/shutdown。
busy夹具用已连接的原始Named Pipe写入请求后发ready信号；控制固定在ready后约50ms发出，
避免两个子进程同时启动时shutdown抢先于busy请求。控制仍完整经过Python SDK/dk-ctl；
busy计时从写请求开始，不包含握手/子进程启动。当前服务只有一个pipe连接，控制等待包含连接串行化。
记录两个客户端区间是否重叠，
并确认step先于query/pause完成；这是客户端观测，不声称能精确知道请求抵达owner的时刻。
CPU工作较短，可能没有重叠，这些样本只能代表该发送条件下的延迟。
热query每轮5次（间隔20ms），其他控制每轮1次；pause后等待50ms再次检查步数稳定，
Stop核对Edit快照，shutdown分别测回复及进程退出。simulation.cancel明确为unsupported。

## 读取结果与复算

`summary.json`保留每轮nearest-rank p50/p95/p99/max，`summary.md`列每轮p50的中位数和范围。
GPU采集开销是同规模、配对轮次相对OFF稳态p50的变化；Tracy包含本机capture进程竞争。
冷样本和控制单项每轮只有一次，三轮合计n=3，p95/p99等于max，不能推断可靠尾部分位数。
CPU参考与GPU检查最大分量位置2e-3m/速度2e-2m/s，并要求不同采集模式/重复运行全量数组相同。
GPU父提交区间和compute/draw/transfer子区间不可相加；Tracy嵌套CPU zone也不可重复相加。

```powershell
python scripts/benchmark-simulation.py --summarize out/benchmarks/<本次目录>
```

汇总会拒绝失败、缺轮、错配置、错步数、采集丢失、设备/驱动/编译器变化和数值不一致。
CPU-only参考未测Tracy开销，GUI事件循环/硬件呈现、长期热节流、其他设备/平台未由此覆盖。
## M11.4 有限任务与真实GUI响应

```powershell
cmake --preset windows-editor
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.simulation\.response_editor_smoke$' -Reason 'Real GUI simulation response fixture'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl') -TestRegex '^dk\.simulation\.(response_aggregation|response_runner_smoke)$' -Reason 'Response aggregation and IPC fixture'
python scripts/benchmark-simulation-response.py
```

默认7轮，runner/editor、CPU/GPU及8/16/32规模，初始化/首批/稳态三阶段，共252组。
每组四个独立宿主进程，分别测试pause、cancel、Stop、shutdown，每个控制项跨阶段合计21次；共1008进程。
每个控制动作均对应进程首次模拟初始化，不将同进程再次初始化标为冷启动。
每阶段28次独立进程试验；稳态每轮另测15次query，每宿主/后端/规模合计105次。
控制项按宿主/后端/规模汇总p95，阶段分项保留实际数量和最大值。计时包括SDK、dk-ctl、握手和响应解析。
pause/cancel回复只表示受理；稳定终态、Stop回到Edit和shutdown至进程退出各自计时。
有限任务目标1000000拍保证测量期间持续运行，严格300拍数值由直接API和task_cpu/task_gpu另验。

GUI使用带.dk-editor-smoke标记的一次性空工程及真实SDL/ImGui/Vulkan循环，
窗口就绪30帧后才开始模拟；记录实际事件循环间隔，活动状态与空闲/终态分开。
GUI默认`--gui-validation required`；`--gui-validation disabled`仅用于另行诊断性能开销，
CTest的response_editor_smoke固定required。独立仿真设备保留if_available，与历史runner基线一致；
复用GUI设备时继承required模式。
两种GUI配置结果单列，不能把disabled性能结果声称为required模式延迟保证。
仅单列已知AMD隐式层API1.3/应用1.4警告，完整保存stderr及计数；其他warning/error或存活分配使验收失败。
这不覆盖编辑器首次窗口/资产加载、复杂场景预览或尚未实现的M12模拟视口。

`--hosts`、`--backends`、`--sizes`可缩小范围，`--smoke`仅一轮并标记partial；
正式7..40轮按[既定目标](../design/physics-api.md#m112-响应性测量与后续验收目标)检查p95和max。
不能把子集或partial说成完整M11.4通过。原始目录唯一，保存全部请求/阶段/边界、GUI心跳、日志、
源码及二进制hash、构建缓存和电源策略；错误/超时/缺样失败，不重试选最快轮。

```powershell
python scripts/benchmark-simulation-response.py --summarize out/benchmarks/<response目录>
```

人工中断采集后可仅补齐缺口，不必重新测量所有组合：

```powershell
python scripts/benchmark-simulation-response.py --complete-from out/benchmarks/<原目录> --interrupted-case <最后中断组名> --interruption-reason '记录主动停止的原因与时间'
```

续采要求原目录最后一组确为人工中断，二进制hash及配置一致；保留其原始失败和排除原因。
新目录以hash引用此前全部完成组，每控制项补至20次、每阶段独立进程补至20次、热query补至100次。
不支持静默跳过其他失败，也不能将引擎超时或离群值称为人工中断。汇总复核引用hash，不能修改旧证据。

仅修复某个控制路径时，可用`--actions shutdown --hosts editor --backends gpu --hot-queries 0`
限定复测；默认7轮覆盖三阶段，每规模共21次退出。单项结果保持partial，不能替代完整覆盖；
样本数达到20的分组仍严格检查p95，小样本仍检查max。结合未受修改影响的既有证据时，须在报告中明确版本、范围与替换理由。


### 编辑器共享设备的退出复测

支持同族第二队列时editor将模拟放在独立第二队列，复用GUI设备寿命；runner仍保持独立设备。
stdout和control.json记录实际设备模式、队列数和验证模式。GUI自身启动成本仍单列，
首次模拟保留首次shader/求解器/Graph初始化；不能将它描述为重新创建一个模拟设备。
只复查退出时可执行：

```powershell
python scripts/benchmark-simulation-response.py --hosts editor --backends gpu --actions shutdown --hot-queries 0 --ctl out/build/windows-editor/bin/RelWithDebInfo/dk-ctl.exe
```

每规模21个独立退出样本，子集summary仍为partial；数量充分的完整退出分组检查p95≤400ms、max≤1000ms。
400ms是2026-10-09用户确认的新目标，仅替换完整退出原250ms p95；其余控制项阈值不变。
完整基线与定向回归分别保留，不混合不同设备策略/二进制的数据求百分位。
共享队列三规模300拍、导出及暖进程回收入口为`dk.simulation.task_editor_gpu`，缺少第二队列返回77并记录为跳过。
