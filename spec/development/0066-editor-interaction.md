---
id: "0066"
created_at: "2026-10-08T11:35:00+08:00"
updated_at: "2026-10-08T12:08:19+08:00"
status: completed
design_refs:
  - ../design/editor-interaction.md
  - ../design/geometry-query.md
  - ../design/editor.md
  - ../design/render-data.md
  - ../design/architecture.md
---

# 0066 M8.2 编辑器拾取与交互

## 目标与设计依据

按 [交互设计](../design/editor-interaction.md) 与 [几何设计](../design/geometry-query.md)
实现 CPU 拾取、统一选择、本地 TRS Gizmo、独立相机和单次拖动撤销。

## 实际变更

- [Geometry Query](../../engine/geometry/include/dk/geometry/Query.hpp) 提供有限射线校验、AABB slab、
  双面三角形、局部三角形 BVH、仿射实例最近命中；变换射线不归一化局部方向，保留可比较的世界 t。
  稳定返回原三角形编号；空/退化网格、非有限输入和不可逆实例有明确结果。
- [Camera](../../engine/editor/include/dk/editor/Camera.hpp) 提供会话环绕/平移/缩放/聚焦/重置，
  共享 Vulkan Y 翻转与 [0,1] 深度的投影、射线和屏幕映射；不修改 Scene 或历史。
- [TransformEdit](../../engine/editor/include/dk/editor/TransformEdit.hpp) 为独立预览候选；
  [Workspace](../../engine/editor/src/Workspace.cpp) 捕获 guard、选择、本地 TRS、父世界矩阵，
  松开时用既有 entity.set_transform 提交一次。取消/无变化不提交；过期、草稿、非法值保留真实状态。
  本地旋转/缩放不分解世界 shear；平移用父级逆变换。
- [RenderScene](../../engine/render/data/include/dk/render/RenderScene.hpp) 增加只读单实体 local override，
  后代使用候选父变换，Snapshot/revision 不变，UI 单独用 preview 序号标记临时图像。
- [Viewport](../../engine/editor/src/Viewport.cpp) 在资产加载时构建并复用 CPU 网格 BVH，
  拾取已发布图像对应的相机/世界变换，按最近命中选择 EntityId；失败/过期图像不接受拾取。
  [ViewportInput](../../engine/editor/src/ViewportInput.cpp) 用 ImGui 绘制本地 Move/Rotate/Scale 轴/环与黄色选择框，
  支持图像坐标换算、图外拖动捕获、相机输入、F/Home、Frame/Reset；手势时禁用冲突入口。
  Esc、真实 SDL 失焦/最小化、矩形变化、关闭均取消；原生事件在暂停 NewFrame 前处理。
- 新增 [几何测试](../../tests/unit/GeometryTests.cpp)、Editor 手势/相机测试、Render 预览继承测试及
  [真实交互验收](../../tests/integration/EditorInteractionTest.ps1)，保留 M8.1 smoke。
  使用指南、架构/设计索引、Roadmap、测试选择表同步；未增加/修改命令 schema 或三方依赖。

## 验证记录

Windows x64 / VS2026 / MSVC 14.51.36231 / Debug；定向验证，没有全量或应用 Release 矩阵。

1. 初次受限构建因 vcpkg 外部缓存写权限失败，测试未运行：`out/verify/20261008-114344-76d9993a`。
   正常权限重跑时 vcpkg 按现有固定 baseline 为当前编译器重建依赖，版本未变。
2. `verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_geometry_tests','dk_editor_tests','dk_render_data_tests','dk_editor_app')`
   配合 `^dk\.(geometry\.|editor\.(workspace|runtime|camera|interaction)|render\.data\.)`：
   **17 CPU passed**，涵盖几何/投影、父级 shear、手势提交/取消/过期、事务历史、保存失败及临时变换继承。
   该前缀还选中了新增 interaction_gpu_validation，22 步扩展前的交互动作通过，但 AMD 隐式层产生 1 条版本告警，
   GPU 用例判失败；证据 `out/verify/20261008-114400-f3ecb7f5`。后续 CPU 筛选在名称前缀后加空格，排除 GPU 后缀。
