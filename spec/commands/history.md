---
module: command-reference-history
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# 历史命令

[返回命令目录与公共类型](README.md)。实现：[SceneService.cpp](../../engine/framework/services/src/SceneService.cpp)。
默认最多保存 64 个历史单元、32 MiB 逻辑载荷，超限淘汰最旧 undo；
单个编辑超过预算时在修改前拒绝。逻辑载荷不等于进程峰值内存。
保存保留历史，new/load 清空历史，新的实际编辑清空 redo；历史不持久化。

## history.status

参数 `{}`，无需 guard，未创建场景时也可查询，返回空历史计数。
effect=query，undoable=false；返回 `{undo_count, redo_count, logical_bytes}`。

```json
{"jsonrpc":"2.0","id":16,"method":"history.status"}
```

## history.undo

必填 guard，需要活动场景和非空 undo；空栈返回 invalid_state。
effect=memory_edit，undoable=false；返回 State。
恢复上一历史单元前的内容，实体 ID 保持；当前 revision 继续递增，dirty=true。
仅恢复内存，不撤销磁盘文件，也不要求原资产文件此时仍存在；再次保存时仍会检查资源。
同一 scene.transaction 对应一个 undo 单元。

```json
{"jsonrpc":"2.0","id":17,"method":"history.undo","params":{"guard":{"document_id":"<document_id>","revision":0}}}
```

## history.redo

必填 guard，需要活动场景和非空 redo；空栈返回 invalid_state。
effect=memory_edit，undoable=false；返回 State。
恢复被撤销操作完成后的内容，revision 继续递增、dirty=true。
undo/redo 操作自身不会再创建新的编辑历史；后续请求须使用返回的新 revision。

```json
{"jsonrpc":"2.0","id":18,"method":"history.redo","params":{"guard":{"document_id":"<document_id>","revision":0}}}
```
