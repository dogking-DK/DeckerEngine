---
module: foundation-jobs
created_at: "2026-09-22T18:20:46+08:00"
updated_at: "2026-09-28T15:29:44+08:00"
status: accepted
---

# M4 CPU 工作队列与后台任务设计

## 目标和边界

用于 M4.4 的本地 CPU 导入/加载。M4.4.1 已实现 `engine/foundation/jobs` target
`dk_jobs / dk::jobs`，依赖 Core、Memory 和标准库线程设施，私有使用 Profiling；不依赖 Scene、资产、JSON、Runtime 或 GPU。
首版一个可关闭的 worker + 有界队列，std::jthread/stop_token/条件变量；不建设协程、任务图或 work stealing。
它支持协作取消，不保证能抢占三方解码器或阻塞的系统 IO。
前置为 M1.7 [内存系统](foundation-memory.md) 与 [Tracy](foundation-profiling.md)，
目前已提供 heap、拥有型接口、scratch/pool、RoutingToken/ThreadContextCache 和采集探针；
M1.7.7 已完成[重复工作负载基线](../benchmarks/2026-09-28-memory.md)，可用于后续接入前后的定向比较。
submit 自动捕获当前系统/持久域的拥有型 RoutingToken；队列本身使用独立资源。
每个 worker 在自身线程通过 ThreadContextCache 按不复用的 SystemId 建立 ThreadContext，执行前绑定 ExecutionScope 和 ScratchScope，
结束/异常后恢复旧路由；业务回调无需逐层接收 thread/heap。token 不包含提交线程的 arena 或 context，
系统已 Closing 时不再启动任务；queued 取消直接释放 token。跨 Runtime 共用 worker 不得残留上一个任务的域。
共享队列和 completion
使用持久 heap/SharedPool。完成数据拥有资源 handle，不携带 worker scratch/local pool 的借用地址。
输入快照同样拥有其存储，取消不能提前回收仍被 worker 访问的输入。
主线程、worker 使用明确线程名称，排队/执行/发布分别埋点并关联 JobId，RAII zone 不跨线程闭合。

## 身份、接口与状态

新增 stduuid-backed `JobId`，与 M3 已完成命令的 `TaskId` 分开。TaskId 表示“提交作业这条命令”的
执行记录，JobId 表示后台作业；提交命令成功不表示资产成功。保留 M3 的响应 envelope 和同步任务查询语义。

概念接口：`submit(work)`、`request_cancel(id)`、`query(id)`、`wait(id,deadline)`、
`drain_completions()`、`close()`。实际公开签名见 `dk/jobs/JobQueue.hpp`；拒绝提交时返回 Result 错误，不分配可查询 JobId。
work 接收 stop_token，返回拥有结果的 completion；用户回调不在队列互斥锁下运行。

状态为 queued → running → succeeded/failed/cancelled；pending publication 属于 running，
资产消费层处理完成数据并确认发布/拒绝后，才进入对应终态。通用队列不得自行声明资产 Ready。
取消是请求标志，不能仅因设置标志就把仍使用输入内存的 worker 当作已退出。

| 场景 | 约定 |
| --- | --- |
| queued 取消 | 移除待执行项，进入 cancelled，不调用 work |
| running 取消 | stop_requested；worker 在读文件/解析/解码/写入阶段之间检查 |
| 取消与完成竞争 | 调用者线程串行决定发布；取消先被接受则拒绝发布，终态成功后取消为幂等查询 |
| 等待超时 | 返回当前状态及 timed_out，不隐式取消，不能理解为操作没有执行 |
| 队列满 / closing | 明确拒绝提交，不阻塞 Runtime 分派线程 |
| 未知/已淘汰 JobId | not_found，不凭 UUID 推测状态 |

作业状态和大数据所有权分开：后台结果通过有界 completion 通道移交；记录仅保留进度阶段、
结果摘要或错误，不保留无限增长的网格/图片。活动作业不可淘汰，终态按有限数量保留。
队列长度、总活动数、终态保留数和输入字节预算须在 M4.4.1 公共 Limits 中明确并测试。
普通 dk::Error 归入 failed；异常在线程边界收敛并通知宿主，资源耗尽不能被包装成可继续正常运行的业务失败。

### M4.4.1 固定接口

`JobQueue(resource, JobLimits)` 在创建线程作为 owner；submit/cancel/drain/close 仅 owner 调用，
query/wait/change_sequence/wait_change 可跨线程。默认 queued=16、active=17、terminal=256、
input_bytes=512 MiB；每次 submit 显式申报所拥有输入的字节数，合计不超过预算，不能靠零申报绕过。
队列不反射任意 C++ closure，调用者负责准确申报；资产适配另有文件/产物限额。
`Work(stop_token) -> Result<shared_ptr<const void>>` 只返回拥有型数据；
`drain(Consumer(JobId, payload) -> Result<string>)` 在 owner 确认发布，摘要最多 4096 字节。
错误正文/上下文也截断为有限元数据。completion 数量由 active 限额约束；终态释放 payload、work、token。
无消费者或取消的完成项不调用发布。owner 串行化取消/发布，不允许 consumer 重入队列写操作。
wait 不消费完成项；宿主用 change sequence + deadline 等待并 drain，不丢失完成唤醒。
worker 每次安全点清空自己的 ThreadContextCache（包括闲置前），同系统不同域可重新装配 scratch。
所有未知异常（包括 bad_alloc）保存 exception_ptr，并停止接收；owner drain/rethrow_failure 原样重抛，
不能将基础设施异常伪装成业务失败。close 不抛出，取消并 join，释放所有未发布拥有值。

