---
module: physics-api
created_at: "2026-10-08T17:14:07+08:00"
updated_at: "2026-10-09T10:46:49+08:00"
status: accepted
---

# 模拟世界与固定步长

## 目标、边界与依赖

M10.1 建立 Edit/Play 所有权和固定时钟；M10.2 增加可选 [CPU XPBD](physics-xpbd.md) 和连续粒子数据，
不为粒子创建场景实体。GPU Graph 和模拟可视化在 M10.3 设计。
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
Play 拥有 Project 的值副本，但不固定外部资源文件字节；本节不加载资产、不创建 GPU 资源。
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
求解状态与候选时钟成功后一起提交；数值/预算失败均保留旧状态。步长/布片/指标约定见 [XPBD设计](physics-xpbd.md)。

SimulationService 在 owner 线程用 steady_clock 驱动，启动基准在克隆完成后采样，不计入克隆耗时；
start 可显式提供受控时间，pump(now) 支持受控时间测试。
自动推进出错时保留时钟，冻结为 paused 并保存 fault；恢复/单步拒绝直到 stop/start。
Runtime::pump 调度模拟及已有异步服务；dispatch 前 pump 意味着 running 查询/失败命令前也可能正常推进。
pause 响应后不再推进。严格 N 步必须 start(paused=true) 后 step(N)，不能先 running 再 pause 期望零拍。
Runtime::next_pump_deadline(fallback) 返回模拟下一拍和宿主截止时间的较早者，Windows stdio、pipe、
jobs.wait 使用它；编辑器沿用逐帧 pump。同步 batch/Luau 只在命令安全点推进；纯脚本长循环不后台模拟。
非 Windows stdio 仍是同步读输入，本节不新增跨平台 reader。

## 命令、持久化与失败边界

七条命令 start/pause/resume/step/stop/query/particles 的准确参数与结果见
[模拟命令参考](../commands/simulation.md)（共五条控制加两条查询）。全部不可撤销、不可加入 scene.transaction，
query/particles 为 query effect，其他为 control。start 使用编辑 guard，后续使用独立 run_id。
Luau query 模式可查询模拟；edit/project 可使用所有模拟控制，仍受原命令预算约束。
Python 可通过通用 Client.call 调用；M9 记录重放白名单本节不扩大，实验记录在 M10.4 接入。
运行配置/计数为会话内状态，不写入场景文件；场景保存不等于模拟检查点，不提供恢复检查点。

服务校验和独立候选构造在发布前完成；普通参数、guard、状态、计时边界错误不破坏 Play/Edit。
JSON 结果分配、OOM 或进程退出不承诺回滚已完成控制，沿用 Runtime 的致命错误边界。

## 验证

整数时间分片一致、余量、过载限制/丢弃计数、负数与溢出无变化；受控时间验证暂停/恢复与 N 步。
使用非空层级/变换场景验证克隆独立、编辑保存/撤销/new/load 后 Play 不变和 Stop 不覆盖 Edit；
验证坏 guard/配置/run_id/状态、重启新身份、shutdown、命令发现及 Luau 能力。
Windows stdio/pipe 真实进程空闲推进测试须超过 catch-up 上限，证明 owner 等待期间持续 pump；
暂停无额外步、Stop 后编辑态一致。当前不运行求解器/GPU 测试。

相关：[Runtime](runtime.md)、[Services](application-services.md)、[0074](../development/0074-simulation-world.md)。
