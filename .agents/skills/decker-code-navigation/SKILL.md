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
| Git 状态或搜索结果太长 | 在已启用下方 hook 的 Codex 会话中直接运行 `git status`、`rg -n 关键词 目录`。列举 CTest 时用 `rtk ctest --test-dir out/build/windows-dev -C Debug -N -R '^dk\.ipc\.client_help$'` 按名称筛选。 |

图查不到时先确认仓库路径和索引覆盖，尤其是 Slang、生成文件和条件编译。查询索引状态或上次显式更新用 `python tools/dk.py index status`。本地建库与更新见[开发工作台](../../../spec/guides/development-cli.md)。一次查询已给足源码时可直接使用，避免重复读取。同会话复用已读且未变化的契约；公共规则保持稳定，动态状态放在交接文档里按需读取。

## Codex 中的 RTK

在仓库根目录分别直接运行 `git status` 和 `rg -n namespace engine`。项目的 [PreToolUse hook](../../../.codex/hooks.json) 生效时，前者以 `* main` 等分支摘要开头，后者显示命中数、文件数和折叠提示。可用 `rtk bypass git status` 对照 Git 的原始输出。RTK 1.2.0 在自动精简成功时也可能打印 `No hook installed`，以裸命令的实际输出判断。

| 现象 | 下一步 |
| --- | --- |
| 裸命令返回原始输出，`rtk git status` 可以精简 | 在 Codex 的 Hooks 设置或 CLI 的 `/hooks` 核对本仓库的 `rtk hook codex`。首次启用或定义变更后需信任该项。已信任的旧会话仍可能沿用原状态，可在仓库根目录打开新会话复测。当前会话可继续显式使用 RTK。 |
| Windows 报找不到 RTK，或启动 `rtk.exe` 时提示拒绝访问 | 在报错的会话运行 `Get-Command rtk.exe` 核对 PATH 与实际路径。若路径能解析，运行 `rtk --version` 验证执行权限。若仍提示拒绝访问，用 `icacls (Get-Command rtk.exe).Source` 读取该文件的权限。本机独立 CLI 所用的 `CodexSandboxUsers` 组需要该文件的读取和执行权限（RX）。 |
| RTK 能启动，但独立 CLI 报找不到 `rg` 或 Claude 配置目录 | 针对相应报错，在该进程用 `Get-Command rg.exe` 核对 PATH，或将 `CLAUDE_CONFIG_DIR` 指向本机已有的 Claude 配置目录。 |

少量命中可能逐行保留，`rg` 零命中返回 1。`rtk hook check --agent codex 'git status'` 可预览改写规则。[本机验收记录](../../../spec/development/0183-development-workflow.md#codex-的-rtk-hook) · [Codex hook 文档](https://developers.openai.com/codex/hooks/) · [RTK 1.2.0 的 Codex 集成](https://github.com/rtk-ai/rtk/blob/v1.2.0/hooks/codex/README.md)

### 跨命令取回折叠结果

复制当前搜索提示里的哈希，运行 `rtk recall <hash> --lines 20` 预览折叠行，去掉 `--lines 20` 可读出该条目全部折叠行。需要原始缩进或完整长行时用 `rtk bypass rg -n 关键词 路径`。

若刚生成的哈希马上返回 `no matching entry`，先用 `rtk config recall` 核对模式。本机使用 `sqlite`，Windows 桌面沙箱每次 shell 调用的 `LOCALAPPDATA` 不同，默认恢复库也随之切换。此工作副本已在本机 `.codex/config.toml` 的 `[shell_environment_policy.set]` 中，将 `RTK_RECALL_DB` 设为仓库下 `out/rtk/recall.db` 的绝对路径。在其他工作副本复现时，使用该副本自己的绝对路径。新会话中检查 `$env:RTK_RECALL_DB`，在根目录搜索后可从子目录取回。[RTK 恢复库路径规则](https://github.com/rtk-ai/rtk/blob/v1.2.0/src/core/retriever.rs#L160-L185) · [Codex 子进程环境配置](https://developers.openai.com/codex/config-advanced/#shell-environment-policy)

已打开的会话若仍读不到该变量，在**搜索和取回各自的 PowerShell 调用**开头都加上下面一行。切到固定库后重新运行原查询，使用这次输出的哈希。

```powershell
$env:RTK_RECALL_DB = Join-Path (git rev-parse --show-toplevel) 'out/rtk/recall.db'
```

上游参考：[RTK](https://github.com/rtk-ai/rtk) · [codebase-memory MCP](https://github.com/DeusData/codebase-memory-mcp#mcp-tools) · [CodeGraph](https://github.com/colbymchenry/codegraph#mcp-tools)。
