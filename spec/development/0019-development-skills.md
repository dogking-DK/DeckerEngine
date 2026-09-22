---
id: "0019"
created_at: "2026-09-22T17:14:09+08:00"
updated_at: "2026-09-22T17:33:00+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
---

# 0019 三个开发辅助 skill

基线 3d8fc09，按用户要求实现构建验证、状态契约和命令开发三个 skill。
这是一个合并任务记录，先更新 [工程设计](../design/project-foundation.md)。
核心约定为局部验证优先、契约按模式复用、具体命令文档独立维护。

## 实际变更

- `.agents/skills/decker-build-verify`：默认单配置、目标行为及直接受影响链路验证；
  按需参考提供当前 target/CTest 入口，要求继续缩小筛选，不把模块目录映射成必跑套件。
- `.agents/skills/decker-state-contracts`：明确状态所有者、成功/no-op/失败、提交点和行为证据；
  按需参考仅保留内存编辑/历史与保存/加载模式。具体操作契约继续写在模块设计中。
- `.agents/skills/decker-command-development`：沿 Commands/Operations/Services 实现，核对
  schema、guard、effect、撤销/事务；同步命令发现、功能页和相关示例，不复制完整命令目录。
- [verify.ps1](../../scripts/verify.ps1)：要求显式 target+regex；先构建，再发现和运行匹配测试。
  构建失败、零匹配或测试失败返回失败；显式 Full+Reason 才覆盖全部已配置测试。
  独立保存提交/工作区信息、日志、JUnit 和通过/失败/跳过摘要，全部跳过标为 skipped。
- [check-spec.ps1](../../scripts/check-spec.ps1)：将临时文档检查版本化，支持指定 Markdown 文件，
  检查本地文件链接、时间、编号和 JSON 清单。
- AGENTS、根 README、spec 规范补充入口与定向验证约定；更新工程设计和索引。
  保留小改合并或免写日志规则，不增加常驻 agent 或额外审批流程。

## 验证记录

环境：Windows、Visual Studio 18 2026、PowerShell 7，以及 Windows PowerShell 5.1 兼容性检查。

实际运行以下目标命令，Debug 构建成功，命中并通过 **1 条**命令发现测试：

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Target dk_commands_tests -TestRegex '^dk\.commands\.discovery is sorted' -Reason '验证定向构建脚本只运行指定命令发现用例'
```

结果位于 `out/verify/20260922-172734-e3f4ae6b/summary.json`：passed=1、failed=0、skipped=0。

在 `out/skill-helper-fixture` 建立无 C++ 编译的独立 CMake 夹具：一个成功构建 target、一个故意失败
target；三个 CTest 用例分别成功、失败和按 SKIP_RETURN_CODE 跳过。
通过本地 `out/test-development-helpers.ps1` 执行以下检查，全部符合预期：

| 场景 | 实际结果 |
| --- | --- |
| 选择成功与跳过用例 | 1 通过、1 跳过，passed_with_skips，退出 0 |
| 选择失败用例 | 1 失败，stage=tests，退出 1 |
| regex 无匹配 | stage=discovery，退出 1，无 tests.log，未运行测试 |
| 构建故意失败 | stage=build，退出 1，无 tests.log，未运行测试 |
| 显式 Full+Reason，仅针对三用例夹具 | 1 通过、1 失败、1 跳过，退出 1 |
| 缺少定向范围或 Full 原因 | 拒绝执行，非零退出 |
| 指定文档的有效/失效本地链接 | 有效链接通过；失效链接拒绝 |

Windows PowerShell 5.1 另运行 fixture.skip：0 通过、0 失败、1 跳过，status=skipped；
运行 `scripts/check-spec.ps1 -Path README.md` 通过。没有把全部跳过记作通过。

三个 skill 均通过 skill-creator 的 `quick_validate.py`（使用 `python -X utf8`）；
仓库文档再通过 `pwsh -NoProfile -File scripts/check-spec.ps1`。
人工核对触发边界：IDE 分组只做生成结果检查；状态编辑使用契约；新增命令同步实现和命令参考；
文档修改不触发 C++ 全量测试。这属于内容审阅，未进行独立 agent 行为评测。

## 遇到的问题与边界

- 初次运行发现 PATH 中有多个 git.exe，已将工具解析收敛到第一个应用程序，真实验证随后通过。
- 普通沙箱中 MSBuild FileTracker 拒绝访问，Windows PowerShell 5.1 也受该账户执行策略限制；
  在获准的当前用户环境重跑上述检查通过，没有更改全局执行策略或工具链配置。
- 脚本不自动推断测试依赖：调用者仍需保证构建 target 覆盖所选测试程序/进程夹具。
  文档检查只验证支持的行内本地文件链接存在，不验证 Markdown 锚点或远端页面。
- 夹具与详细运行产物位于忽略的 out 目录；不增加引擎测试注册项。
  本次未执行引擎全量回归、Release 矩阵、GPU 或其他平台验证，不推进 M4。
