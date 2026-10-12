---
created_at: "2026-10-10T16:18:00+08:00"
updated_at: "2026-10-12T14:12:00+08:00"
---

# 实验批处理、恢复与对比报告

[返回项目入口](../../README.md)。Python 3.11+ 标准库模块提供计划展开、串行批量执行、进度与本地产物核验。
批处理连接已有 Windows runner/editor，支持中断恢复、显式重试及自动生成可追溯对比报告。
单次实验仍使用[模拟指南](simulation.md)的命令和配置，M9 [记录/重放](replay.md)保持独立。

## 准备计划

从仓库根执行（out 必须存在，输出目录必须不存在）：

```powershell
$env:PYTHONPATH = (Resolve-Path sdk/python).Path
python -m decker.experiments examples/automation/cloth-plan.json --output out/cloth-plan
python -m decker.experiment_index out/cloth-plan
```

[示例计划](../../examples/automation/cloth-plan.json)展开 8 组 CPU 数值实验：两种 compliance、
两种 iterations、两个 seed，各 300 拍。输出包含完整配置、稳定 experiment_id、总步数和工作量；
index.json 初始全部为 planned，verify 返回 verified=0，不将尚未运行的配置记作成功。
不带 --output 时只向 stdout 输出展开 JSON；非法输入退出码为 2，且不创建索引。

计划顶层字段全部必需，不接受未知字段：

| 字段 | 含义 |
| --- | --- |
| format / version | DeckerExperimentPlan / 整数 1；未知版本拒绝 |
| base | steps 0..1000000、fixed_dt_ns 1000000..33333333、cloth 对象；cloth 省略字段使用当前 XPBD 默认值 |
| sweep | steps、fixed_dt_ns 或 cloth.<参数> 到非空数组的映射；空对象表示不扫描 |
| seeds | 显式非空 uint32 数组，不随机生成；base.cloth/sweep 不再重复定义 seed |
| solvers | xpbd_cpu / xpbd_gpu 的非空数组 |
| output | image 布尔值、width/height 各 1..2048；数值/粒子/配置/来源始终必需 |
| budget | max_experiments、max_total_steps、max_work_units 均为正整数，全部必须满足 |

cloth 支持 columns/rows/spacing/height/particle_mass/compliance/iterations/gravity_y/floor_y/damping，
范围见[模拟命令](../commands/simulation.md)；所有组合还必须满足 height>floor_y。
浮点归一为 float32；相同有效数值不能重复作为轴值。按轴名字典序、轴数组声明顺序、solver 顺序、seed 顺序展开。
计划键顺序或显式补齐默认值不改变身份；数组顺序和预算会改变 plan_id，但相同配置/输出的 experiment_id 不变。

硬上限为 4096 项、10 亿总拍数、1 万亿 work_units。计量公式为
`steps * (particles + iterations * (particles + constraints))`，横/纵约束及双对角均计入。
它用于拒绝超量工作，不预测耗时，也不包括初始化、绘图或 IO 成本。无宿主时可离线展开 GPU 计划；
是否具备求解/导出能力仍须在实际调用前检查 runtime.capabilities。

## 一次运行整个计划

以下 CPU-only 示例从仓库根开始，使用[构建指南](build.md)的 windows-scripting 构建。
先在第一个终端创建新工程目录、启动宿主；保留输出的绝对路径：

```powershell
cmake --build out/build/windows-scripting --config Debug --target dk_run dk_ctl
$root = Join-Path $PWD ('out/experiment-demo-' + [guid]::NewGuid().ToString('N').Substring(0,12))
New-Item -ItemType Directory -Path $root | Out-Null
Write-Output $root
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root $root --pipe experiment-demo
```

第二个终端仍在仓库根，将 `$root` 设为上面输出的路径，然后创建场景并启动一次批处理：

```powershell
$env:PYTHONPATH = (Resolve-Path sdk/python).Path
$ctl = (Resolve-Path out/build/windows-scripting/bin/Debug/dk-ctl.exe).Path
$runner = (Resolve-Path out/build/windows-scripting/bin/Debug/dk-run.exe).Path
python -c "from decker import Client; import sys; Client('experiment-demo', sys.argv[1]).call('scene.new', {'name':'Cloth sweep'})" $ctl
python -m decker.experiment_batch run examples/automation/cloth-plan.json --pipe experiment-demo --ctl $ctl --runner $runner --project-root $root --output batch-001
python -m decker.experiment_batch status "$root/batch-001"
python -m decker.experiment_index "$root/batch-001"
```

