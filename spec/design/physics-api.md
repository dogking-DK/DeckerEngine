---
module: physics-api
created_at: "2026-10-08T17:14:07+08:00"
updated_at: "2026-10-09T12:09:00+08:00"
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

状态为 edit（无 Play）、running、paused。start 默认 running，可用 paused=true 原子进入暂停态。
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
