---
module: experiment-management
created_at: "2026-10-10T16:08:00+08:00"
updated_at: "2026-10-10T16:19:00+08:00"
status: accepted
---

# 实验计划与产物索引

## 目标与边界

M13.1 在 Python 标准库 SDK 中增加离线计划展开和本地产物核验。入口为
`decker.experiments`（计划）与 `decker.experiment_index`（索引），不创建 Runtime、
不调度模拟、不修改 C++ 命令及 M9 Recorder 格式，不增加依赖。
M13.2 再接串行执行器，M13.3 再接任务恢复，M13.4 再接比较报告。

## 计划格式与稳定身份

`DeckerExperimentPlan` version 1 严格接受 base、sweep、seeds、solvers、output、budget。
base 包含 steps（0..1000000）、fixed_dt_ns（1000000..33333333）、cloth（可省略成员，
显式补齐当前 XPBD 默认值；seed 只来自 seeds）。sweep 是 steps、fixed_dt_ns 或
cloth.<成员> 到非空值数组的映射，禁止 seed/solver 重复定义。seeds 是 uint32 数组，
solvers 是 xpbd_cpu/xpbd_gpu 数组。output 包含 image 布尔值和 1..2048 的 width/height；
数值、粒子、配置、来源始终必需，image=false 不要求 Vulkan 图像。

cloth 数值范围跟随当前命令和 XPBD 校验；浮点规范化为 float32（含负零归零），
所有展开组合要求 height>floor_y。拒绝未知字段、非有限值、bool 充当整数、重复 JSON 键、
重复轴值及 float32 归一后重复实验。固定相机沿用 DeckerSimulationExperiment v1。
轴名按字典序排列，数组保持声明顺序，先轴组合、再 solver、再 seed；不使用随机 seed。

budget 必须显式给出 max_experiments、max_total_steps、max_work_units，硬上限分别为
4096、1000000000、1000000000000。先计算组合数，超限不物化笛卡尔积；完整校验结束前
不创建输出、不调用引擎。每项 work_units = steps * (particles + iterations *
(particles + constraints))，constraints 为横/纵边加双对角；它是静态工作量而非时间预测。
初始化、图像、IO 成本未包含，单项尺寸和文件额度另有限制。

plan_id 为补齐默认/规范化后的计划 canonical JSON SHA256，包含预算/数组顺序；
experiment_id 为完整单次 config 与 output 的 SHA256，独立于计划位置/预算/路径/环境。
单次 config 保持 DeckerSimulationExperiment v1，不混入批处理身份。

## 索引、环境和来源

`DeckerExperimentIndex` version 1 嵌入规范化计划、plan_id、总预算、创建环境和有序实验项。
每项保存完整 config、output、work_units、experiment_id 和 attempts。空 attempts 表示 planned；
登记成功结果后为 verified，含独立 attempt_id、登记时间、运行环境、来源、文件相对路径、
字节数和 SHA256。verified 只表示登记时/重新 verify 时证据一致，不表示调度器任务终态。
运行身份 run_id 不作为稳定实验身份；同一配置多次运行保留多个尝试，禁止重复登记同一目录。

环境记录 SDK 格式版本、Python/OS，以及调用方提供的 runtime capabilities、runner 二进制摘要
和设备/驱动备注；未知项显式为空，不推断硬件。环境在每次尝试单独保存，不参与实验身份。
索引内嵌数据重新展开后必须一致；核验不能只信磁盘中缓存的 ID 或状态标签。

登记要求产物位于索引根内的新目录；支持现有五文件 simulation.export，也支持同结构的
四文件数值产物（config/metrics/particles/provenance）。数值产物的 provenance.files 不含 image，
配置仍保留固定视图用于未来对照；本阶段定义/核验该格式，不新增数值执行器。
调用者提供 expected_run_id，核对 config、run_id、步数、整数时间、来源 UUID/revision、
粒子/约束计数、完整粒子数组、有限指标、图像尺寸/完整长度及文件集合。
首次收集保存摘要，重新 verify 必须重新读取文件并比对，能发现缺失/篡改/交换来源。
摘要用于本地完整性与追溯，不提供恶意改写整套证据时的真实性认证。

## 发布与失败契约

索引只拥有新根目录及 index.json；外部产物只读，不复制、不覆盖、不删除。
创建前完成计划/环境校验，拒绝已有根目录；根内路径拒绝越界、链接、Windows reparse point、
大小写别名和缓存目录。单文件上限 16 MiB，索引上限 32 MiB。
登记以根内排他锁保护，读取并核验旧索引/所有已有证据、构造候选、写同目录临时文件并 fsync，
最后 os.replace 为唯一索引提交点；失败保留旧索引和全部产物，清理本次临时文件/锁。
崩溃可遗留锁，先人工核实没有写者再移除；不宣称断电原子性、敌对文件系统隔离或多机并发安全。

后续执行器应记录 accepted/unknown/failed/cancelled 等状态，超时先用票据/run_id 核实，
成功跳过前再次校验证据，重试从零且使用新目录；这些行为本阶段不实现。
后续报告分开数值误差、batch_submit_to_completion、GPU timestamp、端到端时间；
缺失值保持缺失，同环境与跨后端比较分别声明容差，不从摘要推断数值一致。

## 验证计划

标准库测试覆盖稳定顺序/身份、float32 边界、非法组合、预算拒绝、版本/字段拒绝，
真实目录下缺失/篡改/来源错配、重复登记、路径和发布故障保护。真实 CPU-only runner
采集数值证据，真实 GPU runner 使用原 export 五文件，核验配置与源运行绑定。
不运行批量调度/恢复/报告验收，不扩大到无关渲染或全工程测试。

使用入口：[实验管理指南](../guides/experiments.md)、[计划示例](../../examples/automation/cloth-plan.json)。
相关：[Python](automation-python.md)、[模拟服务](physics-api.md)、
[Roadmap](../roadmap.md)、[0089](../development/0089-experiment-plan-index.md)。
