---
created_at: "2026-10-08T15:06:00+08:00"
updated_at: "2026-10-08T15:40:14+08:00"
---

# Luau 场景脚本

[返回项目入口](../../README.md)。支持离线 `.luau` 源码，通过同一 Runtime 命令服务构造、
编辑与保存场景。M9.2 默认启用执行预算、VM 内存额度和命令上限；支持宿主取消及 Windows 控制台取消。
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
成功退出 0；参数、文件读取、编译、预算终止及未处理脚本异常退出 2；取消退出130，宿主致命异常退出3。诊断写 stderr。
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

默认 project 能力模式允许的命令集合：

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
转换沿用深度64/节点200000边界；参数转换在复制字符串前还检查累计1 MiB逻辑载荷。命令参数/结果须满足原编码尺寸和 schema 限制。
结果转换失败可能发生在命令提交之后；普通 dk.command 转换错误保留 task_id，预算/取消可能截断脚本返回值。宿主可查 tasks.list 与当前场景状态，不能盲目重试。

每次宿主 `run_luau` 调用使用新的 VM；全局变量不会跨脚本保留。
内置库及 dk 表冻结，没有 io/package/require/loadstring/文件或进程 API，
但允许的 scene.load/save 仍有工程文件效果；不将这些限制视为完整引擎安全沙箱。

## 执行预算和能力

runner 可显式调整脚本预算，例如：

```powershell
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root out/luau-demo --script examples/scripting/create-scene.luau --script-timeout-ms 2000 --script-max-commands 100 --script-memory-mib 32 --script-access project
```

| 选项 | 默认值 | 范围/含义 |
| --- | --- | --- |
| --script-timeout-ms | 5000 | 1–600000，run_luau 开始后计时，含编译/命令耗时 |
| --script-max-interrupts | 1000000 | 1–1000000000，VM 非GC安全点次数，非精确指令数 |
| --script-max-commands | 10000 | 1–100000，dk.command 调用次数，含被拒绝/失败的调用 |
| --script-memory-mib | 64 | 1–256 MiB，VM allocator 在用请求字节上限 |
| --script-access | project | query 只读；edit 允许内存编辑；project 额外允许显式加载/保存命令 |

这些选项仅能与 --script 一起使用，不允许重复、0预算或无界值。源码固定最多1 MiB，加载字节码最多16 MiB。
C++ LuauOptions 可进一步缩小源码额度及将 VM 额度降到256 KiB；其余范围同表。

query 仅允许 commands.list/describe、runtime.capabilities、tasks.list/get、scene.query、entity.get、history.status。
edit 允许原场景/实体/历史操作，但拒绝 scene.load、scene.save 和 project.save；
服务可能为资产校验读取文件元数据，因此 edit 并不承诺完全无文件读取。project 沿用完整 M9.1 白名单。
任何模式都不能调用 runtime.shutdown 或 assets/jobs/render。脚本无法提升宿主所选权限。

VM 超限、取消、时间或命令预算耗尽会终止这次脚本，pcall/xpcall/协程不能清除终止状态。
C++ 返回 invalid_state，Error.context[0] 分别为 luau.memory_limit、luau.cancelled、luau.timeout、
luau.command_limit 或 luau.interrupt_limit。源码/配置超限返回 invalid_argument。
已提交的命令和文件仍保留；终止后应查询当前状态，不能把终止视为自动回滚。
顶层 coroutine.yield 没有调度器接续，返回错误并回收 VM；脚本内部协程可正常使用。

VM 额度不包含 Compiler、C++ JSON/服务状态、allocator 元数据或整个进程 RSS。
执行时限与取消在 VM 安全点和原生命令前后检查，不能硬抢占编译器、长标准库 C 调用或文件 IO，
因此不保证精确毫秒级退出，也不构成完整引擎安全沙箱。编辑器脚本入口尚未接入。

## 取消与关闭

在 Windows 控制台执行脚本时，Ctrl+C 或 Ctrl+Break 请求协作取消；runner 清理 VM 后退出130。
控制台强制关闭/系统关机不作优雅退出保证。若已经进入一条同步服务操作，该操作返回后才观察取消。

C++ 宿主通过 std::stop_source 发出请求：

```cpp
std::stop_source cancellation;
dk::LuauOptions options;
options.stop = cancellation.get_token();
options.access = dk::LuauAccess::edit;
options.limits.timeout = std::chrono::milliseconds{2000};
// 在 Runtime owner 线程执行。控制线程可调用 cancellation.request_stop()。
auto result = dk::run_luau(runtime, source, "example.luau", options);
```

关闭顺序为 request_stop → 等待 owner 上 run_luau 返回 → 销毁 Runtime。
其他线程仅操作 stop_source，不读写 VM/Runtime；不要 detach 执行线程或提前释放宿主状态。
同一个 Runtime 在取消/超限后仍可接受后续脚本或命令，每次脚本获得新的 VM 和预算。

## 定向验证

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.luau\.' -Reason 'Luau 场景绑定和 runner 保存重载'
```

绑定验收见 [0070](../development/0070-luau-command-bindings.md)，预算、取消与退出验收见 [0071](../development/0071-luau-execution-limits.md)。下一项为 M9.3 Python 自动化客户端。
