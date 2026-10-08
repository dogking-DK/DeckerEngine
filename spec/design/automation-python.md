---
module: automation-python
created_at: "2026-10-08T16:00:00+08:00"
updated_at: "2026-10-08T16:21:00+08:00"
status: accepted
---

# Python 自动化客户端

## 目标与边界

M9.3 在 `sdk/python/decker` 提供 Python 3.11+ 标准库客户端，通过已有 Windows
`dk-ctl` 连接 runner 或编辑器的 Named Pipe。源码目录加入 PYTHONPATH 即可使用，
不增加 Python/C++ 三方依赖、CMake 引擎 target 或协议分支；不嵌入 Python、不直连 ECS。
接口复用 [客户端](automation-client.md)、[协议](automation-protocol.md) 与
[截图](render-capture.md)。本阶段不包含 M9.4 记录/重放、远程网络、异步 Python API 或进程托管。

## 接口与数据

- `Client(endpoint, executable="dk-ctl", timeout=5)`；`hello()` 返回发现对象；
  `call(method, params, timeout=..., retry=Ticket(...))` 返回 Reply，保留完整输出、
  session/request_id、TaskId 和 value。timeout 单位为秒，单次范围 0.001–60。
- `Command(method, params)` 用于 `batch(commands, timeout=...)`：1–128 条顺序调用，
  第一次错误停止，BatchError 保留已成功 Reply、失败索引和原因；全批共享截止时间。
  无隐式 guard、无回滚。`transaction(guard, commands)` 映射 scene.transaction，
  由服务保证一次 revision/撤销和失败原子性，不将普通 batch 伪装为事务。
- `task(task_id)` 查询同步 TaskRecord；`wait_job(job_id, timeout=30)` 用 jobs.wait
  短轮询，共享整体截止时间，每次 server wait 最多 1000ms 且短于客户端调用预算。
  succeeded 返回完整 Job，failed/cancelled 抛 JobError；未知/淘汰 ID 保留 RpcError。
  WaitTimeout 保留 JobId 和最后快照，既不重交任务也不隐式取消；显式取消使用 jobs.cancel。
- `capture(params, timeout=60)` 共享提交和等待的截止时间，返回 Capture（提交 Reply、
  成功 Job）。核对 document_id/scene_id/revision/frame/尺寸/output 与提交完全一致。
  等待阶段失败或超时保留 submission，便于继续等待或查询。`Capture.collect(project_root)`
  在调用者指定的本地工程根内解析输出文件，验证 P6 RGB8 的尺寸与完整长度，返回
  Artifact（绝对路径、尺寸、字节数、SHA256）；不创建目录、不复制文件、不隐式保存场景。
  根内解析拒绝路径越界；这不是敌对文件系统的沙箱。共享同名输出可能被后续任务覆盖，
  示例采用唯一文件名；元数据校验不能证明外部修改后的文件仍是原任务内容。

## 生命周期、截止时间与失败

Client 不持有连接/worker/Runtime，也不隐式启动或关闭服务。每次调用创建独立 UTF-8
无 BOM 参数临时文件，以参数列表和 shell=false 启动 dk-ctl，Windows 隐藏窗口；
结束后回收子进程和临时目录。参数在启动前检查 JSON 原生类型、有限数值、整数范围、
深度 64/节点 200000 和 1MiB 大小；共享字符串按每个出现位置累计逻辑字节，
在 JSON 序列化前拒绝超量，避免重复引用放大。响应必须符合 dk-ctl 输出及 Task envelope，
有限 JSON 数值和 Job 状态/字段，不能将非法响应当成功。

单调截止时间覆盖参数准备、子进程启动后的等待和通信；传给 dk-ctl 的毫秒预算不超过剩余值。
操作系统进程创建/文件 IO/kill 后回收本身不承诺硬实时上限。Python 截止时只终止自身
dk-ctl 子进程并回收；已发送命令/后台 Job 可能继续执行。收到完整输出则保留
not_sent/unknown/received 与 ticket；无可靠输出的子进程超时/异常退出保守标 unknown
（hello 不执行业务，保持 not_sent）。启动失败为 not_sent。

异常分 ClientError、TransportError/CallTimeout、RpcError、BatchError、JobError、
WaitTimeout、ArtifactError；业务异常保留 JSON-RPC code/message/data 和 TaskId。
不自动重试；显式 retry 必须由用户提供原 ticket、方法和参数，由服务检查一致性。
SDK 不缓存或重写 Scene guard；成功编辑的提交点仍在 Services，超时不会撤销已提交状态。

## 验证计划与取舍

标准库 unittest 验证参数拒绝、错误映射、非法输出、真实子进程超时与回收、整体期限、
批量部分成功、等待终态、截图身份和文件检查。真实 CPU runner 验证 Unicode、整数、
显式重试/过期 session、guard、事务/普通批量失败、保存重载与 TaskId；真实 GPU runner
验证批量编辑后截图、JobId 等待、产物尺寸/颜色/摘要及失败保留。CTest 在找到 Python3
解释器且有 dk_run/dk_ctl 时注册，GPU 用例要求 render capture；缺失 Python 明确提示。
不开启引擎全量或 Release 矩阵，不修改已有命令 schema。

## 相关记录

[0072 M9.3 实现与验收](../development/0072-python-automation.md)。
