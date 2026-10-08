---
created_at: "2026-10-08T15:06:00+08:00"
updated_at: "2026-10-08T15:06:00+08:00"
---

# Luau 场景脚本

[返回项目入口](../../README.md)。M9.1 支持可信离线 `.luau` 源码，通过同一 Runtime 命令服务构造、
编辑与保存场景。执行时长、取消和内存预算尚未实现；不要在编辑器 owner 循环中执行可能无限运行的脚本。
完整边界见 [Luau 设计](../design/scripting-luau.md)，参数/状态语义见[命令参考](../commands/README.md)。

## 构建与运行

在仓库根运行：

```powershell
cmake --preset windows-scripting
cmake --build out/build/windows-scripting --config Debug --target dk_run
New-Item -ItemType Directory -Force out/luau-demo | Out-Null
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root out/luau-demo --script examples/scripting/create-scene.luau
```

[示例](../../examples/scripting/create-scene.luau) 创建三个实体、设置名称/TRS/父子关系，
输出 `out/luau-demo/scene.json` 和 `project.json`。可在 runner 的 batch/stdio 模式中
调用 `scene.load`（manifest 为 `project.json`）和 `scene.query`，或在编辑器打开清单。

`--script FILE.luau` 与 `--batch`、`--stdio`、`--pipe` 互斥，不能搭配 `--auto-guard`。
脚本路径相对当前工作目录；命令内的持久化路径相对 project-root，父目录必须存在。
只接受小写 `.luau` 扩展名并始终编译源码，不接受外部字节码。
成功退出 0；参数、文件读取、编译及未处理脚本异常退出 2，诊断写 stderr；宿主致命异常退出 3。
脚本模式成功时 stdout 为空，默认不提供 print 或协议响应；需结果时通过命令保存场景。

`DK_BUILD_SCRIPTING_LUAU=ON` 要求 Scene/Framework Runtime，自动选择 scripting feature。
默认 windows-dev 不引入 Luau；windows-scripting 继承 windows-dev，仍为无窗口、无 Vulkan 的 CPU 配置。

## 命令调用

```luau
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
local s = call("scene.new", {name = "Example"})
local created = call("entity.create", {
    guard = {document_id = s.document_id, revision = s.revision},
})
```

`dk.command` 返回 `{ok=true,value,task_id}` 或 `{ok=false,error,task_id}`。
error 包含原引擎 `code`、`name`、`message`、`context`；task_id 可用于 `tasks.get`，
没有进入 Runtime 分派的值转换/能力拒绝错误使用 `dk.null`。
业务错误由脚本决定是否 `error` 中止；检查 `ok` 后再读取 `value`。
宿主致命异常由宿主重新抛出，即使脚本用 `pcall` 捕获回调错误，也不能恢复该次宿主运行。

允许的命令集合：

| 功能 | 允许命令 |
| --- | --- |
| 场景/工程 | scene.new、scene.load、scene.query、scene.save、scene.transaction、project.save |
| 实体 | entity.create、entity.get、entity.delete、entity.set_name、entity.set_transform、entity.set_parent、entity.set_assets |
| 历史 | history.status、history.undo、history.redo |
| 发现/状态 | commands.list、commands.describe、runtime.capabilities、tasks.list、tasks.get |

未在集合中的命令返回 not_supported，包括 runtime.shutdown、assets/jobs/render 命令。
commands.list/describe 与 runtime.capabilities 描述宿主能力，不能作为脚本授权清单。
命令参数仍执行原 schema、业务前置条件和 guard 校验；不会自动补 guard 或重试过期请求。

脚本不是自动事务：后续脚本异常不会回滚已完成命令或磁盘保存。使用 `scene.transaction`
完成一组原子内存编辑；失败保持文档/历史，成功形成一个撤销单元。文件效果沿用原服务范围。

## 值转换

| Luau 值 | 命令 JSON |
| --- | --- |
| 字符串键 table / `{}` | 对象；字符串和键保留完整长度，由原命令验证 UTF-8/NUL 等约束 |
| 连续 1-based 数字键 table | 数组 |
| `dk.array()` | 空数组；可用 table.insert 填充，返回的 JSON 数组也保留标记 |
| `dk.null` | null；nil 表项代表缺字段 |
| boolean/string/有限 number | 原值；整数 number 自动变为整数 token，绝对值须 ≤ 2^53−1 |
| `dk.integer("18446744073709551615")` | 完整 int64/uint64 范围整数；用十进制字符串构造 |

结果中的大整数自动成为不透明整数值，`tostring` 获取精确文本；直接回传，例如 guard.revision，
保持原整数值。不要先转换为 number，也不对这类 userdata 做算术；最新 revision 从命令结果获取。
循环表、混合键、稀疏数组、普通带 metatable 的对象、非有限数值、函数等非 JSON 类型均被拒绝。
转换沿用深度 64/节点 200000 边界，命令参数/结果还须满足原编码尺寸和 schema 限制。
结果转换失败可能发生在命令提交之后，返回错误仍保留 task_id；应查询状态，不能盲目重试。

每次宿主 `run_luau` 调用使用新的 VM；全局变量不会跨脚本保留。
内置库及 dk 表冻结，没有 io/package/require/loadstring/文件或进程 API，
但允许的 scene.load/save 仍有工程文件效果；不将这些限制视为完整引擎安全沙箱。

## 定向验证

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.luau\.' -Reason 'Luau 场景绑定和 runner 保存重载'
```

实际验收及限制见 [0070](../development/0070-luau-command-bindings.md)。预算、取消与退出控制按 M9.2 推进。
