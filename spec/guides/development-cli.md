# 开发工作台

在仓库根运行 `python tools/dk.py --help`。需要 Python 3.11+；构建和 Tracy 采集沿用项目的 PowerShell 7、CMake 和既有构建目录。子命令接受 `--json`，非零退出码表示命令失败。

| 任务 | 示例 |
| --- | --- |
| 交接当前工作 | `python tools/dk.py work` |
| 查模块、决策或报告 | `python tools/dk.py docs ReSTIR --kind design --limit 5` |
| 查测试目标与筛选表达式 | `python tools/dk.py test list dk_core_tests` |
| 定向构建与 CTest | `python tools/dk.py test run --target dk_core_tests dk_log_probe --regex "^dk\.core\." --build-dir out/build/windows-dev` |
| 验证文档或复算样本 | `python tools/dk.py test run --script spec`；另有 `spec-tests`、`index-tests`、`pathtracing-replay` 和 `restir-quality` |
| 列出或采集 profile | `python tools/dk.py profile list`；例如 `python tools/dk.py profile run cpu` 或 `python tools/dk.py profile run gpu` |
| 查看索引及最近三次更新 | `python tools/dk.py index status` |
| 创建或刷新一个索引 | `python tools/dk.py index update codegraph` 或 `python tools/dk.py index update codebase-memory`；省略名称时更新两者 |

`docs` 按标题、节标题、正文排序。同排名按仓库相对路径排序，每份文档只显示一处命中。已知文件或符号可直接按[导航 skill](../../.agents/skills/decker-code-navigation/SKILL.md)读取。

`test run --target` 支持多个目标，底层 [verify.ps1](../../scripts/verify.ps1) 负责构建、发现、执行和计数。日志与 JUnit 在返回的 summary.json 同一目录。CPU / GPU 采集的前置构建见 [Tracy 指南](../../tools/profiling/README.md)；专项云采集使用 `profile run cloud`。原始采集写入 out/。

在已配置的 VS 2026 树中同时指定多个普通 `dk_*` 目标时，共用一次 MSBuild 调度。解决方案中的文件夹由脚本解析，调用者仍填写 CMake 目标名。旧树首次重新配置后即可使用。摘要的 `build_seconds` 只统计构建阶段，包含需要的工程重新生成；测试耗时见 `tests.log`。

索引命令优先复用本机 Codex 配置中的同名 MCP 启动器，也支持 PATH 上的上游 CLI。更新完成后须从当前仓库查到 StableId.cpp 中的 parse_id_bytes 才会写入[更新记录](code-index-history.md)。CodeGraph 首次建库后显式更新使用增量同步。codebase-memory 按本仓库根目录选项目；命令只记录实际完成并验证的显式更新，文件监视器的活动不写入表格。
