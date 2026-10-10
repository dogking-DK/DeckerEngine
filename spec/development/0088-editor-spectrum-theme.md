---
id: "0088"
created_at: "2026-10-10T15:40:00+08:00"
updated_at: "2026-10-10T15:53:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
---

# 0088 Adobe Spectrum 编辑器主题迁移

## 目标与设计依据

按用户选定的 overlay 补丁方案实施[编辑器主题设计](../design/editor.md)。
基于官方 v1.92.9b-docking，移植 Adobe 提交
`3252c1c6aaa72ef9b59c6039e3127e2307171c76` 相对 v1.92.8 的 Spectrum 改动。
保留现有 vcpkg baseline、中文字体、docking 与 SDL3/Vulkan 接入；按用户追加要求默认浅色（白色/浅灰背景）。
用户已有 spec/README.md 和 AI 文档指南改动不纳入本任务提交。

## 实际变更

- [overlay](../../cmake/vcpkg-ports/imgui/portfile.cmake)增加显式 spectrum feature（依赖 docking），
  保存[补丁](../../cmake/vcpkg-ports/imgui/spectrum.patch)、[来源与升级说明](../../cmake/vcpkg-ports/imgui/SPECTRUM.md)
  和许可证；本地 port 修订为 #1，官方源码 tag/hash 和 root baseline 不变。
- 四个 ImGui 核心文件移植控件/文字/箭头绘制，增加 Spectrum 色板源文件及安装头；
  冲突处保留新版 SelectableRounding/docking 路径，去掉 Adobe 未使用的 IsComboPopup 字段。
- 额外色板通过 ColorToOutput 按输出格式转换，直接绘制时应用一次全局/禁用 alpha；
  修复 selectable 覆盖 alpha 和 mixed checkbox 使用外框计算 padding 的问题。
- [GuiRenderer](../../engine/editor/src/GuiRenderer.cpp)改用浅色主题，保存原始 style；
  每次交换链初始化从原始色值设置 style 和额外色板输出模式。补齐表格/停靠/新版颜色。
- 保留现有中文系统字体，不带入 Adobe 可选的内嵌字体。未增加主题切换设置。
- [主题测试](../../tests/unit/EditorThemeTests.cpp)验证输出模式往返、明暗色板以及实际禁用控件顶点透明度/有限几何。
  复用 dk_editor_tests，仅 DK_BUILD_EDITOR 时链接 ImGui，模型单独构建不增加 GUI 依赖。

## 验证记录

`cmake --preset windows-editor` 实际由 vcpkg 下载缓存中的官方源码并成功应用 Spectrum 补丁、构建安装。
首次生成发现测试目录看不到其他目录的 IMPORTED imgui target，补充该目录 find_package 后解决。
首次 verifier `out/verify/20261010-154414-e186bebe` 因此链接失败，未运行测试，不作为通过证据。

以下均使用 windows-editor/RelWithDebInfo；真实窗口均带 --background。
沿用[编辑器指南](../guides/editor.md)记录的本机 AMD 隐式层隔离变量，进程结束恢复环境。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_editor_app','dk_editor_tests','dk_run') -TestRegex '^dk\.editor\.(theme |workbench_gpu_validation$|interaction_gpu_validation$)' -Reason '验证 Spectrum 色板与禁用控件几何、真实工作台和交互'
```

`out/verify/20261010-154505-3d2fe000`：构建成功，4通过、0失败、0跳过。
包括2项主题测试（覆盖明/暗色板）及真实工作台、选择/Gizmo/取消/保存/runner重载交互。

用户随后指定白色，将唯一默认主题参数改为浅色；其余实现未改变，仅补验默认工作台：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target dk_editor_app -TestRegex '^dk\.editor\.workbench_gpu_validation$' -Reason '按用户要求补验 Spectrum 浅色工作台'
```

`out/verify/20261010-155003-10e7a9ba`：构建成功，1通过、0失败、0跳过。
另用一次性夹具执行后台12帧截图，`out/adobe-imgui-migration/preview.log`记录
validation errors=0、warnings=0、liveAllocations=0；
`out/adobe-imgui-migration/workbench-spectrum-light.png`人工检查白色/浅灰背景、蓝色标签、字体与禁用控件。
截图首次在受限执行环境无法打开夹具目录，恢复常用执行权限后成功；未改动工程路径保护。
文档检查通过：8个Markdown文件、193个本地链接及元数据/索引/清单；git diff --check通过。

未运行全量、引擎双配置、GPU模拟数值回归或完整DPI矩阵；本次未改动这些计算和缩放逻辑。
vcpkg 按现有 triplet 常规构建自身 Debug/Release 库，不代表额外执行引擎双配置验证。

## 维护限制

Adobe 本身并未实现完整 Spectrum 控件库；本次移植其已有控件外观。
其色板和输出模式为进程级状态，当前编辑器仅一个 ImGui context；以后多 context 独立主题需要调整。
升级 ImGui 时需重新核对补丁及 docking 内部接口，详细步骤见 overlay 说明。无阻塞项。

## 修改记录

- 2026-10-10T15:40:00+08:00：固定源提交，更新设计并创建记录。
- 2026-10-10T15:53:00+08:00：按追加要求采用浅色，完成补丁、定向测试与实际截图检查。
