---
id: "0091"
created_at: "2026-10-12T10:19:00+08:00"
updated_at: "2026-10-12T10:44:00+08:00"
status: completed
design_refs:
  - ../design/experiment-management.md
---

# 0091 M13.3 中断恢复与失败重试

## 目标与设计依据

按[实验管理](../design/experiment-management.md)实现批处理恢复、成功项核验跳过、
原任务/票据核实和显式从零重试；保留每次尝试与所有旧产物，取消不误报成功。
原有spec/README.md和AI文档流程指南改动保留，不纳入本次提交。

## 实际变更

- [experiment_batch.py](../../sdk/python/decker/experiment_batch.py)增加resume_plan及resume CLI，
  与run共享有界调用、运行核验、采集、Stop等待和失败发布逻辑。新batch v2持久保存原run请求，
  不被后续查询覆盖票据；兼容读取v1并在继续时升级。plan/index格式不变。
- [experiment_recovery.py](../../sdk/python/decker/experiment_recovery.py)先核验全部已登记证据，跳过成功前缀，
  核对原endpoint/session/场景，再查询运行；已知run_id接回原任务，缺失回执时只用原ticket/参数去重取回。
  缺失/过期票据、其他run或session改变不猜测归属、不换票据启动新任务。
  核实请求自己的not_sent或回调中断不能推断原请求未执行，仍保留unknown和原票据。
- 恢复能接纳完整未登记产物、已登记但进度未更新的attempt，以及Stop执行后未保存成功的边界。
  已在stopping时只继续查询，避免再次Stop与切换edit的竞争。不覆盖产物，不把缺失/不完整目录当作成功。
- resume默认保留failed/cancelled，显式`--retry-failed`先核实并回收本人绑定的运行，
  再以同一原子快照归档旧history和分配新pending尝试；从零重跑到独立retry目录。
  保留旧配置、run_id、票据、错误、状态、索引和产物；取消不会因后续查询失败被抹去。
- [_experiment_lock.py](../../sdk/python/decker/_experiment_lock.py)增加输出目录写者mutex；
  [experiment_index.py](../../sdk/python/decker/experiment_index.py)在Windows改用随进程退出释放的索引写锁，
  强杀不会遗留本版本的.index.lock。已有根目录/父目录先规范化，文件属性检查纳入快照锁，
  锁名计算不打开待替换文件，修复Windows元数据句柄与os.replace竞争。
- 新增[18项恢复单元测试](../../sdk/python/tests/test_experiment_recovery.py)及
  [真实进程恢复夹具](../../tests/integration/ExperimentRecoveryTest.py)，注册recovery_unit/cpu/gpu，选择表同步。
  原批处理夹具失败时输出最终摘要，避免完整进度事件淹没诊断。
  [指南](../guides/experiments.md)、README/SDK入口、设计索引和Roadmap更新。
  不改C++服务、命令schema、求解器、M9 Recorder或依赖。

## 验证记录

Windows / Python 3.13 / 现有VS2026构建树。CPU-only使用windows-scripting Debug；
图像和GPU恢复使用windows-editor RelWithDebInfo；仅构建dk_run、dk_ctl。

```powershell
$env:TEMP = (Resolve-Path out/test-temp).Path
$env:TMP = $env:TEMP
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Configuration Debug -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(unit|batch_unit|batch_cpu|recovery_unit|recovery_cpu)$' -Reason 'M13.3 恢复及受影响批处理/索引验证'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(batch_gpu|recovery_gpu)$' -Reason 'M13.3 图像恢复及批处理回归'
```

| 范围 | out/verify证据目录 | 结果 |
| --- | --- | --- |
| CPU-only整轮 | 20261012-103155-a0b44454 | 5通过、0失败/跳过，33.28秒 |
| GPU批处理与恢复 | 20261012-103212-89652659 | 2通过、0失败/跳过，46.42秒 |
| Stop竞争/目录锁最终验证：unit、recovery_unit、recovery_cpu | 20261012-103521-e0272d49 | 3通过、0失败/跳过，26.09秒 |
| GPU异步Stop恢复最终验证：recovery_gpu | 20261012-103537-03f291b0 | 1通过、0失败/跳过，35.02秒 |
| 原请求核实语义：unit、batch_unit、recovery_unit | 20261012-104004-53f64b6d | 3通过、0失败/跳过，9.77秒 |
| 发送前故障/核实中断最终回归：batch_unit、recovery_unit | 20261012-104140-aa3ce151 | 2通过、0失败/跳过，7.80秒 |

