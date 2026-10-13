---
id: "0106"
created_at: "2026-10-13T18:27:52+08:00"
updated_at: "2026-10-13T19:16:15+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-pipeline.md
  - ../design/render-capture.md
  - ../design/editor.md
  - ../design/application-services.md
  - ../design/scripting-luau.md
  - ../design/automation-python.md
---

# 0106 M15.6 云编辑、固定时间序列与可追溯用例

## 目标与设计依据

按[云上层契约](../design/render-clouds.md#m156-上层配置预设与用例清单)完成共享预设、GUI/命令/脚本/持久化一致性，
以及显式 reference/temporal 序列、版本化清单和可核验的线性比较产物。开工前冻结模式、预算、提交点和验收条件；M15.7性能目标不在本阶段。

## 实际变更

- [Environment](../../engine/scene/src/Environment.cpp)提供版本1的clear/sparse/overcast/sunset完整环境预设，seed=1729、generator_version=1。
  [environment.preset](../commands/environment.md)为CPU纯查询，不要求活动Scene、不修改历史。Apply仍使用environment.set，持久化仍为environment v2。
- [云面板](../../engine/editor/src/EnvironmentPanel.cpp)编辑预设草稿、物理云参数、会话模式/固定时间/云影区域。
  Workspace保留guard冲突、Apply/Undo/Redo/Revert语义；会话输入不进入Scene。
  后台预览接受空气或云，单active/单replaceable请求；Temporal每次从空历史在同输入预热32帧，显示实际已发布时间、模式、历史和云影代号。
  失败保留旧图及对应元数据。GUI一致性驱动复用真实按钮、IPC和Gizmo，云夹具把根对象抬高2m并显式Frame选择。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)共用EnvironmentRenderSettings：显式时间、reference/temporal、稳定view/camera身份和固定世界阴影区。
  reference保留256/64/64步、无历史、4096像素上限；temporal复用M15.4。空气开启且非零云时默认绑定云透射阴影。
  模式切换/零云仅在成功帧后清理相应历史，失败保留已发布状态；关闭云模块仍可编译/运行大气链路。
