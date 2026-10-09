# 模拟性能与控制延迟基线

协议见[性能设计](../design/foundation-profiling.md#m112-模拟基线协议)，
实测结果见[M11.2报告](../benchmarks/2026-10-09-simulation.md)。
这些工具测量已有实现；不会自动优化模拟或改变服务的暂停/取消能力。

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
python scripts/benchmark-simulation.py
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
`--skip-control`仅作部分测量，不满足M11.2；不允许用Debug或开validation数据冒充正式性能数据。

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
未来M11.3–4按[模拟服务目标](../design/physics-api.md#m112-响应性测量与后续验收目标)重测；
新异步命令契约需要调整控制夹具并保留旧基线，不能沿用旧样本假称满足取消或进度查询。
