---
module: command-reference-discovery
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-09-22T17:00:46+08:00"
status: accepted
---

# 命令发现

[返回命令目录](README.md)。本页命令无需活动场景或 guard，effect=query，undoable=false。
实现：[CommandRegistry.cpp](../../engine/framework/commands/src/CommandRegistry.cpp)。

## commands.list

列出当前注册的命令，按名称排序。参数为 `{}`，可省略。
返回数组，每项包含 `name`、`description`、`effect`、`undoable`。
列表只包括当前运行时实际注册的能力。

```json
{"jsonrpc":"2.0","id":1,"method":"commands.list"}
```

## commands.describe

查询一条命令的完整描述。必填 `name:string`，长度 1–96。
返回 `name`、`description`、`effect`、`undoable`、`parameters`、`result`；
后两个字段分别为参数和业务返回值的 schema。不存在的 name 返回 not_found（-32003）。
这与请求一个未知 method 返回 -32601 不同。

```json
{"jsonrpc":"2.0","id":2,"method":"commands.describe","params":{"name":"entity.set_transform"}}
```
