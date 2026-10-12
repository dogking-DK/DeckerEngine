---
module: experiment-management
created_at: "2026-10-10T16:08:00+08:00"
updated_at: "2026-10-12T10:44:00+08:00"
status: accepted
---

# 实验管理

## 目标与边界

M13.1 在 Python 标准库 SDK 中增加离线计划展开和本地产物核验。入口为
`decker.experiments`（计划）与 `decker.experiment_index`（索引），不创建 Runtime、
不修改 C++ 命令及 M9 Recorder 格式，不增加依赖。
M13.2 的 `decker.experiment_batch` 通过现有 Client/有限任务实现串行执行与进度；
不创建/关闭宿主或编辑场景。M13.3增加任务恢复与显式重试，M13.4再接比较报告。

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
配置仍保留固定视图用于未来对照；M13.2 执行器通过 sample/particles 自动收集该格式。
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
Windows写者改用进程寿命mutex，强杀后可重新取得；历史版本遗留.index.lock仍须人工核实后移除。
其他平台离线登记沿用文件锁；不宣称断电原子性、敌对文件系统隔离或多机并发安全。

执行器状态及失败边界见下节，恢复契约见M13.3节；重试从零且使用新目录，不自动重跑失败实验。
后续报告分开数值误差、batch_submit_to_completion、GPU timestamp、端到端时间；
缺失值保持缺失，同环境与跨后端比较分别声明容差，不从摘要推断数值一致。

## 验证计划

标准库测试覆盖稳定顺序/身份、float32 边界、非法组合、预算拒绝、版本/字段拒绝，
真实目录下缺失/篡改/来源错配、重复登记、路径和发布故障保护。真实 CPU-only runner
采集数值证据，真实 GPU runner 使用原 export 五文件，核验配置与源运行绑定。
M13.2另验证真实单次CLI多组合执行、完成后回到edit、同Runtime跨进程互斥、进度/产物一一对应，
以及失败/取消/超时/采样错配/发布失败不启动下一项。恢复验证增加真实批处理进程强杀、
重新启动后跳过成功项、接回原任务、跨文件发布中断、明确重试和取消证据保护；报告另行验收。

## M13.2 串行执行与进度

`run_plan(client, plan, project_root=..., output=..., experiment_timeout=600,
poll_interval=0.05, runner_executable=None, progress=None)` 从新相对目录开始一个批次；
CLI为 `python -m decker.experiment_batch run <plan> --pipe ... --project-root ... --output ...`。
`read_progress(directory)` / CLI status读取已提交的进度并核对已成功项的索引证据。
先完整展开计划、校验额度/路径，再检查 capabilities、当前编辑场景和空闲Play。
所有计划的GPU/图像能力一起预检，失败无输出目录和引擎状态修改。project_root必须由调用者
指定为宿主工程根；已有协议不暴露该路径，图像路径另以export回执及本地证据核对。

仅Windows本机Named Pipe。用hello的session UUID命名Windows mutex，生命周期覆盖整个批次，
同Windows登录会话内合作执行器即使选择不同输出目录也不能争用同一Runtime；忙时立即拒绝。
mutex不阻止GUI/外部脚本，要求独占模拟控制。批次固定初始scene guard/source，后续编辑变化
导致停止调度；不自动刷新guard、不改Edit、不创建场景。每次控制均使用收到的run_id，
核对响应session及运行配置/来源；外部换运行或重启宿主不能被记作当前实验成功。

正步数使用simulation.run(count=N,batch_steps=8)，查询completed_steps直到succeeded；
0拍通过run(count=1,paused=true)只异步准备，initialized且稳定paused/0拍即达到采集条件。
保留引擎task_status=paused，不伪称它的有限目标已succeeded。每项有独立单调截止时间，
覆盖受理、轮询、采集、登记和Stop；调用预算不超过客户端上限与剩余时间。
记录submitted/completed/batch_active而非将已提交步数当成完成数；采集前要求无fault且无在途批次。

image=false通过simulation.sample和有界particles分页收集，同页身份/步数/整数时间/索引/总数
必须一致，末尾再次查询并核对同一稳定run。四文件先写同父临时目录、fsync并核验，rename发布。
image=true使用simulation.export的原五文件目录，核对回执输出/run_id/步数/文件集，
再核对配置及provenance的初始场景来源。两种路径均登记到原index.json。
初次结果目录为results/<experiment_id>；新批次不覆盖现有输出，已有批次须走resume入口。

`DeckerExperimentBatch` 的batch.json与index.json分离，含batch_id、plan_id、
session/endpoint、源场景、状态、更新序号、时间、每项目标/已提交/已完成拍数、run_id、
受理TaskId/票据、最后请求/响应摘要、artifact_attempt_id和错误。顶层状态为running、
succeeded、failed、cancelled或unknown；项状态为pending、submitting、running、collecting、
stopping、succeeded、failed、cancelled或unknown。每次变化先写临时文件并替换batch.json，
再调用progress回调；CLI把紧凑JSON进度写stderr，最终结果写stdout。
读者只读原子快照；Windows按不打开目标的规范化绝对路径命名短时mutex，序列化路径属性检查、打开/读取/关闭和最终rename，
避免查询句柄阻止os.replace。准备/编码/写临时文件不持该锁，等待上限5秒；其他平台沿用rename语义。
成功项须与index对应attempt/run_id吻合；待执行项不冒充零值结果。

