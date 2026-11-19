---
name: decker-code-navigation
description: "Locate DeckerEngine code from an approximate feature name, trace cross-module callers, or shorten noisy command output with the configured local graph indexes and RTK."
---

# 代码定位与命令输出

接手任务先看 [WORK.md](../../../WORK.md)。已知文档用 `python tools/dk.py docs 关键词 --kind design`。精确字符串、错误文本、shader 宏以及未索引的文件直接用 `rg -n -F`。

| 手上已有的线索 | 首选入口 |
| --- | --- |
| 功能描述、印象中的类名或多个相关词 | CodeGraph 的 `codegraph_explore`。传当前仓库绝对路径作为 `projectPath`，例如 `query="StableId parse_id_bytes"`、`maxFiles=2`。返回的带行号源码可直接用于定位。 |
| 从某个符号追踪调用方、跨模块依赖或影响面 | codebase-memory 先用 `list_projects` 按根路径选择本仓库，`search_graph` 找限定名，`trace_path` 查入向/出向调用，`get_code_snippet` 取目标实现。多跳关系再用 `query_graph`。 |
| Git 状态或搜索结果太长 | 在已启用下方 hook 的 Codex 会话中直接运行 `git status`、`rg -n 关键词 目录`。CTest 可显式用 `rtk ctest --test-dir out/build/windows-dev -C Debug -N`；完整测试日志见 verify 输出。 |

图查不到时先确认仓库路径和索引覆盖，尤其是 Slang、生成文件和条件编译。查询索引状态或上次显式更新用 `python tools/dk.py index status`。本地建库与更新见[开发工作台](../../../spec/guides/development-cli.md)。一次查询已给足源码时可直接使用，避免重复读取。同会话复用已读且未变化的契约；公共规则保持稳定，动态状态放在交接文档里按需读取。

## Codex 中的 RTK

项目的 [hooks.json](../../../.codex/hooks.json) 在 Codex 执行 shell 命令前调用 `rtk hook codex`。本机需要能从 PATH 运行 `rtk`。首次启用或定义变更后，在 Codex 设置的 Hooks 页或 CLI 的 `/hooks` 中核对项目路径并信任该项。

在 Codex 中直接运行 `git status`，精简输出以 `* main` 等分支摘要开头。用 `rg -n namespace engine` 验证大查询，RTK 会显示命中数、文件数及省略提示；只有少数命中的查询可能逐行保留。需要完整输出时用 `rtk bypass git status` 或 `rtk bypass rg -n namespace engine`。

若裸命令仍返回完整输出，先检查该项目 hook 的信任状态。可立即显式使用 `rtk git status` 或 `rtk rg -n namespace engine`。`rtk hook check --agent codex 'git status'` 只预览改写规则，实际启用以当前会话运行裸命令的输出为准。[Codex hook 文档](https://developers.openai.com/codex/hooks/) · [RTK 1.2.0 的 Codex 集成](https://github.com/rtk-ai/rtk/blob/v1.2.0/hooks/codex/README.md)

上游参考：[RTK](https://github.com/rtk-ai/rtk) · [codebase-memory MCP](https://github.com/DeusData/codebase-memory-mcp#mcp-tools) · [CodeGraph](https://github.com/colbymchenry/codegraph#mcp-tools)。
