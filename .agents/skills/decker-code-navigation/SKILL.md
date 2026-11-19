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
| Git、ripgrep、CTest 的终端输出太长 | 直接执行 `rtk git status`、`rtk rg parse_id_bytes engine` 或 `rtk ctest --test-dir out/build/windows-dev -C Debug -N`。`rtk gain` 查看估算节省量；测试的完整失败上下文在 verify 返回的原始日志中。 |

图查不到时先确认仓库路径和索引覆盖，尤其是 Slang、生成文件和条件编译。查询索引状态或上次显式更新用 `python tools/dk.py index status`。本地建库与更新见[开发工作台](../../../spec/guides/development-cli.md)。一次查询已给足源码时可直接使用，避免重复读取。同会话复用已读且未变化的契约；公共规则保持稳定，动态状态放在交接文档里按需读取。

上游参考：[RTK](https://github.com/rtk-ai/rtk) · [codebase-memory MCP](https://github.com/DeusData/codebase-memory-mcp#mcp-tools) · [CodeGraph](https://github.com/colbymchenry/codegraph#mcp-tools)。
