---
name: decker-build-verify
description: "Select and run focused build/test verification for DeckerEngine changes. Use after implementation, fixes, or build/dependency changes, and when asked to verify a feature. Prefer only affected tests; documentation-only work needs document checks, not an engine build."
---

# DeckerEngine 定向验证

先确定本次目标行为和直接受影响的调用链，再选择验证范围。只读解释无需执行测试。
目录位置以仓库根为准；构建方式见 [README](../../../README.md)，
局部 target/测试入口见 [选择参考](references/test-selection.md)，只读取本次相关部分。

## 选择与执行

1. 读取本次 diff、相关实现和现有测试。说明选择哪些用例及原因；
   不把“修改某目录”机械等同于测试该目录全部功能。补充能观察到行为变化的测试，避免镜像实现。
2. 默认一个合适配置（通常 Debug），只构建测试所需 target 并运行目标测试。
   仅在优化、断言、条件编译或工具链差异影响本次行为时补充 Release/独立配置。
   用户明确要求全量，或有具体全局影响且已说明理由时才扩大范围；不要求每次提交或阶段结束固定跑全量。
3. 确认每个选中测试的程序和进程夹具都被构建目标覆盖。使用已有、选项正确的构建目录；
   首次配置按 README 执行，不为绕过失败关闭模块、换编译器或清理缓存。
4. 使用版本化 [verify.ps1](../../../scripts/verify.ps1)，显式传入 Target 和 TestRegex；
   需要人工枚举时使用 `ctest --test-dir <build-dir> -C Debug -N -R '<regex>'`。
   构建失败立即停止，不能用旧二进制提供通过证据；零匹配不是通过。
5. 失败时定位本次错误，修复后仅重跑受影响检查；已有结果有效且代码未改变时不重复测试。

从仓库根调用示例（此名称来自当前测试，修改测试名后先核对 CTest 列表）：

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Target dk_commands_tests -TestRegex '^dk\.commands\.discovery is sorted' -Reason '验证命令发现'
```

多个 target 在 PowerShell 中以数组传入脚本：

```powershell
& ./scripts/verify.ps1 -Target @('dk_protocol_tests', 'dk_run') -TestRegex '^dk\.(protocol|runtime)\.' -Reason '协议变更影响单元和进程行为'
```

脚本仅在显式 `-Full -Reason '具体原因'` 时运行整个已配置构建树，仍只使用一个 Configuration。
不自动配置、重试、扩大筛选或执行 Debug/Release 矩阵。同一构建目录的配置/构建/CTest 串行执行。

## 证据与纯文档改动

每次结果写到 out/verify 的独立目录：summary.json、build.log、discovery.json、tests.log、results.xml。
summary 记录提交、工作区状态、配置、目标、实际选中的测试和通过/失败/跳过数；
未到达的步骤计数为空。检查摘要与失败/跳过日志，不能把跳过或未运行说成通过。
它不证明自动选择完整；交付说明须写明已验证范围和有意义的剩余限制。

纯文档使用 [check-spec.ps1](../../../scripts/check-spec.ps1)；可用 `-Path @('README.md', 'spec/commands/entity.md')`
限制 Markdown 扫描。文档中的可执行示例变更只运行相关示例，不因此触发引擎全量回归。
是否留开发记录遵循 [spec 规范](../../../spec/README.md)，此 skill 不额外分配编号。
