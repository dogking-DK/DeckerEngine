---
module: commands
created_at: "2026-09-22T13:12:54+08:00"
updated_at: "2026-09-22T13:12:54+08:00"
status: accepted
---

# 命令注册与能力发现（M3.1）

M3 全部五节已获授权；本节仅实现通用命令注册、schema 校验、结果/错误与能力发现。
engine/framework/commands 提供 dk::commands，PUBLIC 依赖 dk::core 和 nlohmann-json；
不链接 Scene、IO、Eigen、窗口或 GPU。Json 是显式公开的 nlohmann::json 别名。

DK_BUILD_FRAMEWORK 默认 OFF，windows-dev 启用；自动选择 commands feature（仅 JSON）。
本节 framework 只构建 commands；后续 services/operations/runtime 在 Scene 开启时装配，
Scene 关闭时仍可单独构建 commands。bootstrap 与既有独立 Foundation/Scene 配置显式关闭 framework。

## Schema 和描述

CommandDescriptor 包含稳定名称、说明、参数 schema、结果 schema、effect 和 undoable。
effect 为 query/memory_edit/external/control，undoable 只允许 memory_edit。
名称限 1–96 ASCII 字符，首字符字母，后续字母、数字、点、下划线；描述必填。
注册时拒绝重复名称、无 handler、非法 schema；已注册描述由 registry 持有副本。

支持 JSON Schema 片段子集：type（或 type 数组支持 nullable）、properties、required、
additionalProperties（bool）、items、minItems/maxItems、minLength/maxLength、minimum/maximum、
enum。不支持引用、组合或外部 schema，未知关键字在注册时拒绝。
字符串长度按 Unicode 码点；整数必须使用整数 JSON 值（1.0 拒绝），以
x-dk-integer-token=true 扩展说明；浮点要求有限。具体 UUID、层级和 revision 语义由服务验证。
对象可显式禁止未知字段。构造辅助函数生成同一份校验/发现使用的 schema，避免文档与校验分离。

execute 先查命令，再校验参数，然后调用同步 handler，最后检查结果 schema。
参数失败为 invalid_argument、未知命令为 not_found、坏结果或 handler 普通异常为 internal_error；
handler 的业务 Error 保留 code 和有序 context，追加命令名。资源分配异常向上抛出。
registry 仅负责契约，不为任意外部 handler 的副作用提供回滚；M3.3 在可信场景服务建立事务。

## 发现和边界

内建 commands.list 返回排序的命令名称/说明/effect/undoable；
commands.describe(name) 返回含参数/结果 schema 的完整描述。
注册表不可复制/移动，handler 可以引用存活时间更长的服务；调用方串行调用。
单次 JSON 深度最多 64、节点最多 200000；参数编码最多 1 MiB，结果最多 16 MiB。
parse_command_json 拒绝重复键、坏 UTF-8、非 JSON 数值及尾随文本，供后续协议复用。
没有动态插件卸载、并发注册、后台任务、Scene 行为或传输层。

验证独立 commands 构建、发现排序、重复/坏注册、嵌套 schema、未知/缺失字段、
整数/Unicode/非有限边界、坏结果与异常错误，以及默认 Debug/Release 回归。

参考：[JSON Schema 对象](https://json-schema.org/understanding-json-schema/reference/object)；
记录：[0013](../development/0013-command-registry.md)。
