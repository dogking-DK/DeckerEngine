---
id: "0090"
created_at: "2026-10-12T09:42:00+08:00"
updated_at: "2026-10-12T10:04:00+08:00"
status: completed
design_refs:
  - ../design/experiment-management.md
---

# 0090 M13.2 批量执行与进度

## 目标与设计依据

按[实验管理设计](../design/experiment-management.md)完成基于有限任务的串行批量执行、
进度文件/CLI、CPU-only数值收集和GPU导出登记。保持计划与index v1，恢复/重试/比较报告留到后续。
原有spec/README.md和AI文档流程指南改动保留，不纳入本次提交。

## 实际变更

- [experiment_batch.py](../../sdk/python/decker/experiment_batch.py)提供run_plan、read_progress以及run/status CLI。
  先完成计划/预算/路径/能力/空闲场景预检，再创建新批次；同Runtime的合作进程以session命名mutex互斥。
  固定初始场景来源/guard，核对受理session、run_id、配置，正步数等待有限任务完成，零步数只异步初始化。
  每项独立截止时间，保存提交/完成步数、初始化/在途状态、票据/TaskId、artifact_attempt_id及错误。
- image=false通过sample/particles分页自动收集四文件数值目录，核对页身份、步数、时间及最终稳定边界；
  image=true复用原export五文件并核对回执及来源。先发布产物，再登记原index，Stop确认edit后才写项成功并启动下一项。
  不改Edit/history、C++服务/命令schema、求解器、M9 Recorder或依赖。
- 单独的batch.json v1以临时文件/fsync/os.replace提交进度，保持plan/index v1兼容。
  失败、取消与unknown区分，后续项保持pending；保留活动run、成功项及已发布证据。
  传输结果未知、超时和中断不隐式Stop、取消或重试；进度写入本身失败时保留最近快照并返回recording_error。
- [_experiment_lock.py](../../sdk/python/decker/_experiment_lock.py)封装Windows命名mutex；
  [experiment_index.py](../../sdk/python/decker/experiment_index.py)在快照打开/读取/关闭和最终替换之间共享路径锁，
  解决Windows读句柄阻止原子替换的问题。读者验证完整索引/文件摘要，成功项还须绑定正确的attempt/run_id。
- [SDK单元测试](../../sdk/python/tests/test_experiment_batch.py)、
  [索引并发读取测试](../../sdk/python/tests/test_experiments.py)及
  [真实进程验收](../../tests/integration/ExperimentBatchTest.py)覆盖执行和故障保护；CMake新增三项定向CTest，选择表同步。
  [指南](../guides/experiments.md)、README/SDK/设计索引和Roadmap更新，提供可执行CPU示例及GPU能力要求。

## 验证记录

Windows / Python 3.13 / 现有VS2026构建树，仅构建dk_run、dk_ctl。
CPU-only使用无RenderSimulation的windows-scripting Debug，图像验收使用windows-editor RelWithDebInfo；
两种配置用于不同能力边界，不是全量Debug/Release矩阵。

```powershell
$env:TEMP = (Resolve-Path out/test-temp).Path
$env:TMP = $env:TEMP
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Configuration Debug -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(unit|batch_unit|batch_cpu)$' -Reason 'M13.2 串行批处理、CPU数值、故障保护及并发进度读取'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.batch_gpu$' -Reason 'M13.2 真实CPU/GPU批处理图像和进度产物绑定'
```

| 范围 | 证据目录（out/verify/） | 实际结果 |
| --- | --- | --- |
| CPU-only功能验收 | 20261012-095448-36d998c9 | 3通过、0失败/跳过，9.29秒；unit与batch_unit各14个unittest |
| CPU/GPU图像批处理 | 20261012-095555-ba98431f | 1通过、0失败/跳过，11.29秒 |
| 最终CPU-only夹具/注册 | 20261012-100304-2bb01d11 | batch_cpu 1通过、0失败/跳过 |
| 图形构建数值用例兼容 | 20261012-100324-18644ac8 | batch_cpu 1通过、0失败/跳过 |

