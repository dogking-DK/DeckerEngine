---
module: command-reference-scene
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# 场景与工程命令

[返回命令目录与公共类型](README.md)。实现：[SceneOperations.cpp](../../engine/framework/operations/src/SceneOperations.cpp)、
[SceneService.cpp](../../engine/framework/services/src/SceneService.cpp)。

## scene.new

创建空场景和工程描述，返回 State。effect=control，undoable=false。
首次创建可不传 guard；替换已有文档必须使用当前 guard，即使旧文档 dirty 也会被替换，需提前保存所需修改。
成功后 revision=0、dirty=true，新建 SceneId/DocumentId 并清空撤销历史；失败保留原文档。
本命令不创建磁盘文件。

| 参数 | 必填 | 默认及约束 |
| --- | --- | --- |
| name | 否 | Untitled；1–1024 UTF-8 字节，无 NUL |
| scene_file | 否 | scene.json；规范工程相对路径，使用 `/`，不含空路径段、`.`、`..`、冒号或反斜杠，最多 4096 UTF-8 字节 |
| assets | 否 | 空数组；最多 10000 条 AssetRecord，ID 不可重复，path 遵循 scene_file 的路径规则 |
| guard | 已有场景时必填 | 当前会话与 revision；无活动场景时应省略 |

资产清单描述引用关系；此时不导入或解码文件。后续绑定/加载/保存场景时会校验所用资产。

```json
{"jsonrpc":"2.0","id":3,"method":"scene.new","params":{"name":"Command Demo","scene_file":"scene.json","assets":[]}}
```

## scene.load

必填 `manifest:string`，工程根下的清单相对路径，schema 长度 1–4096 个 Unicode 码点。
`guard` 与 scene.new 相同：首次加载省略，替换已有场景时必填。
加载清单和其中指定的场景，返回 State；effect=control，undoable=false。
成功保留磁盘 SceneId/EntityId/revision、生成新 DocumentId、dirty=false、清空历史；失败保留原状态。
常见错误：文件/所用资产缺失 not_found，格式/引用非法 invalid_argument，版本不支持 not_supported，旧 guard 为 conflict。

```json
{"jsonrpc":"2.0","id":4,"method":"scene.load","params":{"manifest":"project.json"}}
```

此示例适合新进程；同一进程替换活动场景时增加 guard。

## scene.query

需要活动场景，无需 guard；effect=query，undoable=false。
可选 `offset`（默认 0，整数 0–10000）、`limit`（默认 128，整数 1–256）。
返回 `{state, entities, offset, has_more}`，entities 为按持久 EntityId 排序的 Entity 数组。
后续页使用 `offset + entities.length`；多次查询之间发生编辑时不保证跨页快照一致性。
无活动场景返回 invalid_state。

```json
{"jsonrpc":"2.0","id":5,"method":"scene.query","params":{"offset":0,"limit":128}}
```

## scene.save

必填 `guard`，需要活动场景；返回 State。effect=external，undoable=false。
保存到 scene.new/已加载清单指定的场景文件，不接受另一个输出路径参数。
检查资产引用，验证临时文件后原子替换；成功清除当前保存版本的 dirty，不增加 revision、不清空历史。
父目录须已存在；当前安全保存范围为 Windows 本地普通文件。
缺失资产返回 not_found；写入/替换失败返回 IO 错误并保留旧文件和 dirty 状态。

```json
{"jsonrpc":"2.0","id":6,"method":"scene.save","params":{"guard":{"document_id":"<document_id>","revision":0}}}
```

## project.save

必填 `guard`、`manifest:string`（相对路径，schema 长度 1–4096 个 Unicode 码点）；返回 State。
需要活动场景，effect=external，undoable=false。保存工程清单，不保存场景，也不清除场景 dirty。
拒绝清单路径直接等于场景路径；父目录须已存在。
scene.save 和 project.save 是两个独立的文件操作，不提供跨文件原子提交。
首次创建工程一般先保存场景，再保存清单。

```json
{"jsonrpc":"2.0","id":7,"method":"project.save","params":{"guard":{"document_id":"<document_id>","revision":0},"manifest":"project.json"}}
```

## scene.transaction

必填 `guard`、`commands`（1–128 项 `{method, params}`）；需要活动场景。
effect=memory_edit，undoable=true。内部仅允许 entity.create/delete/set_name/set_transform/set_parent/set_assets。
每个 params 不得带 guard；多个操作引用同批新建实体时，在创建时指定 ID。
查询、保存、会话替换、历史操作和嵌套事务均不允许。

返回 `{state, created_ids}`，created_ids 与输入顺序一一对应：创建操作为新 UUID，其他为 null。
全部验证成功后一次提交，真实 revision 只加 1，形成一个撤销单元；
中途失败，文档和历史保持原样。最终内容与原场景相同则保持 revision、dirty 和 redo。
历史预算规则见 [历史命令](history.md)。

```json
{"jsonrpc":"2.0","id":8,"method":"scene.transaction","params":{"guard":{"document_id":"<document_id>","revision":0},"commands":[{"method":"entity.create","params":{"id":"33333333-3333-4333-8333-333333333333"}},{"method":"entity.set_name","params":{"id":"33333333-3333-4333-8333-333333333333","name":"Transaction Entity"}}]}}
```