已有场景可直接运行，省略 scene.new。执行器固定开始时的场景身份与版本，保留 Edit 和撤销历史；
每项产物登记且 Stop 确认回到 edit 后才开始下一项。运行期间应独占模拟控制，避免 GUI/其他脚本编辑或切换模拟。
同一 Runtime 的合作批处理进程互斥，即使输出目录不同也不能并行运行。
批次结束后宿主仍运行，用户可继续使用；需要退出时显式调用 runtime.shutdown。

`--project-root` 必须与宿主工程根一致，`--output` 是其下的新相对目录，父目录须存在。
不能把上节已准备的索引目录传给 run；run创建新批次，resume继续已有的批处理进度。
计划/预算/能力/空闲状态预检失败时不创建输出或启动模拟。`--runner` 可选，用于记录实际宿主二进制摘要。
默认每次调用最多60秒，每项实验最多600秒；可用 `--call-timeout`、`--experiment-timeout`、
`--poll-interval` 调整。总期限覆盖初始化、运行、收集和Stop，文件IO/进程回收不提供硬实时保证。

CPU-only 的 image=false 自动生成 config.json、metrics.json、particles.json、provenance.json。
GPU求解或图像输出须使用具备相应能力的图形构建；将计划 solvers 设为
`["xpbd_cpu", "xpbd_gpu"]` 可逐项对照，image=true 额外生成 image.ppm。
同步提高 budget 以容纳增加的组合。图像导出复用 simulation.export；不为缺失GPU能力静默降级。
0拍实验只等待初始化并采集初态；正步数等待完成计数精确达到目标，不以提交计数代替完成。

## 进度与失败结果

运行时 stderr 每行输出一个 JSON 进度事件，含各状态计数、目标/已完成总拍数与当前项；
stdout 输出最终批次摘要及comparison_report路径。退出码0表示执行成功且已请求的数值比较通过
（没有对照时标not_compared）；1表示批次停止或数值比较失败/不完整，2表示预检/用法/核验/报告发布失败。
执行status与comparison_report.comparison_status分别表达执行和数值结果。
`status` 无需连接宿主，可在运行中读取；它输出当前持久化快照并核验已登记产物，不轮询引擎。
完整目录包括：

- `batch.json`：批次/session/场景身份、每项状态与步数、run_id、受理票据/TaskId、产物attempt_id、错误及尝试history。
- `index.json`：原计划、完整配置、环境和已登记文件摘要。
- `results/<experiment_id>/`：各项独立产物。
- `reports/<report_id>/`：自动生成的JSON、CSV和HTML报告；每次新目录，不覆盖旧报告。

项依次经历 pending、submitting、running、collecting、stopping、succeeded；批次顶层保持running直到结束。
明确失败或取消分别记failed/cancelled，超时、结果未确认或Ctrl+C记unknown；后续项保持pending。
不会自动取消/Stop已接受的任务、重发命令或覆盖已有产物。先用保存的session、票据、run_id
检查宿主状态，再决定后续操作；不要把unknown当作未执行。磁盘故障或强杀可留下非终态快照，
也可能已有有效产物但批次尚未确认成功。index的verified只证明文件/来源一致。

Python API复用已有Client：

```python
from decker.experiments import read_json
from decker.experiment_batch import run_plan, read_progress, ExperimentBatchError

try:
    report = run_plan(client, read_json("examples/automation/cloth-plan.json"),
                      project_root=root, output="batch-002", progress=print)
except ExperimentBatchError as error:
    print(error.progress, error.cause, error.recording_error)
saved = read_progress(root / "batch-002")
```

progress回调收到已落盘的独立快照，回调异常会停止调度。写进度本身失败时，异常的recording_error
说明记录失败，磁盘上保留最近成功快照；异常中的progress可能比磁盘新。API不负责创建场景或启停宿主。

## 恢复中断的批次

原宿主和场景仍在时，在新的终端继续同一输出目录：

```powershell
python -m decker.experiment_batch resume --pipe experiment-demo --ctl $ctl --project-root $root --output batch-001
python -m decker.experiment_batch status "$root/batch-001"
```

resume从索引中的原计划继续，不接收替换计划。它先重新读取所有已登记文件、检查摘要及来源，
跳过已核验成功项，再接回未完成项。损坏的成功产物会阻止恢复，不会被静默重新生成。
已完成批次是仅核验的空操作，不要求原宿主仍运行。

对于已受理的模拟，使用已保存run_id核对配置和来源，等待原任务完成。提交回执丢失但保留了
原票据时，使用相同session/ticket/参数取回原请求结果，不分配新票据重新启动。
产物已写完但尚未登记时，核验后登记；索引已更新但进度未更新时，重新绑定原attempt。
Stop已执行或仍在进行时，确认回到edit后才继续下一项，不把索引verified直接当作任务成功。