成功发布顺序为产物目录 → index登记 → Stop且query确认edit → batch项succeeded → 下一项。
三者不构成跨文件事务：磁盘错误或进程退出可留下有效产物/索引而进度未确认，保留这些证据，
不删除、不继续调度。先记录submitting/请求意图再发命令；正常返回保存票据/TaskId/run_id，
unknown传输错误保存可用票据及最后快照。已受理任务的超时、断连或中断不隐式取消/Stop/重试。
明确引擎failed/cancelled分别落状态；其余项保持pending。普通采集/写入失败保留活动run供检查。
KeyboardInterrupt保守记unknown，强杀可能留下非终态，恢复时须核实。
progress回调失败停止调度，不撤销已经提交的文件或运行状态。记录写入本身失败时返回异常并保留
最近可读快照，不能保证落盘失败时仍写入终态；恢复把这类非终态视为需要核实。

## M13.3 恢复与显式重试

`resume_plan(client, project_root=..., output=..., retry_failed=False, ...)`及CLI
`experiment_batch resume --pipe ... --project-root ... --output ... [--retry-failed]`恢复已有批次。
先规范化已有根目录别名、以输出路径mutex排除同目录写者，再完整校验batch/index/所有已登记文件；损坏或篡改在修改之前拒绝。
仍使用Runtime session mutex排除不同输出目录争用。Windows索引登记也使用进程寿命mutex，
放弃的mutex可重新取得，不因强杀遗留本版本的文件锁。互斥不约束非合作外部控制者。

新batch格式为v2：每项增加持久的run请求request及history数组；不被后续query覆盖请求票据。
history保存被显式重试的failed/cancelled尝试完整快照，最多127个旧尝试，当前最多第128次。
首目录沿用results/<id>，第n次为results/<id>-retry-<n>；分配前拒绝已有目录，
同一batch快照原子提交旧尝试归档与新pending尝试。旧数值/图像/索引登记不删除、不覆盖。
v1进度仍可只读核验；resume在实际继续时内存升级并原子写v2，缺失的请求只能从已有submission/last_call提取。
plan/index格式不变，所有历史来源/票据仍绑定原session和scene。

成功前缀每次重新核验文件摘要及attempt/run_id后直接跳过；全部成功时无需连接引擎。
其余恢复要求同一session、endpoint与完整scene source，先查询simulation状态。
已知run_id必须匹配当前run并验证配置、来源、目标、完成计数；继续等待原任务，不重新simulation.run。
缺失run_id时，只允许用持久化的原ticket和相同run参数完成原请求的去重调用，取得原TaskId/run_id；
过期/丢失票据、结果不可交付、session改变、其他活动run均不能猜测归属或换新票据重发。
核实调用自身的not_sent不能推断原命令未发送，仍保留原票据与unknown。
无票据的发送窗口崩溃保持unknown，需人工核实；不把相同配置的其他run认作当前实验。
恢复有新的等待期限，仍不保证IO硬实时；原任务的超时不触发自动取消。

已发布完整目录先核验并登记；索引已登记但batch未记录attempt时按唯一目录/run_id重新绑定。
这些步骤不重新导出或覆盖目录。Stop已执行而回执/进度未提交时，只有完整产物证据且当前edit，
才能确认成功。已在stopping时仅继续查询等待，避免再次Stop与转为edit的竞争。
已知run消失且无产物的尝试记failed；无run身份的未知发送仍是unknown。
不完整/错来源的遗留目录保留并记失败，由显式重试分配新目录；私有临时目录不充当成功证据。

resume不自动重试failed/cancelled，取消始终保留为cancelled；后续查询失败可使批次unknown，
但不能覆盖已确认尝试的failed/cancelled及原错误。
`--retry-failed`显式包含失败及取消项：先核实已知run或确认空闲，必要时Stop本人已绑定的运行并等到edit，
再原子归档旧尝试，从零运行新尝试；不得Stop不属于当前尝试的run。
unknown必须先核实成可继续的同一运行或明确失败/取消，不能凭此开关直接重试。
成功项不因重试而重跑；后续pending按原计划执行，不能跨过未解决项。
宿主重启和场景身份改变拒绝自动迁移；此功能恢复批处理清单与现有任务，不恢复模拟检查点。

无新命令或依赖；验证入口与使用示例见[0090](../development/0090-experiment-batch.md)。

使用入口：[实验管理指南](../guides/experiments.md)、[计划示例](../../examples/automation/cloth-plan.json)。
相关：[Python](automation-python.md)、[模拟服务](physics-api.md)、
[Roadmap](../roadmap.md)、[0089](../development/0089-experiment-plan-index.md)。