- [CaptureService](../../engine/framework/services/src/CaptureService.cpp)扩展单张capture，新增[render.sequence](../commands/render.md#rendersequence)：
  冻结Scene/相机/时间列表，独立Pipeline，先完成GPU帧再推进历史；可选首样本0–64预热、回退/跳跃/cut显式重置。
  单序列1–128帧、每帧≤65536像素、总计≤262144像素，非零云reference仍≤4096像素；Jobs逻辑输入预约和64MiB输出上限明确。
  单个JSON原子发布，含身份/完整输入/shader/设备、每帧RGBA8/线性RGBA/摘要/历史/云影和wall计时；失败/取消保留旧文件，Jobs摘要≤4096字节。
- [Luau白名单](../../engine/scripting/luau/src/Luau.cpp)允许各能力查询preset，sequence仅project；query/edit拒绝外部输出。
  [Python](../../examples/automation/cloud_sequence.py)与[Luau](../../examples/scripting/cloud-sequence.luau)真实示例输出相同图像和线性样本。
- [render_cases](../../sdk/python/decker/render_cases.py)与[清单示例](../../examples/automation/cloud-cases.json)提供独立schema_version=1格式。
  核验规范化请求（含float32曝光）、ticket/result/序列、设备/历史/像素/摘要；记录输入/runner/产物SHA256、shader来源，提取PPM和little-endian RGBA32F。
  相同物理输入/相机/时间/场景版本才比较，记录线性RGB nRMSE/p99和静态帧差。失败/取消/缺失保留错误，未测耗时/缺失指标为null。
  CLI显式管理隐藏runner；只写新目录、不保存Scene、不支持resume或跨文件事务。
- 同步设计、命令发现/参考、[云使用指南](../guides/clouds.md)、模块/开发索引、Roadmap和测试选择表；无三方依赖、项目格式或Scene持久化schema升级。

## 验证记录

Windows/MSVC Debug；NVIDIA GeForce RTX 4070 Laptop GPU、驱动596.49。
GPU用例要求真实设备、Khronos validation与同步validation；运行期间临时设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并恢复。
最终25个独立CTest名称均通过（20个CPU/CPU进程、5个GPU/窗口/真实脚本链路）；跨配置重跑不重复计数，所有最终选中项无跳过。
Python cases_unit内6项来源/失败/数值检查通过；另实际运行指南清单CLI通过。未运行全量、Release或跨平台验证。

| out/verify运行目录 | 结果与处理 |
| --- | --- |
| 20261013-183745-3a21fb17 | 构建通过，7选中、1通过/6失败；preset结果schema缺整数token标记。改用schema::integer，未放宽schema校验。 |
| 20261013-184329-c9a53222 | 构建失败：测试错误假定DocumentState支持整体相等。改为核对document_id/revision/dirty；未运行测试。 |
| 20261013-184958-22bf24ab | 12/12通过：服务预设/环境guard与历史、编辑器草稿/重载、capture预算/取消/失败/schema及渲染设置。 |
| 20261013-185241-99971448 | worker与capture通过；sequence末尾Luau未加入白名单，cloud GUI末尾Gizmo未选中抬高后的对象。补齐能力限制与明确选择/Frame，先前通过不冒充整例成功。 |
| 20261013-185655-caa0b908 | 5/5通过：Luau能力、sequence预算、case单元、完整GPU序列及云GUI/IPC/脚本/重载。 |
| 20261013-190359-4e59351a | windows-graphics：Clouds OFF、warnings-as-errors ON，pipeline构建和7项CPU通过。 |
| 20261013-190413-56f013ff | windows-scripting：Capture/Clouds OFF，CPU跨进程预设/持久化与Luau隔离2/2通过。 |
| 20261013-190549-f18d1494 | 增强请求/产物来源核验后cases与sequence通过；纯大气GUI读取刚原子重命名的gizmo.json遇短暂PermissionError。测试端增加100秒原截止内的有界读取重试，不吞JSON错误或放宽像素断言。 |
| 20261013-190920-b471acd0 | 3/3通过：大气/云GUI完整链路、6项case单元，含合法float32曝光归一化。 |
| 20261013-191154-280758e5 | 视口元数据移到成功图像发布之后，重新构建并复核大气GUI全链路，1/1通过。 |

各目录保留summary/build/discovery/tests/results；GPU完整成功/失败输出另存details.log。
序列覆盖提交后编辑仍用冻结环境、独立重放逐位一致、reference无历史、temporal固定预热、时间回退/跳跃与相机cut、云影实际绑定、失败/取消旧文件保护。
16²双面几何夹具reference对temporal两帧最坏nRMSE=.001347122、p99=.008331374，低于冻结.05/.12门槛；不以sRGB截图代替线性误差。
清单CLI输出`out/cloud-cases-demo/reports/index.json`：原始render-disk夹具，时间[0,0,30]，最坏nRMSE=.001036093、p99=.006391270；两种模式各一个同相机/时间帧对，帧差nRMS=0。
该有限样本不替代M15.7的120帧动态稳定性或1080p性能验收。CLI输出日志为`out/verify/20261013-cloud-cases-cli.log`。

GUI产物`out/build/windows-editor/test-artifacts/Debug/environment-d77b4eedfcea4bbab5dc0d030593fbd7`的workbench.ppm已转换PNG并查看：
预设/草稿/会话区、发布状态、选择与Gizmo布局正常；它是数值夹具界面证据，不是云艺术效果评审。
最终GUI重复运行产物路径见对应details.log。

复现所用定向范围（从仓库根执行；GPU隐式层环境设置同上）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_run','dk_ctl','dk_editor_app','dk_editor_tests','dk_protocol_tests','dk_service_tests','dk_environment_preview_tests','dk_render_pipeline_tests') -TestRegex '^dk\.(services\.environment|editor\.workspace (environment|cloud preset)|protocol\.capture |render\.pipeline\.environment render|clouds\.cases_unit$)' -Reason 'M15.6 guarded presets, sequence budgets, pure report validation and public rendering settings'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_run','dk_ctl','dk_editor_app','dk_environment_preview_tests') -TestRegex '^dk\.(clouds\.(sequence_gpu_validation|capture_gpu_composition)|editor\.(cloud_consistency|environment_worker)_gpu_validation)$' -Reason 'M15.6 required GPU sequence replay, source evidence, scripts and background GUI cloud/capture parity'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_run','dk_ctl','dk_editor_app','dk_luau_tests','dk_protocol_tests') -TestRegex '^dk\.(clouds\.(sequence_gpu_validation|cases_unit)|editor\.cloud_consistency_gpu_validation|luau\.Luau environment|protocol\.capture sequence)' -Reason 'M15.6 Luau access policy, framed GUI gizmo, sequence replay and report contracts'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_render_pipeline','dk_render_pipeline_tests') -TestRegex '^dk\.render\.pipeline\.' -Reason 'M15.6 shared session settings and optional Clouds OFF with warnings as errors'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_run','dk_luau_tests') -TestRegex '^dk\.(environment\.process$|luau\.Luau environment)' -Reason 'M15.6 CPU-only presets, persistence and script access isolation'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_run','dk_ctl','dk_editor_app') -TestRegex '^dk\.(clouds\.cases_unit$|editor\.(environment|cloud)_consistency_gpu_validation$)' -Reason 'M15.6 bounded Windows atomic-checkpoint retry, cloud and air GUI contracts, float exposure provenance'
```

指南清单CLI命令已实际运行，参数和复制夹具步骤见[使用指南](../guides/clouds.md#版本化用例与对比产物)。
`check-spec.ps1 -Path`检查本次23份Markdown通过，核验819个本地链接及元数据/表格/索引/测试入口/JSON清单。
首次差异检查发现4份设计的尾部空行，清理后`git -c core.safecrlf=false diff --check`通过。

## 限制与下一步

单层云、固定世界阴影区，接收体限云底下/地表上/覆盖区内。GUI是固定输入后台预览，每请求重新预热，不提供连续墙钟动画。
高采样参考限4096像素；序列线性读回有独立总预算。GPU pass耗时没有采集，报告gpu_ms=null；没有把墙钟读回时间称为GPU性能。
输入环境/场景被冻结，但资产文件仍在worker读取，不保证外部并发改写资产时的内容快照。
Python报告不能作为敌对文件系统的安全边界；不提供批次恢复、外部账户或跨驱动逐位一致承诺。
下一项M15.7：完整静态/动态场景集、正式质量/性能/显存测量与大气联合交付。

## 修改记录

- 2026-10-13T18:27:52+08:00：冻结设计并开工。
- 2026-10-13T19:15:26+08:00：完成M15.6实现、25个独立定向测试和清单CLI；保留初次失败与修复证据，交接M15.7。
