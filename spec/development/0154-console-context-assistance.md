---
id: "0154"
created_at: "2026-11-03T13:58:00+08:00"
updated_at: "2026-11-03T14:27:23+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/runtime.md
  - ../design/commands.md
---

# 0154 M22.7 嵌套辅助、动态候选与输入诊断

## 目标与设计依据

完成 [M22.7](../roadmap.md#m22console-命令文本输入与-editor-交互)，基线 41c7a37，开始时工作区干净。
依据 Console/Runtime 设计补齐部分 JSON 结构、显式候选 provider、只读分页、100 ms 去抖与来源失效、诊断显示。
不扩展视口/相机设置命令，不自动执行、补 guard 或加载资产。

## 实际变更

- [ConsoleStructure](../../engine/editor/src/ConsoleStructure.cpp) 为 named/JSON envelope 建立有界部分语法树，
  保留 key/value 字节范围、闭合状态、数组层级、重复键与 Unicode 位置；输入 8192 字节、深度 64。
  完整标量复用严格解析，完整节点复用 schema；不修改最终执行解析器。
- [ConsoleAssist](../../engine/editor/src/ConsoleAssist.cpp) 按实际 schema 路径提供嵌套字段、数组项、envelope method/params 补全。
  支持 method/params 先后顺序、已填字段过滤、光标中间/整 token 选区；字符串引用、整数精度及预算拒绝保持原契约。
  诊断最多 16 条、各 512 UTF-8 字节，区分 incomplete/error；缺值/未闭合/未填参数为待补充，类型/范围/重复/未知字段为错误。
  不把业务前置条件或 guard 当作静态验证通过；最终提交仍重新解析和校验。
- [SceneOperations](../../engine/framework/operations/src/SceneOperations.cpp) 显式声明 scene.entity、assets.reference，
  [AssetOperations](../../engine/framework/operations/src/AssetOperations.cpp) 声明 assets.any；使用 help JSON Pointer 绑定字段，
  不根据字段恰好叫 id 猜来源。entity.create.id 与 guard 不绑定已有身份；命令名称、schema、effect/undoable 不变。
- [Runtime](../../engine/framework/runtime/src/Runtime.cpp) 提供资产轻量版本与拥有型只读页，要求匹配 available/session_id/revision，
  limit 为 1–256；无目录/模块或停止时不可用。无 dispatch/pump、Task/Job、磁盘读取或自动 load/open。
  [Workspace](../../engine/editor/src/Workspace.cpp) 分页读取已发布实体、SceneReadSnapshot.project 资产映射或 Runtime 活动目录。
  候选来源版本含文档身份/revision、快照发布序号和资产版本，保护映射刷新、删除/改名、换文档/目录。
- [ConsoleAssistCache](../../engine/editor/src/ConsoleAssistCache.cpp) 按输入/光标/选区、目录与来源版本失效，100 ms 稳定后计算，
  每帧最多一页 128 条，最多保留 32 个匹配候选；显式 Tab 可立即计算静态上下文，诊断和动态读取仍等待去抖。
  发布核对请求序号和来源；没有后台 Job，迟到结果由 CPU 测试模拟并拒绝。
- [ConsoleInput](../../engine/editor/src/ConsoleInput.cpp) 显示加载/更多页、候选名称/路径/种类/完整 UUID、上下文提示和定位诊断。
  保留双 Enter、历史、鼠标、IME、选区与粘贴保护；刷新保留仍存在的已选身份。长控制字符名称先转义/限长，再追加完整身份。
  诊断不刷日志、不入提交历史，接受只改文本。元数据和候选读取不刷新草稿或场景。
- 新增 [ConsoleContextTests](../../tests/unit/ConsoleContextTests.cpp) 6 项 CPU 用例和
  [ConsoleContextSmoke](../../engine/editor/src/ConsoleContextSmoke.cpp) 46 步后台 GUI；原 26+48+66 步仍执行。
  去抖 GUI 依据 ImGui 实际时钟等待至少 140 ms，以帧驱动继续运行，无阻塞 sleep 或固定 GPU 帧率假设。
  新文件进入原 dk_editor_model、dk_editor、dk_editor_tests target，无新增依赖或 executable。

## 验证记录

Windows / VS2026 / windows-editor Debug；依赖 baseline/构建选项不变。
使用 scripts/verify.ps1，按直接影响逐轮缩小范围；GUI 使用独立夹具与 --background。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_editor_tests','dk_protocol_tests','dk_commands_tests','dk_editor_app','dk_run') `
  -TestRegex '^dk\.(editor\.(console |workspace console|catalog )|protocol\.catalog |commands\.(help |catalog ))' `
  -Reason 'M22.7 partial structure, diagnostics, readonly references and discovery'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_editor_tests','dk_editor_app') `
  -TestRegex '^dk\.editor\.(console |workspace console|catalog |workbench_gpu_validation$)' `
  -Reason 'M22.7 source versions, readonly publication, legacy Console and GUI'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_editor_tests','dk_editor_app') `
  -TestRegex '^dk\.editor\.(console (context |assistance )|workbench_gpu_validation$)' `
  -Reason 'M22.7 preserve selection across debounce and locate nested overflow'
```

证据位于 out/verify，各轮保留 summary/build/tests/results；两次成功 GUI 另保存 LastTest.log。

| 目录 | 实际结果与处理 |
| --- | --- |
| 20261103-140531-7a1b043e | 构建失败，std::string 与 Json 比较的 MSVC 重写运算符冲突；改为提取 JSON 字符串，未运行测试 |
| 20261103-140822-31257c70 | 35 项 CPU 全通过，无失败/跳过；初版纯辅助、既有 Console/Workspace 与 Commands/Protocol 目录兼容 |
| 20261103-141408-c97bccae | 10 通过、2 失败；删除夹具选中了有子节点父实体，改为叶节点；GUI 暴露场景加载不打开异步目录，按实际语义分离工程引用/活动目录来源 |
| 20261103-141700-89d8c4b5 | 构建失败，新测试 INFO 三元表达式缺括号；加括号并修复局部变量遮蔽警告，未运行测试 |
| 20261103-141900-38629d72 | 29 CPU + 1 GUI 全通过，无失败/跳过；新增 39 步 GUI 完整通过 |
| 20261103-142118-e27a5f9e | 11 CPU + 1 GUI 全通过，无失败/跳过；嵌套非有限数值定位与去抖发布保留选择，GUI 扩为 46 步 |
| 20261103-142254-01c43016 | 1/1 通过，无失败/跳过；控制字符名称转义后仍保留完整 UUID，Editor 消费者重建通过 |

最后补强：`20261103-142507-b30c20de` 对应只读用例 1/1 通过，无失败/跳过；显式打开测试目录后，
100 次读取覆盖 Runtime 活动目录与已发布工程映射，Task/草稿/文件不变。此轮只改测试，生产实现不变。

去重后的定向范围为 **36 项 CPU + 1 项真实后台 GUI**（各轮重复执行不累加）。
CPU 覆盖嵌套/转义/Unicode/精确整数、深度和候选预算、同名身份、kind 前后顺序、260 条分页、空目录、
100 ms 去抖与每页预算、旧输入/目录/文档/请求拒绝、真实实体删除与资产目录替换、只读查询保持草稿/文件/Task。
真实 GUI 验证补全接受不执行、显式 Enter 查询、嵌套 kind 与资产 UUID、envelope 字段/枚举、待补充/类型/重复诊断，
重命名与撤销导致候选失效、重新查询更新名称、快速 Tab/Down 在去抖发布后保持选择，持续检查 revision/历史/错误数。

最终 GUI 输出 `workbench frames=3604 revision=8 draws=3 ... smoke=passed`；
`validation errors=0 warnings=1 liveAllocations=0 knownLoaderWarnings=1`。
唯一告警为既有 VK_LAYER_AMD_switchable_graphics API 1.3/应用 1.4 Loader 提示，无新增验证告警。
最终构建无编译警告。GUI 产物：
`out/build/windows-editor/test-artifacts/Debug/editor-32b388f247d04b24ad46731bd34e2583`。
最终及前一轮产物已转换 PNG 并目视检查候选/完整 UUID、字段提示和诊断布局。
最终 GUI 后的标签转义修正仅影响控制字符名称，由上述定向 CPU 用例验证并重建 Editor；没有重复不受影响的 GUI 流程。

新构建 dk-run 在独立空工程执行 15 次 commands.list/describe：49 条命令，13 条可选 help，
核对新增三类 provider 路径、旧六字段/列表四字段和默认值不变、entity.create 无已有 ID provider；工程目录无输出。
请求/回执/摘要保留 `out/m22-7-discovery`。首次误用 Editor 的 --root 参数被 runner 拒绝，未执行查询；
按 --help 更正为 --project-root 后全部通过，不涉及生产代码修正。

未运行：全量、Release/其他平台、独立关闭 Asset Runtime 的配置、渲染/物理/IPC/Python/Luau 全矩阵。
未增加或验证 M22.8+ 的设置命令/面板搜索。IME 沿原 SDL 事件注入覆盖，不代表人工逐个核验操作系统输入法候选窗口。
`scripts/check-spec.ps1 -Path` 检查本次 14 份 Markdown、927 个本地链接、元数据/表格/索引/测试入口与 JSON 清单通过。
`git diff --check` 通过；仅 Git 提示若干现有 CRLF 文件在入库时规范化为 LF，无空白错误。

## 偏差与决策

候选页采用 owner 线程按帧限量读取，不引入后台 Job；请求序号保护迟到页/结果。
实际验证发现 scene.load 与 assets.open 的目录生命周期不同，故 entity.set_assets 取当前工程映射，
assets.load/status/unload 取活动目录；该区分避免跨工程误导，也无需辅助层擅自打开目录。
Workspace 每次快照发布递增序号，允许保守失效，以覆盖不改变 Scene revision 的工程映射刷新。

## 遗留问题与下一步

M22.7 实现和定向验证完成，独立本地提交；下一阶段 M22.8，先设计 Edit 视口宿主接口/命令表/会话 guard，再接入设置。
部分 token/跨字段选区继续保守不补全；无目录时明确不可用；最终业务校验仍不可由候选或诊断替代。

## 修改记录

- 2026-11-03T13:58:00+08:00：建立记录并细化本阶段设计与失败边界。
- 2026-11-03T14:24:35+08:00：实现、CPU/后台 GUI、runner 发现通过；同步中间失败修正与实际候选来源，最后执行静态检查并独立提交。

- 2026-11-03T14:27:23+08:00：最后标签/只读用例通过，文档与 diff 静态检查通过；完成阶段留档。
