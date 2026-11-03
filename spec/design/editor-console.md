---
module: editor-console
created_at: "2026-11-03T10:31:20+08:00"
updated_at: "2026-11-03T10:36:53+08:00"
status: accepted
---

# Console 命令文本输入首期设计

## 目标与本次范围

让用户在 Editor Console 输入 `scene.query limit=20`，调用当前已经注册的 Runtime 命令。
采用本设计不代表功能已经实现；实施状态统一见 [Roadmap M22](../roadmap.md#m22console-命令文本输入首期)。
当前实现仍只接受 JSON `{method,params}`，基线为 `2d77103`，入口见
[Workspace](../../engine/editor/src/Workspace.cpp) 与 [Console UI](../../engine/editor/src/WorkbenchUi.cpp)。

首期交付：命令名与命名参数、标量/字符串/数组/对象、定位语法错误、原 JSON 输入兼容、
有界会话命令历史及上下键回填、真实 GUI 输入验收和使用说明。
不新增 Runtime 命令，不增加依赖或持久化格式，不改变 IPC、dk-ctl、Luau 或 Python 的输入协议。

以下后置：位置参数及别名、补全/schema 提示、多行脚本/注释/分号串联、管道、变量/表达式、
文件包含、历史持久化、自动 guard，以及 `restir on`、`raytracing iterate 12`、`cam.orbit 15 20`。
后三者需要另行开放编辑器视口状态与定义完成语义，本次文本解析不能使其自动成为可调用命令。

## 模块边界和依赖方向

```text
WorkbenchUi 的单次 Enter / Run
  -> Workspace::execute_console(text)
  -> parse_console_command(text) -> {method, params}
  -> 既有命令发现、草稿/等待检查
  -> Runtime::dispatch -> Commands -> Operations -> Services
  -> 既有快照刷新、结果/错误显示
```

- 解析器属于 `dk_editor_model` / `dk::editor_model`，复用 `dk::Json` 和命令 JSON 校验工具。
  拟新增 `engine/editor/include/dk/editor/Console.hpp`、`engine/editor/src/Console.cpp`；公开类型使用 `dk::editor`。
- 拟提供 `ConsoleCommand { std::string method; Json params; }` 和
  `Result<ConsoleCommand> parse_console_command(std::string_view text)`。拥有解析结果，不借用 UI 缓冲区。
  纯解析不读取场景、schema 或 Runtime，不调用命令，不依赖 SDL/ImGui/Vulkan。
- `Workspace::execute_console(std::string_view)` 保持原调用签名和 `Result<Json>` 结果；
  命令字段、业务约束和 effect 仍以现有注册表为准，不在解析器手写第二套命令目录。
- 历史模型使用 Editor 内部类型，拟放在 `engine/editor/src/ConsoleHistory.hpp/.cpp`，
  随 `dk_editor_model` 构建以便 CPU 验证；由 WorkbenchUi 持有实例，ImGui 仅适配输入回调。
- 更新现有 Editor CMake 源清单与测试源清单，不新增 library/executable target；
  新文件进入现有 target 的 PRIVATE/PUBLIC FILE_SET，不使用全局 include/link。

## 输入语法与类型

首期规范形式为 `method [key=value ...]`，参数之间用一个或多个 ASCII 空格/Tab 分隔。
允许首尾空格/Tab，以及 `=` 两侧空格/Tab；不接受实际 CR/LF/NUL，粘贴多条命令整段拒绝，不执行首行。
命令名区分大小写，沿用注册名规则：1–96 个 ASCII 字符，首字符字母，后续字母/数字/点/下划线。
顶层参数名为 `[A-Za-z_][A-Za-z0-9_]*`，区分大小写；重复参数一律拒绝，不采用最后值覆盖。
所有参数必须命名；`scene.query 20` 明确提示使用 `limit=20` 一类命名写法，不猜测位置。

以下均为待实现语法示例，当前 JSON-only Console 尚不可直接执行：

```text
scene.query
scene.query limit=20 offset=0
commands.describe name=render.capture
entity.set_name id="<entity_id>" name="Hello World" guard={"document_id":"<document_id>","revision":12}
entity.set_transform id="<entity_id>" transform={"translation":[1, 2, 3],"rotation":[0,0,0,1],"scale":[1,1,1]} guard={"document_id":"<document_id>","revision":13}
```

示例 UUID 与 revision 必须来自实际 `scene.query`；首期保留显式 guard，历史回填不刷新其值。
无需参数的命令形成空对象 `{}`。参数顺序不影响含义，不由 schema 字段顺序解释参数位置。

| 值写法 | 结果与规则 |
| --- | --- |
| `limit=20`、`offset=-1` | 完整匹配 JSON 整数字面量时保留整数；是否合法由命令 schema 决定 |
| `exposure=1.25`、`value=1e3` | 完整匹配 JSON 浮点字面量时为有限浮点，`1.0` 不转换成整数 |
| `enabled=true`、`value=null` | 精确小写 `true/false/null` 转相应 JSON 类型 |
| `name=hello`、`name=render.capture` | 其余合法裸词为字符串；无变量替换或环境展开 |
| `name="Hello World"`、`name="123"` | JSON 双引号字符串；支持 JSON 转义，保留字符串类型 |
| `manifest='folder with spaces/project.json'` | 单引号原样字符串；反斜杠不转义，内部不能含单引号；需单引号内容时使用双引号形式 |
| `assets=[]`、`translation=[1, 2, 3]` | 严格 JSON 数组；括号内空格不拆参数 |
| `guard={"document_id":"…","revision":12}` | 严格 JSON 对象；键必须双引号，拒绝重复键 |

裸词不能包含空白、单双引号、`[`、`]`、`{`、`}`、`=`、`;`、`&` 或竖线；这些内容必须放进字符串。
反斜杠在裸词中原样保留。对象/数组内部完全使用 JSON 规则，不扩展为单引号或裸键语法。
值结束后必须是输入末尾或空白，不接受 `name="a"suffix`、`assets=[]next=1`。
`#` 没有注释语义；末尾追加注释会因缺少命名参数而失败。
`guard.revision=12` 不支持，直接报告非法参数名，不自动生成嵌套对象。

类型转换只按完整词法形态判断，不依赖目标 schema：`01`、`1e`、`NaN`、`Infinity` 为裸字符串，
用于数值字段时由 schema 拒绝；它们绝不变成非有限数值。需要强制字符串的 `true` 或数字应加引号。
所有合法 JSON 整数字面量（含数组/对象和兼容 JSON 中的值）只接受 int64/uint64 联合范围，
不允许溢出后降级为 double；完整 uint64 guard.revision/seed 必须精确保留。
JSON 数字 token 在词法检查阶段区分整数与浮点并校验整数范围，再交现有 JSON 工具构造/验证。
超限或非有限浮点为解析错误，不按字符串恢复。

## JSON 兼容与输入边界

去掉首部空格/Tab 后以 `{` 开头的输入，进入兼容 JSON 分支；必须恰好是
`{"method":"…","params":{…}}`。其他结构、额外字段、缺字段仍拒绝，不接受 JSON-RPC envelope 或 batch。
JSON 分支失败后不回退文本分支，避免错误被另一种语法重新解释。

两种入口共用最终 method/params 检查与执行路径。复用
[parse_command_json / validate_command_value](../../engine/framework/commands/include/dk/commands/Schema.hpp)
的 UTF-8、重复键、深度/节点和有限数值保护；新增扫描只负责文本边界、整数 token 与位置，不复制 schema 校验器。
当前 Console 直接使用 `Json::parse`；改造会明确收紧重复 JSON 键和整数溢出，其他合法单行旧请求保持原行为。
本期两种入口均限定单行；直接调用 execute_console 也不接受实际换行的格式化 JSON，字符串内的 JSON 转义 `\n` 仍合法。

- 原始输入最多 8192 UTF-8 字节，解析前检查，所有引号/括号扫描有界。
- 最终命令对象沿用命令值深度 64、节点 200000、编码 1 MiB 上限；深度在递归构造前检查，
  不先建立任意深度 DOM 再拒绝。最终完整 params 再校验，不能仅逐参数放行累积超限。
- 空/全空白输入由 UI 当作无操作；直接调用解析器/execute_console 时返回 invalid_argument。
- UI 缓冲区容纳 8192 字节和终止 NUL。超额粘贴必须明确拒绝并保留原输入，不能静默截断后执行前缀；
  若使用 ImGui resize 回调，扩容只用于捕获/拒绝超限编辑，不保留无界历史或提交截断命令。

## 错误、状态与提交点

语法错误使用现有 `ErrorCode::invalid_argument`，消息包含简短原因与 1-based 字符列号；
Error.context 包含稳定分类 `console.syntax` 或 `console.limit` 和 0-based UTF-8 byte offset，
UTF-8 非法时仅报告字节位置。列号按 Unicode 码点计算，不用字节数冒充中文字数。
例如 `scene.query limit=` 报告缺少值；未闭合引号/括号定位起始分隔符，重复参数定位第二次出现。
不把整个输入再次嵌入错误，保留 UI 原输入供修改。已有命令错误保留 code/message/context，
不强行标为文本语法错误；未知命令仍为 not_found，字段非法仍由 schema 给出参数路径。

解析器先完成全行验证并生成候选对象，成功后才进入发现/分派。失败时不调用 Runtime，
不产生业务 Task、修改 Scene/revision/dirty/undo、写文件或创建后台 Job。
命令发现自身沿用现有 Task 记录，不能把“发现成功”显示成业务执行成功。
之后沿用现有 Workspace 状态契约，并补充第 5 项的提交后诊断：

1. `commands.describe` 读取真实描述；未应用 Inspector/Environment 草稿只允许 query。
2. 所有需要 guard 的命令显式填写；不补 guard、不纠正旧 revision、不自动重试。
3. Console 的 `jobs.wait` 仅允许零等待；省略 `timeout_ms` 沿用服务默认值 0，显式值必须为整数 0，
   非整数或非零在业务分派前拒绝。可使用 `jobs.get` 轮询，不改变 IPC 的 0–1000 ms 等待范围。
4. 真正提交点仍在对应 Service；脚本/文本前端不新增事务、补偿或后台执行线程。
5. 成功后沿用快照刷新。若业务已提交而后续刷新失败，报告“命令已执行，视图刷新失败”，
   保留原错误并在 context 追加 `console.executed_refresh_failed`，提示先查询；
   不得暗示操作回滚或主动再次执行。保持 Result<Json>，由 UI 识别该标记区分提交前失败。
6. UI 解析/显示/历史均在 owner 线程；后台 Job 仍按原服务协议受理/查询，不因控制台输入方式改变而同步等待。

## 命令历史与 UI

历史由当前 WorkbenchUi 拥有，最多 64 条，每条最多 8192 字节；是输入历史，不是 Scene 撤销历史。
单独保存当前输入草稿和浏览游标，日志仍最多 64 条、单条结果/错误正文最多 4096 字节，截断不切断 UTF-8 字符。

- Enter 与 Run 走同一提交动作；同帧两个触发合并成一次。只执行用户明确提交的完整当前输入。
- 输入框显示 `scene.query limit=20` 一类提示。成功后清空输入；失败保留输入并显示定位错误。
  已分派成功但刷新失败仍保留输入，并明确标明已执行状态。
- 有效 UTF-8、未超输入预算的非空提交尝试进入历史，包括语法/业务失败；连续完全相同文本去重，
  仅裁掉首尾空格/Tab，不改内部文本。浏览/回填本身不追加历史。
- 输入框聚焦时 Up 进入最近历史、继续 Up 查看更早项；Down 返回较新项，越过最新项恢复进入浏览前的草稿。
  边界不循环；浏览后编辑则退出浏览，编辑内容成为新草稿。输入法组合期间不吞方向键候选操作。
- 回填后再次 Enter 才重新执行。完整 guard/UUID 保持原样；切换文档不自动重写历史，旧 guard 由原服务拒绝。
- 历史与草稿随窗口销毁，不写磁盘，不属于工程保存/撤销/重放格式。打开不同文档保持本窗口历史。
- 提交时有界回显命令，并分别标识成功/失败；异步返回 JobId 只显示已受理，完成状态使用已有查询命令查看。

## 实施步骤和验证计划

| 阶段 | 改动范围 | 独立验收 |
| --- | --- | --- |
| M22.1 设计落档 | 本设计、Editor 设计入口、Roadmap、开发记录与索引 | 文档一致性；语法/兼容/guard/历史/预算和非目标明确；不声称功能实现 |
| M22.2 解析与执行 | Console.hpp/.cpp、Workspace、Editor CMake、现有 dk_editor_tests 新增解析/执行用例 | 两种语法映射一致；类型/Unicode/整数/边界；全行失败无副作用；显式 guard/草稿/零等待/刷新失败契约 |
| M22.3 历史与 GUI 交付 | ConsoleHistory、WorkbenchUi、CPU 历史用例、真实输入 smoke、指南/命令参考入口 | Up/Down/草稿/去重/容量；Enter/Run 一次执行；超限粘贴拒绝；后台 GUI/旧 JSON 回归；首期完整验收 |

每一实现小阶段分别定向构建、测试、留档和本地提交。首期最终完成要求 M22.2/3 全部通过。
代码分工与选择范围如下，实际测试名在实施时注册并枚举，不把下列候选当成已运行结果：

- 复用 [dk_editor_tests](../../tests/unit/CMakeLists.txt)，拟新增 `ConsoleTests.cpp` 和 `ConsoleHistoryTests.cpp`。
  候选前缀为 `dk.editor.console `、`dk.editor.workspace console`；继承原 `workspace console shares explicit guards...` 回归。
- 类型用例：引号/空格/转义/中文/原样路径、嵌套 JSON、空数组/null、true 与字符串 true、
  整数与浮点 token、int64/uint64 边界与越界（含嵌套及兼容 JSON）、未知/重复参数/JSON 键。
- 拒绝用例：未闭合引号/括号、缺值/尾随垃圾、位置参数、非法 UTF-8/NUL/CRLF、8192/8193 字节、
  深度边界、分号/管道；确认一个合法编辑前缀后接错误尾部也完全不执行。
- 状态用例：同一参数的两种入口有相同结果；实体编辑一次 revision/Undo，缺失/旧 guard 和草稿保护；
  `jobs.wait` 省略/显式零允许与非整数/非零拒绝；通过故障注入验证已提交后刷新失败不重试。
- 历史 CPU 用例验证状态转换与容量；真实 GUI 通过现有独立夹具/输入驱动机制，使用 `--background`，
  覆盖键盘输入、粘贴超限、Up/Down 草稿恢复、成功清空/失败保留和旧 JSON。
  可扩展 [SmokeDriver](../../engine/editor/src/SmokeDriver.cpp) 的工作台流程，
  优先运行 `dk.editor.workbench_gpu_validation`；若实现时需新增独立 smoke，须同步测试选择表。
- 定向构建使用 [verify.ps1](../../scripts/verify.ps1)，CPU 用例只需 `dk_editor_tests`；GUI 用例需
  `dk_editor_tests`、`dk_editor_app`，配置选择现有可用单配置。无需整个渲染/物理/IPC/Python/Luau 测试矩阵。
- 发布时更新 [编辑器指南](../guides/editor.md) 和 [命令参考入口](../commands/README.md)，
  新增文本示例并标注只适用于 Editor Console；原 JSON/IPC 示例继续有效，schema/命令目录无新增能力。

## 决策与后续扩展

首期使用命名参数，不推断参数位置，不在前端转换业务单位。数组/对象复用 JSON，避免引入第二套嵌套数据语言。
显式 guard 保持当前可复现/冲突检测契约；便捷 guard、点号嵌套参数或常用别名若后续需要，单独定义覆盖与冲突规则。
补全可在未来消费现有 commands.list/describe，首期不为补全改动 schema。
当前设计已足够进入 M22.2；无需外部依赖选型或新增审批。渲染控制/相机命令仍需独立设计。

## 相关记录

[Editor 设计](editor.md)、[命令设计](commands.md)、[服务状态契约](application-services.md)、
[原 Console 实现记录](../development/0127-editor-media-pathtracing.md)、
[0148 首期计划](../development/0148-console-command-input-plan.md)。
