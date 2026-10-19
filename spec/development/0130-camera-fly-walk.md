---
id: "0130"
created_at: "2026-10-19T09:12:00+08:00"
updated_at: "2026-10-19T09:48:00+08:00"
status: completed
design_refs:
  - ../design/editor-interaction.md
  - ../design/editor.md
  - ../design/platform.md
---

# 0130 M18.2 Fly 与水平 Walk

## 目标与设计依据

按[交互设计](../design/editor-interaction.md)和[Roadmap M18.2](../roadmap.md)实现两视口共享的连续导航。
Walk 固定世界 Y，无碰撞/重力；模式/速度属于会话，不改场景和历史。相对鼠标通过[Platform 借用窗口](../design/platform.md)接入。

## 实际变更

- [CameraNavigation](../../engine/editor/src/CameraNavigation.cpp) 与公开输入类型进入 dk_editor_model：Fly 观察轴移动/世界 Y 升降、Walk 固高/yaw 水平前进；组合方向归一化，Ctrl 优先，dt≤0.05 秒，首拍不补移动，非法输入保留完整候选之前的状态。
- Camera 保存 NavigationMode，切模式保持 eye/yaw/pitch/focus distance/lens/revision；Home 保留模式，Restore defaults 返回 Orbit。
- [NavigationUi](../../engine/editor/src/NavigationUi.cpp) 供 Edit/Simulation 共用；右键捕获相对鼠标，滚轮调速，模式按钮及固定高度的两行提示。Scene/dirty/history 不变，捕获期间不允许拾取/Gizmo。
- 主线程 RelativeMouse owner 借用 SDL 窗口，仅消费本窗口 xrel/yrel，过滤重复 ImGui motion；捕获失败不发布 owner。关闭/失焦/隐藏/最小化、文本/弹层、模式/视图切换、图像不可用与退出释放；后台窗口不抢焦点、不跨应用恢复指针。
- Surface only 路径预览此前每帧取消，持续 Fly 时没有新图。现在相机请求等待当前完整图像，再提交最新姿态；暂停/失败/物理变化仍可恢复，Scene media 保留既有低分辨率及 100ms 策略。旧图保留真实相机来源，拾取要求当前图。
- 扩展现有 Camera 单元、交互/模拟/路径/桌面 smoke，无新增测试程序、依赖或命令 schema。

## 验证记录

使用现有 windows-editor / Release（与已有 GPU 验收配置一致），NVIDIA GeForce RTX 4070 Laptop GPU / 596.49。
真实窗口均 --background、一次性工程副本、required Vulkan/sync validation；验证子进程设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，排除已知 AMD 1.3 loader 警告，未改变引擎验证规则。

| 验证目录（out/verify 下） | 范围与结果 |
| --- | --- |
| 20261019-092303-d47df114 | 构建 dk_editor_tests/dk_editor_app/dk_run/dk_ctl；9 项相机单元通过，0 失败/跳过 |
| 20261019-092939-1b7c50df | windows-dev CPU-only / Release，dk_editor_tests；4 项新增导航测试通过，0 失败/跳过 |
| 20261019-093037-70d886a1 | 交互/CPU+GPU模拟通过；路径连续 Fly 显示饥饿失败，引出请求合并修复 |
| 20261019-094139-89fe132b | dk_editor_app/dk_ctl；Surface only 与 Scene media 两项真实路径测试通过，0 失败/跳过 |
| 20261019-094323-7bdc498d | 交互/模拟通过；Sponza 介质分辨率切换引发相对捕获取消失败 |
| 20261019-094538-b19ce677 | dk_editor_app/dk_run/dk_ctl；交互/模拟/Sponza 桌面 3 项全部通过，0 失败/跳过 |
| 20261019-094622-53a46b22 | windows-dev CPU-only / Release，dk_editor_tests；最终 10 项相机回归全部通过，0 失败/跳过 |