恢复要求同一endpoint、Runtime session和完整场景来源。宿主重启、场景切换或编辑变化会拒绝自动接续；
成功产物仍可离线核验。没有保存run_id或票据、票据已淘汰、出现其他活动run时保留unknown，
需要人工核实，不能凭相同配置猜测归属。强杀可能发生在发送和保存回执之间，这个窗口不保证自动恢复。
每次resume使用新的等待期限；`--call-timeout`、`--experiment-timeout`、`--poll-interval`与run相同。

## 显式重试失败或取消项

普通resume保留failed/cancelled，不自动从零重跑。确认需要重试时：

```powershell
python -m decker.experiment_batch resume --pipe experiment-demo --ctl $ctl --project-root $root --output batch-001 --retry-failed
```

`--retry-failed`明确包含failed和cancelled项。先核实原运行或当前空闲状态，必要时Stop属于该尝试的运行，
等到edit，再创建新尝试。无法核实的unknown不会因为该选项而直接重跑。
每项首次目录为`results/<experiment_id>`，后续为`results/<experiment_id>-retry-2`等，最多128次。
旧尝试的状态、错误、票据、run_id、产物引用进入history；旧目录和索引记录全部保留。
再次成功只表示新尝试成功，不改写历史取消/失败；其他已成功项不重跑。
后续查询失败也不会抹去已确认的取消状态。

Python入口：

```python
from decker.experiment_batch import resume_plan

report = resume_plan(client, project_root=root, output="batch-001")
# 仅在显式决定从零重试失败/取消项时：
report = resume_plan(client, project_root=root, output="batch-001", retry_failed=True)
```

新批次使用batch v2；v1批次可读取，并在实际恢复时原子升级，plan/index v1保持不变。
批处理进程退出后输出目录/Runtime的mutex会释放，重新启动可取得；普通进度读取不占用整个批次写锁。
这恢复的是任务清单与仍存在的任务，不是模拟检查点或宿主重启迁移。

## 登记已有产物

图像实验应设置 output.image=true，并按完整 config 运行到指定拍数。使用 simulation.export
将结果放到索引根内的新子目录，例如项目根是 out、索引位于 out/cloth-plan 时，output 为
`cloth-plan/run-001`。export 的父目录需已存在。不能用原单次示例的 10000 拍限制推定该示例支持所有计划步数。

Python 登记入口如下，示例变量 root/client/runner_path/export_reply 来自调用方已有单次实验：

```python
from decker.experiment_index import environment, register_artifacts, verify_index

index = verify_index(root)
entry = index["experiments"][0]
observed = environment(executable=runner_path,
                       runtime=client.call("runtime.capabilities").value)
attempt = register_artifacts(root, entry["experiment_id"], "run-001",
                            expected_run_id=export_reply["run_id"],
                            execution_environment=observed)
verified = verify_index(root)
```

create_index(root, plan, execution_environment=observed) 可在准备阶段保存已观察的宿主信息；
默认只记本机 Python/OS，runtime、executable、device 为 null。environment 的 device 可传入调用方
已核实的设备/驱动对象，不会凭空推断 GPU。每次登记单独记录环境、attempt_id 和时间。
手工准备/登记入口位于 `decker.experiments` 与 `decker.experiment_index`，不改变 Client.call。

image=true 要求目录恰有 config.json、metrics.json、particles.json、image.ppm、provenance.json。
image=false 要求前述文件去掉 image.ppm；CPU-only 无需创建 Vulkan 设备。
数值目录使用相同 DeckerSimulationExperiment v1 配置、完整采样指标、粒子数组和 provenance 字段，
provenance.files 为 config.json/metrics.json/particles.json。来源包含 run_id、document_id、scene_id、revision、
steps、simulated_time_ns、width、height、performance（无批次观测时 null）。批处理自动收集此格式；
单独登记现有查询结果的测试示例见[验收夹具](../../tests/integration/ExperimentIndexTest.py)。

登记核对完整配置、seed/后端/固定步数、预期 run_id、来源身份、数值长度/范围和图像格式；
保存文件相对路径、大小、SHA256。再次 verify 会重新展开计划、重读全部登记文件；缺失、字节变化、
错误版本或身份错配均失败。verified 表示证据一致，不是数值误差合格或模拟任务调度状态。
不把 JSON 的性能样本当 GPU 时间或端到端时间，performance.scope 保留 batch_submit_to_completion。

## 自动报告与独立重跑

CLI run/resume完成或保留失败进度后，自动核验来源并发布报告。
打开stdout的`comparison_report.directory`下的report.html，直接查看离线表格与SVG散点图；
每个点对应表格序号，展开实验可看完整配置、全部指标及原始文件链接。
report.json含完整来源/环境/摘要、历史尝试、比较值与容差；experiments.csv和comparisons.csv便于后处理。
报告文件本身不依赖服务/网络；原始文件链接需保持批次目录结构。

