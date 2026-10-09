---
module: physics-api
created_at: "2026-10-08T17:14:07+08:00"
updated_at: "2026-10-09T15:37:04+08:00"
status: accepted
---

# 模拟世界与固定步长

## 目标、边界与依赖

M10.1 建立 Edit/Play 所有权和固定时钟；M10.2 增加可选 [CPU XPBD](physics-xpbd.md) 和连续粒子数据，
不为粒子创建场景实体。独立 [GPU Graph求解](physics-xpbd-gpu.md) 与 [可视化](render-simulation.md) 已提供；M10.4 通过可选 GPU 服务后端接入命令和脚本。
`engine/physics/api` 提供 `dk::physics_api`，PUBLIC 仅 Core；整数纳秒时钟不依赖 Scene、窗口或 GPU。
Framework/Scene 同时开启时装配该模块和 `dk::simulation_services`、`dk::simulation_operations`；
Service PUBLIC 依赖 SceneServices/Physics API/Physics XPBD，Operations PUBLIC 依赖 Commands/SimulationServices，
PRIVATE 复用 SceneOperations 的 guard/state 编解码。Runtime 拥有 Service，注册表先于服务析构。
无新增三方库或依赖 feature。

## 世界与生命周期

SceneService 始终拥有 EditWorld，现有 scene/entity/history/save/load、read_scene 和截图仍指向编辑态。
`simulation.start` 要求当前 EditGuard；先复制拥有型 Project/Scene 快照，再用 SceneDocument::stage
构造独立 PlayWorld，生成非持久 SimulationId，全部成功后发布。Play 保留 SceneId/EntityId，
保存启动时 DocumentState 作为来源；独立 SceneDocument revision 从 0 起，不能作编辑 guard。
只读 Play 快照携带 run_id、来源、时钟、Project 和独立 SceneSnapshot，不暴露可写文档引用。
当前 Play 场景内容保持启动时的值；XPBD求解输出存放在独立粒子数组，不通过 SceneDocument 逐粒子编辑。

旧同步start入口状态为 edit（无 Play）、running、paused；有限任务的扩展状态与边界见下文M11.3。start 默认 running，可用 paused=true 原子进入暂停态。
活动期间再 start 返回 invalid_state；pause/resume 同态为 no-op；step 仅在 paused 下允许。
pause 清除不足一拍的余量，resume 从当前 steady_clock 重新计时，暂停时间不补算。
stop 丢弃 Play，返回 edit；不把 Play 写回 Edit，不调用 undo，不自动保存。无 Play 的控制返回 invalid_state。
所有后续控制和 read_play_scene 必须携带 run_id；过期 ID 返回 conflict，防止旧控制误作用新运行。
编辑可以在 Play 期间继续、撤销、保存甚至 new/load；Play 保持原输入，Stop 保留最新编辑态及历史。
Play 拥有 Project 的值副本，但不固定外部资源文件字节；本节不加载资产；GPU 后端由下述私有实现持有设备资源。
Runtime shutdown 清理 Play；已有截图仍按原编辑快照独立完成/关闭，Stop 不取消截图。

## 固定时钟

FixedStepConfig：fixed_dt_ns 默认 16666667，范围 1000000–1000000000（1 ms–1 s）；
max_catch_up_steps 默认 8，范围 1–64。配置仅在 start 接受，不在运行中改步长。
FixedStepClock 独立接受整数 elapsed_ns，维护 steps、simulated_time_ns、accumulator_ns、dropped_time_ns。
严格单步 count 默认 1，范围 1–10000；每拍推进固定 dt，时间等于 steps * dt，不累加浮点误差。
墙钟推进计算整拍，单次最多 max_catch_up_steps；超出的整拍计入 dropped_time_ns，保留小于 dt 的余量。
因此过载时减慢模拟，不无限追赶；dropped 指墙钟整拍，暂停清除的不足一拍余量不计入其中。
负 elapsed、非法配置/count、纳秒累积或模拟时间 int64 溢出在提交前拒绝，计数保持原样。
默认 solver="none" 每拍仅更新时钟；显式 xpbd_cpu 每拍推进物理，能力发现 solver="xpbd_cpu" 表示已编译实现。
CPU 求解状态与候选时钟成功后一起提交；数值/预算失败均保留旧状态。GPU 提交/失败语义见下文。步长/布片/指标约定见 [XPBD设计](physics-xpbd.md)。

