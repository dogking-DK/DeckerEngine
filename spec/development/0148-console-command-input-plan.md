---
id: "0148"
created_at: "2026-11-03T10:31:20+08:00"
updated_at: "2026-11-03T10:36:53+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/commands.md
  - ../design/application-services.md
---

# 0148 M22.1 Console 命令文本输入首期计划

## 目标与设计依据

落实用户“先进行首期开发，先确定开发计划、设计”的要求。本轮范围为首期设计与开发计划落档，
功能实现分为后续 M22.2/3，不把设计采用状态当作 Console 已支持新输入。
依据 [Console 设计](../design/editor-console.md)、[Editor](../design/editor.md)、
[命令注册](../design/commands.md) 和 [服务状态](../design/application-services.md)。

基线 `2d77103`；开始时工作区干净。已核对 Workspace::execute_console、WorkbenchUi、
现有 Console CPU 测试、命令 JSON 解析工具和 [0127](0127-editor-media-pathtracing.md)。

## 实际变更

- 新增 editor-console 专项设计，定义文本语法/类型、JSON 兼容、全行解析与有界输入、错误定位、
  guard/草稿/Job 等待、提交后刷新失败及会话命令历史；归属现有 editor_model/editor target。
- Roadmap 新增 M22.1–3：设计、解析/执行、历史/UI/交付；与 M20/M21 路线独立，用户指定时优先当前路线。
- Editor 主设计与设计索引链接专项设计；命令参考/Editor 指南添加明确标注未实现的规划入口，保留现有用法。
- 不修改 C++、CMake、命令注册、依赖或协议；不添加 ReSTIR/相机/采样控制命令。

## 验证记录

以下定向文档检查通过：8 份 Markdown、822 个本地链接，以及元数据、表格、全局索引、测试入口和 JSON manifests。

```powershell
& ./scripts/check-spec.ps1 -Path @(
    'spec/design/editor-console.md', 'spec/design/editor.md', 'spec/design/README.md',
    'spec/development/0148-console-command-input-plan.md', 'spec/development/README.md',
    'spec/roadmap.md', 'spec/commands/README.md', 'spec/guides/editor.md'
)
git diff --check
```

`git diff --check` 通过；人工复核了语法、旧入口差异、Job 默认超时、整数精度、历史与提交点，以及设计/实现状态区分。
本轮仅文档，不运行引擎构建、CPU/GPU 或 GUI 测试；设计中的测试为后续验收要求。

## 偏差与决策

- 首期规范形式为 `method key=value`，不提供位置参数别名或点号嵌套赋值；复杂值使用 JSON。
- 显式 guard 保持现有契约，避免文本和 JSON 入口产生不同编辑行为；历史仅回填，不自动刷新身份或执行。
- 当前 Console JSON 直接使用 Json::parse；后续复用命令值工具时需明确拒绝重复键与整数溢出，
  不把旧实现偶然接受的歧义输入当作兼容保证。
- 核对 JobOperations 确认 jobs.wait 省略 timeout_ms 的默认值是 0；保留该合法用法，
  不将省略参数误判为阻塞等待，显式参数仍要求整数 0。
- Workspace 当前业务成功后刷新失败返回普通错误；计划明确“已执行、刷新失败”，保留错误且禁止自动重试。

## 遗留问题与下一步

下一步 M22.2：实现纯解析器及 Workspace 接入，以现有 dk_editor_tests 验证类型/精度/无副作用失败、
旧 JSON、guard/草稿、零等待与刷新失败行为。之后 M22.3 接入历史/输入回调、真实后台 GUI 和最终使用指南。
语法和首期边界没有需用户补充的阻塞项；运行行为仍未实现。

## 修改记录

- 2026-11-03T10:31:20+08:00：创建首期设计与三阶段开发计划；文档验证待执行。
- 2026-11-03T10:36:53+08:00：文档检查通过；完成 M22.1 设计阶段，M22.2/3 保持待开始。
