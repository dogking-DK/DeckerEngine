---
module: commands
created_at: "2026-09-22T13:12:54+08:00"
updated_at: "2026-11-03T13:06:50+08:00"
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
effect_name 改由 PRIVATE 依赖 magic_enum 反射，未知值仍返回 invalid，非法 effect 仍拒绝注册。
Operations 的资产种类 schema 从 asset_kind_names() 构造，名称和参数/结果协议保持不变；
不将 task 的 bool 状态或 JSON Schema 关键字改造为新的枚举。迁移见 [0028](../development/0028-magic-enum.md)。
名称限 1–96 ASCII 字符，首字符字母，后续字母、数字、点、下划线；描述必填。
注册时拒绝重复名称、无 handler、非法 schema；已注册描述由 registry 持有副本。

支持 JSON Schema 片段子集：type（或 type 数组支持 nullable）、properties、required、
additionalProperties（bool）、items、minItems/maxItems、minLength/maxLength、minimum/maximum、
enum（最多 256 项）。不支持引用、组合或外部 schema，未知关键字在注册时拒绝。
字符串长度按 Unicode 码点；整数必须使用整数 JSON 值（1.0 拒绝），以
x-dk-integer-token=true 扩展说明；浮点要求有限。具体 UUID、层级和 revision 语义由服务验证。
对象可显式禁止未知字段。构造辅助函数生成同一份校验/发现使用的 schema，避免文档与校验分离。
M3.2 补充数值比较：显式区分 signed/unsigned 和浮点范围，不依赖 JSON 库的跨整数类型排序，
确保完整 uint64 revision 边界正确；enum 的深层数值相等也沿用此规则。

execute 先查命令，再校验参数，然后调用同步 handler，最后检查结果 schema。
参数失败为 invalid_argument、未知命令为 not_found、坏结果或 handler 普通异常为 internal_error；
handler 的业务 Error 保留 code 和有序 context，追加命令名。资源分配异常向上抛出。
registry 仅负责契约，不为任意外部 handler 的副作用提供回滚；M3.3 在可信场景服务建立事务。

## 命令使用文档

[命令参考](../commands/README.md) 面向人和 AI 的调用需求，按功能分组维护名称、
用途、参数、返回值、guard、副作用/撤销范围、错误和示例。模块内部设计继续由本文件、
服务设计和协议设计维护；skill 仅规定维护步骤，不复制具体命令目录。
新增或修改命令时同步对应参考页，使用 commands.list 检查目录覆盖，
使用 commands.describe 对照参数/结果 schema 与 effect/undoable；业务前置条件同时核对服务实现。
验证只覆盖改动涉及的命令和示例；纯文档更新不新增编号开发记录。

## 发现和边界

内建 commands.list 返回排序的命令名称/说明/effect/undoable；
commands.describe(name) 返回含参数/结果 schema 的完整描述。
注册表不可复制/移动，handler 可以引用存活时间更长的服务；调用方串行调用。
单次 JSON 深度最多 64、节点最多 200000；参数编码最多 1 MiB，结果最多 16 MiB。
parse_command_json 拒绝重复键、坏 UTF-8、非 JSON 数值及尾随文本，供后续协议复用。
没有动态插件卸载、并发注册、后台任务、Scene 行为或传输层。

验证独立 commands 构建、发现排序、重复/坏注册、嵌套 schema、未知/缺失字段、
整数/Unicode/非有限边界、坏结果与异常错误；按本次行为选择相关用例和配置。

## M22.5 帮助描述与只读目录

