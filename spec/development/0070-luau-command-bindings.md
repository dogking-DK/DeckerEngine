---
id: "0070"
created_at: "2026-10-08T14:52:00+08:00"
updated_at: "2026-10-08T15:13:00+08:00"
status: completed
design_refs:
  - ../design/scripting-luau.md
  - ../design/runtime.md
  - ../design/architecture.md
  - ../design/project-foundation.md
---

# 0070 M9.1 Luau 命令绑定

## 目标与设计依据

按 [Luau 设计](../design/scripting-luau.md) 完成源码编译/VM、受控命令绑定、runner 入口与错误恢复。
脚本复用现有服务，不绕过 guard、事务或历史；预算/取消归 M9.2。

## 实际变更

- [Luau 模块](../../engine/scripting/luau) 提供 `dk::scripting_luau` 和 owner 线程同步 `run_luau`。
  每次源码执行创建独立 VM、编译并加载内部字节码，RAII 关闭；无外部字节码 API。
- `dk.command` 复用 Runtime::dispatch，固定场景/实体/历史/查询白名单，返回结构化结果、原 Error 和 TaskId。
  保留服务 schema、显式 guard、revision、事务和撤销行为；脚本出错不回滚先前成功命令。
- 实现 JSON/table 转换、空数组标记、null 哨兵和精确 int64/uint64 userdata；拒绝循环/稀疏/混合表、
  非有限/不精确 number、非法类型和超深输入。普通异常可恢复；宿主致命异常独立保留并重新抛出，不能被脚本 pcall 消费。
- [runner](../../apps/runner/src/main.cpp) 条件支持 `--script FILE.luau`，与已有输入模式互斥，auto-guard 仍为 batch-only。
  成功静默退出 0，参数/脚本错误 stderr + 2，宿主致命错误沿用 3；支持 Unicode 脚本路径和工程路径。
- `DK_BUILD_SCRIPTING_LUAU` 默认 OFF；新增 CPU `windows-scripting` 预设，模块显式 PUBLIC Runtime、PRIVATE Compiler/VM。
  默认 windows-dev 不引入 Luau，GPU/窗口选项未改变。沿用 0069 固定 Luau 0.741 / port #0，无版本升级。
- 新增 [脚本指南](../guides/scripting.md)、[可运行示例](../../examples/scripting/create-scene.luau)、
  [绑定测试](../../tests/unit/LuauTests.cpp) 与 [runner 进程测试](../../tests/integration/LuauRunnerTest.cmake)，同步设计/开发索引、测试选择表、依赖说明和 Roadmap。
  没有新增 Runtime 命令；命令目录增加 Luau 入口导航。

## 验证记录

环境：Windows x64，VS2026，项目编译器 MSVC 19.51.36260（工具目录 14.51.36231），CMake 4.2.1。
Luau 官方 port tree `0e4dc3aa2ddb251a1f656073da80cde78bbfb002`，包 ABI
`b166fe342f4cc8140a46d96e77ca87d7dce8a6ddf523a6efe32644acc1f44062`。
依赖使用同一 MSVC 14.51、动态 CRT（Debug /MDd），Luau 为静态库；Compiler/VM 的实际链接及运行通过。
`windows-scripting` 与默认 `windows-dev` 的 GRAPHICS_DEVICE/RENDER_CAPTURE 均 OFF。

实际在仓库根运行：

```powershell
cmake --preset windows-scripting
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.(luau\.|runtime\.(batch_roundtrip|batch_errors|stdio_interactive)$)' -Reason 'M9.1 Luau bindings and runner input mode regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_luau_tests','dk_run') -TestRegex '^dk\.luau\.' -Reason 'Verify final Luau host exception boundary and public target interface'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_errors)$' -Reason 'Verify Luau-disabled runner option branch and existing CPU-only dependency isolation'
./scripts/check-spec.ps1
git diff --check
```

- 首轮 10/10 通过、0 跳过，`out/verify/20261008-150312-1718a7a3`：6 项绑定测试、1 项 runner 脚本集成和3项既有 batch/stdio 回归。
- 宿主异常边界与头文件 target 收尾后，7/7 Luau 测试重新通过、0 跳过，`out/verify/20261008-150531-42668e50`。
  测例涵盖多实体/父子/TRS/名称/空资产数组保存重载、错误后继续、VM 全局隔离、TaskId、guard/schema、事务失败/undo/redo、
  null、循环/深度/非法表、Unicode、完整 64 位整数和超过 2^53 的 revision 编辑；实际查询所有注册命令的描述 schema。
- 默认 Luau 关闭配置新增条件分支验证 2/2 通过、0 跳过，`out/verify/20261008-150940-4c607ef2`。
- 文档检查通过：148 个 Markdown、1494 个本地链接，元数据/表格/索引/测试入口及 JSON 清单通过；diff 空白检查通过。
- 新构建 runner 的 commands.list 返回原有33条命令；commands.describe 确认 scene.transaction 仍为 memory_edit/undoable。默认配置实测拒绝 --script 并返回2。证据保存在 `out/luau-migration/m9-discovery`。
- 初始配置失败：沙箱不能写入全局 vcpkg registry 缓存，错误表象为 `luau does not exist`；
  用允许写依赖缓存的配置执行后解决。首次 CMake 包名 `luau` 不匹配官方导出，核验安装文件后修正为 `unofficial-luau`。
  所有通过证据均来自修正后的真实构建，没有使用旧二进制代替失败结果。

## 偏差与决策

Luau number 无法表达完整 uint64，而现有 revision/schema 使用它，因此补充不透明精确整数表示；
原命令 schema 和持久化格式保持不变。标准库冻结和命令白名单是宿主边界，不声称具备完整引擎安全沙箱。
Luau 会捕获 C++ std::exception，故使用 exception_ptr 记录宿主致命错误，避免降级成可恢复业务失败。
Luau Compiler/VM 使用三方默认内存分配器，未接入 Memory 域统计。

## 遗留问题与下一步

M9.1 已验收。下一项 M9.2：执行预算、取消、退出和进一步能力约束；本次没有运行无限循环/取消或内存耗尽注入。
未运行 Release、GPU、GUI 和完整引擎测试：没有相关行为变更，当前仅验收 Debug CPU 脚本及直接受影响入口。
脚本运行态行为上下文、编辑器脚本入口、Python SDK 和记录重放仍未实现。

## 修改记录

- 2026-10-08T14:52:00+08:00：建立设计和记录。
- 2026-10-08T15:13:00+08:00：完成实现、示例、定向验证与文档，M9.1 验收通过。
