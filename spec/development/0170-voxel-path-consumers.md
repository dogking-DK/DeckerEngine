---
id: "0170"
created_at: "2026-11-12T16:19:50+08:00"
updated_at: "2026-11-12T16:55:54+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/editor.md
  - ../design/render-capture.md
  - ../design/application-services.md
---

# 0170 M23.12 原生体素路径消费者

## 目标与状态契约

实施 [M23.12](../roadmap.md#m23实体体素渲染) 和[消费者契约](../design/render-voxels.md#m2312-原生路径消费者实施契约)。复用既有服务/命令，接通同资产双后端、冻结独立 capture 与匹配完成帧的拾取/Frame；保持 guard、任务发布、来源和失败保护。联合性能交付留给 M23.13。

## 验证计划

后台真实 GPU Editor GUI/Console/IPC 与独立 runner，固定 spp/双投影/四 DI/介质及线性对照；接受后源破坏、编辑/重载失效、取消/迟到/关闭；禁用 RT 能力拒绝与光栅可用。选择消费者直接回归，不重跑无关积分器大样本长测。

## 实施

- `PathPreview/PathViewport` 随匹配完成帧发布 voxel CPU 查询 owner、实例数、mesh 查询、Scene 和相机，复用 nearest/Frame 与 Gizmo/Undo。请求在 owner 侧携带启用代号；恢复路径时清空累积/复用，即使暂停与恢复被 worker 合并也有效；同周期目标 spp 调整仍沿用原调度语义。
- `CaptureService/PathCapture` 开放原生体素路径 v1/v2/v3，worker 消费接受时冻结的全部 CPU 资产；保留路径 quality，结果增加 voxel_count，shader 指纹覆盖原生 voxel/HDDA/adapter/PNanoVDB 闭包。Jobs 输入额度、owner 原子发布、取消和关闭契约不变。
- 增加一次性工程标记保护的后台 `VoxelPathSmoke` 驱动及 Python 编排，真实点击 GUI 控件、Console 输入、IPC 写入与独立 runner 捕获。设备 RT 禁用用例独立执行。测试请求文件的 Windows 原子替换竞争采用有界写重试、未打开读下帧重试。
- 同步消费者设计、命令参考、体素指南和测试选择表；`voxel_capture.py` 增加路径、介质、四 DI、正交与固定 spp。旧光栅用例按宿主实际路径能力验证新支持。没有新增依赖、CUDA 运行时或命令种类。

## 验证结果

设备为 NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49；使用 Debug，后台窗口、required validation 和同步验证。结果按实际测试项计数，不将失败整轮记为通过。

| 范围 | 入口与结果 | 证据 |
| --- | --- | --- |
| M23.12 完整消费者 | `dk.voxels.path_consumers_gpu_required` 通过；29 检查点、16 次直接捕获及 1 次示例捕获，126.88 s | `out/verify/20261112-164935-daa4b873`；`out/build/windows-voxel-editor/test-artifacts/Debug/voxel-path-2fa7f9ee25ce4418aee89a75a68f541e/summary.json` |
| RT 不可用 | `dk.voxels.path_no_rt_gpu_required` 通过，8.71 s；显式关闭设备 RT，请求拒绝且 guard 不变，光栅截图仍成功 | `out/verify/20261112-164301-b86e90e9/results.xml` 的该项（该轮消费者因测试参数错误失败，已在上行复验） |
| M23.6 旧光栅闭环 | `dk.voxels.editor_capture_gpu_required` 通过，41.04 s；包含既有 Python 示例及开放后的 v1 路径 | `out/verify/20261112-164935-daa4b873` |
| 无 VDB 旧 mesh | Editor 与 dk-run/dk-ctl 编译通过；`dk.pathtracing.capture_gpu_required`（41.35 s）、`dk.editor.pathtracing_gpu_validation`（58.01 s）均通过 | `out/verify/20261112-165258-766e833c` |
| 文档与语法 | Python AST 通过；最终 check-spec 通过 14 个 Markdown、1098 个本地链接、元数据/表格/索引/测试入口与 JSON manifests | 版本化检查脚本 |

完整消费者观察：

- 同资产 GUI 双后端、Console 联合复用、IPC 初始/时间/空间设置与透视/正交；29 个已发布/失败/在途检查点。体素与 mesh 最近拾取、有限域 Frame、Gizmo/Undo 及 Scene/session guard 分离均验证。
- Surface only / Scene media 的 Classic 和四 DI，对显式相机/尺寸/seed/spp 的独立捕获比较线性 RGB，预设误差上限 `2e-5 * max(1, reference_max)`。独立 runner 与恢复帧的线性结果完全相同；版本 1/2/3、命令描述及可执行示例实际运行。
- 接受后同时破坏 voxel/mesh 源仍精确重现冻结输入；坏源新请求拒绝，坏源重载保留旧画面并非 current。恢复、palette/grid 替换、时间和实例变换清空历史并更新来源。
- 高目标在途切换光栅后无迟到覆盖，切回相同路径严格完成 3/3 spp、epoch 0，原因 `Path view resumed`；带在途工作关闭 Editor 成功。独立 runner 取消和关闭活动 capture 都保留原输出。
- Editor 输出 `validation errors=0 warnings=0 liveAllocations=0 knownLoaderWarnings=0`；路径 capture 在队列关闭后强制零验证错误/警告，否则不发布文件。

主要复现命令：

```powershell
$env:VCPKG_ROOT = 'C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target @('dk_editor_app','dk_run','dk_ctl','dk_voxel_asset_process_probe') -TestRegex '^dk\.voxels\.(path_consumers|path_no_rt|editor_capture)_gpu_required$' -Reason 'M23.12 voxel consumers and direct regression' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.(editor\.pathtracing_gpu_validation|pathtracing\.capture_gpu_required)$' -Reason 'M23.12 no-VDB mesh regression' -Parallel 4
```

上面的体素命令将三项合并便于复现；本次实际分轮执行，证据以表格为准。

## 失败、修复与边界

首次编译的 history 计数字段误作函数已修正；第一轮 GUI/Console 之后遇到请求文件原子替换读失败，已修正。第二轮 `out/verify/20261112-163632-749169da` 暴露真实恢复缺陷：旧累积 12 spp 被标为新目标 3 spp 的完成帧。启用代号修复后回归证明从空历史重新达到 3 spp。
后续 `out/verify/20261112-164301-b86e90e9` 已通过整个 Editor 与独立线性对照，但旧版本测试给出 max_bounces=2、默认 rr_start=3 被正确拒绝；测试和新示例显式设置 rr_start=2 后完整验收通过。没有放宽错误处理、像素阈值或固定采样判据。

尚未运行全量/Release、物理非 RT 显卡或其他厂商设备；禁用 RT 是同一实机的功能不可用路径验证。M23.13 的联合质量/资源/性能基线仍未实施，M23 保持开放。voxel sequence、光栅空气/云组合与 Scene media 表面降噪信号边界保持；同步冻结最多 64 MiB 的准备可能阻塞 owner，不承诺外部文件自动热加载。