本阶段实施契约如下，完成/验证状态见 [0152](../development/0152-command-help-catalog.md)。
CommandDescriptor 尾部增加 `std::optional<Json> help`，旧 aggregate 初始化继续有效；未提供时 describe 不输出 help。
help 严格为 `{version:1,fields:{<schema JSON Pointer>:<entry>}}`，最多 64 KiB UTF-8 JSON 编码。
entry 至少含一个字段，可选 label/description/unit/default/examples/aliases/completion_provider；未知字段拒绝。
label/unit/description 分别最多 256/64/4096 UTF-8 字节，非空且无 NUL；说明可用换行/Tab，其他显示字符串不含控制字节。
examples 最多 8 个，aliases 最多 16 个、各最多 128 字节且不重复；default 可为 null，但必须符合目标 schema。
JSON Pointer 最多 1024 字节，只能沿 properties/<name> 和 items 到达真正 schema 节点，空路径表示参数根。
引用、UTF-8/深度/预算、默认值与各示例均在注册前校验；帮助不填充调用参数，schema 和业务校验不放宽。

注册表提供 `register_completion_provider(id)` 注册语义标识；只有已声明标识才能被 help 引用。
这只是拥有型名称目录，不持有回调或读取场景；宿主实际候选提供器在 M22.7 接入，目前内建命令未宣称具备动态候选。
标识沿命令名字符/长度规则，重复或非法注册拒绝。Commands 仍不依赖 Scene/Editor/GPU。

`CommandCatalogVersion {registry_id, revision}` 用独立 StableId 区分注册表重建；每次成功添加命令或候选标识后 revision 增一，
拒绝/重复注册不变，耗尽时在修改前拒绝。注册表构造生成身份失败时抛出构造错误，分配异常继续向上传播。
发布点为 map/set 成功插入之后；所有帮助/描述校验在插入前完成。
`catalog_version()` 返回轻量代号；`catalog_snapshot()` 返回包含代号、有序 CommandDescriptor 副本和候选标识列表的
`CommandCatalogSnapshot`。无 handler/借用引用，快照修改或 owner 销毁不影响其他快照/注册表；调用沿现有 owner 线程串行契约。
Runtime/Workspace 分别提供同名用途的 `command_catalog_version()` 与 `command_catalog()`，直接读取注册表而不 dispatch，
不创建 Task/Job、不刷新快照、不改 Scene/历史/草稿/文件；UI 可以先比较代号再决定是否复制整个目录。
场景切换不改变命令目录代号，动态实体候选须另按 document_id/revision 失效，不能复用目录代号代表场景状态。

首批真实帮助覆盖 commands.describe、scene.query、environment.preset/set；分页缺省值与 handler 使用同一常量。
环境完整对象的必填字段只提供说明/单位/示例，不伪造缺省值；未补充的命令继续按原 schema/描述降级。
Console 弹窗、动态候选和 Editor 会话控制仍由后续阶段实现。

### 扩展设计的兼容边界

[Console/Editor 扩展设计](editor-console.md#m224-扩展设计帮助补全与设置命令)使用独立的可选 `help` 元数据，
服务于命令补全、参数说明和面板帮助；不往现有严格 schema 中直接添加未知说明关键字。
帮助条目引用参数 schema 的 JSON Pointer，类型/必填/范围/枚举继续以 schema 为唯一来源；
说明、单位、明确默认值、示例/搜索别名和候选提供器标识独立保存，缺失时退回已有信息。
注册时校验引用、UTF-8/预算及默认值/示例，帮助不改变参数缺省行为或业务校验。

M22.5 让 commands.describe 增量返回 help；commands.list 与已有字段含义保留。
Runtime/Workspace 的 UI 描述查询从同一 registry 取得拥有型快照，不走 dispatch，避免击键时创建 Task；
外部主动调用发现命令仍走原 Runtime 路径。Commands 不持 Editor widget、Scene 或 GPU 引用。
接口/兼容用例按上述实施契约验证；帮助是可选字段，不能据此宣称每个命令都已附带完整帮助。
M22.8/9 的 editor.* 设置命令须由宿主附加服务后注册，不由通用 Commands 层实现视口行为；runner 不假装提供 Editor 状态。
扩展记录：[0151](../development/0151-console-assistance-plan.md)。

参考：[JSON Schema 对象](https://json-schema.org/understanding-json-schema/reference/object)；
记录：[0013](../development/0013-command-registry.md)。
