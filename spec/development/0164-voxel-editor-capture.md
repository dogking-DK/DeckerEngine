---
id: "0164"
created_at: "2026-11-12T09:31:24+08:00"
updated_at: "2026-11-12T10:31:11+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/editor.md
  - ../design/application-services.md
  - ../design/render-disk.md
  - ../design/render-capture.md
  - ../design/render-data.md
  - ../design/render-pipeline.md
---

# 0164 M23.6 体素 Editor 与独立截图闭环

## 目标与设计依据

实现 [M23.6](../roadmap.md#m23实体体素渲染) 和[使用闭环契约](../design/render-voxels.md#m236-使用闭环实施契约)。
复用现有资产/Scene 操作与 viewport/capture 入口，保持会话设置和持久编辑边界。

## 实际变更

- DiskScene 只读加载并去重 voxel 源/产物，校验 ID/kind，按 Nano/palette 计量；独立 mesh/voxel cache 上传。
- Editor 共享显示设置、GUI 弹层和参数帮助；完成画面的 mesh/voxel 最近命中、有限域 Frame、实体 Gizmo/Undo；按引用/路径或 Refresh 替换候选。
- 独立 capture 接受前冻结含体素的有界 CPU 资产；显式显示参数、完整 voxel shader 指纹与来源；不读 viewport、不修改 Scene。
- 后台 GUI/Console/IPC 与像素对照验收、CPU 契约测试、可执行 Python 示例及无 VDB 构建回归。

## 验证记录

环境：Windows x64、VS 2026/MSVC 19.51、Debug、`/W4 /WX`；RTX 4070 Laptop、驱动 596.49。
使用 `windows-voxel-editor`，VDB/render_voxels 开启、无 CUDA；另用 `windows-graphics` 验证关闭 VDB 的构建。
GPU 使用 required Khronos validation 与 synchronization validation；设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1` 排除无关 AMD implicit layer，未关闭验证层。

| 检查 | 实际结果与本地证据（`out/verify/`） |
| --- | --- |
| DiskScene、capture 协议、viewport 状态 | 23 CPU 通过；`20261112-095621-de66eff3`（该轮 GPU 失败，后续复验覆盖） |
| 相机/渲染策略 | 10 CPU 通过；`20261112-100400-ae026965`（该轮工作流驱动失败，后续复验覆盖） |
| 设置帮助/目录 | 6 CPU 通过；`20261112-101526-3b31d719` |
| M23.4 合成与寿命 | 2 GPU 通过：`dk.voxels.raster_composition_required`、`dk.voxels.raster_lifetime_required`；同上 |
| M23.6 与旧 mesh 宿主回归 | 6 GPU 全通过、无跳过；`20261112-101933-79ff620c`，包含体素工作流、runtime.capture、Editor workbench/interaction/consistency/settings_help |
| 中文项目路径最终复验 | 体素完整工作流再次通过；`20261112-102347-1b925aee`，项目目录为 `project 稀疏` |
| VDB/render_voxels OFF | 编译通过、21 CPU 全通过；`20261112-095546-41e0de2d` |

按测试名去重并取最后结果，39 CPU + 8 GPU 共 47 项通过，无失败/跳过；不是一次全量测试。
关闭 VDB 的 21 项为额外配置执行，不重复计入 47 项；汇总索引 `out/agent-notes/m236-acceptance-index.json`。

真实工作流 14 个检查点覆盖后台 Editor 的加载、GUI 草稿/Apply、Console/IPC、拾取、Gizmo/Undo/Redo、Frame、透视/正交、失败保留及恢复。
独立 capture 与完成视口的 PPM 字节逐项相等，覆盖 Unlit/方向光/曝光、两种投影、mesh/voxel 双向遮挡。
接受后破坏源文件仍能完成冻结截图；下一请求拒绝且保留旧输出。独立 dk-run、Python 示例亦完成实际 GPU 截图。
测试检查 Scene/会话/历史边界、来源/帧号/尺寸、path/sequence/environment 明确拒绝及显式 inspection 不修改 Scene。
最终后台进程 `validation errors=0 warnings=0 liveAllocations=0 knownLoaderWarnings=0`。
最新明细、PPM 和进程日志保存在 `out/build/windows-voxel-editor/test-artifacts/Debug/voxel-workflow-3acf01a226f449b1bd8cc1d3dc5e4b8b/`。

可重复入口（完整参数见各轮 `summary.json`，构建指南提供组合入口）：

```powershell
$env:VCPKG_ROOT = 'C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_editor_app,dk_run,dk_ctl,dk_voxel_asset_process_probe -TestRegex '^dk\.voxels\.editor_capture_gpu_required$' -Reason 'M23.6 Editor and independent capture acceptance' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_render_disk_tests,dk_editor_tests,dk_protocol_tests,dk_render_pipeline_tests -TestRegex '^dk\.(editor\.viewport |protocol\.capture |render\.disk\.|render\.pipeline\.)' -Reason 'M23.6 CPU contracts' -Parallel 4
```

`scripts/check-spec.ps1 -Path <本阶段文档列表>` 通过：18 个 Markdown、1137 条本地链接及元数据/表格/索引/测试入口/JSON manifests。
Python 示例和工作流两文件的 AST 语法检查、`git diff --check` 均通过。

## 偏差与决策

不新增自动热重载；显式 Retry 或引用/路径变化触发候选替换。
体素 capture 接受前冻结有界 CPU 输入，避免后台任务读取后来的资产版本。
真实 Editor 首帧暴露 M23.4 复用大气远近裁剪比 10000 的限制；体素新增独立 1e6 比例上限支持默认镜头，大气限制不变。
正交镜头 180° 旋转产生 sin 舍入残差，触发共享 HDDA 极小方向拒绝；只在光栅射线生产端归零相对主分量不超过 32×double epsilon 的分量，保留 HDDA 的严格检查。
资产重载键使用显式 UTF-8 路径，中文目录真实运行通过。

早期失败包括错误 target 名、CaptureRequest 聚合初始化兼容、测试头文件/Catch 括号/角度类型，以及 GUI 驱动焦点/事件节流、Python SDK timeout 上限；均已修复并复验。
旧 settings-help GPU 初轮只有无关 AMD loader warning，最终上述环境复跑通过；不将失败轮次或旧二进制作为完成证据。

## 遗留问题与下一步

M23.6 完成。CPU 输入 64 MiB 约束针对保留的 mesh/NanoVDB/palette 数据，不代表 OpenVDB 解码/转换的内部暂存或进程 RSS 上界。
接受前 CPU 准备仍同步进行，较大导入可能阻塞宿主；没有自动文件监视或逐体素编辑。
本阶段未运行 Release、全量/跨平台矩阵或性能基线；M23.7 继续质量/内存/转换/上传/帧耗时基线。
原生路径 M23.8/9、体素 sequence、大气/云组合和金属方向光不在本阶段交付范围。

## 修改记录

- 2026-11-12T09:31:24+08:00：创建记录与实施契约。
- 2026-11-12T10:31:00+08:00：完成跨入口/冻结截图/中文路径验收、旧 mesh 与无 VDB 回归，关闭 M23.6。