SimulationService 在 owner 线程用 steady_clock 驱动，启动基准在克隆完成后采样，不计入克隆耗时；
start 可显式提供受控时间，pump(now) 支持受控时间测试。
自动推进出错时冻结为 paused 并保存 fault；提交前失败保留时钟，GPU 已提交时保留新时钟；恢复/单步拒绝直到 stop/start。
Runtime::pump 调度模拟及已有异步服务；dispatch 前 pump 意味着 running 查询/失败命令前也可能正常推进。
pause 响应后不再推进。严格 N 步必须 start(paused=true) 后 step(N)，不能先 running 再 pause 期望零拍。
Runtime::next_pump_deadline(fallback) 返回模拟下一拍和宿主截止时间的较早者，Windows stdio、pipe、
jobs.wait 使用它；编辑器沿用逐帧 pump。同步 batch/Luau 只在命令安全点推进；纯脚本长循环不后台模拟。
非 Windows stdio 仍是同步读输入，本节不新增跨平台 reader。

## 命令、持久化与失败边界

七条基础命令 start/pause/resume/step/stop/query/particles 及可选 export 的准确参数与结果见
[模拟命令参考](../commands/simulation.md)（共五条控制加两条查询）。全部不可撤销、不可加入 scene.transaction，
query/particles 为 query effect，export 为 external，其余为 control。start 使用编辑 guard，后续使用独立 run_id。
Luau query 模式可查询模拟；edit/project 可使用模拟控制，export 仅 project，仍受原命令预算约束。
Python 可通过通用 Client.call 调用；M9 Recorder v1 保持场景协议；M10.4 通过独立实验配置和 Python 示例从零重放。
运行配置/计数为会话内状态，不写入场景文件；场景保存不等于模拟检查点，不提供恢复检查点。

服务校验和独立候选构造在发布前完成；普通参数、guard、状态、计时边界错误不破坏 Play/Edit。
JSON 结果分配、OOM 或进程退出不承诺回滚已完成控制，沿用 Runtime 的致命错误边界。

## 验证

整数时间分片一致、余量、过载限制/丢弃计数、负数与溢出无变化；受控时间验证暂停/恢复与 N 步。
使用非空层级/变换场景验证克隆独立、编辑保存/撤销/new/load 后 Play 不变和 Stop 不覆盖 Edit；
验证坏 guard/配置/run_id/状态、重启新身份、shutdown、命令发现及 Luau 能力。
Windows stdio/pipe 真实进程空闲推进测试须超过 catch-up 上限，证明 owner 等待期间持续 pump；
暂停无额外步、Stop 后编辑态一致。M10.4 另运行真实 GPU 命令/脚本及导出验收。

相关：[Runtime](runtime.md)、[Services](application-services.md)、[0074](../development/0074-simulation-world.md)。

## M10.4 GPU 命令与实验文件

启用 RenderSimulation 时 SimulationServices PRIVATE 链接该模块，公开编译能力 DK_SIMULATION_GPU。
CPU-only 保持不创建 Vulkan；GPU start 才延迟创建独立 MemorySystem、Queue 和求解器，Stop/shutdown 排空并释放。
start 增加 xpbd_gpu；CPU 初始求解器保留拓扑/初始固定点供显式回读计量，之后不参与 GPU 步进。
GPU step 每次 1..8 拍，max_catch_up_steps 最大 8；脚本循环分批完成精确 N 拍。
每批提交后等待完成以限制在途资源，但不默认回读或重新上传粒子。提交前失败保留旧时钟；
已提交但等待失败时保留真实提交步数并冻结 fault，不能假称 GPU 状态回滚。query 的 GPU metrics 为 null，
particles/export 是显式同步回读；回读发现数值异常冻结运行。GPU 状态不通过 CPU solver 伪装推进。

新增 external/non-undoable simulation.export，仅接受 paused、无 fault、有布片的 run_id 和匹配的 expected_steps。
仅 graphics 构建注册；支持 CPU/GPU 两个后端。CPU 图像在导出时上传一次当前数组；GPU 直接使用常驻数组。
一次零步 Graph 同时回读粒子和图像，步数不增加。统一目录包含 config.json（完整参数、seed、N、dt、视图）、
metrics.json（N、整数模拟时间及数值指标）、particles.json、image.ppm 和 provenance.json（来源与会话身份）。
config 可从零重放，不是中途恢复检查点；同环境同输入复现数值/像素，跨设备采用容差比较。

