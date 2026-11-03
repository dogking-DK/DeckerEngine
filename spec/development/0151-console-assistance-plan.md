---
id: "0151"
created_at: "2026-11-03T12:44:55+08:00"
updated_at: "2026-11-03T12:49:13+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/commands.md
---

# 0151 M22.4 Console 补全与 Editor 交互扩展计划

## 目标与设计依据

落实用户将智能补全、提示和 Editor 交互“扩展到 M22 路线中”的要求，结合前文视口/相机设置命令需求。
本次仅做设计与路线扩展，不实施新增运行功能。基线 `4277d65`，开始时工作区干净。
依据 [Console](../design/editor-console.md)、[Editor](../design/editor.md) 和 [Commands](../design/commands.md)，
保留 [M22.1–3 首期交付](0150-console-history-ui.md) 的完成事实。

## 实际变更

- [Roadmap](../roadmap.md#m22console-命令文本输入首期) 增加 M22.4–11：扩展设计、共享帮助/描述快照、基础补全、
  嵌套/动态候选/诊断、视口命令、相机命令、Editor 帮助/搜索、集成交付；列出逐阶段依赖和独立验收。
  保留原 M22 锚点与旧记录链接；首期完成与扩展待实现分开，下一实现阶段为 M22.5。
- 更新 Console 设计：独立帮助元数据而非放宽 schema、无 Task 的描述快照、UTF-8/光标/替换预算、
  Tab/Enter/历史/IME 协调、只读动态候选/缓存失效、未完成输入与确定错误区分。
- 定义视口/相机宿主命令的状态归属、会话 guard、提交/失败/异步显示与关闭边界；
  具体命令表/参数与宿主接口在各实现阶段先补设计，不在当前命令目录伪列新接口。
- 定义 Editor 主要面板帮助的共享来源、禁用原因、稳定控件搜索定位与显式命令模板插入；
  同步 Commands/Editor 设计、设计/开发索引、命令参考和使用指南的规划入口。
- 未改动 C++、CMake、测试程序、依赖清单、注册结果或传输协议；不更新测试选择表的现有程序清单。

## 验证记录

定向文档检查通过：9 份 Markdown、857 个本地链接及元数据、表格、全局索引、测试入口与 JSON manifests。

```powershell
& ./scripts/check-spec.ps1 -Path @(
    'spec/design/editor-console.md','spec/design/editor.md','spec/design/commands.md','spec/design/README.md',
    'spec/roadmap.md','spec/development/0151-console-assistance-plan.md','spec/development/README.md',
    'spec/commands/README.md','spec/guides/editor.md'
)
git diff --check
```

`git diff --check` 通过；人工核对阶段依赖、保留的 M22 锚点、当前能力与未来设计区分，
并从 CMake/测试源码确认计划复用的 dk_commands_tests、dk_protocol_tests、dk_editor_tests 入口存在。
无检查失败；本轮不构建引擎或运行 CPU/GPU/GUI 测试，因为只有规划文档变化。
新增补全示意是编辑中的不完整输入，不是已支持的可执行新命令；现有使用示例未变。
后续功能用例写入设计与 Roadmap，不能作为本次已通过的运行证据。

## 偏差与决策

- 现有 schema 白名单拒绝任意 description/default/unit；help 独立、可选且版本化，类型/范围仍取原 schema。
- Runtime 的主动发现命令会产生 Task；交互辅助计划通过只读拥有型描述快照访问同一注册表，避免每次击键 dispatch。
- M22.6 明确将 Tab 从键入空白改为补全键；文本语法/粘贴 Tab 保持兼容，弹窗中的 Enter 只接受候选，后续 Enter 才提交。
- 视口/相机先做命名参数和宿主能力注册；保持 Scene 环境命令、独立捕获和编辑器会话控制的状态归属。
  位置参数简写、自动 guard、持久历史、多行脚本、大模型代执行、精确追加 N 个样本和 Simulation 独立视口命令后置。
- 无新增依赖需求；按状态契约 skill 定义辅助只读、设置发布/失败边界，按命令 skill 保留当前参考目录真实性。

## 遗留问题与下一步

M22.4 已完成扩展设计与文档验收；M22.5–11 全部仍待实现，不重开 M22.1–3。
下一步 M22.5：实现可选帮助描述及注册校验、Runtime/Workspace 只读描述快照、发现兼容和相关定向测试。
M22.8/9 开工前需锁定具体 editor.* 命令表、会话服务公开接口和各参数单位/冲突策略；不自动推进 M20/M21。

## 修改记录

- 2026-11-03T12:44:55+08:00：建立扩展记录，更新 M22.4–11 设计与阶段计划。
- 2026-11-03T12:49:13+08:00：定向文档与差异检查通过，标记 M22.4 设计完成，保留扩展功能待实现状态。
