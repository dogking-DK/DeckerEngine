---
id: "0071"
created_at: "2026-10-08T15:22:00+08:00"
updated_at: "2026-10-08T15:40:14+08:00"
status: completed
design_refs:
  - ../design/scripting-luau.md
  - ../design/runtime.md
  - ../design/architecture.md
---

# 0071 M9.2 Luau 执行限制与取消

## 目标与设计依据

按 [Luau 设计](../design/scripting-luau.md) 扩展 M9.1，完成有限默认预算、宿主取消、能力收紧和 runner 退出。
保留已提交命令与文件；退出时等待 owner 完成，不从其他线程操作 VM/Runtime。

## 实际变更

- [公开接口](../../engine/scripting/luau/include/dk/scripting/Luau.hpp) 新增 LuauOptions/LuauLimits/LuauAccess。
  三参数调用使用默认5000 ms、1000000安全点、10000命令、64 MiB VM、1 MiB源码上限；所有限额验证为正且有上界。
- [实现](../../engine/scripting/luau/src/Luau.cpp) 通过 lua_resume/interrupt/lua_break 执行和返回宿主，
  终止原因锁定，pcall/xpcall/协程不能吞掉取消或额度终止。非yield C/metamethod边界用错误展开。
  VM采用计数realloc/free allocator，超限拒绝分配；初始化、加载和被pcall捕获的OOM也最终返回memory_limit。
- stop_token 可在外部线程请求取消；VM/Runtime仍只在owner线程访问。检查编译/加载前后、VM安全点、命令前后，
  返回invalid_state及稳定context原因。命令提交后遇到终止仍保留状态；源文件/配置非法先拒绝。
- 输入转换在C++字符串复制前累计1 MiB逻辑载荷，避免共享大字符串重复展开导致无界分配。
  query/edit/project能力分别为只读、禁止显式加载保存的内存编辑及原完整白名单，不改变注册表schema。
- [runner适配](../../apps/runner/src/ScriptRunner.cpp) 提供预算/能力选项、有界源码读取及Windows Ctrl+C/Ctrl+Break取消。
  handler通过受锁保护的stop_source请求停止，执行返回后注销handler，再销毁Runtime；取消退出130，预算错误2，致命故障3。
- 新增[限制/取消测试](../../tests/unit/LuauLimitsTests.cpp)和[真实控制台测试](../../tests/unit/LuauConsoleTests.cpp)，
  扩展[CLI进程回归](../../tests/integration/LuauRunnerTest.cmake)。控制台夹具创建自有隐藏console，
  仅把事件发给该子进程与可丢弃测试进程，不广播至用户/CTest父进程控制台；失败清理只终止夹具自己创建的进程。
- 同步设计、[指南](../guides/scripting.md)、依赖内存边界、测试选择表与Roadmap。没有新增Runtime命令、依赖或异步作业系统。

## 验证记录

环境沿用0070：Windows x64、VS2026/MSVC 14.51，固定Luau 0.741 / port #0，Debug CPU-only配置。
未改依赖版本或CMake功能选项；windows-dev仍关闭Luau，两个验证配置都关闭GPU。

实际从仓库根执行：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.luau\.' -Reason 'M9.2 VM limits cancellation capability and M9.1 recovery regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.(luau\.(Luau runner Ctrl|runner_roundtrip)|runtime\.(batch_roundtrip|batch_errors|stdio_interactive)$)' -Reason 'M9.2 real console cancellation CLI budgets and existing runner modes'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_errors)$' -Reason 'M9.2 runner parsing changes with Luau disabled'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target dk_luau_tests -TestRegex '^dk\.luau\.Luau runner console cancellation' -Reason 'Verify both Windows Ctrl C and Ctrl Break cancellation with saved-state recovery'
./scripts/check-spec.ps1
git diff --check
```

- `out/verify/20261008-152739-fad8bcd8`：14/14通过、0跳过。新增7项限制/取消/能力测试，原6项M9.1绑定和runner重载回归全部通过。
  覆盖纯循环、pcall、xpcall错误处理器、resume/wrap协程、__tostring、table.sort比较器中的受保护循环，
  并在同一Runtime继续下一段脚本。覆盖VM运行/加载超限、被捕获OOM、源码/逻辑载荷边界、参数拒绝、
  命令次数保留已提交状态、能力拒绝、预取消、保存之后运行中取消和owner request_stop/join后销毁。
- `out/verify/20261008-153116-e6cc420d`：5/5通过、0跳过。真实Ctrl+Break、扩展CLI进程检查及3项batch/stdio回归通过。
  CLI检查包含超时/安全点/命令/VM额度、权限模式、数值范围、重复参数、仅script可用和超长源码读取。
- `out/verify/20261008-153254-e244bdb8`：默认关闭Luau配置2/2通过、0跳过，验证新runner解析代码的关闭分支。
- 最终控制台测试改名并参数化为Ctrl+C和Ctrl+Break两种事件；`out/verify/20261008-153715-bbdeadd2`：1/1通过，
  两种事件都退出130、stderr标识luau.cancelled、stdout为空，且取消前保存的单实体场景可重新加载。
- 本阶段所有C++构建和已执行测试均通过，未出现需要使用旧二进制掩盖的构建/测试失败；日志没有编译警告。
  文档检查通过149个Markdown和1509个本地链接，元数据/表格/索引/测试入口/JSON清单通过；diff空白检查通过。

## 偏差与决策

核验固定Luau源码与[官方嵌入接口](https://luau.org/api/)后采用公开break/resume机制，
避免普通lua_error被pcall吞掉；在不可yield的C边界展开错误并保留终止原因。
时限包括编译/命令耗时，但这些同步阶段只能前后观察；不引入强杀线程或后台调度器。
VM额度计入allocator在用请求字节，不包含Compiler、C++ JSON、服务状态、分配器元数据或进程RSS；
真正宿主分配失败沿用致命异常，VM额度耗尽是可恢复的本次脚本终止。
最初设计只列Ctrl+Break进程验证，收尾扩展到同时验证Ctrl+C，未改变接口范围。

## 遗留问题与下一步

M9.2已验收，下一项M9.3 Python自动化客户端。M9父阶段仍进行中。
未运行Release、GPU、GUI、全量引擎测试或系统OOM注入：本次仅改CPU脚本/runner，VM预算OOM已覆盖。
不承诺安全沙箱、严格毫秒退出、原生C函数/同步IO硬抢占、控制台强关/系统关机的优雅退出，
也未接入编辑器脚本入口、行为脚本、Compiler内存硬限额或Memory域统计。

## 修改记录

- 2026-10-08T15:22:00+08:00：先行更新设计并建立记录。
- 2026-10-08T15:40:14+08:00：完成预算/取消/能力与runner退出、全部定向验证和文档，M9.2验收通过。
