---
id: "0065"
created_at: "2026-10-08T09:36:00+08:00"
updated_at: "2026-10-08T10:21:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/platform.md
  - ../design/graphics-presentation.md
  - ../design/runtime.md
  - ../design/architecture.md
---

# 0065 M8.1 编辑器工作台

## 目标与设计依据

按 [编辑器设计](../design/editor.md) 完成窗口、Hierarchy、Inspector、Viewport 与保存/加载。
复用统一命令、历史、原子保存和当前内存场景渲染；不提前实现 M8.2 的拾取/Gizmo/相机交互。

## 实际变更

- [Workspace](../../engine/editor/include/dk/editor/Workspace.hpp) 提供 CPU 工作台模型：
  选择、带 guard 的 Inspector 草稿、一次 scene.transaction 提交名称/TRS、Undo/Redo、Save、Open。
  未应用草稿阻止切换选择/历史；失败保留输入和文档；重载成功清空会话选择。
- Runtime::read_scene 仅转发 guard 校验的拥有型 SceneReadSnapshot，不暴露可写 ECS。
  编辑器 UI 经 Runtime::dispatch 操作，未增加或改变命令 schema。
- [dk-editor](../../apps/editor/src/main.cpp) 与私有 UI/渲染桥实现 SDL3/ImGui docking 工作台，
  包含 Hierarchy、Inspector、Viewport、只读 Assets 和有界 Console。
  Open/Reload/关闭遇到脏场景或草稿时提供 Save / Discard / Cancel；Save 失败不继续。
- Viewport 从当前内存快照提取版本，按会话复用 GPU 资产，按 revision/尺寸失效重绘。
  候选图成功后替换预览；错误保留旧图并显示实际版本/过期状态。首版读回 RGBA8 后上传采样纹理，
  sRGB 交换链使用 sRGB 纹理解码和线性 UI 样式；Windows 可用时读取系统微软雅黑，不复制字体资产。
- Platform 增加主线程 SDL 句柄借用、同步事件 sink 与清除关闭请求；
  Frame 增加本代 image_count。ImGui 原生录制集中于 GuiRenderer，使用 unsafe_record 的访问声明、
  ImageView 保留及引擎提交/完成票据；每帧完成后更新/释放 UI 资源，异常路径也先排空 Presenter。
- 新增 DK_BUILD_EDITOR / windows-editor / dk_editor_app，模型依赖 Runtime，CPU-only 构建无 SDL/Vulkan。
  ImGui 1.92.9（实际源码 v1.92.9b-docking）与核验的官方 port 相同；
  [overlay](../../cmake/vcpkg-ports/README.md) 仅为 ImGui 启用 VK_NO_PROTOTYPES 和私有函数加载，
  避免直接函数链接与 volk 同名全局变量冲突。SDL3 复用 3.4.16#1，固定 baseline 不变。
- [使用指南](../guides/editor.md)、构建入口、依赖说明、架构、Roadmap 与测试选择表同步。

## 验证记录

Windows x64 / Visual Studio 2026 / MSVC Debug，定向验证；没有全量或应用 Release 矩阵。
vcpkg 按自身默认构建 ImGui Debug/Release 包，不等同于应用 Release 验收。

1. `cmake --preset windows-editor` 成功，固定 baseline 安装 ImGui；加入私有函数加载 overlay 后重新配置成功。
   编译 dk_editor_app / dk_editor_tests 无新增编译警告。
2. `verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_editor_tests') -TestRegex '^dk\.editor\.(workspace|runtime)'`：
   **5 passed / 0 failed / 0 skipped**，证据 `out/verify/20261008-095535-fd97ecda`。
   覆盖事务/历史、草稿隔离、非法四元数与过期 guard、保存重载、失败打开和保存、不可变 Runtime 快照。
3. `verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_editor_tests','dk_run') -TestRegex '^dk\.(editor\.(workspace|runtime)|runtime\.batch_roundtrip$|bootstrap\.version$)'`：
   **7 passed / 0 failed / 0 skipped**，证据 `out/verify/20261008-095857-ef27b352`。
   CMakeCache 中 EDITOR、PLATFORM、全部 GRAPHICS/RENDER 选项 OFF，确认模型和 CPU Runtime 隔离。
