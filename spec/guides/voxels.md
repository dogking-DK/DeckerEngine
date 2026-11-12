---
module: guide-voxels
created_at: "2026-11-12T09:48:00+08:00"
updated_at: "2026-11-12T18:22:13+08:00"
status: accepted
---

# 实体体素双后端工作流

M23.6/12 使用 OpenVDB/NanoVDB 实体材质索引，不是密度云或 SDF。文件格式、有限域、palette 与单位变换见
[体素设计](../design/render-voxels.md#m235-资产实施契约)。需要 `windows-voxel-editor` 预设；无 CUDA 依赖。

## 加载、编辑与刷新

工程的 voxel AssetRecord 指向 `.dkvoxel` 或编译产物 `manifest.json`，Scene 的 `dk.AssetReferences` 使用同一 ID 与 `kind=voxel`。
已有 [assets.import / assets.register / assets.load](../commands/assets.md) 支持该种类；
首次创建 meta 时显式提供 `outputs=[{key:"voxel/0",kind:"voxel"}]`，具体 schema 以 `commands.describe` 为准。
可通过 [entity.set_assets](../commands/entity.md) 绑定或替换资产，通过原有 Inspector 编辑整实体 TRS。
打开工程后 Editor 自动装配 mesh 与 voxel；视口点击按最近表面选择，Frame 使用整个有限域，Gizmo 提交沿用 Scene Undo/Redo。
文件内容在外部修改后点 Refresh；引用或路径改变会自动创建候选。错误保留旧画面并标记非 current，不允许用错误画面继续拾取。
不自动监视文件，不提供逐体素编辑。

## 显示设置

Viewport 的 Voxel settings 弹层支持 Unlit/Directional、单位世界光方向及线性 RGB irradiance。
编辑完各分量后点击 Apply 整体校验，Cancel 丢弃草稿；弹层打开后其他入口更新视口会使旧草稿 Apply 返回 conflict，重新打开即可读取新值。
GUI、Console、IPC 使用同一 [editor.viewport.set](../commands/editor-viewport.md) 服务：

```text
editor.viewport.query
editor.viewport.set guard={"view_id":"<查询值>","document_id":"<查询值>","revision":0} voxel_shading=directional voxel_light_direction=[0,0,-1] voxel_irradiance=[2,2,2] raster_exposure=1
```

使用实际返回的 guard。显示设置不改变 Scene dirty/revision/history；设置接受后还需等待 `presentation.current=true`。
混合场景方向光要求 mesh 材质与 voxel palette 均非金属；光栅大气/云组合暂不支持，需主动启用 Unlit inspection，Scene 的环境数据仍保留。
具备 RT 能力时可在同一视口切到 Path tracing，拾取、Frame、Gizmo 和 Undo 继续使用同一资产。
路径支持 Surface only / Scene media、Classic MIS / ReSTIR DI 及初始/时间/空间/联合复用；影响渲染内容的设置、相机、Scene、资产刷新改变后从空历史重算。
Console/IPC 示例（guard 取最新 query，`classic` 是视口枚举）：

```text
editor.viewport.set guard={"view_id":"<查询值>","document_id":"<查询值>","revision":0} renderer=pathtrace target_spp=16 medium_mode=surface_only direct_lighting=restir_di temporal_reuse=true spatial_reuse=true
```

先检查 `editor.viewport.query.capabilities`；设备不支持 RT 时路径请求拒绝，光栅仍可用。
设置接受只说明会话配置已提交；须等待 `presentation.current=true` 和目标 spp 完成再比较画面。

## 独立截图

`render.capture` 明确接收相机、尺寸、曝光与相同体素参数；不会读取 Editor 当前相机、显示参数或未提交 Gizmo。
含体素的光栅和原生路径任务在接受前冻结最多 64 MiB 的全部 CPU 资产（包括 mesh）；`provenance.asset_snapshot=accepted`，后续源文件替换不影响该任务。
任务 succeeded 表示 GPU 完成、读回和原子文件替换均完成。详见[截图命令](../commands/render.md#实体体素光栅截图)。

已打开/加载工程的 Editor 或 dk-run（例如 `--pipe voxel-demo`）可运行：

```powershell
$env:PYTHONPATH = "$PWD/sdk/python"
python examples/render/voxel_capture.py --endpoint voxel-demo --client out/build/windows-voxel-editor/bin/Debug/dk-ctl.exe --output voxel.ppm --directional
```

示例使用 eye=(0,0,-2)、target=(0,0,0)；实际工程按物体位置传 `--eye x y z --target x y z`。
输出父目录必须存在，覆盖同名输出；示例只读取已加载 Scene，不修改工程。
原生路径示例（JSON 为线性 RGB，PPM 为 tone 后图像）：

```powershell
python examples/render/voxel_capture.py --endpoint voxel-demo --client out/build/windows-voxel-editor/bin/Debug/dk-ctl.exe --pathtrace --restir joint --orthographic --spp 16 --output voxel-path.json
```

加 `--media` 使用已有 Scene 空气/云，时间固定为 0；相机近面须处于地面以上的支持域。
截图独立从空历史开始，不复用 Editor 的历史或相机；对照时须显式匹配尺寸、相机、spp、seed、光照和介质参数。
provenance 保留 pathtrace 版本质量标识，报告接受时资产快照、输入/几何/环境/场/shader 指纹与实际采样；结果含 `voxel_count`。

## 光栅质量与性能基线

M23.7 基准目标 `dk_voxel_benchmark` 使用生产转换器和渲染路径；固定程序输入不计磁盘 VDB 解码、资产缓存或 Editor GUI 开销。
覆盖 64³/128³/256³ 的空/稀疏/实心输入、64³ 交错 leaf、大跨度多岛和 root 常量 tile；两种分辨率与投影。
Windows 进程峰值是额外观测，不冒充 OpenVDB 内部 scratch 精确上界；详细口径见[设计](../design/render-voxels.md#m237-光栅基线实施契约)。

```powershell
cmake --preset windows-voxels-raster
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxels-raster -Target dk_voxel_benchmark -TestRegex '^dk\.voxels\.benchmark_(gpu_required|aggregation)$' -Reason 'M23.7 required validation and evidence checks'
./scripts/verify.ps1 -BuildDir out/build/windows-voxels-raster -Configuration Release -Target dk_voxel_benchmark -TestRegex '^dk\.voxels\.benchmark_aggregation$' -Reason 'M23.7 optimized performance executable'
python scripts/benchmark-voxels.py --exe out/build/windows-voxels-raster/bin/Release/dk-voxel-benchmark.exe --output out/benchmarks/voxels
```

正确性检查要求真实 GPU、Khronos/sync validation；性能脚本使用 Release、关闭 validation、三轮串行，先固定 64 帧设备预热，再记录每个视图首帧/2 预热/8 采样帧。
性能采集期间不要并发运行其他 GPU 测试；结果写入 `round-0/1/2.json`、`summary.json` 和 `summary.md`，失败不会被汇总成通过。
已有原始数据可用 `--aggregate-only <round-0.json> <round-1.json> <round-2.json> --output <目录>` 独立复算，也支持 `.json.gz`；`--plot` 可选生成 SVG/PNG（需 matplotlib）。
已归档的设备条件、完整表格和能力边界见 [2026-11-12 基线报告](../benchmarks/2026-11-12-voxel-raster.md)。

## 联合质量、资源与性能基线

M23.13 的 `dk_voxel_joint_benchmark` 复用上述光栅夹具，并装配同资产的原生路径。
默认 RT 保留 8³ 叶块与非零常量 tile；显式 32/128 保守合并用于尺度对照，大 tile 不展开。
两后端对同一 DenseGrid/解析 oracle 比较几何；光栅 Unlit 与四反弹路径的颜色和工作量不同，不把两列耗时比作同画质加速。

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target @('dk_voxel_joint_benchmark','dk_voxel_sparse_tests','dk_voxel_path_tests') -TestRegex '^dk\.voxels\.(joint_gpu_required|joint_aggregation|sparse\.Pooled|path_cpu\.Pooled)' -Reason 'M23.13 joint quality and evidence validation'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target dk_voxel_joint_benchmark -TestRegex '^dk\.voxels\.joint_aggregation$' -Reason 'M23.13 optimized benchmark build'
python scripts/benchmark-voxel-joint.py --exe out/build/windows-voxel-editor/bin/Release/dk-voxel-joint-benchmark.exe --output out/benchmarks/voxel-joint --quality out/benchmarks/voxel-joint/quality.json --run-quality --plot
```

最后一条先使用同一源码/程序运行 required validation 质量门禁，然后串行执行三轮关闭 validation 的完整 Release 矩阵。
保存全部样本、源码与程序 SHA-256、设备和 shader 指纹、原始统计均值/二阶矩及 95% 区间；缺轮、缺视图、来源混用、质量或资源回收失败均拒绝聚合。
正式测量期间不要改源码/程序或并发其他引擎验证。GPU 时间戳、CPU 提交、读回、AS 构建与转换分开记录；内存逻辑载荷、AS 要求、VMA 和采样峰值各有边界。
131072 格双岛仍记录原生路径精度域拒绝，完整联合矩阵采用 16384 格双岛；大跨度光栅证据继续见 M23.7，不能据此宣称任意跨度 RT 支持。

`--plot` 需要 matplotlib，采集/聚合本身只用 Python 标准库；离线复算与最终边界见[联合交付报告](../benchmarks/2026-11-12-voxel-joint.md)。required 质量矩阵包含 9 输入，以及 sparse64/128/256、solid256 的两分辨率 11 spp 斜向边界回归。
