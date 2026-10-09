---
module: scripting-luau
created_at: "2026-10-08T14:52:00+08:00"
updated_at: "2026-10-09T15:30:00+08:00"
status: accepted
---

# Luau 场景命令绑定

## 目标与范围

M9.1 提供同步场景构造/编辑脚本，经现有 Runtime → Commands → Operations → Services 执行。
只接受 `.luau` 源码，由官方 Compiler 编译后交给 VM；不提供外部字节码入口。
脚本错误、预算终止和取消后，后续脚本与宿主命令仍可使用同一个 Runtime。M9.2 提供有限默认预算和协作取消；
仍不接入编辑器事件循环，不承诺完整引擎安全沙箱或同步原生调用的硬抢占。

## 模块与依赖

`engine/scripting/luau` 提供 `dk_scripting_luau` / `dk::scripting_luau`。
公开头文件 `include/dk/scripting/Luau.hpp` 暴露 Runtime/Result/string_view 及预算、能力和 std::stop_token 配置，PUBLIC 依赖 Runtime，
PRIVATE 链接 Luau.Compiler/Luau.VM；Luau 指针、栈和字节码不跨公开接口。
vcpkg 实际导出名为 unofficial::luau::Luau.Compiler / unofficial::luau::Luau.VM，包名 unofficial-luau。
`DK_BUILD_SCRIPTING_LUAU` 默认 OFF，要求 Framework/Scene Runtime，自动选择 scripting feature。
`windows-scripting` 为继承 windows-dev 的 CPU 预设；runner 条件链接脚本模块，Runtime 不反向依赖脚本。
沿用 0069 固定的官方 Luau 0.741 / port #0，不升级其他依赖。

## 接口与数据

`run_luau(Runtime&, source, chunk_name, LuauOptions)` 在调用线程同步执行一次源码，返回 `Result<void>`。
宿主必须在 Runtime owner 线程串行调用。每次执行创建独立 VM，结束即关闭，不持有 Runtime 所有权。
`dk-run --project-root ROOT --script FILE.luau` 与 batch/stdio/pipe 互斥；auto-guard 仍仅用于 batch。
成功退出 0，参数/读取/预算/脚本错误退出 2，取消退出 130，宿主致命异常沿用 3。脚本模式 stdout 不输出协议响应。

脚本全局 `dk` 为只读表：

- `dk.command(method, params?)` 返回 `{ok=true,value,task_id}` 或 `{ok=false,error,task_id}`。
  error 为 `{code,name,message,context}`，task_id 为字符串或 `dk.null`；命令失败不会自动抛 Luau 异常。
  调用复用 Runtime::dispatch，禁止隐式 guard。脚本可用 `error`/`assert` 决定是否停止。
- `dk.array()` 生成空 JSON 数组标记表；普通空表为对象，非空连续 1-based 整数键表为数组，
  字符串键表为对象。混合键、稀疏数组、循环、非受支持 userdata/function/thread/vector/buffer 拒绝。
  标记数组仍必须为连续整数键；拒绝普通带 metatable 的对象，转换不执行 metamethod。
- `dk.null` 是不可伪造的 null 哨兵；nil 表项表示缺字段。返回数组保留空数组标记与 null 元素。
- Luau number 转 JSON 时有限整数转整数 token，其余转浮点；整数绝对值须不超过 2^53−1。
  更大 JSON 整数返回不透明整数 userdata，回传时精确保留；`dk.integer(decimal_string)` 可构造
  int64/uint64 范围内整数，`tostring` 取得十进制文本。这支持完整 revision 和命令发现中的 uint64 上限，
  不提供 userdata 算术；不得先用 number 表达超出精确范围的整数。
  转换深度/节点沿用命令值边界，字符串保留长度（含 NUL），UTF-8/schema 校验仍由 Commands 执行。

固定允许 scene.new/load/query/save/transaction、project.save、entity 的七条命令、
history.status/undo/redo、commands.list/describe、runtime.capabilities、tasks.list/get；M10.1 增加文末所列模拟命令。
精确名称白名单拒绝 shutdown、资产/作业/渲染和未来新增命令；发现返回宿主注册表，不能视为脚本授权清单。
无裸 ECS、Vulkan、文件系统、进程或模块加载能力；标准库用 Luau sandbox 冻结，移除 print/require/loadstring。
脚本仍可通过已有 save/load 命令访问工程路径，其范围和原子性沿用 Services，不新增文件系统沙箱承诺。

