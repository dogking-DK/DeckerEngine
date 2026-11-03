---
id: "0152"
created_at: "2026-11-03T12:54:41+08:00"
updated_at: "2026-11-03T13:07:54+08:00"
status: completed
design_refs:
  - ../design/commands.md
  - ../design/editor-console.md
  - ../design/runtime.md
  - ../design/editor.md
---

# 0152 M22.5 共享帮助与只读命令目录

## 目标与设计依据

完成 [M22.5](../roadmap.md#m22console-命令文本输入与-editor-交互)，依据 [Commands](../design/commands.md) 与
[Console 扩展设计](../design/editor-console.md)，基线 564a547，开始时工作区干净。
本阶段交付独立帮助元数据、注册校验、发现兼容和 Runtime/Workspace 无 Task 的拥有型描述快照。
补全弹窗、实际动态候选、设置命令与面板帮助仍在后续阶段。

## 实际变更

- [CommandRegistry](../../engine/framework/commands/include/dk/commands/CommandRegistry.hpp) 在描述尾部增加可选、拥有型 help，
  不影响旧 aggregate 初始化。commands.describe 增量输出 help，原六个字段与 commands.list 四字段保持不变。
- [帮助校验](../../engine/framework/commands/src/CommandHelp.cpp) 实现 help v1：JSON Pointer 引用真实参数 schema 节点，
  说明/单位/默认值/示例/别名/候选提供器标识；严格字段与类型、UTF-8、64 KiB 编码、深度和条目预算。
  默认值/示例复用 schema 校验，完整 uint64 精度保留；错误注册不发布命令、不推进目录 revision。
- [目录快照](../../engine/framework/commands/src/CommandRegistry.cpp) 使用独立 registry_id 与递增 revision，
  候选标识仅在已声明时可被引用。快照按名称排序，包含数据副本，不含 handler/借用引用，owner 销毁后仍有效。
- [Runtime](../../engine/framework/runtime/include/dk/runtime/Runtime.hpp) 与 [Workspace](../../engine/editor/include/dk/editor/Workspace.hpp)
  提供 command_catalog_version/command_catalog；直接读取注册表，不 dispatch/pump/refresh，不产生 Task/Job 或覆盖草稿。
  无活动文档仍可读取；场景重载不改变静态目录代号。
- [SceneOperations](../../engine/framework/operations/src/SceneOperations.cpp) 为 scene.query、environment.preset/set 添加真实帮助，
  配合内建 commands.describe 共四条；分页默认值与 handler 共享常量，必填环境字段不伪造默认值。
  其他命令仍用原描述/schema 降级；没有新增业务命令或改变参数/guard/撤销行为。
- 新增 8 项行为用例：[Commands 5 项](../../tests/unit/CommandHelpTests.cpp)、[Runtime/RPC 2 项](../../tests/unit/RuntimeCatalogTests.cpp)、
  [Workspace 1 项](../../tests/unit/EditorCatalogTests.cpp)，复用现有测试 target，无新增依赖。
  同步 Commands/Runtime/Editor/Console 设计、命令参考、指南、索引、Roadmap 与测试选择表。

## 验证记录

Windows / VS2026 / windows-editor Debug；沿用既有依赖 baseline，没有变更依赖或构建选项。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_commands_tests','dk_protocol_tests','dk_editor_tests','dk_run','dk_editor_app') `
  -TestRegex '^dk\.(commands\.|protocol\.(catalog |RPC |runtime |synchronous task)|editor\.(catalog |console |workspace console))' `
  -Reason 'M22.5 explicit optional help construction; recheck catalog, discovery and direct consumers'
```

- 首次构建失败：新测试将 const Json 隐式构造为 optional<Json> 被编译器拒绝；改为显式 in_place 构造。
  该轮未运行测试，日志 `out/verify/20261103-130001-b2785941`，不作为通过证据。
- 修正后五个 target 均构建通过，**37 项通过、0 失败、0 跳过**；证据
  `out/verify/20261103-130116-8a61edb1/{summary.json,build.log,results.xml,tests.log}`。
  覆盖非法/缺省帮助、引用/预算/整数边界、失败注册不变、快照副本/owner 寿命、RPC 兼容，
  以及 100 次 Runtime/Workspace 读取后 Task、场景/文件、历史、选择和未应用草稿保持。
- 新构建的 dk-run 在独立空工程执行 6 条 JSON-RPC batch 请求：commands.list 与上述四条帮助命令及 entity.create 的 describe。
  验证 49 条排序能力、旧字段/effect/undoable、四条 help、分页默认 128/范围 1–256、太阳强度单位，以及无 help 的降级。
  全部成功，无工程文件产生；请求/原始响应/程序摘要保存在 `out/m22-5-discovery/`。

```powershell
out/build/windows-editor/bin/Debug/dk-run.exe --project-root out/m22-5-discovery/project --batch out/m22-5-discovery/requests.jsonl
```

- 构建有一条既存 RenderOperations.cpp:103 的 C4456 局部变量遮蔽警告；该文件本阶段未修改。
- `scripts/check-spec.ps1 -Path <本阶段 14 个 Markdown 文件>` 通过：915 个本地链接、元数据、表格、索引、测试入口与 JSON 清单；
  `git diff --check` 通过。仅有既有 CRLF→LF 规范化提示，无 whitespace 错误。
- 未运行：全量 CTest、Release/其他配置、GPU/真实 GUI/跨传输 Python/Luau 矩阵。
  本阶段是 CPU 描述数据与直接只读 API；已编译 Editor 消费者，无 UI 输入或 GPU 行为改变，不将这些未运行项计为通过。

## 偏差与决策

沿独立 help 元数据方案，不放宽 schema、不注入默认参数；目录查询绕过 dispatch，不刷 Task/Job。
目录使用 registry_id 与 revision，区分相同命令集合的不同 Runtime；场景候选代号独立留到 M22.7。
候选提供器本阶段仅注册/校验语义标识，不伪造场景候选功能。

## 遗留问题与下一步

M22.5 范围已完成；下一阶段 M22.6 接入 Console 基础补全与提示。
实际动态候选、嵌套输入诊断、视口/相机命令、面板帮助/搜索保持各自 M22.7–10 状态。
当前可以在 Console 执行 `commands.describe name=scene.query` 查看帮助数据；Tab 尚不选择候选。

## 修改记录

- 2026-11-03T12:54:41+08:00：建立记录，补充实施接口、失败边界及验证范围。
- 2026-11-03T13:05:36+08:00：完成实现，修正测试编译问题，37 项定向测试与真实 runner 发现检查通过。
- 2026-11-03T13:07:20+08:00：同步阶段状态与测试入口，14 个 Markdown / 915 个本地链接及 diff 静态检查通过。