单元测试覆盖预检拒绝、同线程递归占用、零步/非整批步数/272粒子分页、明确failed/cancelled、
提交回执丢失和票据保留、总期限、过期采样/错run分页、登记失败、Stop未确认、Edit版本改变、
写进度失败、回调异常、中断/宿主session改变、成功attempt篡改，以及原计划/索引13项回归。
新增索引测试让旧快照读者与登记线程并存，确认旧快照完整可读，读者关闭后新索引可验证。

真实CPU CLI一次运行4组17×16布片，steps=[0,9]、seed=[42,43]、iterations=2；
真实图形CLI一次运行8组4×4布片，再组合CPU/GPU两后端，每项导出96×72图像。
均在CLI运行中从另一进程持续读取验证快照，核对串行事件顺序、独立run_id、票据及索引attempt，
同时覆盖不同输出目录的跨进程互斥、重复目录拒绝、全部成功后edit/scene/history不变。
另外在两种宿主上实际取消首个CPU长任务，确认cancelled、后项pending且无伪造产物；测试显式Stop清理。
GPU同步validation错误0，记录8次已有AMD Loader版本警告，未跳过设备验收。

持久产物分别位于：

- `out/build/windows-scripting/test-artifacts/Debug/experiment-batch/560063ce8e6549f39815e7f7c643ad1b`
- `out/build/windows-editor/test-artifacts/RelWithDebInfo/experiment-batch/bd247fd247334544bc27530d4f2284ab`

各含plan、report、实时snapshots、CLI摘要/进度事件、batch/index/results及取消批次证据。
使用指南中的原始cloth-plan.json另经一次真实CLI执行，8项各300拍全部成功，总完成2400拍；
run、status、experiment_index三个入口均通过，结果位于`out/m13-batch-example-2e303575ecf9`。
该次仅为文档命令验收，夹具`out/m13-batch-guide-check.py`负责启动/回收自己创建的宿主。

早期verify `20261012-094630-420acd01`为2通过/1失败：实时状态读句柄导致batch.json原子替换WinError5。
先尝试共享删除句柄后，`20261012-094953-a6ea502b`为1通过/2失败，持有目标句柄仍无法可靠替换。
最终保持os.replace提交点，改为合作读写者的短时路径mutex；上表为修复后的有效结果。
失败批次保留既有成功项/产物并停止调度，没有通过关闭状态验证规避问题。
没有自动重试失败运行；每轮测试使用独立输出目录。

文档检查通过：9个Markdown文件、547个本地链接及全局索引/元数据/表格/测试入口；git diff --check通过。
最终复核补齐CTest配置区分：batch_cpu在无RenderSimulation构建中显式要求CPU-only能力；
在图形构建中也可复用同一数值用例，不把已编入但未使用的GPU能力当作失败。
为此最后两轮verify使用相同dk_run/dk_ctl目标与`^dk\.experiments\.batch_cpu$`筛选，
分别在上述Debug CPU-only和RelWithDebInfo图形目录执行；仅重跑受影响的夹具。

## 遗留问题与下一步

M13.2范围无剩余实现项，下一项M13.3中断恢复与失败重试；M13整体继续进行。
本次只支持Windows本机新批次，不恢复/复用已有输出，没有resume/retry/cancel CLI或比较报告。
mutex约束合作批处理进程，GUI和外部脚本仍须遵守独占模拟控制约定。
project_root由调用者指定；现有协议不查询宿主根目录，图像回执和本地产物核验不替代正确路径配置。
磁盘错误/强杀可留下非终态和孤立但有效的证据，产物/index/batch不是跨文件事务；不承诺断电恢复。
未运行全工程、M12 GUI、Luau、性能矩阵、真实进程强杀恢复或其他平台/驱动验收；本次未修改这些实现。