## 生命周期、状态与失败

编译/加载失败在执行命令前返回 invalid_argument，并携带 chunk 名和编译诊断；运行异常返回
invalid_state，包含 chunk/行号或非字符串错误的固定诊断。VM 用 RAII 清理；脚本全局不跨调用保留。
宿主资源不足/后台致命异常继续抛给宿主，不能混同可恢复脚本错误。
Luau 会把 escaping std::exception 转成脚本错误，因此绑定单独保存宿主 exception_ptr，
致命故障后拒绝继续进入命令；即使脚本 pcall/coroutine 消费了错误，外层执行结束仍重新抛出。
终止时停止该 VM 的后续执行并关闭；已经进入的同步原生调用在返回后观察终止，不进行线程强杀。

单条命令的提交点、guard、dirty、revision 和撤销历史完全沿用服务；脚本不是自动事务。
前面成功的命令及文件效果在后续错误时保留。需要一组原子编辑时显式调用 scene.transaction；
其中失败保持文档/历史。普通值转换错误的 dk.command 返回保留 task_id；预算/取消可能截断脚本返回值，Runtime 仍保留该命令的 TaskId，可通过 tasks.list 和状态查询复核，不能盲目重试。
无新异步线程/任务状态；每条进入 Runtime 的命令使用原 TaskId 记录。

## 验证计划

- 真实 Luau 创建多实体、名称/TRS/层级、null/空数组、保存并在新 Runtime/runner 重载比较。
- 与直接 Runtime 命令对比 guard、schema、revision、事务失败/撤销重做和 TaskId。
- 编译错误无副作用；运行错误保留已提交命令；新脚本和直接命令可继续，VM 全局不泄漏。
- 循环/稀疏/混合表、非有限/越界整数、深度、非法类型、能力拒绝及 sandbox 表保护。
- runner 参数互斥、Unicode 路径、脚本失败诊断、batch/stdio 既有入口定向回归。
- CPU-only 配置验证 Compiler/VM 链接与运行；本阶段无 GPU 变更，不跑 GPU 验收。

## 相关记录

[架构](architecture.md)、[Runtime](runtime.md)、[Commands](commands.md)、
[Services](application-services.md)、[0069 依赖选型](../development/0069-luau-selection.md)、
[0070 M9.1](../development/0070-luau-command-bindings.md)。

## M9.2 执行限制、取消与能力

`LuauOptions` 包含 `LuauLimits`、`std::stop_token` 和 `LuauAccess`；原三参数调用使用有限默认预算。
options 在入口复制，调用方不得并发修改输入；取消由 stop_source 请求，其他线程不读写 VM 或 Runtime。
每次调用独立记录终止原因、已使用预算和 VM 内存。没有后台脚本线程、JobId 或跨调用共享 VM。

| 预算 | 默认值 | 允许范围与含义 |
| --- | --- | --- |
| timeout | 5000 ms | 1–600000 ms；steady_clock 从 run_luau 入口起计，含编译和命令耗时 |
| max_interrupts | 1000000 | 1–1000000000；非 GC VM 安全点次数，不是指令条数或跨版本确定性计数 |
| max_commands | 10000 | 1–100000；dk.command 尝试次数，含拒绝/失败；超限前不进入下一次 dispatch |
| max_vm_bytes | 64 MiB | 256 KiB–256 MiB；自定义 VM allocator 的在用请求字节，含 VM/slab/栈/加载字节码 |
| max_source_bytes | 1 MiB | 1 byte–1 MiB；编译前检查，runner 读取时也封顶；字节码固定上限16 MiB |

Compiler、C++ JSON、Runtime/服务状态、allocator 元数据不计入 VM 内存额度；不是进程 RSS 硬限制。
输入转换另在分配 C++ 字符串前累计1 MiB的逻辑载荷，避免共享 Luau 大字符串被重复展开造成无界复制。
既有深度/节点/schema/编码限制继续生效。取消和时限在编译/初始化/加载前后、VM safepoint、命令前后检查。
Luau 编译、标准库长 C 调用、文件 IO 和单条同步服务操作不可强抢占，因此不承诺严格墙钟退出上限。
允许参数不能禁用所有运行限制；非法预算/能力参数在编译或修改场景之前返回 invalid_argument。

