---
id: "0080"
created_at: "2026-10-09T15:15:00+08:00"
updated_at: "2026-10-09T15:38:18+08:00"
status: completed
design_refs:
  - ../design/physics-api.md
  - ../design/physics-xpbd-gpu.md
  - ../design/runtime.md
  - ../design/scripting-luau.md
---

# 0080 M11.3 有界模拟任务与暂停/取消

## 目标与设计依据

依据[模拟设计](../design/physics-api.md#m113-有界有限步任务)与M11.2的冷初始化阻塞证据，
为长实验提供有限任务、真实提交/完成进度、暂停/取消和安全退出。保留旧start/step同步契约；
GUI入口集成与全样本性能对照继续由M11.4验收。

## 实际变更

- SimulationService增加run/cancel，count限制1..1000000，单批1..8；同一Service只保留一个Play/任务。
  AsyncSimulation私有worker从初始化到析构拥有后端；GPU的ThreadContext/queue不跨线程销毁。
  不扩建通用Jobs，不增加JobId；run_id是本轮实验身份，命令TaskId仍只记录受理命令。
- GpuSimulation增加submit/poll，仅保留一个pending frame，使用零超时完成轮询；无默认CPU粒子回读。
  CPU/GPU同样以完成数推进公开时钟，提交数单独报告，末批精确补足目标。
- 请求和领取批次共享互斥边界。pause/cancel/Stop先受理，已领取的一批可完成；随后不得领取下一批。
  paused/cancelled表示稳定边界，Stop需后台完成资源释放后owner才清除Play。
  shutdown请求关闭并join，不detach；终态保留到Stop，取消与成功竞争有明确先后。
- 初始化/提交前失败不伪增计数；提交后等待失败保留真实提交数和旧完成数并冻结failed。
  显式诊断错误同样冻结；基础设施异常通过exception_ptr回到owner，未伪装为普通Error。
  owner最多5ms后pump发布状态；诊断还要求owner已发布稳定边界，防止新粒子数组附带旧步数。
- simulation.run/cancel及完整task schema、capabilities上下限与Luau edit/project白名单同步；
  query权限不能控制实验，async_tasks仍为false。particles/export只在已初始化且健康的稳定状态允许。
  旧同步入口与统一实验目录输出保持兼容。Services通过PRIVATE Threads/Profiling声明依赖，无新增第三方包或baseline变化。
- 同步命令参考、模拟指南、模块设计/索引与定向测试选择表。旧M11.2控制采集器检测新cancel命令后主动拒绝采样，
  指南说明其需要M11.4更新，历史基线不改写。

## 验证与结果

均使用scripts/verify.ps1的显式目标和筛选；日志在out/verify对应目录。CPU-only Debug与真实GPU RelWithDebInfo
分别验证条件编译隔离及真实驱动生命周期，未执行全量或无依据的双配置矩阵。

| 验证 | 结果/证据 |
| --- | --- |
| windows-scripting Debug；dk_simulation_tests/dk_run/dk_ctl/dk_luau_tests/dk_asset_command_tests；`^dk\.simulation\.(fixed\|simulation\|finite)` | 16/16通过，0失败/跳过；20261009-152048-93f08b8b |
| 同配置同目标；`^dk\.(simulation\.(task_cpu\|stdio\|pipe)$\|luau\.Luau simulation\|asset_commands\.)` | 7/7通过；新CPU真实IPC、旧stdio/pipe、Luau权限和命令发现；20261009-152641-c454ce2a |
| 最终CPU：windows-scripting Debug；dk_simulation_tests/dk_run/dk_ctl/dk_luau_tests；`^dk\.(simulation\.(finite \|task_cpu$)\|luau\.Luau simulation)` | 13/13通过，0失败/跳过；20261009-153322-6749550e |
| GPU：windows-graphics RelWithDebInfo；dk_simulation_tests/dk_run/dk_ctl；`^dk\.simulation\.(task_gpu$\|finite )` | 初轮9/9通过，0失败/跳过；20261009-152936-cbca8c3a |
| 最终GPU：同目标/配置；`^dk\.simulation\.(finite \|task_gpu$\|experiment_gpu$)` | 13/13通过，0失败/跳过；20261009-153323-acd31cb1 |

可控后端闸门覆盖初始化中控制、领取后取消、提交后暂停/Stop、取消与最终完成竞争、暂停稳定/恢复剩余目标、
初始化/提交前/提交后/显式诊断错误、bad_alloc不被吞掉、在途资源保活及创建/销毁线程相同。
真实IPC覆盖三规模8/16/32、seed42、dt10ms、迭代12、批次8、严格300拍；CPU数组与同步参考完全相同，
GPU核对全量数组、固定点/地面、导出不推进、Edit/历史不变，以及独立进程冷Stop/cancel/shutdown与运行中退出。
允许已记录的AMD可选层版本警告，其余诊断失败；不通过跳过设备测试声明GPU通过。

构建修复记录：第一次CPU配置因Threads::Threads未在Services作用域find_package而失败（20261009-151710-35127a68）；
第一次GPU测试构建因私有后端头需要显式RenderSimulation测试依赖而失败（20261009-152642-1694bae3）。
均在构建失败处停止、没有运行旧二进制；补齐target依赖后上述定向构建通过。

最终GPU报告：`out/build/windows-graphics/test-artifacts/RelWithDebInfo/simulation-tasks/gpu-429210db8d2e41799422bff1ff0dd479/report.json`。各规模最大位置/速度分量差如下，低于2e-3 m / 2e-2 m/s上限：

| 规模 | 位置差 m | 速度差 m/s |
| --- | --- | --- |
| 8×8 | 1.05798244e-05 | 3.12924385e-05 |
| 16×16 | 5.12599945e-06 | 4.09781933e-05 |
| 32×32 | 9.1791153e-06 | 7.15255737e-05 |

真实GPU进程首次run受理36.24ms，单次query 30.95ms，
pause请求31.93ms，cancel请求30.47ms。
这些是功能夹具的少量样本，不计算或冒充M11.4 p95；Cold Stop/取消终态/退出的全样本延迟另待复测。

文档检查：`scripts/check-spec.ps1 -Path <本阶段14个Markdown文件>`通过，核验534个本地链接、元数据、表格、索引、测试入口与JSON。
首次检查发现验证表格中的正则竖线未转义，修正后通过。`git diff --check`通过。
原有`spec/README.md`和`spec/guides/ai-documentation-workflow.md`改动保留，不纳入本阶段提交。

## 限制与交接

M11.3任务生命周期已完成；下一项为M11.4。旧start/step仍同步，显式particles/export也可能等待驱动/文件操作；
第三方shader编译或驱动调用不可硬抢占，Stop/退出最终回收耗时取决于其安全返回。
功能夹具的少量端到端计时不是p95响应阈值验收，未运行GUI事件循环心跳、20次冷控制/100次热查询矩阵或Tracy全基线对照。
M11.4需要更新控制采集器、验证GUI/IPC入口并对照M11.2历史数据；不把这些后续工作写成已完成。