最后四轮沿用同一verify命令/target和相应构建配置，只把TestRegex缩小为表中所列测试，
用于新增回归及直接影响的检查。最终unit=14、batch_unit=14、recovery_unit=18，共46个unittest。
恢复单元覆盖成功项跳过/无宿主空操作、原票据及过期/丢失票据、查询未发送/回调中断、
期限后接回原run、失败/取消显式重试、旧产物/索引字节保护、产物/索引/Stop发布中断、
运行消失、成功产物篡改、session/source/其他run拒绝、v1升级、目录占用、
重试快照写入失败、后续查询失败保留取消、不完整孤立目录保留及stopping→edit竞争。

真实CPU/GPU夹具各使用三项4×4布片、seed42/43/44、iterations=2、精确9拍；GPU导出96×72图像。
每种宿主各在以下五处真正强杀自己创建的批处理Python子进程，再用新CLI进程resume：

- 第一项成功、第二项已受理：核对跳过首项，前两项run_id不变。
- 产物目录已发布、索引未登记：接纳原完整目录。
- 索引已登记、进度未绑定：复用原attempt，不重复登记。
- Stop已被受理或执行、成功尚未保存：查询edit并补齐成功。
- 持有索引写锁、提交索引前：强杀后恢复获取写锁，完成登记。

各验证所有既有产物字节不变、每项只登记一次、最后回到edit；还验证活跃批次拒绝第二个resume进程。
真实受理后注入回执丢失，新的CLI通过原ticket返回相同TaskId/run_id；这属于交付故障注入，
不是声称测试了物理断网。产物登记后注入观察者失败，默认resume拒绝重跑，显式重试保留旧文件与第二个attempt。
另对0拍暂停任务发真实cancel，默认resume保持cancelled及pending尾项，显式重试后旧history仍为cancelled。
两个夹具全过程Edit/source/history不变；GPU同步validation错误0，最终恢复用例记录26次既有AMD Loader版本警告。

最终真实恢复产物位于：

- `out/build/windows-scripting/test-artifacts/Debug/experiment-recovery/81c595a8f5e841e6b0b361600894a7e8`
- `out/build/windows-editor/test-artifacts/RelWithDebInfo/experiment-recovery/60585728d6da42a7853779110aedfcfe`

各保留plan/report、五处中断批次、回执丢失/失败重试/取消批次、子进程日志及各次resume CLI输出。
指南中的resume和`resume --retry-failed`入口由上述真实进程夹具实际执行。

首轮`20261012-102842-ba8f926d`为4通过/1失败：恢复夹具通过，但原并发进度用例
暴露锁名realpath及锁外属性检查仍打开Windows目标句柄，导致batch.json替换WinError5。
修复锁边界后整轮通过；随后针对Stop竞争和原请求unknown语义新增回归，未扩大到全工程矩阵。
文档检查通过：9个Markdown文件、551个本地链接及元数据/表格/全局索引/测试入口；git diff --check通过。

## 遗留问题与下一步

M13.3范围已完成，下一项M13.4对比报告与闭环验收；M13整体仍进行中。
恢复需要可读的batch/index及原session/场景，不能在宿主重启或场景改变后自动迁移。
发送到保存回执之间强杀且没有run_id/票据，或票据已淘汰时保持unknown，需人工核实；
`--retry-failed`不覆盖此限制。旧版本遗留.index.lock须人工核实后移除，新版本Windows写者无此遗留。
不提供模拟检查点、跨文件断电事务、cancel CLI或对比报告。每项最多128次尝试，文件/索引额度沿用现有限额。
未运行全工程、M12 GUI、Luau、性能矩阵、其他平台/驱动或断电验收；本次未修改这些实现。
