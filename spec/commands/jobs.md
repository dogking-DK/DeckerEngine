---
module: jobs-command-reference
created_at: "2026-09-28T15:02:00+08:00"
updated_at: "2026-09-28T15:02:00+08:00"
status: accepted
---

# 后台 CPU 作业

JobId 与同步命令 TaskId、JSON-RPC id 各自独立。提交命令返回的 TaskId 为 succeeded，只说明作业被接受。
通过 JobId 查询准备/发布状态；无需活动 Scene 或目录 guard。所有命令不可撤销、不进入事务。

| 命令 | 参数 | 返回 | effect |
| --- | --- | --- | --- |
| jobs.get | id（必填非 nil JobId） | Job | query |
| jobs.wait | id；可选 timeout_ms=0，整数 0–1000 | `{job,timed_out}` | query |
| jobs.cancel | id | `{job,accepted}` | control |

Job 为 `{id,state,cancel_requested,result,error}`，state 为 queued/running/succeeded/failed/cancelled。
成功 result 为 `{root_id,key,cache_hit}`，其余为 null；失败 error 为 `{code,message,context}`，其余为 null。
准备完成但尚未主线程发布仍为 running；succeeded 表示发布已经完成。
queued 取消不执行 work；running 取消请求协作停止，保留输入直到 worker 退出。
accepted=true 后不得发布；已经终态时 accepted=false，保持原终态。取消请求不保证抢占三方解码器/系统 IO。

wait 超时仅返回快照及 timed_out=true，不取消、不代表失败。wait 在 owner 线程继续消费完成项，
不会重入其他命令；同一串行连接先长 wait 后 cancel，cancel 需等该 wait 返回。
推荐短 wait 轮询或先 cancel 再 wait；最大 1000 ms。stdin 保持打开且没有下一条请求时，Runtime 也会处理完成项。

默认 queued=16、active=17、terminal=256、input_bytes=536870912；资产作业各预留 134217728 字节，
因此输入预算可能先于数量耗尽。满队列立即 invalid_state，不分配可查询 ID。
只淘汰已终态记录；未知/已淘汰 ID 返回 not_found，重启后的旧 JobId 也为 not_found。
基础设施异常导致 runner 退出 3，不伪装成可恢复 failed 作业；正常关闭等待 worker 回收，没有硬性退出截止保证。

```json
{"jsonrpc":"2.0","id":10,"method":"jobs.wait","params":{"id":"<job_id>","timeout_ms":1000}}
{"jsonrpc":"2.0","id":11,"method":"jobs.get","params":{"id":"<job_id>"}}
{"jsonrpc":"2.0","id":12,"method":"jobs.cancel","params":{"id":"<job_id>"}}
```

将 `<job_id>` 替换为 assets.import 或 assets.load 返回值；TaskId 不可代用。