执行通过 lua_resume 进入，VM interrupt 观察预算或取消后，以 lua_break 返回宿主；GC 回调只记录状态。
可 yield 的 pcall/xpcall 和协程都传播 break，脚本不能通过捕获普通异常清除终止状态。
不能 yield 的 C/metamethod 边界使用脚本错误展开到可中断层，终止仍保持。
终止原因第一次观察后锁定；同一检查点优先既有终止，再取消、超时、安全点/命令额度。
内存申请超过额度时拒绝并锁定 memory_limit，即使脚本 pcall 消费 OOM，仍禁止后续命令并返回终止。
初始化/加载也在保护调用中处理预算 OOM；真正宿主分配失败仍按致命 bad_alloc 上抛。
正常顶层 yield 没有调度器接续，返回 invalid_state；关闭整个 VM 回收所有悬挂协程。

终止返回 ErrorCode::invalid_state，context 的首项为稳定原因：`luau.cancelled`、`luau.timeout`、
`luau.interrupt_limit`、`luau.command_limit` 或 `luau.memory_limit`，其后为 chunk 名。
源码/编译产物超限和非法配置返回 invalid_argument。普通语法/脚本/业务错误保持 M9.1 语义。
终止不撤销已成功的命令、文件或事务；若在已提交命令返回后观察取消，其状态仍保留，调用方应重新查询。
完成边界是执行返回后的最后一次检查；该检查之后才到达的取消不追溯改变成功结果。
宿主关闭时 request_stop，等待 owner 上 run_luau 返回，再销毁 Runtime；禁止 detach 或从取消线程释放 VM。

`LuauAccess::project` 默认允许 M9.1 的完整白名单；`edit` 拒绝 scene.load/save/project.save；
`query` 仅允许 discovery、runtime.capabilities、tasks.list/get、scene.query、entity.get、history.status。
这控制显式命令能力；服务可能为资产合法性读取文件元数据，edit 不承诺完全无文件读取。
脚本不能提升权限，拒绝返回普通 not_supported；不计为终止，但消耗命令次数。

runner 增加仅 script 模式可用的 `--script-timeout-ms`、`--script-max-interrupts`、
`--script-max-commands`、`--script-memory-mib`、`--script-access query|edit|project`。
Windows Ctrl+C/Ctrl+Break handler 只请求 stop_source；同步执行返回后注销 handler 并销毁 Runtime。
取消退出130，预算/普通脚本错误沿用2，宿主致命错误3。控制台关闭、系统关机强制终止不作优雅退出保证。
用独立隐藏控制台的真实子进程验证信号、退出和持久化，不给用户控制台广播事件。

验证覆盖默认例子回归、无限循环/pcall/xpcall/协程/非 yield 边界、VM 额度与被捕获 OOM、
命令上限保留已提交状态、能力拒绝、预先/运行中取消、owner 关闭 join、后续脚本恢复，
以及 runner 参数范围/模式、预算退出、真实 Ctrl+Break 和重新加载。
实施证据见 [0071](../development/0071-luau-execution-limits.md)。
## M10.1 模拟命令

白名单增加 simulation.query（所有能力模式）与 simulation.start/pause/resume/step/stop（edit/project）。
运行态控制不加入场景事务；严格步数实验从 paused=true 启动并显式 step。
长循环仍不会后台推进模拟，只有宿主 pump/命令安全点调度，详见 [Physics API](physics-api.md)。

M10.2 增加 simulation.particles 到所有模式的查询白名单，start可选xpbd_cpu，step/query复用原能力门槛；见 [XPBD](physics-xpbd.md)。

M10.4：simulation.export 仅 project 权限允许，query/edit 在进入 Runtime 前拒绝；GPU 控制仍复用既有命令预算。真实 GPU 脚本须给足同步原生调用预算（示例120秒）；不承诺硬抢占 GPU wait。见 [实验示例](../../examples/scripting/gpu-experiment.luau)。

M11.3：edit/project 白名单增加 simulation.run/cancel；query 权限仍只能读取模拟状态/粒子。
run返回受理结果，脚本通过query的task状态判断完成，不能以命令TaskId成功代替模拟成功。
取消Luau脚本本身不等于取消Play；须显式simulation.cancel或由Runtime关闭清理。
有限任务的paused/cancelled安全边界见[模拟命令](../commands/simulation.md)。