3. 增量受限 MSBuild FileTracker 拒绝访问，测试未运行：`out/verify/20261008-115031-1cea1be3`。
   正常权限与进程局部 AMD 环境重跑 `out/verify/20261008-115602-7c5547d5`：M8.1 **1 passed**，
   M8.2 原生交互零 validation 错误/告警，但 runner 脚本误读 Task envelope 导致验收失败；修正为 result.value。
   `out/verify/20261008-115743-55737b1d`：M8.2 **1 passed / 0 failed / 0 skipped**。
4. 补齐原生失焦、最小化和尺寸变化取消后，最终
   `verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_run') -TestRegex '^dk\.editor\.(workbench|interaction)_gpu_validation$'`：
   **2 passed / 0 failed / 0 skipped**，证据 `out/verify/20261008-115951-05c40c2f`，无新增编译告警。
   M8.2 窗口实际执行 22 个输入步骤 / 277 帧：空白清选、最近三角形选择、三种 Gizmo、预览不改 revision/history、
   一次拖动一条历史、Undo 图像恢复、Redo、Esc/SDL 失焦/resize/minimize 取消、环绕/平移/缩放/聚焦/重置、保存重载与再次拾取。
   全部实体保存后的 TRS 与独立 runner 重载一致。夹具不修改真实 demo。
   最终图像 `out/editor-interaction.png` 已目视检查选择框、Gizmo、Inspector 与布局。
5. `verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_geometry_tests','dk_editor_tests','dk_run')`
   配合 `^dk\.(geometry\.|editor\.(workspace |runtime |camera |interaction )|runtime\.batch_roundtrip$|bootstrap\.version$)`：
   **15 passed / 0 failed / 0 skipped**，证据 `out/verify/20261008-120119-d79a4373`。
   该 cache 的 EDITOR、PLATFORM、全部 GRAPHICS/RENDER 选项均 OFF，验证 Geometry/模型与 CPU Runtime 不依赖 GPU。
6. 默认 Sponza 实际运行 `dk-editor --validation --frames 8 --screenshot out/editor-m82-sponza.ppm`：
   NVIDIA GeForce RTX 4070 Laptop GPU / driver 596.49，8 帧、revision=1、103 draws、796x446；
   **validation errors=0 / warnings=0 / liveAllocations=0**。日志 `out/editor-m82-sponza.log`，
   PPM SHA256 `8B5FB747EC799859F28590830EEC819DEBCEA24FAFF85A72C053ECBE65B08061`；PNG 已目视检查。
   GPU 最终验收均仅在测试进程设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1` 并在 finally 恢复，保留 Khronos/同步校验。
7. `scripts/check-spec.ps1` 最终通过 **138 Markdown / 1387 本地链接**，包括元数据、索引、表格、测试入口与 JSON；
   `git diff --check` 通过。工作区输出的 CRLF 转换提示不是格式错误。

## 偏差与决策

使用现有 ImGui 绘制本地轴/环，不引入新的依赖；首版双面几何拾取不检测纹理 alpha。
平移/缩放按投影轴的屏幕位移计算，旋转使用射线与局部环平面；接近端视、投影不足 15px 的平移/缩放轴隐藏，
可环绕相机后操作。保留同步预览读回/上传桥；不宣称大场景交互性能或 GPU BVH 已优化。

## 遗留问题与下一步

M8.2 已验收。下一项 M8.3 IPC 和 dk-ctl；M8.4 外部并发编辑一致性仍待实现。
纹理 alpha 精确拾取、多选、世界轴/吸附、飞行相机、后台流式预览均未实现；未测试其他 GPU/平台或 Release。

## 修改记录

- 2026-10-08T11:35:00+08:00：建立设计和开发记录。
- 2026-10-08T12:05:00+08:00：完成实现、定向 CPU/GPU/runner 验收及默认 Sponza 预览，记录实际失败与修复过程。
