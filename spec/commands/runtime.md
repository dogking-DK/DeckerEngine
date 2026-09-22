---
module: command-reference-runtime
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# Runtime 与任务命令

[返回命令目录](README.md)。本页命令均无需活动场景或 guard。
实现：[Runtime.cpp](../../engine/framework/runtime/src/Runtime.cpp)。

## runtime.capabilities

参数 `{}`；effect=query，undoable=false。
返回当前能力对象：protocol="jsonrpc-2.0-jsonl"、async_tasks=false、task_retention=256、
max_line_bytes=1048576、max_batch_requests=128、transactions=true、guard="document_id+revision"。
客户端应查询能力，当前任务同步完成，没有 wait/cancel 命令。

```json
{"jsonrpc":"2.0","id":19,"method":"runtime.capabilities"}
```

## tasks.list

参数 `{}`；effect=query，undoable=false。
返回按完成顺序排列的 TaskRecord 数组，最多 256 条；进程重启后清空，超限淘汰最旧记录。
TaskRecord 字段：

| 字段 | 含义 |
| --- | --- |
| id | TaskId UUID |
| method | 执行的命令名称 |
| status | succeeded 或 failed |
| error_code | 失败时的引擎 ErrorCode 数值，成功为 null；不是 JSON-RPC 的负数错误码 |
| document | 完成时的 State 快照；无活动场景时为 null |

记录不包含原始大结果。tasks.list 查询本身也会产生任务，但在返回后才登记，
所以本次列表不会包含查询自身；查询也会消耗保留名额。

```json
{"jsonrpc":"2.0","id":20,"method":"tasks.list"}
```

## tasks.get

必填 `id:UUID`，使用成功响应 result.task_id 或已分派失败的 error.data.task_id。
effect=query，undoable=false；返回一条 TaskRecord。未知或已淘汰 ID 返回 not_found，nil/非法 UUID 返回 invalid_argument。
查询只能看到此前已完成的记录。

```json
{"jsonrpc":"2.0","id":21,"method":"tasks.get","params":{"id":"<task_id>"}}
```

## runtime.shutdown

参数 `{}`；effect=control，undoable=false。返回 `{stopping:true}`，刷新当前响应后结束读取并退出。
不会自动保存场景；需要保留的数据须提前调用 scene.save/project.save。
同一 JSON-RPC batch 中排在 shutdown 后的有效命令会返回 invalid_state；后续输入行不再读取。

```json
{"jsonrpc":"2.0","id":22,"method":"runtime.shutdown"}
```