在上文宿主和场景仍可用时，从零运行同一计划到新目录，并与已有批次比较：

```powershell
python -m decker.experiment_batch run examples/automation/cloth-plan.json --pipe experiment-demo --ctl $ctl --runner $runner --project-root $root --output batch-002 --baseline "$root/batch-001"
```

同批计划同时包含CPU/GPU时自动按其余配置、seed、步数和输出配对；`--baseline`按experiment_id配对独立重跑。
重排轴/seed数组不影响配对。两侧采用实验并集，缺项、失败、取消、unknown或pending均不会通过数值验收。
对照目录或batch_id相同拒绝，不能将自己或复制出来的同一批次充当重跑证据。
已成功项比较全部粒子位置/速度最大绝对分量差和RMS，逆质量要求精确一致；指标逐项列差，不混用量纲。
图像保留来源和摘要，当前不做像素差异验收。

没有宿主时，可对已有批次重新生成报告；SDK执行API不自动报告，可显式调用：

```powershell
python -m decker.experiment_report "$root/batch-002" --baseline "$root/batch-001"
```

```python
from decker.experiment_report import write_report
result = write_report(root / "batch-002", baseline=root / "batch-001")
print(result["directory"], result["comparison_status"])
```

`DeckerExperimentTolerances` v1默认策略如下，可保存JSON并在两种CLI中传`--tolerances <文件>`，
SDK用tolerances对象；报告始终嵌入实际使用的策略。默认重跑阈值是本SDK策略，跨后端阈值沿用M11；
它们不是任意场景/设备的普遍精度保证。

```json
{
  "format": "DeckerExperimentTolerances",
  "version": 1,
  "same_solver": {"position_abs": 0.000001, "velocity_abs": 0.00001},
  "cross_solver": {"position_abs": 0.002, "velocity_abs": 0.02}
}
```

数值比较状态为passed/failed/incomplete/not_compared。未成功实验的指标为JSON null、CSV空单元格、
HTML“—”，图中不绘制该点；已登记文件损坏/缺失直接拒绝报告，不能把损坏当普通缺测。
环境字段是否相等另行标识；没有记录设备不证明两次硬件相同。不同环境仍使用显式same_solver策略，
须按实际用途选择容差。二进制摘要和产物摘要用于追溯，不用来推断数值是否相同。

计时遵循[M11口径](simulation-benchmark.md)：last_batch只表示最近一次提交到完成的墙钟，
带累计steps和该样本batch_steps。300拍、每批8拍时末批是4拍，不能将该值当300拍求解耗时。
只在采样steps/batch_steps相同的配对间列差值和比率；分母0比率留空，不计算稳态p50或宣称显著提速。
GPU timestamp没有采集出口，明确null。CLI另记录本次run/resume调用总墙钟，包含预检、等待、采集、
登记和Stop但不含报告生成；一次resume耗时不代表整批生命周期。离线再生成的该项为空。
报告生成失败时保留已完成批次和旧报告，stdout包含report_error；修复原因后使用离线入口，无需重跑实验。

## 失败和限制

已有根目录不能被 create_index 覆盖，已有产物从不被 SDK 修改。登记以独占锁和临时文件替换更新
index.json；重复目录、错误产物、写入失败都保留旧索引。一次实验可以登记不同目录的多次尝试。
索引只支持本地串行写者；Windows本版本使用随进程退出释放的mutex。
历史版本遗留.index.lock仍须确认无写者后人工移除；其他平台的离线登记继续使用文件锁。
每份产物最大 16 MiB，索引最大 32 MiB，单实验最多 128 次登记。拒绝越界、链接/reparse point 和缓存路径。
不承诺断电持久性、恶意并发路径替换隔离或对整套伪造证据的真实性认证。

定向验收使用 `scripts/verify.ps1`，CPU-only 批处理筛选 `^dk\.experiments\.(unit|batch_unit|batch_cpu)$`，
图形批处理筛选 `^dk\.experiments\.batch_gpu$`，目标均为 dk_run、dk_ctl。
批处理GPU用例要求可用设备，不把跳过当作验收通过。原独立索引用例为index_cpu/index_gpu。
恢复/重试使用`^dk\.experiments\.recovery_(unit|cpu)$`；GPU恢复使用`^dk\.experiments\.recovery_gpu$`。
报告使用`^dk\.experiments\.report_(unit|cpu)$`，GPU图像/数值重跑使用`^dk\.experiments\.report_gpu$`。
设计与实际证据见[实验管理](../design/experiment-management.md)、
[0089](../development/0089-experiment-plan-index.md)、[0090](../development/0090-experiment-batch.md)、
[0091](../development/0091-experiment-recovery.md)、[0092](../development/0092-experiment-reports.md)。