4. `verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_platform_probe','dk_presentation_probe') -TestRegex '^dk\.(editor\.workbench_gpu_validation|platform\.windows|presentation\.frames_validation)$'`：
   平台与呈现 **2 passed**；首轮 editor smoke 失败，证据 `out/verify/20261008-095844-48505772`。
   新增事件 sink 转发、close veto、错误线程和 Frame image_count 检查均通过。
5. 首轮 UI smoke 的鼠标释放被 SDL 轮询/模态框焦点恢复干扰，修复为同一输入帧消费并保持按下/释放坐标；
   后续真实按钮链路通过，但 PowerShell 验收误把版本化组件字段当成 entity.name，改读 components.'dk.Name'.value。
   失败证据保留在 `20261008-100133-db38a93b`、`20261008-100320-6a7a0097`。
6. 最终 `verify.ps1 -BuildDir out/build/windows-editor -Target dk_editor_app -TestRegex '^dk\.editor\.workbench_gpu_validation$'`：
   **1 passed / 0 failed / 0 skipped**，证据 `out/verify/20261008-102007-c6c0f2ff`。
   `20261008-101130-069e6afb` 完成最后一轮画面修正，`20261008-101804-0b98892f` 验证 UI 私有依赖边界；
   最终补齐字体/平台初始化异常的 ImGui 上下文清理后，再次运行上述 smoke 通过。
   在独立夹具窗口实际注入 ImGui 鼠标/键盘，覆盖选择、名称/TRS、Apply、Undo/Redo、保存重载、
   重载取消、SDL 关闭请求取消、Revert、resize、失败 Open；最后校验场景文件。
   TRS 修改使预览 RGBA8 哈希改变，Undo 恢复原哈希，Redo 再次改变；当前文档版本与预览一致。
   UI 画面读回 PPM，并转换 `out/editor-workbench-smoke.png` 检查布局、层级、错误信息和 Inspector。
   之前成功运行 `20261008-100624-22c419d0`、`20261008-100913-af90aecc` 保留字体/颜色完善过程的证据。
7. `dk-editor --validation --frames 8 --screenshot out/editor-sponza.ppm` 默认 Sponza 成功：
   revision=1、103 draws、Viewport 796×518，窗口画面 1440×900。
   最终 PPM SHA256=D2234FB38C7780962A06198745FCA9E73BAD03461C3E089125B0250C3AC08C8E。
   日志 `out/editor-sponza.log`，转换 `out/editor-sponza.png` 后实际查看；默认项目和资产没有被修改。

GPU 验收要求 Khronos validation（含同步验证），最终窗口 smoke 与 Sponza 均为
**errors=0 / warnings=0 / liveAllocations=0**。沿用已知驱动冲突的进程局部
DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，finally 恢复；没有关闭 Khronos 或同步验证。
最终程序日志确认 GPU=NVIDIA GeForce RTX 4070 Laptop GPU、driver=596.49；未升级驱动/SDK。

scripts/check-spec.ps1 通过：135 Markdown、1354 本地链接及元数据/索引/测试入口/JSON。
提交前 git diff --check 通过；产物及验收截图保留在忽略的 out 目录。

## 偏差与限制

- Viewport 使用同步导入、固定相机、现有 unlit preview 和 RGBA8 读回/上传桥；大场景加载会阻塞窗口。
  不宣称 PBR、HDR IBL/天空盒渲染、异步资源流送或性能优化已经实现。
- 只编辑已有项目，保存场景文件；manifest/资产映射只读，无新建/Save As 向导。
- UI 图形验收覆盖当前 Windows/设备/桌面；未验证其他平台、多显示器 DPI 切换或交换链格式切换。
- 后续为 M8.2：CPU 射线查询、Gizmo、相机交互。M8.3 IPC 与 M8.4 一致性仍待开始。