输出是 Play 启动项目下的新相对目录，父目录必须存在，拒绝已有目标、根路径、越界和重解析点。
先完成读取/渲染，在同父临时目录序列化并写齐文件，再以目录 rename 为发布点；失败清理本次临时文件，
既有目标和 Edit/步数保持不变。不承诺断电持久性或与外部进程恶意改路径的竞态隔离。
Luau 仅 project 权限允许导出；Python 示例使用 Client.call 从配置分批严格步进并重放，
M9 场景 Recorder v1 的身份归一化/白名单保持原协议，物理实验使用独立版本化配置，不混入墙钟控制记录。
验证真实命令发现、严格 N/暂停/Stop、重复运行、CPU/GPU 容差、导出失败无半成品、Luau 权限和 CPU-only 能力。

## M11.1 观测边界

GPU timestamp/Tracy 由独立队列接口与探针显式启用，见 [性能设计](foundation-profiling.md#m111-gpu-观测)。
本阶段不变更 simulation 命令、时钟或每批同步等待行为；不将脚本总耗时或 wait CPU zone 当成求解 GPU 时间。
长任务控制按 Roadmap 的后续子阶段验收，基线和目标如下。

## M11.2 响应性测量与后续验收目标

测量协议见[性能设计](foundation-profiling.md#m112-模拟基线协议)，基线见[实测报告](../benchmarks/2026-10-09-simulation.md)，
重跑入口见[指南](../guides/simulation-benchmark.md)。直接API时间与真实SDK/Named Pipe端到端延迟分别报告；
当前单连接管道使控制还需等待忙碌连接释放，延迟不是纯owner队列耗时。
冷start和首个批次均同步阻塞owner；同一count/readback组合热运行可复用Graph计划。
这些是M11.2基线所用同步入口的行为；M11.3另增有限任务入口，见下文。

后续M11.3–4以相同设备、RelWithDebInfo、固定三种规模/seed42/dt10ms/迭代12/批次上限8为参考验收环境：

| 观测 | 后续目标 | 完成含义 |
| --- | --- | --- |
| 长任务受理、query/进度、pause和cancel请求 | p95≤100ms、单次≤250ms | 查询可返回初始化中；pause受理与暂停完成分别计时，观察到paused后步数稳定；cancel须真实受理，不能以客户端超时替代 |
| Stop、cancel终态、shutdown至进程退出 | p95≤250ms、单次≤1000ms | 在途工作到达安全完成/故障边界，资源有界回收；Stop保持Edit，已提交工作不伪装撤销 |
| GUI事件循环心跳间隔 | p95≤50ms、单次≤100ms | 在冷初始化和持续实验期间采样单调时钟；与呈现帧率分开验收 |

100ms请求预算给本机SDK/dk-ctl约30ms空闲成本留出调度和短批次余量；250ms/1000ms是
外部调用/排空的有界上限目标，不是跨设备硬实时承诺。冷初始化shader总耗时可以更长，
但不能因此阻塞查询、受理或事件循环；需要异步状态与安全发布，具体实现见下文M11.3设计。

M11.4每个规模分别覆盖进程首次初始化、首次Graph编译和稳定实验，保存全部成功、超时、错误及重叠证据。
热query/进度至少100个请求；pause/cancel/stop/exit以及冷场景每项至少20次独立试验，
分位数采用nearest-rank并另报max与样本数。任意超时/丢响应算失败，不能只统计成功样本。
冷busy夹具须在请求写入后再发控制；若CPU短批次没有重叠，另用长任务覆盖，不能声称验证了中途取消。
异步命令接入后同步更新夹具，明确受理、提交、完成、终态边界，不能照搬同步start/step返回假设。
UI心跳必须在实际GUI事件循环测量；M11.2只建立该目标，没有GUI响应性验证。

数值继续要求精确N拍、位置最大分量差≤2e-3m、速度≤2e-2m/s、固定点不动、无地面穿透，
暂停完成后稳定、Stop前后Edit相同；性能改动不得放宽这些条件或依赖禁用validation才能正确。

## M11.3 有界有限步任务

新增simulation.run(guard, count, solver=xpbd_cpu, cloth, fixed_dt_ns=10000000, batch_steps=8)，
创建独立Play并受理有限实验，count为1..1000000、batch_steps为1..8，只支持CPU/GPU XPBD。
沿用run_id标识唯一活动实验，simulation.query返回task进度；它不是Commands的TaskId或Foundation的JobId。
同一Service只允许一个Play，无额外排队；终态在Stop前保留一个摘要和后端，不保留无限历史。
旧start/step的同步返回语义保持兼容，长实验和冷初始化使用run；GUI与旧入口的响应性集成归M11.4。

owner完成参数/guard/初始CPU布片校验和Scene克隆后才发布候选Play。专属worker拥有并创建/销毁求解后端，
GPU设备、shader编译、首批Graph编译和后续录制不阻塞owner。worker不访问Scene、Runtime、命令注册表或stdout。
选择单个持久worker是因为GPU后端的ThreadContext和设备生命周期要在同一线程结束，不扩建通用Jobs调度器。
一个任务最多一个活动批次、一个GPU提交和一个显式诊断请求；GPU推进后以零超时轮询完成，未完成期间保活frame/资源，
不默认回读粒子。内部进度和异常通过互斥保护的单份快照传递；owner的pump发布到Play时钟。
owner等待截止时间最多5ms后检查任务，不依赖后续命令才完成发布；未在worker设置ECS或公开时钟。

task包含目标/已提交/已完成步数、批次上限、是否有活动批次、初始化是否完成、状态；错误通过run.fault发布。
状态为initializing/running/pausing/paused/cancelling/cancelled/succeeded/failed/stopping。
公开steps对任务表示已完成步数，submitted_steps可能领先最多batch_steps；simulated_time_ns按完成数计算。
CPU推进成功后在同一批次发布提交/完成（跨两次锁观察时也可能短暂看到提交领先）；GPU提交成功即更新提交数，只有实际完成轮询成功才增加完成数。
提交前错误不增加计数；提交后等待/设备失败保留真实提交数、旧完成数并冻结failed，禁止继续推进。
普通Error成为任务fault，基础设施异常保留exception_ptr并由owner重抛，不伪装成可恢复业务失败。

pause/cancel/stop不等待冷编译：返回pausing/cancelling/stopping表示受理。
已被worker领取的一个批次可以提交/完成，之后不得领取下一批；不会假装撤销已提交GPU工作。
paused/cancelled仅在当前批次完成且初始化安全点到达后发布；只有paused状态才表示暂停完成、步数稳定。
resume只恢复未终结的paused任务，保留剩余目标，不追赶墙钟；cancelled/succeeded/failed不能恢复。
cancel保留已完成结果可显式读回，重复取消终态幂等；pause/resume不能撤销已接受cancel/stop。
cancel与完成竞争由同一互斥边界决定：完成先发生则succeeded，取消先被接受则cancelled，可已完成全部目标。
Stop先置stopping，worker完成当前批次并在所属线程销毁GPU/context后，owner才清除Play返回edit；期间拒绝新实验。
shutdown拒绝后续命令、请求关闭并join，不detach、不持锁join；不可抢占第三方编译/驱动调用，不承诺硬截止。

particles/export只允许已初始化、无fault、稳定paused/succeeded/cancelled状态，由有界单请求通道在worker执行诊断。
诊断还要求owner已发布稳定边界，不能把后台新粒子附到旧完成步数；未发布时拒绝，等待下一次pump。
这些显式读回/文件操作仍同步；query/控制不会通过读回获取进度。导出保持原目录发布和Edit保护契约。
取消初始化尚未建立有效后端时不能读回；Stop/关闭仍安全。输入校验失败不发布Play；异步初始化失败保留failed状态供查询。

验证：可控假后端闸门验证初始化/提交前后暂停取消、完成竞争、故障冻结、单在途上限、过期run_id和关闭；
真实CPU/GPU有限300拍、暂停稳定/取消上界/Stop保护/冷受理/空闲发布和退出，核对数值容差、命令发现与CPU-only。
不以本阶段功能验证替代M11.4的大样本GUI/IPC响应性复测。M11.2“pause回复后稳定”对任务明确为观察到paused终态后稳定，
受理延迟和到达安全边界的延迟分开测量。
