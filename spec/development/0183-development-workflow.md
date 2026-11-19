---
id: "0183"
created_at: "2026-11-19T09:55:00+08:00"
updated_at: "2026-11-19T10:55:38+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
---

# 0183 开发工作台与性能证据整理

依据[工程基础设计](../design/project-foundation.md#开发工作台)实现当前工作交接、短报告、CLI 和本地代码索引。
验收覆盖保留的两组离线样本、文档链接、实际脚本调用和两个数据库的源码定位。

## Benchmark 整理与验证

21 份正式报告保留结论、对照、测量条件与复现入口。删除 9 个 ZIP、逐帧数据、重复图像和过时的复算器；最终汇总、独立 UI 验收图和两组离线样本保留。工作树目录由 84,035,666 字节降至约 10.38 MiB。新采集进入 out/，四个既有报告生成器只在 Markdown 中展示最终指标。

- M16.11 的原始 7 个文件完整保留。verify-pathtracing-evidence.py 复现 3 个质量夹具和三轮性能。
- M19.8 原目录在 2026-10-20 已裁为摘要。从 f1e5734 恢复 40 个质量输入后，--verify-quality 重算全部十场景四模式并匹配原汇总。历史完整归档在 out/workflow-0183/historical 恢复并通过 --verify-archive，包括所有图像和哈希。首次复算暴露旧 sampler 2 与当前 sampler 3 不同，校验器按冻结 manifest 选择相应协议。
- 光栅与联合体素的三轮真实存量输入重新生成短表，模拟的原三轮样本重新生成六行 OFF 对照。完整 Memory 采集和 GPU 性能矩阵未重跑。

## 工作台与代码索引

根 WORK.md 保存当前交接与候选，按任务切换/验收更新。tools/dk.py 集中 work、docs、profile、test 和 index，Python 3.11+ 标准库，参数与数组透传已有脚本。定位与更新方式见[开发工作台](../guides/development-cli.md)及[项目导航 skill](../../.agents/skills/decker-code-navigation/SKILL.md)。

- 提交 `5755dfb` 交付短报告与离线输入。两组体素最终汇总与原 JSON 逐字段一致，光栅复算同时匹配冻结二进制与输入摘要。
- CodeGraph 1.6.2 和 codebase-memory-mcp 0.12.1 已分别为本仓库建库。两个 MCP 实际返回 engine/foundation/core/src/StableId.cpp 第 40 行的 parse_id_bytes。codebase-memory 初次返回按文件分组的行，兼容该结构后验证通过；遇到文件监视器占用写锁时有界重试。RTK 1.2.0 显式执行 rg 可定位同一符号，本机 RTK 提示自动 hook 未安装。
- [索引历史](../guides/code-index-history.md)各保留三次成功的显式更新。仅查询和 CodeGraph 已是最新时保留原记录。index status 可另外查看数据库自身的 indexed_at、节点/边数与待同步或部分解析信息。自动监视器可能使数据库更新于最后一次显式记录之后。
- 通过 work、docs 限定设计类检索、test list、profile list 及 JSON 编码检查。多目标 dk_core_tests / dk_log_probe 在 windows-dev Debug 定向构建后 17/17 通过，零匹配返回非零。首次沙箱内的 vcpkg 访问失败和 Tracy 连接超时均保留原日志；本机重跑成功。Core 汇总 `out/verify/20261119-101505-58384653/summary.json`。
- 实际采集 CPU（134 个区间、2 条线程）、GPU（928 GPU 区间）及云专项成功，三个入口返回各自的 summary.json。CPU `out/profiling/20261119-101056-77141d37/summary.json` · GPU `out/profiling/gpu-20261119-101323-832a6861/summary.json` · 云专项 `out/profiling/cloud-20261119-101642-1fc7e4f6/summary.json`。
- check-spec.ps1 全仓库文档检查、26 个检查器夹具、skill quick_validate 与四个索引回归通过。索引用例覆盖各自轮转、部分失败、空操作和监视器写锁竞争。

未运行全量引擎或跨设备矩阵；图查询属于静态索引，codebase-memory 报告部分文件仅部分解析。最新性能数值沿用历史测量。下一项引擎任务由 [WORK.md](../../WORK.md) 交接。

## Codex 的 RTK hook

项目在 [.codex/hooks.json](../../.codex/hooks.json) 注册 RTK 1.2.0 的 `PreToolUse`。本机 Codex CLI 0.169.0-alpha.11.1 的 `hooks/list` 识别到项目来源、`Bash` 匹配器与 `rtk hook codex`，首次读取为 `enabled: true`、`trustStatus: untrusted`。

首次登记时，在原 Codex 会话运行 `git status`、`rg -n namespace engine` 得到原始输出。相同工作区状态下显式使用 RTK，Git 状态从 9 行缩到 2 行，搜索从 1000 行缩到 204 行（404 个文件），只计算标准输出。`rg -n parse_id_bytes engine/foundation/core` 的 3 个命中逐行保留，零命中仍退出 1。`rtk bypass` 返回完整输出。

用户信任后，`hooks/list` 返回 `trusted`。该桌面对话复测仍是原始输出。新的临时 Codex CLI 会话接收两条裸命令，执行事件分别显示 `rtk git status`、`rtk rg -n namespace engine`。在干净的工作树中两者退出码均为 0，Git 由 5 行缩至 2 行，搜索由 1000 行缩至 204 行。行数排除 RTK 另发的安装提示。

独立 CLI 的 Windows `elevated` 只读沙箱起初无法运行用户级 WinGet 安装的 RTK。经用户确认，仅为该 `rtk.exe` 添加本机 `CodexSandboxUsers` 的读取和执行权限。通过的测试进程还将 `CLAUDE_CONFIG_DIR` 指向本机实际的 Claude 配置目录，并将 Codex 随附的 `rg.exe` 所在目录加入该进程的 PATH。单独关闭 RTK 的安装提示无法解决配置目录查找错误。

MXC 对照同样使用 Codex 随附的 ripgrep。首次只读试跑中搜索成功，Git 报工作树错误。同一安装在项目日常 `workspace-write` 模式下，将每条命令的工作目录明确设为仓库根目录，`git rev-parse --show-toplevel` 返回本项目，`--is-bare-repository` 返回 `false`。两条裸命令均自动精简且成功退出，`rtk bypass git status` 同时返回 5 行原始状态。此次 MXC 验收使用默认的 Claude 目录解析。
