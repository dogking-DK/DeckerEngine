---
created_at: "2026-10-10T16:18:00+08:00"
updated_at: "2026-10-10T16:18:00+08:00"
---

# 实验计划与产物索引

[返回项目入口](../../README.md)。Python 3.11+ 标准库模块提供执行前计划展开与本地产物核验。
本页的命令只准备/检查索引；批量启动、恢复和报告按[Roadmap](../roadmap.md)后续阶段交付。
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
SDK 的稳定入口位于 `decker.experiments` 与 `decker.experiment_index`，不改变 Client.call。

image=true 要求目录恰有 config.json、metrics.json、particles.json、image.ppm、provenance.json。
image=false 要求前述文件去掉 image.ppm；CPU-only 无需创建 Vulkan 设备。
数值目录使用相同 DeckerSimulationExperiment v1 配置、完整采样指标、粒子数组和 provenance 字段，
provenance.files 为 config.json/metrics.json/particles.json。来源包含 run_id、document_id、scene_id、revision、
steps、simulated_time_ns、width、height、performance（无批次观测时 null）。本阶段可登记/核验此格式；
数值自动收集与运行器尚未提供，真实命令查询的测试示例见[验收夹具](../../tests/integration/ExperimentIndexTest.py)。

登记核对完整配置、seed/后端/固定步数、预期 run_id、来源身份、数值长度/范围和图像格式；
保存文件相对路径、大小、SHA256。再次 verify 会重新展开计划、重读全部登记文件；缺失、字节变化、
错误版本或身份错配均失败。verified 表示证据一致，不是数值误差合格或模拟任务调度状态。
不把 JSON 的性能样本当 GPU 时间或端到端时间，performance.scope 保留 batch_submit_to_completion。

## 失败和限制

已有根目录不能被 create_index 覆盖，已有产物从不被 SDK 修改。登记以独占锁和临时文件替换更新
index.json；重复目录、错误产物、写入失败都保留旧索引。一次实验可以登记不同目录的多次尝试。
索引只支持本地串行写者；崩溃遗留 .index.lock 时，先确认没有写者再人工移除。自动恢复将在后续阶段实现。
每份产物最大 16 MiB，索引最大 32 MiB，单实验最多 128 次登记。拒绝越界、链接/reparse point 和缓存路径。
不承诺断电持久性、恶意并发路径替换隔离或对整套伪造证据的真实性认证。

定向验收使用 `scripts/verify.ps1`，CPU-only 筛选 `^dk\.experiments\.(unit|index_cpu)$`，
图形配置筛选 `^dk\.experiments\.index_gpu$`，目标均为 dk_run、dk_ctl。
GPU 设备不可用以77跳过，其他失败不转为跳过。设计与实际证据见
[实验管理](../design/experiment-management.md)、[0089](../development/0089-experiment-plan-index.md)。
