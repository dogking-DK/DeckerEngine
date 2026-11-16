---
id: "0173"
created_at: "2026-11-16T13:58:36+08:00"
updated_at: "2026-11-16T15:25:58+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/editor-interaction.md
---

# 0173 右侧相机与渲染设置

## 目标与设计依据

按用户要求把中央 Viewport 上方的配置控件移到右侧功能区，扩大图像区域；依据 [editor](../design/editor.md)、[editor-interaction](../design/editor-interaction.md)。

## 实际变更

- WorkbenchUi 增加右上 View settings，分为 Rendering、Camera and navigation；右下保留 Inspector/Environment/Path tracing 标签。中央保留状态、图像和鼠标交互。
- 迁移 Raster/Path trace、Settings、Refresh、Unlit、Voxel settings、Move/Rotate/Scale、Reset/Frame、相机弹窗/投影/标准视图/导航入口。沿用服务、会话设置、guard 和草稿语义。
- SettingsCatalog/CameraSettingsUi 的搜索定位指向新面板；窄侧栏按钮换行。Inspector Apply/Revert 移到属性顶部，实体变化重置滚动；执行命令后重新获取可能被替换的 draft。
- NavigationSmoke 检查 Revert 后草稿确实清除；OrthographicSmoke 在扩大后的中央区域中先正交缩小再验证背景空点，避免原角落现在落到大背景平面。

## 验证记录

均使用后台编辑器与真实 RTX 4070 Laptop GPU；屏蔽机器自带的 AMD switchable 隐式层，Khronos validation 保留。

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target dk_editor_app -TestRegex '^dk\.editor\.settings_help_gpu_validation$' -Reason 'Sidebar settings locate, apply and cancel'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target @('dk_editor_app','dk_run','dk_ctl','dk_voxel_asset_process_probe','dk_voxel_gpu_probe') -TestRegex '^dk\.(voxels\.(gpu_numeric_required|path_consumers_gpu_required)|editor\.interaction_gpu_validation)$' -Reason 'All consumers rebuilt; sidebar and voxel GUI/capture acceptance'
```

- Settings help：`out/verify/20261116-143641-d076586f` 中对应测试通过，涵盖定位/字段编辑/Apply/Cancel。
- 旧 mesh 交互：Debug `20261116-145053-dd22ac0a` 与 Release `20261116-145900-0dcdd195` 对应测试通过，覆盖导航、Gizmo、拾取、透视/正交和草稿。
- 后一 Release scope 的 3 项全部通过，包含体素双后端与 GUI/独立 capture 一致性。体素光栅 capture 在 `20261116-141910-023ac2f3` 中对应项通过。
- 开发中实际失败：窄栏 Frame 被裁剪、Inspector 滚动位置和底部 Revert 不可见、扩大后的 viewport 角落不再为空背景；均修复后通过上述交互验收。
- 一次未完整重建消费端的 Debug scope 出现 temporal 图像不一致；不把该次测试算通过。完整构建 Release 消费端及夹具后通过相同验收。

实际生成的 256 Sponza 工程以 `--background --frames 120 --screenshot` 打开并截取完整工作台：viewport 704×549，当前 revision=1，validation 错误/警告和 live allocations 均为 0。截图确认右侧渲染/相机分组与中央图像，无顶部重复工具栏。

![Sponza 与右侧功能栏](../benchmarks/data/2026-11-16-sponza-voxels/editor-view-settings.png)

文档定向检查通过：14 个 Markdown、1177 个本地链接、元数据/表格/索引/测试入口/JSON 清单；`git diff --check` 通过。未运行全仓回归。

## 偏差与决策

不新增设置存储或命令，不改变模拟视图工具栏。为了避免短侧栏隐藏提交操作，额外调整 Inspector 按钮位置。
路径消费端 checkpoint 在 Windows 短暂文件占用时按原 deadline 重试 PermissionError，未放宽内容或图像判据；相关测试改动随体素扩展记录 [0172](0172-mesh-voxelization.md)。

## 遗留问题与下一步

本项完成。未宣称任意用户自定义停靠布局和所有窗口尺寸均做穷举验收。

## 修改记录

- 2026-11-16T13:58:36+08:00：创建设计与记录。
- 2026-11-16T15:17:00+08:00：完成右侧分组与实际 GUI 回归，记录布局引起的失败与修复。
