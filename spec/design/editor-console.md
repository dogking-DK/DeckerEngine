---
module: editor-console
created_at: "2026-11-03T10:31:20+08:00"
updated_at: "2026-11-03T18:40:35+08:00"
status: accepted
---

# Console 命令输入、补全与 Editor 帮助设计

## 目标与本次范围

让用户在 Editor Console 输入 `scene.query limit=20`，调用当前已经注册的 Runtime 命令。
采用本设计不代表功能已经实现；实施状态统一见 [Roadmap M22](../roadmap.md#m22console-命令文本输入与-editor-交互)。
M22.2 已接入命名参数与兼容 JSON 解析/执行，验收记录见 [0149](../development/0149-console-command-parser.md)；
M22.3 的历史、输入适配及真实 GUI 验收记录见 [0150](../development/0150-console-history-ui.md)。原 JSON-only 基线为 `2d77103`，入口见
[Workspace](../../engine/editor/src/Workspace.cpp) 与 [Console UI](../../engine/editor/src/WorkbenchUi.cpp)。

首期交付：命令名与命名参数、标量/字符串/数组/对象、定位语法错误、原 JSON 输入兼容、
有界会话命令历史及上下键回填、真实 GUI 输入验收和使用说明。
不新增 Runtime 命令，不增加依赖或持久化格式，不改变 IPC、dk-ctl、Luau 或 Python 的输入协议。

M22.4 将共享帮助元数据、Console 补全/提示/诊断、视口与相机命令、Editor 帮助和设置搜索纳入同一路线；
扩展方案见下文，M22.5–7 已接入共享帮助、只读目录、基础/嵌套补全、动态候选与诊断，其他功能按 Roadmap 后续阶段推进。M22.1–3 的首期验收结论不变。
仍排除位置参数及别名、多行脚本/注释/分号串联、管道、变量/表达式、文件包含、历史持久化、
自动 guard 和大模型生成/代执行。`restir on`、`raytracing iterate 12`、`cam.orbit 15 20` 不是现有可调用命令；
扩展先提供注册命令与命名参数，简写/位置语法及精确追加 N 个样本的完成语义不由补全推断。

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
  公开头文件 [Console.hpp](../../engine/editor/include/dk/editor/Console.hpp)、实现 [Console.cpp](../../engine/editor/src/Console.cpp)；公开类型使用 `dk::editor`。
- 提供 `ConsoleCommand { std::string method; Json params; }` 和
  `Result<ConsoleCommand> parse_console_command(std::string_view text)`。拥有解析结果，不借用 UI 缓冲区。
  纯解析不读取场景、schema 或 Runtime，不调用命令，不依赖 SDL/ImGui/Vulkan。
- `Workspace::execute_console(std::string_view)` 保持原调用签名和 `Result<Json>` 结果；
  命令字段、业务约束和 effect 仍以现有注册表为准，不在解析器手写第二套命令目录。
- 历史模型使用 Editor 内部类型，位于 `engine/editor/src/ConsoleHistory.hpp/.cpp`，
  随 `dk_editor_model` 构建以便 CPU 验证；由 WorkbenchUi 持有实例，ImGui 仅适配输入回调。
- 更新现有 Editor CMake 源清单与测试源清单，不新增 library/executable target；
  新文件进入现有 target 的 PRIVATE/PUBLIC FILE_SET，不使用全局 include/link。

## 输入语法与类型

首期规范形式为 `method [key=value ...]`，参数之间用一个或多个 ASCII 空格/Tab 分隔。
允许首尾空格/Tab，以及 `=` 两侧空格/Tab；不接受实际 CR/LF/NUL 或其他非 Tab 的 C0 控制字节，
粘贴多条命令整段拒绝，不执行首行。
命令名区分大小写，沿用注册名规则：1–96 个 ASCII 字符，首字符字母，后续字母/数字/点/下划线。
顶层参数名为 `[A-Za-z_][A-Za-z0-9_]*`，区分大小写；重复参数一律拒绝，不采用最后值覆盖。
所有参数必须命名；`scene.query 20` 明确提示使用 `limit=20` 一类命名写法，不猜测位置。

以下为 M22.2 解析/执行入口支持的语法；历史/输入提示仍属于 M22.3：

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

两种入口共用最终 method/params 检查与执行路径。有界词法预扫描检查 UTF-8、分隔符、
重复 JSON 键、整数 token 与深度，再调用现有 Json 解析器；直接捕获 parse_error 的 byte 位置，
不通过 parse_command_json 丢失结构化位置后再从英文错误文本反推。
最终完整命令复用 [validate_command_value](../../engine/framework/commands/include/dk/commands/Schema.hpp)
的深度/节点/编码/有限数值保护，不复制 schema 校验器。
M22.2 已替换原直接 `Json::parse` 的入口，明确拒绝重复 JSON 键和整数溢出，其他合法单行旧请求保持原行为。
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

M22.2 将上述分派顺序提取为 Editor 私有 ConsoleDispatch helper，Workspace 提供真实 call/stopping/refresh。
测试用同一 helper 和真实 Runtime，仅注入刷新失败；不增加公开故障开关或改变 Runtime 的注册/执行接口。

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

M22.3 实施：ConsoleHistory 为 CPU 内部模型，ConsoleInput 为 ImGui 私有适配；保留固定 8193 字节缓冲。
InputText 调用期间临时包装本 context 的剪贴板读取回调，先检查整段 UTF-8/控制字节与替换选区后的总长度，
拒绝则返回空内容且保留输入/选区，不让 ImGui 先过滤换行或截断前缀；复制/剪切写回调转发原 user data，
RAII 恢复原读写回调和 user data。
普通字符输入通过字符回调按 UTF-8 字节数限额，拒绝的同帧不执行 Enter/Run；不修改系统剪贴板内容。
历史回调只改输入；编辑回调退出历史浏览；提交成功清空后同步 ImGui 活动缓冲。输入框聚焦且处于
本窗口 SDL TEXT_EDITING 组合期间，关闭历史回调并忽略提交；TEXT_INPUT/组合结束或失焦清除组合状态。
真实 GUI 验收扩展现有 workbench smoke，输入仍走 ImGui 键鼠/剪贴板和 SDL 组合事件，使用独立夹具与 --background。
剪贴板验收临时使用测试回调，不读写用户系统剪贴板，覆盖与生产相同的 InputText 粘贴路径。

## 实施步骤和验证计划

| 阶段 | 改动范围 | 独立验收 |
| --- | --- | --- |
| M22.1 设计落档 | 本设计、Editor 设计入口、Roadmap、开发记录与索引 | 文档一致性；语法/兼容/guard/历史/预算和非目标明确；不声称功能实现 |
| M22.2 解析与执行 | Console.hpp/.cpp、Workspace、Editor CMake、现有 dk_editor_tests 新增解析/执行用例 | 两种语法映射一致；类型/Unicode/整数/边界；全行失败无副作用；显式 guard/草稿/零等待/刷新失败契约 |
| M22.3 历史与 GUI 交付 | ConsoleHistory、WorkbenchUi、CPU 历史用例、真实输入 smoke、指南/命令参考入口 | Up/Down/草稿/去重/容量；Enter/Run 一次执行；超限粘贴拒绝；后台 GUI/旧 JSON 回归；首期完整验收 |

每一实现小阶段分别定向构建、测试、留档和本地提交。首期最终完成要求 M22.2/3 全部通过。
代码分工与选择范围如下，实际测试名在实施时注册并枚举，不把下列候选当成已运行结果：

- 复用 [dk_editor_tests](../../tests/unit/CMakeLists.txt)，已新增 [ConsoleTests.cpp](../../tests/unit/ConsoleTests.cpp)，
  [ConsoleHistoryTests.cpp](../../tests/unit/ConsoleHistoryTests.cpp) 覆盖历史与日志边界。测试前缀为 `dk.editor.console `、`dk.editor.workspace console`；
  继承原 `workspace console shares explicit guards...` 回归。
- 类型用例：引号/空格/转义/中文/原样路径、嵌套 JSON、空数组/null、true 与字符串 true、
  整数与浮点 token、int64/uint64 边界与越界（含嵌套及兼容 JSON）、未知/重复参数/JSON 键。
- 拒绝用例：未闭合引号/括号、缺值/尾随垃圾、位置参数、非法 UTF-8/NUL/CRLF、8192/8193 字节、
  深度边界、分号/管道；确认一个合法编辑前缀后接错误尾部也完全不执行。
- 状态用例：同一参数的两种入口有相同结果；实体编辑一次 revision/Undo，缺失/旧 guard 和草稿保护；
  `jobs.wait` 省略/显式零允许与非整数/非零拒绝；通过故障注入验证已提交后刷新失败不重试。
- 历史 CPU 用例验证状态转换与容量；真实 GUI 通过现有独立夹具/输入驱动机制，使用 `--background`，
  覆盖键盘输入、粘贴超限、Up/Down 草稿恢复、成功清空/失败保留和旧 JSON。
  已通过 [ConsoleSmoke](../../engine/editor/src/ConsoleSmoke.cpp) 扩展 [SmokeDriver](../../engine/editor/src/SmokeDriver.cpp) 的工作台流程，
  优先运行 `dk.editor.workbench_gpu_validation`；若实现时需新增独立 smoke，须同步测试选择表。
- 定向构建使用 [verify.ps1](../../scripts/verify.ps1)，CPU 用例只需 `dk_editor_tests`；GUI 用例需
  `dk_editor_tests`、`dk_editor_app`，配置选择现有可用单配置。无需整个渲染/物理/IPC/Python/Luau 测试矩阵。
- 发布时更新 [编辑器指南](../guides/editor.md) 和 [命令参考入口](../commands/README.md)，
  新增文本示例并标注只适用于 Editor Console；原 JSON/IPC 示例继续有效，schema/命令目录无新增能力。

## M22.4 扩展设计：帮助、补全与设置命令

本节为采用的扩展方案；M22.5–7 的帮助元数据、描述快照、补全/参数提示、动态候选与诊断已接入，其余交互尚未实现，
阶段及验证状态以 [Roadmap](../roadmap.md#m22console-命令文本输入与-editor-交互) 为准。
既有命令、解析器、显式 guard、8192 字节输入、会话历史和提交后失败契约继续适用。
这里的 Editor 指 DeckerEngine 工作台，不包括 VS Code/LSP 或独立脚本编辑器。

### 共享元数据和只读发现（M22.5）

当前 `commands.list/describe` 提供命令说明、参数/结果 schema、effect/undoable；M22.5 为部分命令补充字段帮助。
schema 注册仍拒绝未知关键字，不直接塞入任意 description/unit/default；使用独立的可选帮助元数据：

- 在 CommandDescriptor 增加可选、拥有型 `help`（version=1），字段条目通过指向参数 schema 的 JSON Pointer 标识，
  例如 `/properties/limit`、`/properties/pathtrace/properties/target_spp`；数组元素使用 schema 的 `items` 路径。
  条目可含显示名、说明、单位、已定义的默认值、示例、搜索别名和已注册的候选提供器标识。
- 类型、必填、范围、枚举、数组形状始终读取原 schema，不另写一份校验规则；帮助缺失时退回字段名和已知约束。
  省略值没有明确默认行为时不展示“默认值”；帮助里的默认值不自动注入调用参数，也不代替服务实际缺省逻辑。
- 注册时验证帮助版本、字段引用、默认值/示例与 schema 的一致性及 UTF-8/大小预算；每命令帮助编码上限 64 KiB，
  同时服从命令值预算。存在条件关系的限制由服务验证，帮助说明条件，不伪造 schema 已表达的约束。
- `commands.list` 保持轻量结果；`commands.describe` 增加可选 `help`，旧字段含义不变。
  M22.5 首批覆盖 commands.describe、scene.query、environment.preset/set；M22.7 扩展实体/资产候选绑定，
  完整当前覆盖见[命令发现](../commands/discovery.md)，其余命令保留 schema/原说明降级。
- Runtime/Workspace 提供 owner 线程只读 `command_catalog_version()` / `command_catalog()`，来源仍是同一 registry，使用拥有型副本与代号。
  不在每次击键时调用 `dispatch(commands.describe)`，不增加 Task、Job、命令历史或 Scene 变化。
  缓存以注册表/宿主能力代号失效，描述缺失或失败时仍保留正常手动输入与原执行路径。
- 通用 Commands 层只持数据，不依赖 ImGui、SDL、Scene 或 GPU；Editor 会话控件在自己的模块提供帮助/定位映射。
  相同设置的命令和面板复用描述与校验来源，不要求将所有复杂面板自动生成。

具体 help v1 字段、注册前校验、目录身份/修订号与失败发布边界见 [Commands 实施契约](commands.md#m225-帮助描述与只读目录)，
验证记录见 [0152](../development/0152-command-help-catalog.md)。M22.7 已接入下述三类内建动态候选；注册语义标识本身不执行枚举。
Workspace 元数据读取不走 execute_console/refresh，在没有活动文档或存在未应用草稿时也只读取目录；
场景重载不改变静态目录，动态候选独立按文档/快照/资产来源失效。

### Console 基础补全与提示（M22.6）

先覆盖命令名、顶层参数名、布尔/null/枚举值与参数说明；按实际注册能力展示，不把未来命令混进候选。
示意：`scene.qu` → `scene.query`，`scene.query li` → `limit=`，
`render.capture renderer=` → `raster` / `pathtrace`。这些是编辑中的补全上下文，不是可直接提交的完整请求。

- CPU 补全模型接收文本、UTF-8 字节光标/选区和只读描述快照，返回候选、替换范围及说明；不调用业务 handler。
  扫描复用解析器词法规则，能表达未完成 token；最终执行仍使用严格全行解析，不对半成品尝试执行。
- 候选返回规范插入文本与独立显示名，字符串按当前语法正确引用/转义；只替换光标所在 token/选区，保留其他参数。
  不改 guard、UUID、整数精度或用户其他文本；补全后超过 8192 字节时整次替换拒绝，原输入和光标保持。
- 确定性排序：精确前缀优先，其次稳定名称顺序；同名候选以稳定身份区分。最多返回 32 项、弹窗显示最多 8 项并可滚动。
  弹窗说明也有界，候选及替换必须匹配当前输入修订号/光标，旧结果不能写回新输入。
- Tab 触发/接受补全；无候选不改文本。M22.6 已替换 M22.3 的“键入 Tab 插入空白”行为，
  解析与粘贴仍允许 Tab；不能同时启用 ImGui 的 AllowTabInput 与 CallbackCompletion。
- 弹窗打开时 Up/Down 只选候选；关闭时恢复 M22.3 历史行为。Tab、候选点击、弹窗中的 Enter 仅接受候选并关闭弹窗，
  不提交；需要再次 Enter 执行。Run 明确提交当前文本，不擅自接受高亮候选；Esc 只关闭候选并保留输入。
- 补全视为文本编辑并退出历史浏览，不追加提交历史。输入法组合期间暂停补全/诊断刷新，不抢候选方向键、Tab 或 Enter；
  结束后按最新文本重新计算。失焦/换会话关闭弹窗，不持有失效 widget/服务引用。

M22.6 实施约定（记录 [0153](../development/0153-console-completion.md)）：

- 私有 CPU 模型 ConsoleAssist 随 dk_editor_model 构建，复用解析器的字符规则、输入预算和完整值解析。
  M22.6 首批定位文本命令、顶层 key/value；M22.7 已按下述契约扩展 JSON envelope 与嵌套值内部候选。
  不完整字符串可以补全；确定的结构错误保守返回空列表，最终提交仍由原解析器裁决。
- 输入快照包含完整文本、UTF-8 字节光标/选区与递增编辑修订。候选持有来源快照与目录版本，
  接受前逐项比较；拒绝旧文本、光标、选区、目录及超限结果，失败保持原输入。
  无选区替换完整当前 token；选区恰好覆盖 token 时支持替换，跨字段/部分 token 选区保守不补全。
  参数名补全保留已存在的等号，过滤其他位置已填写的参数；字符串枚举用 JSON 引用，数值保留原 JSON 精度。
- 候选按区分大小写的前缀过滤、稳定名称排序，最多 32 项；显示名最多 256 字节、说明最多 2048 字节，插入值最多 8192 字节。
  提示由 schema 的类型/必填/范围/枚举与可选 help 的单位/默认值/示例组成；缺失帮助时仍显示已有约束。
- 第一按 Tab 打开候选，弹窗中 Tab/Enter 或鼠标点击接受并关闭，第二次 Enter 才提交。
  参数/命令说明在输入聚焦时显示；Esc 关闭提示，直到输入/光标变化或再次 Tab。
  单次接受不自动添加空格、不提交、不记历史；Run 单独点击仍提交当前原文；同帧候选接受优先，避免连带提交。
- Workbench 按 command_catalog_version 缓存拥有型目录；输入适配只在文本/光标/选区/目录变化时重算。
  组合输入暂停辅助并屏蔽其快捷键；失焦和换文档关闭提示。ImGui 私有适配在输入控件调用期间临时屏蔽已处理的
  Enter/Esc，结束立即恢复按键数据，避免 InputText 额外提交或恢复旧文本；不全局改变快捷键行为。
  粘贴带 Tab 时先整段预检、再在输入回调内按选区替换，避免关闭 AllowTabInput 后 ImGui 过滤已合法的粘贴空白。
  同帧文字和 Tab 的回调优先级不改变历史脱离：旧候选拒绝后，新文字仍视为编辑。
- 验收包含纯 CPU 的上下文/排序/Unicode/选区/引用/8192 边界和陈旧结果拒绝，以及既有后台 workbench smoke 的
  Tab/Enter/Esc/方向键/鼠标、历史脱离、IME、Run、旧 JSON 与无副作用回归；不新增命令或依赖。

### 嵌套值、动态候选与输入诊断（M22.7）

在基础补全上覆盖 JSON 对象字段/数组元素、兼容 JSON envelope 的 method/params，以及实体/资产引用。
以 schema 路径和明确候选提供器绑定语义，不用字段恰好叫 `id` 就猜测其来源。

- 实体候选显示名称与 UUID，实际插入 UUID；资产候选使用已知目录与 schema 种类过滤，不在击键时加载/导入资源或遍历磁盘。
  使用 Workspace 已发布的 Scene 快照和受限、按需的只读目录页；有缺页时提示“更多/加载中”，不能显示为空库。
- 输入/光标更新采用 100 ms 去抖，结果上限沿用 32 项；动态索引以 document_id/revision、目录代号与请求序号失效。
  换文档、资源删除或异步结果过期时丢弃旧候选；接受候选不保证实体仍有效，提交时由原服务重新验证。
- 诊断区分“输入尚未完成”和“确定错误”：缺值/未闭合输入可提示待补充；已闭合但类型错误、重复字段、未知字段显示错误位置。
  完整参数复用 schema 验证，部分输入不能报告尚未输入的必填字段已执行失败；业务前置条件不能仅凭静态诊断判定通过。
- 单次最多展示 16 条诊断，长度/深度预算沿用解析器；字节定位与 Unicode 显示位置对应。
  输入变化不能触发场景编辑、业务 Task/Job、日志刷屏或文件写入；最终 Enter/Run 重新解析校验。
  路径文件浏览、大模型建议、任意表达式和跨工程搜索不在本阶段范围。

M22.7 实施契约（[0154](../development/0154-console-context-assistance.md)）：

- ConsoleStructure 建立有界的部分语法树，保留 key/value 的字节范围、闭合状态与重复键；named 与 JSON envelope 共用
  JSON 对象/数组扫描。已闭合标量复用严格解析器，完整节点复用 schema 校验；未完成节点不补值、不尝试执行。
  envelope 的 method/params 顺序不影响 schema 解析，参数 JSON Pointer 始终相对于命令 parameters。
- 诊断最多 16 条、每条最多 512 UTF-8 字节，区分 incomplete 与 error，提供 byte 与 Unicode column。缺值/未闭合和未填的顶层必填参数为待补充，
  已闭合的错误类型、数值越界、重复/未知字段为错误；不推断 guard、业务状态或资源有效性。诊断只显示于辅助窗口，不刷日志。
- provider 明确绑定：scene.entity 用于现有实体查询/编辑目标及 parent（不用于创建 ID）；assets.any 用于资产 load/status/unload，
  assets.reference 用于 entity.set_assets 的 items/id，读取已发布 SceneReadSnapshot.project 映射，并按同一对象的 kind 过滤，尚未填 kind 时显示各合法种类。
  assets.any 只读活动 Runtime 目录；打开场景不隐式打开该目录，缺目录提示不可用。两种来源分别对应命令实际使用的身份范围。
  这些语义由 Operations 的 help 注册，不能仅据字段名 id 推断；不补 guard、不变更命令行为。
- Runtime 提供资产目录轻量版本和 1–256 条的拥有型只读页，版本含 available/session_id/revision；页面要求匹配版本。
  不 open/pump/load/dispatch，不读磁盘；缺模块/未打开目录明确不可用。Workspace 的候选页组合已发布 Scene 快照和此目录，
  版本同时包含 document_id/revision、Workspace 快照发布序号与资产版本；换文档、删除/改名、映射刷新、目录切换后旧页拒绝。
- ConsoleAssistCache 以输入快照、命令目录与候选来源版本作为请求身份，100 ms 稳定后计算；每次最多读取 128 条，
  按帧继续分页，保留最多 32 个匹配候选并显示“加载中/更多”。所有枚举在 owner 线程同步进行，不创建后台任务。
  显式 Tab 可立即计算静态上下文，动态页与实时诊断仍受去抖约束；任意版本变化立即撤销旧结果。
  发布核对请求序号和来源，接受再次核对当前输入与来源；迟到结果不能覆盖新请求，候选显示名称/路径、种类和完整 UUID。
  同一请求去抖完成或后续页发布时，若当前选中的插入值仍存在则保留选择，避免列表更新跳回首项。
- 继续沿 M22.6 的两次 Enter、历史/IME、预算与失败不变约定；新增结构/诊断/分页/迟到结果 CPU 用例和后台 GUI 输入。

### 视口渲染与相机命令（M22.8–9）

将前面讨论的视口/相机设置纳入独立阶段，基础补全无需等待它们。当前 `render.capture` 继续表示独立捕获，
不会因增加设置命令而隐式修改正在显示的视口；Scene 光照仍使用 environment.query/set，不另设第二个环境状态。

| 阶段 | 首轮设置范围 | 状态归属与限制 |
| --- | --- | --- |
| M22.8 视口 | 查询；Raster/Path trace；Surface/Scene media；Classic/ReSTIR、候选数/时空复用；target_spp、seed、bounces、天空/太阳开关、曝光；暂停/继续/重置；已有介质时间/云散射会话参数 | Edit 视口会话，不改 Scene dirty/Undo；target_spp 为绝对目标，不承诺 `iterate N` 精确追加；能力缺失时明确拒绝 |
| M22.9 相机 | 查询；Orbit/Fly/Walk、透视/正交、标准视图；FOV/裁剪/正交高度、速度/灵敏度；姿态、Orbit/Pan/Zoom、Frame/Reset | Edit 视口相机，角度/距离/步长单位显式；不修改物体 Transform，不引入碰撞/重力；Simulation 独立视口命令后置 |

采用 `editor.*` 宿主命名空间，M22.8 的具体命令表见[视口参考](../commands/editor-viewport.md)；M22.9 见[相机参考](../commands/editor-camera.md)。
通过宿主注入 CPU 可见的会话控制服务接口，由 Editor owner 线程处理；Runtime/Operations 不反向链接 ImGui 或整个 Editor。
只在附加了服务的 Editor 注册，runner 不提供虚假的可用命令；Console、Editor IPC 与 GUI 调用相同会话控制逻辑。

状态契约（M22.8/9 共用）：

- 查询返回明确视图身份、会话修订号、设置和能力；写入携带匹配的会话 guard，不能静默选择最后聚焦窗口。
  Scene guard 与视口会话 guard 分离；换文档、关闭/替换视图使旧观察失效，不自动更新历史命令里的 guard。
- 构造候选设置、全部校验后一次发布；无变化不增加修订号或重置累积。坏参数/失效 guard/不支持模式拒绝后保留原设置和已发布画面。
  活动 Gizmo/导航手势、相关未应用草稿的冲突策略须在阶段命令表明确，首轮拒绝冲突写入，不覆盖用户正在编辑的值。
- 命令成功表示设置已接受，GPU 完成/当前显示来源继续通过查询状态区分；后续渲染失败保留上一完整画面并标出失败/过期，
  不把设置回执当作新画面完成。曝光等保留采样、算法/物理参数等重置采样沿用现有契约。
- 会话修改标记 control、不可进入 Scene 事务/Undo、不随 scene.save 持久化；环境/实体编辑保持原来的事务与撤销语义。
  关闭时先停止接收写入并解除回调，再释放视图/服务，验证挂起任务与迟到结果不会访问已销毁 owner。

### M22.8 实施契约：Edit 视口会话

- 新增纯 CPU EditorViewportService（framework/services）与 EditorViewportOperations；Runtime 只在显式附加宿主时注册
  editor.viewport.query/set/pause/resume/reset。runner 默认无这些命令；Workspace 持有会话，GUI 不再保存另一份权威路径参数。
  Runtime 不反向依赖 Editor/ImGui/Render。路径预览设置的纯值类型下移，worker 继续消费拥有型副本。
- query 无参数，返回 guard、settings、capabilities、blocked_reason、presentation。guard 为
  `{view_id,document_id,revision}`；view_id 标识本次 Edit 视口，document_id 从 Runtime 当前场景实时读取，revision 计视口设置与 Edit 相机偏好/姿态变化。
  文档切换保留用户视口偏好，但旧文档 guard 失效；detach/重新 attach 分配新 view_id，旧服务关闭并清除上下文回调。
- set 使用顶层可选命名参数：renderer=raster/pathtrace、medium_mode=surface_only/scene_media、direct_lighting=classic/restir_di、
  initial_candidates=1..64、temporal_reuse/spatial_reuse、target_spp=1..UINT32_MAX（绝对目标）、seed=0..UINT32_MAX、
  bounces=1..32、environment_rgb=[0..1000] 三元组、sky_enabled、sun、exposure_ev=-16..16、raster_exposure=0..1e6、
  unlit_inspection、time_seconds=-1e9..1e9、cloud_albedo=0..1、cloud_anisotropy=-.95...95。
  set 至少一个设置；pause/resume/reset 仅 guard，要求当前 renderer=pathtrace 且能力可用。reset 单独递增重置序号及会话 revision。
- query 为 query；其余为 control、undoable=false，不支持 scene.transaction，不保存 Scene/工程、不修改 dirty/Undo。
  set 从当前值构造候选，Operations schema 与 Service 完整强类型校验后一次发布；坏参数/过期 guard/能力不足保留全部原设置。
  无变化 set/pause/resume 不增 revision、不重置采样。path 模块/GPU 不可用时拒绝显式路径字段，即使当前 renderer=raster。
- 活动 Edit Gizmo/导航、未应用 Inspector/Environment 草稿及打开的 Camera 弹层拒绝视口命令写入；查询仍允许。GUI 同样调用服务，禁用相关控件，
  不因命令修改取消手势或覆盖草稿；Simulation 画面不成为默认目标，命令始终只设置明确 guard 标识的 Edit 视口。
- 服务不等待 GPU。渲染观察由 owner 在绘制后发布；presentation 同时标出观察时 guard、已显示图像 guard/Scene 来源、
  current/phase/error/spp/generation。新设置先 accepted，只有实际匹配的完成图像才 current；失败保留前一图像来源。
  曝光/目标/暂停保留已有样本，物理设置/算法/介质与 reset 走既有 worker 失效规则；不承诺精确追加 N spp。
- Console/IPC 经同一 Runtime/Operations/Service；GUI 直接使用同一 Service 与校验，无按帧业务 Task。
  GUI detach 在 renderer/device 销毁前执行；Runtime shutdown 先关闭宿主，后续写入拒绝，迟到观察不得复活会话。
- 验收：CPU guard/原子拒绝/no-op/能力/生命周期/草稿/旧文档/发现隔离；真实后台 GUI 按钮、Console 输入和 IPC 交替控制，
  核对曝光保留、reset/模式变化、来源/失败画面以及关闭。沿用单一 Debug 定向验证，记录见 [0155](../development/0155-editor-viewport-commands.md)。

### M22.9 实施契约：Edit 相机命令

- 新增 editor.camera.query/set/orbit/pan/zoom/frame/reset，仅随 Edit 宿主附加注册。沿用 M22.8 的 view_id/document_id/revision guard；
  相机与视口设置共享修订号，GUI 改动也使旧 guard 失效。Scene/Undo/文件不变；query 为 query，其余 control、不可事务/撤销。
- Camera 的纯 CPU 值模型移至 dk_editor_services（PUBLIC Geometry/Math），保留 dk/editor/Camera.hpp 兼容入口及 dk::editor 命名空间；
  CameraNavigation 保持 CPU 输入模型。EditorViewportService 拥有唯一 Edit Camera，Simulation 继续独立持有 Camera。
  不让 Runtime/Operations 反向依赖 Editor、SDL、ImGui 或 Render，不引入依赖版本变更。
- query 返回 guard、settings（navigation_mode、projection、standard_view、镜头/导航设置及 eye、yaw_deg/pitch_deg、focus_distance）、
  effective_clip（near_plane/far_plane）、render_revision、aspect、blocked_reason 与已显示 presentation。guard 计所有会话偏好；render_revision 只计实际姿态/投影变化，
  改速度/导航模式不重置路径累积。渲染上下文使用当前文档环境和当前渲染器推导，不等待 UI 下一帧同步；读取时只刷新派生上下文缓存，使自动裁剪的 render_revision 保持单调，
  不修改偏好或 guard。aspect 为最近实际 Edit 视口比例，初始 1。
- set 至少一个顶层设置：navigation_mode=orbit/fly/walk、projection=perspective/orthographic、standard_view=front/back/left/right/top/bottom；
  fov_y_deg (1,179)、ortho_height [.01,100000] m、automatic_clip、near_plane/far_plane（有限 0<near<far）、
  move_speed [.01,1000] m/s、fast_multiplier [1,100]、slow_multiplier [.01,1]、look_sensitivity/orbit_sensitivity [.0001,.05] rad/pixel、
  pan_multiplier [.01,10]、wheel_sensitivity [.01,1]、invert_y；eye 各分量 [-1e9,1e9] m、yaw_deg [-180,180]、pitch_deg [-90,90]、focus_distance [.1,3000] m。
  省略保留当前值，所有字段先在候选 Camera 上操作/验证后整体提交；投影换算先于其余设置，换算越界时先单独修正高度/距离。
  显式 pose 字段与 standard_view 互斥，六视向仅正交，正交只允许 Orbit；沿用已有数学与后端镜头限制。
- orbit(yaw_deg,pitch_deg) 为角度增量（可省略单轴，至少一个轴），正 yaw 增加眼点绕 pivot 的方位角、正 pitch 抬高眼点；
  自由 pitch 按既有 Orbit 饱和到 ±1.55 rad。pan(right,up) 为相机右/上方向的米数，至少一个轴；不依赖窗口像素或灵敏度。
  zoom(steps) 为滚轮步数，正数拉近，沿用当前 wheel_sensitivity 与距离/正交高度饱和规则；Orbit/Pan/Zoom 要求 Orbit 模式。
- frame 接受 entity UUID 或 bounds={minimum[3],maximum[3]}，恰选其一。显式 bounds 为世界米；entity 必须来自当前文档，
  宿主只用已发布且同文档/Scene revision 的几何（含子树）取边界，不触发磁盘/GPU 等待；未准备好/失配明确拒绝。
  GUI Frame 复用相同取景数学与边界；reset(defaults=false) 只恢复本视口默认姿态，defaults=true 恢复模式/投影/镜头/导航。
- 外部写入拒绝未应用 Inspector/Environment 草稿、打开的 Camera 草稿、活动 Edit Gizmo/导航；query 允许。
  GUI 的草稿 Apply 与其拥有的连续导航可提交同一服务；沿用 M18 的显式 GUI 操作规则，保留 Inspector/Environment 草稿也可移动相机，
  不修改或覆盖这些草稿，Gizmo 仍阻止相机写入。外部命令不能借用 GUI 豁免。
  相机弹层取消/关闭/文档切换清除阻止标记；GUI 候选不在服务中形成第二份权威状态。
- 同值/零增量不增 guard、不重置样本；坏参数、旧 guard、不支持模式、无法 frame 等保留全部相机/设置/图像。
  采样提交继续异步；query 的 current 同时核对最新相机渲染修订号，旧画面不能被当作新姿态完成。关闭清除 frame 回调，迟到操作拒绝。
- 定向 CPU 验证候选原子性、单位/六视图/模式、共享 guard、纯偏好不改 render_revision、草稿/手势、Frame 来源、关闭与无 Scene 副作用；
  后台 GUI/Console/IPC 交替改动，核对真实相机/图像来源、失败保留、路径样本和活动关闭，回归原有相机/导航/正交输入与 M22.8。

### Editor 帮助与设置搜索（M22.10）

首轮覆盖 Console、Inspector Transform、Environment、Edit 相机、Path tracing 和 Simulation 已有设置控件。
悬停显示名称/含义、类型/范围/单位、明确默认值、持久化/Undo/采样重置影响；禁用控件显示当前不可用原因。
只显示确有定义的信息，不能把 GPU 可用性、当前显示值和待应用草稿混为一谈。

设置搜索支持显示名、中文搜索别名与规范参数名，展示分组/面板路径和可用性；选择结果只打开面板、定位并突出对应控件。
定位用稳定控件标识，不记录屏幕坐标或直接执行修改。可复制/插入相关命令模板，但保留必填占位，需用户明确提交；
没有命令接口的字段只能定位与解释，不生成看似可执行的命令，也不自动补 guard。
可用搜索按钮/命令面板入口，具体快捷键实施时检查已有绑定；不要求整个工作台重排或自动生成所有表单。

#### M22.10 实施契约

- editor_model 增加纯 CPU SettingsCatalog：稳定语义 ID、面板/栏目、显示名、中文搜索别名、相关命令及 schema JSON Pointer。
  schema/help 从 Workspace 的拥有型只读目录解析，与 Console 共用字段说明格式化入口；没有声明的默认值不猜测。
  面板专有的草稿/持久化/Undo/采样影响由控件绑定补充，GUI-only 字段明确无命令模板，不把 Edit 命令套用到 Simulation 相机。
- 工具栏 Settings 打开可关闭的搜索窗口；查询限制 256 UTF-8 字节，按 ASCII 不区分大小写的空白分词与中文原文匹配，结果最多 64 条、稳定排序。
  查询覆盖规范 ID/参数、显示名、面板栏目、别名及共享帮助。目录变化重新生成元数据；不 dispatch、不刷新场景、不建 Task/Job。
- HelpUi 只持有目录副本和 UI 状态。每帧读取当前可用条件；悬停含 disabled 控件可见统一说明及具体原因。
  选择 Locate 使用语义 ID，下一帧聚焦/展开面板、栏目或 Camera 弹层，滚动并短时描边；不保存屏幕坐标，不切换渲染/导航模式，不填实体选择。
  相机弹层仅在原本关闭时建立当前设置副本，已存在的草稿不重置；换文档清除待定位状态。无选择/无能力时定位面板内的说明，保留请求目标。
  ReSTIR/Scene media 专属参数在未启用时仍可定位，以禁用控件和启用条件说明呈现；搜索不代替用户启用它们。
- 模板从实际注册命令 schema 生成，包含全部顶层及嵌套必填值和目标字段；UUID/guard/修订号等全部保留带名称的占位，不读取当前值自动补齐。
  Copy/Insert 均需明确点击；Insert 只允许空 Console，遵守 8192 字节/UTF-8 输入限制，失败保留原输入。插入不执行、不记历史，不覆盖 IME 组合输入。
  未注册命令和无命令的本地渲染质量设置不产生模板；必填完整对象（例如 environment）仍必须由用户填写后通过原业务校验。
- 首期覆盖 Console 输入/执行说明、Inspector 名称/TRS、Environment 物理与本地预览设置、Edit 相机镜头/导航、Path tracing 现有设置、Simulation 实验/结果导出设置及控制。
  搜索/定位/提示保持 Scene revision/dirty、Undo/Redo、文件、任务账本、相机/路径权威设置和各面板未应用值不变；允许面板展开、滚动、焦点与空 Console 模板变化。
- 验证：CPU 对照真实目录的 schema/help、中文/多词/限额、缺能力/缺命令、模板必填/占位/无自动 guard 以及只读性；
  真实后台 GUI 点击/输入搜索、打开折叠栏目与 Camera 弹层、禁用提示、模板复制/插入/非空保护，核对 Scene/Task/草稿及两入口元数据。
  定向回归 Console 输入/补全/上下文和已接入的相机/视口 GUI，不扩大到既有 Simulation GPU 发布频率修复；记录见 [0157](../development/0157-editor-settings-help.md)。

### 文件范围与定向验收（M22.5–11）

| 范围 | 接入位置 | 独立检查 |
| --- | --- | --- |
| 帮助与描述快照 | Commands 描述/注册、Runtime 与 Workspace 只读入口；既有 schema 保持职责 | 帮助引用/预算/缺省正确；坏元数据拒绝；查询不产生 Task/场景变化；旧发现结果兼容 |
| 补全与诊断 | editor_model 中 ConsoleStructure/ConsoleAssist/ConsoleAssistCache 纯 CPU 模型；ConsoleInput/WorkbenchUi 弹窗；ConsoleTests/ConsoleSmoke | 光标中间/选区、中文/转义、半成品/完整错误、精确整数、超限拒绝；候选/历史/IME/Enter/Run 冲突 |
| 动态候选 | Workspace 快照与受限目录提供器 | 重名与类别、分页、空结果/加载中区分、删除/换会话/迟到结果、只读性 |
| 视口/相机 | EditorViewportService/EditorCamera、EditorViewportOperations/EditorCameraOperations；PathPanel/CameraSettingsUi/导航共享入口 | GUI/Console/IPC 同状态；guard/草稿/活动手势冲突；重置/保留累积；异步失败与关闭；runner 无宿主命令 |
| 面板帮助/搜索 | 共享帮助适配、各面板稳定控件定位与 WorkbenchUi | 文案/单位对应元数据；禁用原因；搜索只定位；缺命令不造模板；无 Scene/任务副作用 |

CPU 验证优先复用 dk_commands_tests、dk_protocol_tests（含 Runtime 用例）、dk_editor_tests，实际测试名在实施时核对并更新选择表。
GUI 必须通过真实 ImGui/SDL 输入及 `--background` 验证补全弹窗、历史/Tab/IME 与搜索定位；视口命令阶段才增加相关渲染、IPC 和来源对照。
M22.11 已汇总前面独立证据，并以实际窗口/IPC 完成“发现/补全 → 设置 → 面板同步 → 只读状态/图像确认”的跨入口流程，
保留首期 JSON/命名参数/guard/预算回归。默认单一适用配置，不把全量渲染/物理或双配置矩阵设为固定门槛。
本扩展复用现有依赖；CPU 辅助不依赖 GPU 初始化，完整闭环/示例与阶段证据见 [0158](../development/0158-console-workflow-delivery.md)。

### M22.11 实施契约：扩展集成闭环与交付

- 新增独立后台原生 GUI 验收，不改变命令/解析/服务协议：通过实际 Console 键鼠完成命令名及 renderer 枚举补全，
  明确提交后通过 Settings 中文搜索定位 Camera/Path tracing 控件，并读取实际输入框显示值核对同步。
  GUI Apply 后验证旧 guard 拒绝，旧 JSON 输入与命名参数具有相同相机/画面结果。
- 原生检查点仅在匹配的 Scene/相机画面发布后写入观察记录及 PPM；外部 Python 客户端独立查询命令发现、schema/help、
  guard、参数及 presentation 来源。命令接受不等同于画面完成；路径等待 current 且达到绝对目标 spp，不将旧图或错误当作通过。
  设置/相机变化保持 Scene revision/dirty、Undo、文件和 Simulation 相机不变；补全接受只改文本，明确 Run/Enter 才执行。
- 提供可运行 Python 示例：连接 Editor，发现并预检能力/模式/草稿，从 query/成功回执显式携带共享 guard，启用 ReSTIR、设置绝对 spp、执行角度 Orbit，
  有界轮询只读 presentation。发现并发 guard 变化/错误/超时立即报告，不重试写入、不覆盖用户状态，不自动保存或关闭宿主。
  错误判定使用 observed_guard 对照本次成功回执，避免把尚未被 owner 更新的上一轮失败当成本次失败；确定性脚本测试覆盖这条异步观察边界。
  多条命令顺序提交，不承诺跨命令事务或自动回滚；当前操作失败时已完成操作保留，文档明确其可观察边界。
- 在一次适用 Debug 配置运行新闭环、首期命名参数/JSON/guard/预算/历史 CPU 回归和原真实 Console UI；汇总 M22.1–10 既有独立证据，
  不为了关闭里程碑重跑无关渲染/物理矩阵。既有 Simulation GPU 发布频率失败继续单列，不修改门槛、不算作通过。
- 更新指南、示例入口、命令参考与能力/限制表，确认宿主命令与普通 runner 隔离；完成后关闭 M22.11 及 M22，其他路线阶段不自动推进。
  实际证据见 [0158](../development/0158-console-workflow-delivery.md)。

## 决策与后续扩展

首期使用命名参数，不推断参数位置，不在前端转换业务单位。数组/对象复用 JSON，避免引入第二套嵌套数据语言。
显式 guard 保持当前可复现/冲突检测契约；便捷 guard、点号嵌套参数或常用别名若后续需要，单独定义覆盖与冲突规则。
补全和面板帮助共享现有 schema 与独立帮助描述，阶段划分见 Roadmap；基础输入首期不因扩展规划而重开验收。
视口/相机命令已按下述契约接入宿主服务；位置语法与自动 guard 保持后置。

## 相关记录

[Editor 设计](editor.md)、[命令设计](commands.md)、[服务状态契约](application-services.md)、
[原 Console 实现记录](../development/0127-editor-media-pathtracing.md)、
[0148 首期计划](../development/0148-console-command-input-plan.md)、[0150 首期交付](../development/0150-console-history-ui.md)、
[0151 扩展路线设计](../development/0151-console-assistance-plan.md)、[0154 嵌套辅助与诊断](../development/0154-console-context-assistance.md)、[0156 相机命令](../development/0156-editor-camera-commands.md)。
