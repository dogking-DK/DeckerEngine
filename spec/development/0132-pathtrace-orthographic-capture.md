---
id: "0132"
created_at: "2026-10-19T11:34:00+08:00"
updated_at: "2026-10-19T11:51:29+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-capture.md
  - ../design/editor-interaction.md
  - ../design/editor.md
---

# 0132 M18.4 路径追踪与固定捕获正交相机

## 目标与设计依据

依据[相机设计](../design/editor-interaction.md#正交支持分层与截图兼容)，扩展 CPU/GPU Surface only、Scene media，
同步固定 capture 与 render.sequence 的相机 schema、有效来源和示例；光栅环境正交不在本阶段。

## 实际变更

- PathTraceReference 校验刚体透视/正交矩阵；正交使用每像素抖动近面起点、平行 -Z 方向和独立主网格裁剪范围。
- PathTraceRenderer 共用固定/渐进视图参数，提供近面中心、像素步进和平行方向；介质用中心双精度径向/云相位加像素位移，保留 far 只裁剪网格的既有物理边界。
- CaptureCamera / Operations / CaptureService 增加 projection、ortho_height 和显式 FOV 标记；先校验全部相机再入队，非法组合保留 Scene/history/文件。结果只报告有效投影参数，正交声明 primary_ray_origin=near_plane。
- Editor 开放正交 Surface only / Scene media；禁用不兼容的光栅环境切换，复用原相机 revision / 累积失效与完成帧发布。
- 增加 CPU 射线/介质、GPU 解析/历史、真实 GUI 三次投影操作和固定捕获/序列测试；同步命令参考、编辑器指南和 Python/Luau 示例。

## 验证记录

使用现有 windows-editor / Release，RTX 4070 Laptop GPU、驱动 596.49；GPU 为 required RT/validation/synchronization，GUI 全部 --background。
设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 排除系统 AMD 可切换显卡层告警，未放宽实际 validation 断言。
所有构建/测试均通过 scripts/verify.ps1，固定 -BuildDir out/build/windows-editor -Configuration Release；各轮完整 target/regex 和选中名称见对应 summary.json。

| 验证证据（out/verify 下） | 构建目标 | 实际结果与复验 |
| --- | --- | --- |
| 20261019-114008-142ccb9f | dk_pathtracing_reference_tests、dk_pathtracing_probe、dk_editor_tests、dk_protocol_tests、dk_run、dk_ctl、dk_editor_app | 23 项中 21 通过；两个新表面用例误设 max_bounces=0，按既有契约改为 1 后复验 |
| 20261019-114543-154cc04f | dk_pathtracing_reference_tests、dk_pathtracing_probe、dk_run、dk_ctl、dk_editor_app | 10 项中 9 通过；新增 near=1 GPU 对照将几何恰放在裁剪平面，浮点边界产生差异，改为非边界几何后复验；近/远面内外仍由独立断言覆盖 |
| 20261019-114845-afc13ea0 | dk_pathtracing_probe、dk_protocol_tests、dk_editor_app、dk_ctl | GPU 表面裁剪/累积与介质 GUI 后端保护通过；新序列拒绝用例被默认尺寸预算提前拒绝，缩至 16×16 让相机校验真正执行 |
| 20261019-115017-ee78bb44 | dk_protocol_tests | 两个正交捕获边界用例通过：非法/含糊镜头、后续非法 sample、环境光栅拒绝、Scene/history 与旧文件保护 |

去重并取每项最新结果，共 25 项通过、0 失败、0 跳过；聚合在 out/m18.4-verification.json。
最近三次定向 TestRegex 分别为：

```text
^dk\.(pathtracing\.(camera |volume orthographic|orthographic(_medium)?_gpu_required|(media_)?capture_gpu_required)|editor\.((media_)?pathtracing|interaction)_gpu_validation)
^dk\.(pathtracing\.orthographic_gpu_required|protocol\.capture orthographic|editor\.media_pathtracing_gpu_validation)
^dk\.protocol\.capture orthographic
```

具体证据：

- CPU 明确断言近面起点、平行方向、像素抖动、近/远裁剪、等深度覆盖；介质图像与逐样本独立构造射线完全一致。复查消除了用逆矩阵(3,3)判断投影的问题，near=1 的透视也保留眼点/发散方向。
- GPU Surface 解析覆盖与 CPU 一致；固定与 tile 累积一致，投影/高度重置，同值和拒绝配置保留完成帧，不提交额外几何/AS 工作。
- GPU 空气及空气+云的 4×4×512 spp 对照均 max_abs=0；逐像素差异分别为 0.285611、0.546966，证明并非复用单一眼点；far 只裁剪网格的兼容行为保持。
- Surface GUI 6 个、Scene media GUI 10 个线性比较点全部 max_abs=0；新增正交/高度/透视点分别比较 57024 和 9216 像素。介质正交下实际点击不兼容 Raster 仍留在 Path trace，原交互/拾取/Gizmo smoke 通过。
- 当前 runner 的 commands.list / commands.describe 已由真实进程测试检查 render.capture、render.sequence 的 effect/undoable、请求与结果 schema，并保存 .schema.json。旧请求默认 perspective/fov_y=65/near=.05/far=100；正交来源只含生效高度和 near_plane 约定。
- 基础光栅正交序列、路径 Surface/Scene media、实际 Python/Luau 正交示例通过；取消/关闭/失败输出保护继续通过。
- 定向 check-spec.ps1 与 git diff --check 通过。未执行全量、第二构建配置和 M18.5 光栅环境正交，范围与本阶段一致。

## 偏差与决策

实际注册的序列命令为 render.sequence，沿用原名称，不新增别名。
M17 原约定 near/far 只裁剪主网格，介质继续到真实边界；M18.4 保留该约定，正交仅把积分起点移到逐像素近面。
光栅环境在 owner 校验时明确拒绝，不以后台失败或隐式改投影代替支持。

## 遗留问题与下一步

本阶段无遗留项。M18.5 另行处理光栅大气/云正交；当前仍明确拒绝该组合。

## 修改记录

- 2026-10-19T11:34:00+08:00：创建设计和阶段记录。

- 2026-10-19T11:51:29+08:00：完成实现、定向复验、来源/命令文档与 Roadmap 同步。
