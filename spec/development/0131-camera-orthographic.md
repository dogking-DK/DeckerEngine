---
id: "0131"
created_at: "2026-10-19T10:58:00+08:00"
updated_at: "2026-10-19T11:24:37+08:00"
status: completed
design_refs:
  - ../design/editor-interaction.md
  - ../design/foundation-math.md
  - ../design/editor.md
  - ../design/render-pipeline.md
  - ../design/render-simulation.md
---

# 0131 M18.3 基础正交视口

## 目标与设计依据

按[交互设计](../design/editor-interaction.md)完成[Roadmap M18.3](../roadmap.md)：正交 Orbit/Pan/缩放、六视图、
基础光栅和模拟、显式 Unlit inspection 及拾取/Gizmo；投影数学见[Math](../design/foundation-math.md)。
路径与光栅环境正交、固定截图 schema 留在后续阶段，不新增依赖或命令。

## 实际变更

- [Projection](../../engine/foundation/math/src/Projection.cpp) 提供正交矩阵与近裁剪面平行射线；Math 无新增依赖。
- [Camera](../../engine/editor/src/Camera.cpp) 实现焦平面尺度往返、正交高度/平移/缩放、精确六视向、完整边界取景与原子拒绝；正交只允许 Orbit，返回透视恢复之前的导航模式。
- 两视口共用镜头/视向面板；模拟正交 Frame cloth 保留视向。固定极点返回透视后零导航输入保留姿态，实际观察输入才退出固定视向。
- [Viewport](../../engine/editor/src/Viewport.cpp) 的会话 Unlit inspection 显式选择基础管线，key 包含显示选项，取消旧环境请求；原 Scene 不变。UI 与实际提交分别拒绝路径/环境正交，旧图禁止拾取。
- 扩展现有数学、相机、管线及窗口验收；[OrthographicSmoke](../../engine/editor/src/OrthographicSmoke.cpp) 经真实 ImGui 输入覆盖六视图、拾取、三种 Gizmo、取消/历史、导航、外部环境拒绝与 Unlit 恢复。无新增测试程序/命令 schema。
- 修复成功 Camera Apply 后残留错误导致视口尺寸改变、暂停路径累积被重置；后台 smoke 输出及时刷新，便于定位原生失败。

## 验证记录

使用现有 windows-editor / Release 与 windows-dev / Release CPU-only；RTX 4070 Laptop GPU / 596.49。
窗口均 --background、一次性工程、required Vulkan/sync validation。验证子进程设置
DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，排除已有 AMD loader 警告；不改变引擎验证规则。

| out/verify 记录 | 实际结果 |
| --- | --- |
| 20261019-110205-fdc24c60 | 沙箱内 MSBuild 未推进，取消；未运行测试 |
| 20261019-110337-5759ecc7 | 数学/相机 17 项通过，0 失败/跳过；之后补充极点回归 |
| 20261019-110825-c48514c8 | 最终 CPU-only 数学/相机 18 项通过，0 失败/跳过；Editor/Platform/Graphics 关闭 |
| 20261019-110552-76978f57 | 4 项管线/路径 CPU、基础 GPU 管线、Scene media 窗口通过；交互点击与 Surface only 暂停失败。模拟结果因随后发现的旧对象布局混用不作最终证据 |
| 20261019-110948-d1e2a274 | Surface only 窗口通过；模拟/交互原生崩溃，后定位为旧 smoke 对象布局混用 |
| 20261019-111219-fefe31b1 | 交互通过透视原有链路、正交六视图/拾取/移动/撤销/重做/取消；旋转固定点击落在图像外失败 |
| 20261019-111545-7cfb716f | 全部编辑器编译单元统一重编后，完整 CPU/GPU 模拟和透视+正交交互 2 项通过，0 失败/跳过；full-tests.log 保留完整步骤 |
| 20261019-111825-cfe443ca | 最终禁用提示及重编后的交互/Surface only/Scene media 3 项通过，0 失败/跳过 |
| 20261019-112031-bd415cbf | 紧凑/可换行工具栏的最终交互、CPU/GPU 模拟、Surface only、Scene media 4 项通过，0 失败/跳过；full-tests.log 保留全部步骤 |

基础光栅测试比较四张完整图，验证同尺寸几何在不同深度相等、移动深度图像不变、近远裁剪正确。
完整模拟驱动分别验证 CPU/GPU 正交、Top/Frame/Orbit/Pan/缩放、切视图独立和返回透视，直至 Stop/restart/活动关闭；
交互驱动额外 31 个步骤完成正交与 Unlit 验收，Scene/history 只有显式 Gizmo 或环境服务操作改变。
上述最后两项均 validation errors=0、warnings=0、liveAllocations=0；界面截图检查控件和图像布局。

最终覆盖 27 项不同用例（22 CPU + 5 GPU/真实窗口），全部通过，0 跳过；18 项数学/相机已在 CPU-only 独立复验。
最终四项窗口日志均 errors=0、warnings=0、liveAllocations=0，构建无编译警告/错误。
Surface only 与 Scene media 的 GUI/capture 线性比较均最大误差 0；两种视口的最终截图已做视觉复查，
验收预览位于 out/m18-orthographic-interaction.png 和 out/m18-orthographic-simulation.png（检查用，不入库）。

复现命令（仓库根 PowerShell；同一构建目录串行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration Release -Target @('dk_math_tests','dk_editor_tests') -TestRegex '^dk\.(math\.(perspective|orthographic)|editor\.(camera |environment camera ))' -Reason 'M18.3 CPU-only camera'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release -Target @('dk_render_pipeline_probe','dk_render_pipeline_tests','dk_pathtracing_reference_tests') -TestRegex '^dk\.(render\.pipeline_gpu_validation|render\.pipeline\.(orthographic|atmosphere perspective)|pathtracing\.camera rays)' -Reason 'M18.3 raster and projection support boundary'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.(interaction|simulation|pathtracing|media_pathtracing)_gpu_validation$' -Reason 'M18.3 final background UI acceptance'
```

未运行全量、Debug/其他设备/平台、关闭路径模块的 GUI 配置或性能基准；不把后台输入注入视为前台人工鼠标操作验收。
不扩展正交 capture/sequence、CPU/GPU 路径主射线及光栅环境积分；这些继续显式拒绝。
文档检查限定本次 10 个 Markdown 文件，741 个本地链接及元数据/表格/索引/测试入口/JSON 校验通过；git diff --check 通过。

## 偏差与决策

保留任务开始时已有的 spec/README.md 修改及未跟踪的 spec/guides/ai-documentation-workflow.md。
首次实现期间修改私有 SmokeDriver 布局与构建重叠，部分编译单元读取旧头但对象时间较新，增量构建没有重编。
原生诊断出现模拟参数分派到交互驱动的证据；触碰编辑器 cpp 后统一重编，不能把此前的偶然通过当作新驱动验收。
窗口高度变化后，旧测试固定点击中心不能保证命中已移动实体，改为真实投影位置；旋转验收坐标只从可见圆弧选取，不改变 Gizmo 命中算法或降低断言。

## 遗留问题与下一步

M18.3 范围完成。下一项 M18.4：路径 CPU/GPU 正交主射线及固定 capture/sequence schema、来源与示例。
光栅大气/云正交属于 M18.5；相机偏好不跨启动保存。

## 修改记录

- 2026-10-19T10:58:00+08:00：建立实施契约与开发记录。
- 2026-10-19T11:22:00+08:00：完成实现、27 项定向验收与文档同步；保留所有失败诊断及实际支持边界。
