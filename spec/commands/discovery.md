---
module: command-reference-discovery
created_at: "2026-09-22T17:00:46+08:00"
updated_at: "2026-11-03T17:57:12+08:00"
status: accepted
---

# 命令发现

[返回命令目录](README.md)。本页命令无需活动场景或 guard，effect=query，undoable=false。
实现：[CommandRegistry.cpp](../../engine/framework/commands/src/CommandRegistry.cpp)。

## commands.list

列出当前注册的命令，按名称排序。参数为 `{}`，可省略。
返回数组，每项包含 `name`、`description`、`effect`、`undoable`。
列表只包括当前运行时实际注册的能力。
effect 的文本值为 `query`、`memory_edit`、`external`、`control`；与 commands.describe 使用同一来源。

```json
{"jsonrpc":"2.0","id":1,"method":"commands.list"}
```

## commands.describe

查询一条命令的完整描述。必填 `name:string`，长度 1–96。
返回 `name`、`description`、`effect`、`undoable`、`parameters`、`result`；
后两个字段分别为参数和业务返回值的 schema。不存在的 name 返回 not_found（-32003）。
这与请求一个未知 method 返回 -32601 不同。
资产种类的参数/结果 schema 均列出 `mesh`、`material`、`texture`，区分大小写，不接受数字或首尾空白。

```json
{"jsonrpc":"2.0","id":2,"method":"commands.describe","params":{"name":"entity.set_transform"}}
```

M22.5 增加可选 `help`，已有六个字段与 `commands.list` 的四字段条目保持不变。
未提供帮助的命令省略 help，消费者应按 description/parameters 降级；不能把没有帮助理解为命令不可用。
当前带帮助的命令为 `commands.describe`、`scene.query`、`environment.preset/set`，
以及 `entity.get/delete/set_name/set_transform/set_parent/set_assets`、`assets.load/status/unload` 和 `simulation.run`（共 14 条，随构建选项变化）。
附加视口的 Editor 另有 `editor.viewport.query/set/pause/resume/reset` 五条及 `editor.camera.query/set/orbit/pan/zoom/frame/reset` 七条 help v1；普通 runner 不注册这些宿主命令。
M22.10 的 Editor 字段提示与 Settings 搜索复用这些 schema/help；实体 TRS 补充单位/坐标含义，simulation.run 补充已定义的时间步、批次与布片参数默认值/单位。
帮助不改变必填字段或自动填值；模板全部保留占位符，使用见 [Editor 指南](../guides/editor.md#字段帮助与设置搜索m2210)。

```json
{"jsonrpc":"2.0","id":3,"method":"commands.describe","params":{"name":"scene.query"}}
```

对应结果的 help 为 `{version:1,fields:{...}}`，fields 的键是指向 parameters schema 节点的 JSON Pointer，
例如 `/properties/limit`、`/properties/environment/properties/sun/properties/irradiance`。
空路径指参数根，数组沿 items；`~0`/`~1` 分别转义属性名中的 `~`/`/`。
每个条目可含以下字段，均为可选；类型、范围、必填和枚举仍从原 schema 取得。

| 字段 | 含义 |
| --- | --- |
| label / description | 显示名称与字段说明；未提供时保留原参数名/约束 |
| unit | 单位说明；不会自动换算输入值 |
| default | 服务已明确的缺省值；没有该字段表示没有声明缺省行为，显式 null 仍是一个值 |
| examples | 最多 8 个符合该字段 schema 的示例，不保证当前业务前置条件成立 |
| aliases | 最多 16 个搜索别名，不是可执行的参数名或命令别名 |
| completion_provider | 已声明的候选语义标识；候选枚举由宿主实现，字段本身不执行任何查询 |

例如 scene.query 的 `/properties/limit` 含 `label="分页数量"`、`unit="entities"`、`default=128` 与示例 `[1,20,128]`；
范围 1–256 来自 parameters schema。environment.set 的必填环境字段只提供说明/示例，不隐式补齐完整环境。
帮助只读，不改变 handler 的参数、默认值、guard 或副作用。Console 已支持静态/嵌套补全与动态候选：
`scene.entity` 标记已有实体 ID/parent，`assets.any` 标记活动目录 ID，`assets.reference` 标记工程资产引用 ID；
创建 ID 和 guard 没有身份候选。只有候选接受会改输入，真正执行仍需明确提交。
通过 Runtime/JSON-RPC 主动调用发现命令仍记录正常 Task；Editor 内部无 Task 的目录快照 API 见
[Commands 设计](../design/commands.md#m225-帮助描述与只读目录)，它不是新增的传输命令。
