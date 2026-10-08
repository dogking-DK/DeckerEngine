---
module: scripting-luau
created_at: "2026-10-08T14:52:00+08:00"
updated_at: "2026-10-08T15:11:00+08:00"
status: accepted
---

# Luau 场景命令绑定

## 目标与范围

M9.1 提供同步场景构造/编辑脚本，经现有 Runtime → Commands → Operations → Services 执行。
只接受 `.luau` 源码，由官方 Compiler 编译后交给 VM；不提供外部字节码入口。
脚本错误可恢复，后续脚本和宿主命令仍可使用同一个 Runtime。M9.2 才实现执行预算、取消和退出控制；
本阶段仅供可信离线脚本，不接入编辑器事件循环，不承诺完整引擎安全沙箱或无限循环恢复。

## 模块与依赖

`engine/scripting/luau` 提供 `dk_scripting_luau` / `dk::scripting_luau`。
公开头文件 `include/dk/scripting/Luau.hpp` 仅暴露 Runtime/Result/string_view，PUBLIC 依赖 Runtime，
PRIVATE 链接 Luau.Compiler/Luau.VM；Luau 指针、栈和字节码不跨公开接口。
vcpkg 实际导出名为 unofficial::luau::Luau.Compiler / unofficial::luau::Luau.VM，包名 unofficial-luau。
`DK_BUILD_SCRIPTING_LUAU` 默认 OFF，要求 Framework/Scene Runtime，自动选择 scripting feature。
`windows-scripting` 为继承 windows-dev 的 CPU 预设；runner 条件链接脚本模块，Runtime 不反向依赖脚本。
沿用 0069 固定的官方 Luau 0.741 / port #0，不升级其他依赖。

## 接口与数据

`run_luau(Runtime&, source, chunk_name)` 在调用线程同步执行一次源码，返回 `Result<void>`。
宿主必须在 Runtime owner 线程串行调用。每次执行创建独立 VM，结束即关闭，不持有 Runtime 所有权。
`dk-run --project-root ROOT --script FILE.luau` 与 batch/stdio/pipe 互斥；auto-guard 仍仅用于 batch。
成功退出 0，参数/读取/脚本错误退出 2，宿主致命异常沿用 3。脚本模式 stdout 不输出协议响应。

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

M9.1 固定允许 scene.new/load/query/save/transaction、project.save、entity 的七条命令、
history.status/undo/redo、commands.list/describe、runtime.capabilities、tasks.list/get。
精确名称白名单拒绝 shutdown、资产/作业/渲染和未来新增命令；发现返回宿主注册表，不能视为脚本授权清单。
无裸 ECS、Vulkan、文件系统、进程或模块加载能力；标准库用 Luau sandbox 冻结，移除 print/require/loadstring。
脚本仍可通过已有 save/load 命令访问工程路径，其范围和原子性沿用 Services，不新增文件系统沙箱承诺。

## 生命周期、状态与失败

编译/加载失败在执行命令前返回 invalid_argument，并携带 chunk 名和编译诊断；运行异常返回
invalid_state，包含 chunk/行号或非字符串错误的固定诊断。VM 用 RAII 清理；脚本全局不跨调用保留。
宿主资源不足/后台致命异常继续抛给宿主，不能混同可恢复脚本错误。
Luau 会把 escaping std::exception 转成脚本错误，因此绑定单独保存宿主 exception_ptr，
致命故障后拒绝继续进入命令；即使脚本 pcall/coroutine 消费了错误，外层执行结束仍重新抛出。
这不提供无限循环抢占，执行预算与退出保证仍属 M9.2。

单条命令的提交点、guard、dirty、revision 和撤销历史完全沿用服务；脚本不是自动事务。
前面成功的命令及文件效果在后续错误时保留。需要一组原子编辑时显式调用 scene.transaction；
其中失败保持文档/历史。命令已提交但结果转换失败时返回错误并保留 task_id，调用者应查询状态，不能盲目重试。
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
