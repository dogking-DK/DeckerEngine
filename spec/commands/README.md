---
module: command-reference
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# DeckerEngine 命令参考

本目录供人和 AI 查阅当前可调用命令。适用于 M3 完成后的 CPU Runtime；
`windows-dev` 构建启用此功能，最小 bootstrap 仅提供 `--help/--version`。
当前 22 条命令以实际 `commands.list` 为准，完整字段 schema 可通过 `commands.describe` 查询。

## 按功能查找

| 分组 | 命令 | 说明 |
| --- | --- | --- |
| [发现](discovery.md) | `commands.list`、`commands.describe` | 列出命令、查询参数和结果 schema |
| [场景与工程](scene.md) | `scene.new`、`scene.load`、`scene.query`、`scene.save`、`project.save`、`scene.transaction` | 文档会话、查询、保存和事务 |
| [实体](entity.md) | `entity.create`、`entity.get`、`entity.delete`、`entity.set_name`、`entity.set_transform`、`entity.set_parent`、`entity.set_assets` | 实体编辑和查询 |
| [历史](history.md) | `history.status`、`history.undo`、`history.redo` | 内存撤销与重做 |
| [运行时与任务](runtime.md) | `runtime.capabilities`、`runtime.shutdown`、`tasks.list`、`tasks.get` | 能力、同步任务和关闭 |

各参考页的“参数”指请求的 `params`，“返回”指响应的 `result.value`。
所有示例使用 JSON-RPC；排版为多行的单个 JSON 对象，发送到 runner 时须压缩为一行。
示例里的 `<document_id>`、`<task_id>` 要替换为实际响应值；示例 revision=0 也要替换为最新值。
实体示例中的固定 UUID 仅供演示，使用前按对应命令的前置条件创建实体。

## 启动和快速使用

从仓库根目录执行；工程根必须已经存在。以下批处理会在 `out/command-demo` 保存示例场景：

```powershell
New-Item -ItemType Directory -Force out/command-demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/command-demo --batch examples/automation/create-scene.jsonl --auto-guard
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/command-demo --batch examples/automation/load-scene.jsonl
```

[创建并保存](../../examples/automation/create-scene.jsonl) 与 [重启后加载](../../examples/automation/load-scene.jsonl)
是可执行的完整请求序列。批文件路径相对于调用者工作目录，命令中的工程路径相对于 `--project-root`。

交互调用使用：

```powershell
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/command-demo --stdio
```

每输入一行请求立即得到响应，不需要先关闭 stdin。stdio 必须显式传 guard；
批处理的 `--auto-guard` 只给省略 guard 的请求填入当前状态，不会纠正显式过期的 guard。

## 请求、响应与 guard

请求含 `jsonrpc: "2.0"`、`method`、可选对象 `params` 和可选 `id`；省略 params 相当于 `{}`。
id 支持字符串（最多 256 个 UTF-8 字节）、int64 整数或 null。省略 id 是通知，合法通知不返回响应。
参数、公共类型均不接受额外未知字段；重复 JSON 键、非法 UTF-8、非有限数值也会被拒绝。

成功响应示意：

```json
{"jsonrpc":"2.0","id":1,"result":{"task_id":"<task_id>","status":"succeeded","value":{"stopping":true}}}
```

`id` 用来关联请求，`task_id` 用来查询 Runtime 的执行记录；两者互不替代。
命令页说明的返回对象都位于 `value` 内。

`guard` 为 `{ "document_id": "会话 UUID", "revision": 非负整数 }`。
从 scene.new/load 的直接返回值，或 scene.query/实体编辑/事务返回的 `state` 中获取。
new/load 每次生成新的 document_id；持久 scene_id 不能用作 document_id。
后续请求必须跟随新的 revision；无变化编辑保持 revision，真实编辑或 undo/redo 会递增。
旧 guard 返回 conflict，并保留当前文档。

## 公共数据类型

| 类型 | 字段及语义 |
| --- | --- |
| UUID | 规范的 36 字符 UUID，非 nil；输入大小写均可，输出小写 |
| State | `document_id`、`scene_id`、`revision`、`dirty`、`entity_count`；revision 为 uint64 |
| Transform | `translation[3]`、`rotation[4]`、`scale[3]`，三个字段均必填；四元数顺序 x,y,z,w |
| AssetReference | `{id, kind}`；kind 为 mesh/material/texture |
| AssetRecord | `{id, kind, path}`；用于 scene.new 的工程资产清单 |
| Entity | `id`、`name`、`transform`、`parent`、`assets`、`world_matrix`；parent 为 UUID 或 null |

数学使用右手系、列向量、米/弧度；局部 TRS 按缩放→旋转→平移组合。
`world_matrix` 是世界矩阵的 16 个数，JSON 按行展开；本身仍采用列向量变换约定。
整数参数必须为 JSON 整数，`1.0` 和字符串 `"1"` 均不能代替 revision。
四元数必须有限且非零，服务会按设计规范化；缩放可为零或负值。

## 副作用与错误

命令发现中的 effect 为 query（业务查询）、memory_edit（内存修改）、external（文件写入）、control（会话/进程控制）。
Runtime 也会为成功分派的查询记录任务。undoable=true 表示编辑进入场景撤销历史；
history.undo/redo 自身 undoable=false，表示不会再添加一个“撤销命令”的历史单元。
文件保存、会话替换和关闭均不在撤销范围。

| JSON-RPC code | 含义 |
| --- | --- |
| -32700 | 无法解析 JSON、超过行长度等 |
| -32600 | 请求结构或 id 无效 |
| -32601 | 未注册的 method |
| -32602 | 参数类型、schema 或业务参数非法 |
| -32603 | 内部错误 |
| -32002 | 当前状态不允许，如无活动场景或 Runtime 正在关闭 |
| -32003 | 实体、文件、资产或任务不存在 |
| -32004 | IO 错误 |
| -32005 | 不支持的版本或平台操作 |
| -32007 | document_id/revision 冲突 |

错误位于 `error.{code,message,data}`；引擎错误 data 含 `engine_code`、`engine_name`、`context`。
进入命令分派后的失败还包含 task_id 和 status=failed。无效协议、未知命令等不产生任务。
结构或业务失败可继续处理后续请求；响应输出失败不保证之前的操作未执行，应重新查询状态。

每行最多 1 MiB，JSON-RPC batch 最多 128 条且顺序执行；batch 中部分失败不会回滚已成功命令。
需要内存原子编辑时使用 scene.transaction。支持通知、CRLF、最后一行无换行。
批处理退出码：0 成功、1 有请求错误、2 启动/参数错误、3 致命流/资源错误；
stdio 在 EOF/shutdown 正常退出时为 0，先前可恢复的请求错误不改变此退出码。

## 文档维护

命令新增、变更或删除时同步对应功能页与本目录，不将每个命令写入 skill。
通过 commands.list 核对目录，通过 commands.describe 核对 schema/effect/undoable，
服务语义同时对照设计与实现；只运行受影响的命令示例或相关测试。
设计原因见 [命令层](../design/commands.md)、[应用服务](../design/application-services.md)、
[Runtime](../design/runtime.md) 和 [自动化协议](../design/automation-protocol.md)。
