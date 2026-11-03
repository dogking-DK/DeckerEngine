---
id: "0149"
created_at: "2026-11-03T10:46:07+08:00"
updated_at: "2026-11-03T10:58:59+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/commands.md
  - ../design/application-services.md
---

# 0149 M22.2 Console 解析与执行接入

## 目标与设计依据

完成 [M22.2](../roadmap.md#m22console-命令文本输入首期)，依据 [Console 设计](../design/editor-console.md)。
基线 a1259f3，工作区干净；实现命名参数与 JSON 兼容、错误定位、全行验证后分派和提交后刷新失败诊断。
M22.3 的历史/输入 UI/真实 GUI 验收不属于本次范围。

## 实际变更

- [Console.hpp](../../engine/editor/include/dk/editor/Console.hpp) 与 [Console.cpp](../../engine/editor/src/Console.cpp)：
  8192 字节单行命名参数/旧 JSON、字符串/复合值、完整整数精度、Unicode/深度/重复键/错误位置；
  完整候选验证完成后才允许执行。
- [Workspace](../../engine/editor/src/Workspace.cpp) 接入 [私有分派 helper](../../engine/editor/src/ConsoleDispatch.hpp)，
  保留 schema/guard/草稿、零等待、shutdown 与原命令错误；提交后刷新失败保留原 code/context 并标明已执行。
- 现有 editor_model target 增加公开解析头与私有实现；现有 dk_editor_tests 增加
  [ConsoleTests.cpp](../../tests/unit/ConsoleTests.cpp) 的 12 项 CPU 用例，复用原 Console 回归。
- 同步设计/开发索引、Roadmap、命令参考、Editor 使用指南及[测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)。
- 不新增 Runtime 命令、依赖或协议；不修改 UI 输入框/历史，也未重新构建 dk-editor 应用。

## 验证记录

环境为 Windows / VS2026 / 现有 windows-dev Debug，纯 CPU，Graphics/Render Capture 关闭。
最终验证命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration Debug `
    -Target @('dk_editor_tests') -TestRegex '^dk\.editor\.(console |workspace console)' `
    -Reason 'M22.2 修复测试命名空间歧义后复验解析与执行CPU链路'
```

结果：构建成功，13/13 通过，0 失败、0 跳过；含 12 项新增用例及原 Console guard/草稿回归。
证据：`out/verify/20261103-105632-6497a865/summary.json`、build.log、discovery.json、tests.log、results.xml。
真实 Runtime/Workspace 验证文本/JSON 等价、commands.list/describe 的既有 schema/effect/undoable、
单次编辑/撤销重做、缺失/旧 guard、Inspector/Environment 草稿、零等待以及提交后刷新失败只执行一次。
全行错误还核对 Scene/revision/dirty/history、原文件、未创建输出和未增加业务任务。

中间失败明确保留：

- `20261103-105148-5d2ea9c1`：沙箱构建无法访问现有 vcpkg 目录，CMake 配置失败，测试未运行。
- `20261103-105221-977527f1`：提升该次构建权限后恢复固定依赖并完成模型编译；新测试中的 detail 命名空间歧义使测试目标编译失败，测试未运行。
  修正为完整 dk::editor::detail 后上述最终运行全部通过；未使用旧测试二进制替代验收。

`scripts/check-spec.ps1 -Path` 定向检查本次 9 份 Markdown（Console/Editor 设计与索引、0149 与开发索引、
Roadmap、命令参考、Editor 指南、测试选择表）：通过，839 个本地链接及元数据/表格/索引/测试入口/JSON manifests 一致性通过。
`git diff --check` 通过；最终构建无编译 warning/error。源代码在最终 CPU 验证后未改动。
未运行 GUI/GPU、全量/Release 双配置、IPC 进程/Python/Luau 矩阵；命令注册/协议没有变化，
CPU 模型验收不代表真实窗口或历史交互通过，M22.3 仍待实施。

## 偏差与决策

先同步设计：有界 JSON 词法扫描保留整数精度/重复键位置，直接 Json 解析保留 parse_error.byte；
最终复用 validate_command_value。用私有分派 helper 注入刷新失败，避免公开测试开关。

## 遗留问题与下一步

M22.2 无剩余实现项。下一阶段 M22.3：有界历史与输入提示/草稿/回填、超限粘贴保护、
真实后台 GUI 输入和旧 JSON 回归；命令补全/ReSTIR/相机控制仍不属于首期。

## 修改记录

- 2026-11-03T10:46:07+08:00：建立记录并固定 M22.2 实施/验证范围。
- 2026-11-03T10:57:49+08:00：Debug 13 项定向 CPU 检查全部通过，M22.2 完成；同步当前能力与后续 UI 边界。
- 2026-11-03T10:58:59+08:00：9 份文档、839 个本地链接及差异检查通过，准备独立本地提交。
