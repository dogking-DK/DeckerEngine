---
module: command-reference-entity
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# 实体命令

[返回命令目录与公共类型](README.md)。所有命令都要求活动场景。
除 entity.get 外，均要求当前 guard，effect=memory_edit、undoable=true，
返回 `{state, created_id}`；只有创建操作的 created_id 是 UUID，其余为 null。
成功的内容变更增加一次 revision、dirty=true，并进入历史；无变化编辑不增加 revision，
失败保留状态。实体不存在返回 not_found，过期 guard 返回 conflict。
实现：[SceneOperations.cpp](../../engine/framework/operations/src/SceneOperations.cpp)、
[SceneDocument.cpp](../../engine/scene/src/SceneDocument.cpp)。

## entity.create

必填 guard，可选 `id:UUID`；省略 id 时生成新的稳定 ID。
实体初始名称为空、单位变换、无父级、无资产引用。指定重复/nil ID 被拒绝。
当前场景最多 10000 个实体。

```json
{"jsonrpc":"2.0","id":9,"method":"entity.create","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111"}}
```

## entity.get

必填 `id:UUID`，不传 guard；effect=query，undoable=false。
返回 Entity，包括局部 transform、parent、assets 及派生 world_matrix。

```json
{"jsonrpc":"2.0","id":10,"method":"entity.get","params":{"id":"11111111-1111-4111-8111-111111111111"}}
```

## entity.delete

必填 guard、id。仅允许删除无子节点的实体；父节点必须先解除子节点关系或先删除子节点。
不会隐式递归删除整个层级。可以通过 history.undo 恢复删除的实体及其稳定 ID。

```json
{"jsonrpc":"2.0","id":11,"method":"entity.delete","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111"}}
```

## entity.set_name

必填 guard、id、`name:string`。完整替换名称，允许空字符串；最多 1024 UTF-8 字节且不能包含 NUL。
名称不要求唯一，实体身份仍由 ID 确定。

```json
{"jsonrpc":"2.0","id":12,"method":"entity.set_name","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111","name":"示例实体"}}
```

## entity.set_transform

必填 guard、id、`transform:Transform`。完整替换局部 TRS，三个数组均必填。
改变父节点变换会重新计算子树的世界变换；非有限数值、零四元数或派生世界矩阵溢出会被拒绝。
四元数顺序 x,y,z,w；缩放为零时允许正向变换，但不保证可以求逆。

```json
{"jsonrpc":"2.0","id":13,"method":"entity.set_transform","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111","transform":{"translation":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]}}}
```

## entity.set_parent

必填 guard、id、`parent:UUID|null`。parent=null 表示解除父级。
保持局部 TRS，世界变换随新父级重新计算；不会自动保持原世界坐标。
父级不存在、自引用、形成循环或计算溢出时失败。
以下示例要求两个实体都已创建。

```json
{"jsonrpc":"2.0","id":14,"method":"entity.set_parent","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111","parent":"22222222-2222-4222-8222-222222222222"}}
```

## entity.set_assets

必填 guard、id、`assets:AssetReference[]`，0–64 项，完整替换引用列表；空数组清空引用。
各 AssetId 必须已注册到工程清单，kind 匹配，文件存在且为普通文件；引用列表拒绝重复 ID。
引用仅包含 id/kind，文件路径来自 scene.new 或已加载的清单；本命令不导入、解码资产。
未注册或缺失文件为 not_found，种类不符等为 invalid_argument。

```json
{"jsonrpc":"2.0","id":15,"method":"entity.set_assets","params":{"guard":{"document_id":"<document_id>","revision":0},"id":"11111111-1111-4111-8111-111111111111","assets":[]}}
```

绑定已有资产时，将 assets 改为形如 `[{"id":"资产 UUID","kind":"mesh"}]` 的数组。
