---
id: "0153"
created_at: "2026-11-03T13:39:00+08:00"
updated_at: "2026-11-03T13:52:06+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
---

# 0153 M22.6 Console 基础补全与参数提示

## 目标与设计依据

完成 [M22.6](../roadmap.md#m22console-命令文本输入与-editor-交互)，基线 21c4550，开始时工作区干净。
沿 [Console 设计](../design/editor-console.md) 接入只读目录，提供命令名、顶层参数、布尔/null/枚举补全与帮助提示。
本阶段不扩展嵌套 JSON、动态实体/资产候选、设置命令、面板搜索或最终执行协议。

## 实际变更

- [ConsoleAssist](../../engine/editor/src/ConsoleAssist.cpp) 提供纯 CPU 的上下文定位、前缀候选、字段提示与接受校验。
  使用 Commands 拥有型目录，不访问 Runtime/Scene/文件或 handler；候选来自实际名称与 schema，已有参数从其他位置过滤。
  [ConsoleLexical](../../engine/editor/src/ConsoleLexical.hpp) 与严格解析器共享字符规则，已闭合值复用原完整值解析。
- 候选最多 32 个，构造时即限制数量；说明最多 2048 字节，显示名最多 256 字节且保持 UTF-8 完整。
  字符串枚举使用 JSON 引用、控制字节以可见形式显示、数字保留完整 uint64；替换不重写其他参数/guard。
  接受比较来源文本、光标/选区、编辑修订与目录身份/修订；非法/过期或超 8192 字节拒绝，无部分修改。
- [ConsoleInput](../../engine/editor/src/ConsoleInput.cpp) 实现 Tab 打开/接受、Enter 接受后再次提交、Esc 保留文本、
  Up/Down 候选与历史切换、鼠标选择、最多 8 行滚动列表与参数帮助。Run 单独点击仍提交原文。
  组合输入期间暂停辅助，失焦/换文档关闭；输入控件局部屏蔽已处理的 Enter/Esc 后立即恢复按键数据。
  Tab 粘贴先整段预检再在回调内替换，避免 ImGui 过滤；同帧输入/Tab 的旧候选拒绝仍退出历史浏览。
- [WorkbenchUi](../../engine/editor/src/WorkbenchUi.cpp) 按 command_catalog_version 缓存目录，不按键分派发现命令，
  不增加任务/日志/草稿刷新。保留原有执行与提交后失败语义，不新增业务命令、依赖或协议。
- [ConsoleAssistTests](../../tests/unit/ConsoleAssistTests.cpp) 新增 5 项 CPU 用例；
  [ConsoleCompletionSmoke](../../engine/editor/src/ConsoleCompletionSmoke.cpp) 在原工作台验收后追加 66 步真实 ImGui/SDL 输入。
  原 Console 空白 Tab 用例改为粘贴以匹配新键位，继续保留原文与不提交断言；剪贴板与用户系统隔离并恢复。
  文件进入原 dk_editor_model、dk_editor、dk_editor_tests target，未新增 library/executable。

## 验证记录

Windows / VS2026 / windows-editor Debug；沿用现有依赖 baseline，无依赖或选项变更。
所有 GUI 使用独立夹具与 --background，不操作真实工程或系统剪贴板。主要命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_editor_tests','dk_editor_app') `
  -TestRegex '^dk\.editor\.(console |workspace console|catalog )' `
  -Reason 'M22.6 CPU completion, input guards and direct UI consumer build'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target @('dk_editor_tests','dk_editor_app') `
  -TestRegex '^dk\.editor\.(console assistance|workbench_gpu_validation$)' `
  -Reason 'M22.6 preserve pasted Tabs with completion callbacks; bound candidate construction and verify GUI'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target dk_editor_app -TestRegex '^dk\.editor\.workbench_gpu_validation$' `
  -Reason 'M22.6 stale same-frame typing/Tab must detach history; verify final UI routing'

& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Debug `
  -Target dk_editor_tests -TestRegex '^dk\.editor\.console assistance rejects invalid positions' `
  -Reason 'M22.6 Unicode whole-token selection and trailing arguments regression coverage'
```

证据均在 `out/verify/`，各目录保留 summary.json、build.log、tests.log、results.xml；GUI 最终轮额外保存 LastTest.log。

| 目录 | 实际结果与处理 |
| --- | --- |
| 20261103-134102-3bd97d24 | 构建失败，新测试漏 include Console.hpp；修正后重建，该轮未运行测试 |
| 20261103-134211-536e8602 | 构建通过，22 通过/1 失败/0 跳过；行中缺等号的未完成参数名被扫描器拒绝，修正为仅允许当前光标 token 保留未完成状态；其他 18 项旧回归通过 |
| 20261103-134324-fee83378 | 5 项辅助 CPU 全部通过；GUI 失败，关闭 AllowTabInput 后 ImGui 过滤了粘贴 Tab；修复输入适配，保留既有空白原文断言 |
| 20261103-134507-1deb3335 | 5 项辅助 CPU 与 1 项 GUI 全部通过，无失败/跳过；首次完成新增 57 步，已查看 PNG 中的候选、说明与工作台布局 |
| 20261103-134716-001e402c | 最终 GUI 1/1 通过，无失败/跳过；新增同帧文字/Tab 旧候选拒绝与历史脱离，补全共 66 步 |
| 20261103-134906-f9a4698f | 最后补充 Unicode 正反选区、尾部参数保持、非法 UTF-8，对应 CPU 用例 1/1 通过，无失败/跳过；生产实现未改变 |

最终范围为 **23 项 CPU 与 1 项真实后台 GUI**，不把多轮重复执行累加成更多用例。
原工作台 26 步、原 Console 48 步和补全 66 步均验证；场景 revision/实体数、提交历史和预期错误数持续检查。
补全覆盖命令/参数/枚举、显式提交、鼠标、Up/Down、Esc、历史编辑脱离、IME、Run 原文、旧 JSON、无候选、失焦、
8192 字节拒绝、同帧旧候选拒绝；CPU 另覆盖 Unicode 字节定位/选区、完整整数、排序/32 项预算、缺帮助降级与 100 次读取无 Task/草稿副作用。

最终 GUI 输出 `workbench frames=1133 revision=6 draws=3 ... smoke=passed`，
`validation errors=0 warnings=1 liveAllocations=0 knownLoaderWarnings=1`。
唯一运行告警为既有 `VK_LAYER_AMD_switchable_graphics` API 1.3/应用 1.4 Loader 告警，无新增验证告警。
最终构建没有编译警告；早期测试 handler 的不可达 return 警告已修正。
截图与场景：`out/build/windows-editor/test-artifacts/Debug/editor-02b08c3719d549599c50ed0a05aa0b84`，
已将最终 PPM 转为 PNG 并目视确认候选、字段说明和工作台布局。

未运行：全量、Release/其他平台、完整渲染/物理/IPC/Python/Luau 矩阵；没有修改命令描述或传输，
本阶段用真实 Workspace 目录和 Console GUI 验证，不重复 M22.5 的 runner 发现验收。
IME 使用 SDL 组合/提交/按键事件注入，未逐个人工核验操作系统输入法候选窗口。
`scripts/check-spec.ps1 -Path` 的本次 9 份 Markdown、878 个本地链接、元数据/表格/索引/测试入口与 JSON 清单通过；
`git diff --check` 通过，仅有 tests/unit/CMakeLists.txt 的 CRLF→LF 规范化提示。

## 偏差与决策

采用显式 Tab 打开/接受候选；参数提示不执行命令。跨字段和部分 token 选区保守不补全，完整 token 选区可替换。
后续 M22.7 再提供嵌套/动态候选和实时诊断。

## 遗留问题与下一步

M22.6 实现与定向验收完成；下一阶段 M22.7，增加嵌套 JSON/envelope 辅助、实体/资产动态候选和输入诊断。
部分 token/跨字段选区仍保守不补全；用户可继续手动编辑，最终提交校验始终有效。

## 修改记录

- 2026-11-03T13:39:00+08:00：建立记录，明确接口、键位和验收范围。
- 2026-11-03T13:49:06+08:00：辅助模型、界面与最终 66 步 GUI 通过，记录中间问题及修正、同步范围边界。
- 2026-11-03T13:51:22+08:00：最后 Unicode 选区检查通过；查看最终截图，9 份文档/878 个本地链接与 diff 静态检查通过。
