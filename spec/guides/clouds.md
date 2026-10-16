# 云预设、固定时间序列与渲染用例

物理云参数属于 Scene environment v2；相机、固定时间、reference/temporal 模式和云影区域属于渲染会话。
保存/重载保留物理参数；复现图像还要提供相同会话输入。命令字段与预算以[环境命令](../commands/environment.md)、[渲染命令](../commands/render.md)为准。

## 编辑器

使用启用 `DK_BUILD_RENDER_CLOUDS` 的 windows-editor 配置。Environment 面板提供 Clear、Sparse、Overcast、Sunset 四个预设；
按钮只替换草稿，Apply 提交一次可撤销的 environment.set。外部命令修改场景后，旧草稿的 Apply 会报告 guard 冲突，Revert 读取最新值。
物理云区域可编辑 seed、分辨率、覆盖率/密度、消光、高度/衰减、尺度与水平风速；generator_version 固定为 1。

会话区域选择 Reference 或 Temporal，填写固定秒数，或点击 +1 second。
时间不会随墙钟推进。Temporal 每次后台请求从空历史开始，先显示首个完整帧，再每4次更新显示一次，静止时细化至32次；
用于可复现编辑预览，时间不会自动播放。Reference 非零云最多4096像素，较大视口禁用该选项并使用Temporal。
视口显示已发布 revision、实际时间、模式、预热次数、历史重置和云影代号；新请求完成前保留旧图并显示 pending。

云影默认开启，固定世界中心为原点、覆盖宽度 8192m、分辨率 256²。
相机运动不会移动阴影区域；可见接收体必须位于覆盖区、云底以下。完整大气合成允许包围盒跨越地面，并按行星交点遮挡地下片段；独立日光入口仍拒绝地下接收体。
可修改阴影中心/范围或明确关闭云影。空气关闭时云仍可显示，但没有地表太阳直射云影。
默认预设原点位于地面；相机进入地下会给出明确错误，保留旧图，仍可移动相机或在视口按Home恢复。

## 命令和脚本

`environment.preset {"name":"sparse"}` 返回 `{version:1,environment:完整v2对象}`，无需 Scene、无副作用。
四个预设都固定 seed=1729、generator_version=1；clear 关闭云，其余启用云。将返回值交给带 guard 的 environment.set。
预设替换完整环境，包括空气与太阳，不是只修改覆盖率。

单张 `render.capture` 保留 PPM 输出，新增 time_seconds、render_options、warmup_frames。
`render.sequence` 冻结场景和 samples，创建独立 Pipeline，从空历史顺序执行全部相机/时间输入。
Reference 无历史、warmup_frames 必须为 0；Temporal 可预热首个固定样本 0–64 次。
相机 cut 明确使旧历史失效；时间回退或大跳跃沿管线规则重置。
两者都是 external 作业，不进入 Undo；提交后查询 jobs.get/wait，可用 jobs.cancel 请求取消。
失败或取消不会覆盖已有目标文件。

[Python IPC 示例](../../examples/automation/cloud_sequence.py)连接已加载工程的 runner/editor，应用预设并生成 cloud-python.json。
[Luau 示例](../../examples/scripting/cloud-sequence.luau)加载工程后生成同输入 cloud-luau.json；需要 project 能力。
两个示例均使用明确时间与相机，输出 RGBA8 和曝光前线性 RGBA。它们会修改当前环境；需要保留时显式 scene.save。

## 版本化用例与对比产物

Python 3.11+ 标准库工具 `decker.render_cases` 使用独立于布料实验计划的 schema_version=1 清单。
[可执行清单](../../examples/automation/cloud-cases.json)包含相同物理输入的 reference 与 temporal 用例，
固定 seed、16×16 分辨率、32 帧时域预热和时间 `[0,0,30]`。它是链路与来源核验样例，不是完整质量/性能基准。
示例把行星中心下移 500m，保证 render-disk 夹具不在地下；输入中显式保存完整环境、曝光、相机、时间和云影区域。

从仓库根运行，输出目录必须尚不存在：

```powershell
cmake --build out/build/windows-editor --config Debug --target dk_run dk_ctl
Copy-Item tests/fixtures/render-disk out/cloud-cases-demo -Recurse
$env:PYTHONPATH = "$PWD/sdk/python"
python -m decker.render_cases --manifest examples/automation/cloud-cases.json `
  --project-root out/cloud-cases-demo `
  --runner out/build/windows-editor/bin/Debug/dk-run.exe `
  --ctl out/build/windows-editor/bin/Debug/dk-ctl.exe --output reports
```

CLI 启动自己的隐藏 runner，加载清单的 project，串行修改内存环境并提交序列，完成后关闭 runner；不保存 Scene。
Windows GPU 验证要求 Khronos validation，可按[构建指南](build.md)处理本机隐式层问题。
每例必填唯一 id、完整环境或已知 preset、width/height、exposure、完整 render_options、warmup_frames 和 samples。
samples 每项显式提供 camera、time_seconds、camera_cut。相机 up/fov_y/near/far 可采用命令默认值，报告会记录完整值。
清单最多 32 例、64 个比较；每序列 1–128 样本，单帧最多 65536 像素、总计最多 262144 像素，非零云参考单帧仍最多 4096 像素。

产物位于 `out/cloud-cases-demo/reports`：

- `<id>.json`：引擎原子发布的完整序列，含设备/驱动、场景身份、冻结参数/seed、shader 指纹、每帧历史与云影代号、图像、线性数据和摘要。
- `<id>-NNN.ppm`、`<id>-NNN.rgba32f`：核验后提取的 RGB 图像与按行、每像素 RGBA、little-endian float32 的线性文件。
- `index.json`：完整清单、规范化清单/输入 SHA256、runner SHA256、每个文件 SHA256、提交/完成身份、静态帧差和比较指标。

工具按引擎 float32 归一化 exposure，核对请求与规范化环境、ticket/result、序列身份及完整相机/时间/像素/摘要；runner 或 shader 来源不一致、缺文件和非有限值均失败。
只有相同 scene/revision、物理环境、曝光、相机/时间、尺寸及阴影区域允许比较；质量模式和预热次数允许不同。
线性 RGB 的误差以参考图 luminance p95（至少 .001）归一化，门槛 nRMSE≤.05、p99≤.12。
静态帧差只统计连续相同相机/时间且无 cut 的样本；没有有效样本时为 null，不能用零代替未测量。

缺失/失败/取消项保留具体错误，不生成替代图像，未测耗时为 null；依赖失败项的比较为 failed、metrics=null。
序列 JSON 单文件原子发布；Python 提取文件与 index 不承诺跨文件事务，输出目录不支持覆盖或 resume。
wall_ms 是注明范围的墙钟耗时（帧渲染/等待/读回，序列含预热，用例还含命令与产物处理）；gpu_ms 固定 null，尚未测量 GPU pass 时间。
完整动态质量、1080p 性能与显存使用独立[复现流程](cloud-benchmark.md)，实测见[联合交付报告](../benchmarks/2026-10-13-clouds.md)，不能从此小夹具推断。

## 定向验证

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor `
  -Target @('dk_run','dk_ctl','dk_editor_app') `
  -TestRegex '^dk\.(clouds\.(cases_unit|sequence_gpu_validation)$|editor\.cloud_consistency_gpu_validation$)' `
  -Reason '云序列报告、脚本与后台GUI一致性'
```

GUI 测试以 `--background` 启动，比较预设/Apply、IPC、Undo/Redo、保存重载、截图与脚本结果。
验收条件和实际证据见[云设计](../design/render-clouds.md)、[0106](../development/0106-cloud-integration.md)。
