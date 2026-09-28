---
id: "0041"
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-09-28T16:17:15+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
  - ../design/foundation-memory.md
  - ../design/runtime.md
---

# 0041 AI 文档入口与开发协作优化

## 目标与设计依据

依据[工程基础设计](../design/project-foundation.md)，修正文档冲突、拆分长 README、
补全测试路由和状态处理参考，并让文档检查发现结构性遗漏。属于开发基础维护，不推进 M5。

## 实际变更

- [README](../../README.md) 从 1139 行缩为 72 行，保留快速开始、导航和定向验证。
  原有示例迁入五份指南：[构建](../guides/build.md)、[Runtime](../guides/runtime.md)、
  [资产](../guides/assets.md)、[Memory](../guides/memory.md)、[Foundation/Scene](../guides/foundation.md)。
  47 个原有非目录代码块完整保留；相对链接和章节锚点同步迁移。
- [流程规范](../README.md) 明确各类文档职责、按模块关联读取开发记录及有授权时的 agent 分工/交接。
  根 AGENTS 保持简短；[设计索引](../design/README.md)增加源码与测试入口，开发索引修复断表。
  Roadmap 保存阶段状态与下一步，去除导航中的下一开发编号常量，避免新建维护记录后立即过期。
- 修复旧设计的默认全量/双配置要求；Memory、Runtime 按当前主题组织，修正已实现 Jobs/资产能力仍被描述为预留的问题。
  历史阶段测试结果仍保留在原开发记录，不改变原有验收事实。
- 更新三个 skill 入口和两份参考，补齐 Memory/Profiling/Jobs/异步资产/资产命令测试程序；
  增加异步发布、取消、关闭回收与缓存恢复的复用检查方法。保留四个 skill 的职责和按需触发方式。
- 扩展 [check-spec.ps1](../../scripts/check-spec.ps1)：校验元数据/设计引用、索引完整性与状态、表格结构、
  测试 target 注册及 tests 目录程序的入口覆盖。代码围栏和行内代码不会被误当作链接示例。
  新增版本化 [test-check-spec.ps1](../../scripts/test-check-spec.ps1)，使用独立夹具验证成功和拒绝路径。

## 验证记录

环境：Windows，PowerShell 7.6.5 / Windows PowerShell 5.1.26100.9444，Python 3.13。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| `& ./scripts/check-spec.ps1` | 通过，扫描 88 份 Markdown，检查链接、元数据、索引、测试入口与 JSON | 提交前在两种 PowerShell 下执行 |
| `& ./scripts/check-spec.ps1 -Path @('README.md','spec/guides/memory.md')` | 通过，扫描 2 份 Markdown；全局一致性仍检查 | 定向入口 |
| `& ./scripts/test-check-spec.ps1` | 25/25 通过 | out/check-spec-tests/20260928-161325-f3d180d5/summary.json |
| `powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/test-check-spec.ps1` | 25/25 通过 | out/check-spec-tests/20260928-161326-c6761b94/summary.json |
| skill-creator `quick_validate.py` | 4/4 skill 格式通过 | `python -X utf8` 运行四个仓库 skill |
| 迁移内容核对 | README 原 47 个代码块、Memory 设计 3 个代码/图示块原样保留 | out/document-migration-summary.json |
| `git diff --check` | 通过 | 提交前执行 |

脚本夹具覆盖：有效文档、限定扫描、空单元格/转义竖线、围栏内示例、坏链接、元数据缺失/非法状态、
索引漏项/重复/错状态/错键/未知文件、设计引用缺失/越出设计目录、编号冲突/不符、时间顺序/时区、
断表/列数、未闭合围栏、未知 target/未登记测试程序、非法 JSON。
首次夹具运行发现英文表头被误识别为条目；已改为按表格分隔行识别表头，两个宿主最终均通过。

未运行 C++ 构建、引擎回归、Release、GPU、Tracy 采集或独立 agent 行为评测：
本次修改文档与检查工具，不改变引擎实现、依赖或既有示例行为。新 README 快速开始命令使用已有 preset/target。

## 遗留问题与下一步

检查器针对仓库现有格式：行式 frontmatter、带首尾竖线的表格和字面 CMake add_executable。
不解析任意 YAML/CMake、远端链接、Markdown 锚点或自然语言语义；测试 regex 和实际条件配置仍由 verify/CTest 核验。
日志和夹具留在忽略的 out 目录；版本化脚本与本记录保留复现入口和结果。
本次无遗留阻塞；下一引擎阶段仍按 Roadmap 为 M5.1，不因文档维护推进里程碑。