## 主线程发布与关闭

SceneService 和 Runtime::dispatch 继续单线程调用。worker 不持有它们的裸引用，不触碰 ECS、
命令注册表或 stdout。资产结果携带目录会话/generation，晚到结果由所有者拒绝，见
[资产运行时](assets-runtime.md)。

Runtime 在没有新 stdin 请求时也会收集完成项。M4.4.3 已将 Windows stdio 读取改为有界输入队列：
读取侧只接收行，Runtime 所在线程等待“输入/完成/关闭”事件并串行分派/发布。
不能在每次来一条命令时才让作业完成，也不能让 worker 绕过安全点写业务状态。
Windows 读取取消/EOF 唤醒方案在该小节先做最小进程验证，再接入业务，禁止用 detach 隐藏退出问题。

关闭顺序：拒绝新提交 → 请求取消排队/执行中作业 → 唤醒输入和等待者 → 回收 worker/completion
→ 销毁资产服务 → 销毁队列。registry 中捕获的服务引用必须先失效/解除；不得持锁 join。
worker 退出前在所属线程销毁局部对象及 ThreadContext；其移交的 owning 结果可继续存活。
系统退休时在 worker 安全点清除该系统的 context 缓存和路由 lease，避免闲置缓存永久阻止 memory 关闭。
MemorySystem 在队列、服务和 completion 释放后关闭，Tracy client 晚于这些释放事件；
外部仍持有结果时遵循 Memory 的 Closing/延迟释放规则，不能强制清空 heap。
正常 EOF 和 runtime.shutdown 共用关闭流程；stdout 响应先刷新，不自动保存场景。
在有限本地输入、可完成 IO 下验证可退出；无法协作中断的解码/IO 可能延迟 join，不能承诺硬截止或强杀线程。

## M4.4 命令接入

M4.4.3 已注册下表命令，另有 assets.open/catalog 管理独立目录会话；精确参数见 [资产](../commands/assets.md) 与 [作业](../commands/jobs.md)。
实现时复用 AssetService/Operations 和现有 schema，不为每条资产命令写 JSON-RPC 特殊分支。

| 命令 | 参数/结果方向 | effect |
| --- | --- | --- |
| assets.import | 源路径与规范 settings → JobId；终态结果含 root_id，写 meta/缓存，无需活动 Scene | external |
| assets.load | 已登记 AssetId → JobId/既有 Ready 摘要；不返回大块字节 | control |
| assets.status | AssetId → 状态、generation、产物摘要/错误 | query |
| assets.unload | AssetId → 当前状态；已有消费者仍拥有原数据 | control |
| assets.register / assets.rename | 活动工程资产目录 guard + 候选映射/同目录新名称 → 新目录状态 | external |
| jobs.get / jobs.wait / jobs.cancel | JobId，wait 带有界 timeout → 状态/超时或取消是否接受 | query / query / control |

所有上述操作 undoable=false，不加入 scene.transaction。资产目录 guard 使用目录会话 ID/revision；
不复用 Scene revision 保护清单变化。详细参数、分页、限额和 result schema 已同步到
[资产命令](../commands/assets.md)、[作业命令](../commands/jobs.md) 及 discovery/runtime 能力说明。
`runtime.capabilities` 已增加 async_jobs 与作业限额；既有 async_tasks=false 保持其同步 TaskId 语义，
不得改成 true 却继续返回仅有同步终态的 tasks.get。

jobs.wait 在 Runtime 线程等待期间必须继续收集完成项，但不重入分派其他命令；
同一串行连接不能同时发起长 wait 再期待后续 cancel 即时执行，文档须推荐短时轮询或先取消再等待。
超时不是作业失败。首版不新增网络连接或异步 JSON-RPC envelope。

## 验证计划

M4.4.1 使用可控闸门验证队列满、queued/running 取消、等待超时、异常终态、关闭唤醒和回收；
避免依靠随机 sleep 判定竞态。M4.4.2 验证旧代结果不能发布及 Ready/终态一致性。
M4.4.3–4 验证命令 schema/发现和真实 stdio：stdin 保持打开时后台可完成，
导入失败后还能查询、shutdown/EOF 不悬挂、stdout 无日志、重启后仅持久 ID 保留。
仅运行对应 jobs/assets/受影响 Runtime 用例；不要求每节全量或双配置。

关联：[M3 Runtime](runtime.md)、[协议](automation-protocol.md)、
[开发安排](../development/0020-m4-development-plan.md)。

M4.4.4 确认诊断/摘要裁剪会释放多余 capacity，并保持 UTF-8 截断边界；close 将活动作业转为终态时仍执行保留上限。独立 CPU 和完整进程验收见 [0040](../development/0040-cpu-assets-delivery.md)。