首轮 20261019-092251-d9e8991e 因沙箱不能访问已有 vcpkg 工具链而构建失败、测试未运行；获准访问后用原配置恢复。
20261019-092605-dbd77fc1 的 3 项窗口测试失败：新增提示导致 Gizmo 图像矩形变化、小布片导航超出可见范围、未排除 AMD loader 警告。
20261019-092738-2a28d184 模拟通过，另外 2 项失败：dock 窗口不能用 collapse 模拟隐藏、导航提示行数变化触发路径 resize。
分别改为隐藏 dock 内容的注入、固定提示行数，并修正 fixture 的实际移速；没有降低断言。最后补充的切模式/切视图及 Sponza 用例已通过。介质路径切换高低分辨率时显示比例有舍入差异：相对观察不依赖旧矩形，允许其继续；原生 resize/DPI 仍立即取消。

最终覆盖 15 项不同用例（10 CPU + 5 GUI），全部通过；CPU-only 配置保持 GUI/Platform/Graphics 关闭。
交互 smoke 验证原有拾取/Gizmo/持久化、模式切换保姿态、Fly/Walk、松键/松右键、Esc、失焦、最小化、原生隐藏、dock 内容隐藏、resize、文本、设置弹层、关闭取消及 SDL 捕获失败不发布状态。
模拟 CPU/GPU 均验证两模式、切视图期间释放、暂停步数和 Edit 相机/Scene/history 保持。
Surface only 持续 Fly 发布 4 次新相机图像，停机后三个 GUI/capture 比较各 245088 像素、最大误差 0；Scene media 的 7 次比较各 9216 像素误差 0，Console capture 误差 0。
Sponza 的 2 秒持续转向在大气/云光栅分别发布 71/61 次新图，介质空气/云路径分别发布 39/39 次；所有检查点停机后收敛。
路径/介质/Sponza 日志均为 validation errors=0、warnings=0、liveAllocations=0；本次构建未见编译器警告/错误。
最终交互截图另检查工具栏与图像布局，证据在 out/m18-navigation.png（检查用，不入库）。

关键复现命令（仓库根，PowerShell；相同配置的构建/CTest 串行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration Release -Target dk_editor_tests -TestRegex '^dk\.editor\.(camera |environment camera )' -Reason 'M18.2 CPU-only camera regression'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release -Target @('dk_editor_app','dk_ctl') -TestRegex '^dk\.editor\.(pathtracing|media_pathtracing)_gpu_validation$' -Reason 'M18.2 continuous path publication and history'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.(interaction|simulation|desktop_render)_gpu_validation$' -Reason 'M18.2 navigation capture lifecycle and environment publication'
```

未运行全量套件、其他配置/设备/系统及性能基准；未抢占前台验证实际桌面指针越屏/恢复位置，也未覆盖多显示器 DPI 实机输入。这些不冒充后台 SDL 状态验收结果。
文档检查：`check-spec.ps1 -Path` 限定本次 8 个 Markdown 文件，710 个本地链接及元数据/表格/索引/测试入口/JSON 校验通过；`git diff --check` 通过。
设计/记录/索引/指南/Roadmap 同步完成。工作区已有的 spec/README.md 和未跟踪的 ai-documentation-workflow.md 保持原样，不纳入本次提交。

## 偏差与决策

窗口自动化不抢焦点；SDL 相对模式状态和真实 OS 前台捕获分别说明，不用直接 Camera 调用替代 UI 输入验收。
SDL 接口约定核对了[官方相对模式说明](https://wiki.libsdl.org/SDL3/SDL_SetWindowRelativeMouseMode)及本地固定版本头；本次未升级 SDL。
受连续 Fly 回归证实的 Surface only 请求饥饿属于 M18.2 持续导航验收范围，修复先同步到[工作台设计](../design/editor.md)。

## 遗留问题与下一步

M18.2 范围完成。下一阶段 M18.3：正交 Orbit、六视图、基础光栅/模拟与拾取/Gizmo。Walk 无碰撞/重力，偏好不跨启动保存；这些不隐含于当前阶段。

## 修改记录

- 2026-10-19T09:12:00+08:00：建立实施设计与记录。
- 2026-10-19T09:48:00+08:00：实现、连续渲染修复及 15 项定向验收完成，同步设计和用户指南。
