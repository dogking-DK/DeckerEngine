---
id: "0181"
created_at: "2026-11-17T13:56:00+08:00"
updated_at: "2026-11-17T15:07:05+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/editor.md
---

# 0181 Scene media 运动预览颜色稳定性

## 目标与设计依据

移动相机时192像素长边预览被放大，原hero RGB独立通道与1 spp立即发布造成大块纯色闪烁。依据[路径设计](../design/render-pathtracing.md#scene-media-低样本颜色稳定性0181)改进低样本颜色覆盖，并保持原始线性图像、采样来源及完整批次发布。

## 实际变更

- `VolumePathReference`/`path-air.slang`用独立整数散列产生每像素循环排列，默认sampler=3；显式2保持原序列。共享参数携带真实版本，固定/渐进/Classic/ReSTIR和体素共用。
- `PathPreview`介质batch=3，完整批次才发布；`PathViewport`相机变更等待已提交的小预览后合并，避免100ms反复取消。暂停、失败、物理/尺寸变更立即接纳，GUI不等待GPU。
- capture来源、Luau示例、当前基准生成器/聚合校验及用户文档同步为3。场景、Undo、公开命令参数和依赖不变。
- 新增CPU实际颜色/旧序列/分区测试，GPU不同版本与任意起点低spp对照、版本切换历史清空，worker完整周期/尾批/低目标；后台GUI改为240像素，验证192运动预览发布和停动恢复，并与独立capture比较。

## 验证记录

- 20261117-140039-dff1d524：首次沙箱内MSBuild仅输出启动标识后停滞，手动中止；未执行测试，不作通过证据。
- [20261117-140405-36da1a8c](../../out/verify/20261117-140405-36da1a8c/summary.json)：windows-voxel-editor / Release，沙箱外定向构建CPU参考、GPU probe、Editor/worker/benchmark、runner/ctl；8项CPU通过，0失败/跳过。覆盖RGB三样本实际颜色、v2冻结值、彩色吸收、ReSTIR条件期望、分区和非法参数。
- [20261117-140718-57bad5e6](../../out/verify/20261117-140718-57bad5e6/summary.json)：19项定向检查首次18通过、1失败、0跳过。通过体素介质调度、ReSTIR数值/云/复用/调度/状态、正交介质、云渐进、空气统计/渐进/失败、独立media/restir capture、worker、两种后台GUI及两项报告聚合。
- 首次失败为新air_numeric测试的旧v2输出1.60000014与双精度1.6相差约1.4e-7，超过1e-7门槛；将该通道覆盖用例的辐亮度改为可精确表示的0.25/0.5/1，保留门槛，隔离十进制输入转float舍入。未修改积分器或物理统计门槛。
- [20261117-142332-bf4257c7](../../out/verify/20261117-142332-bf4257c7/summary.json)：仅重编dk_pathtracing_probe并复跑air_numeric，1/1通过。合并有效证据，27个唯一检查最终均通过；其余已通过的实现未变，不重复全套。
- 以上均由`scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target ... -TestRegex ...`执行，完整Target/Regex记录于各summary。GPU为RTX 4070 Laptop，required Vulkan/Khronos及同步validation开启；仅对子进程禁用既有AMD switchable隐式层。构建含原有局部变量遮蔽/float窄化警告，未作为本次无警告构建证据。
- check-spec定向11份文档通过，7份修改后的Python脚本语法解析通过，git diff --check通过。

后台GUI运动观察（2秒输入，小型render-disk夹具，192×192预览、240×240恢复，validation开启）：

| 模式 | 完整3 spp预览 | 请求至交付p50/p95 | GUI/capture对照 |
| --- | --- | --- | --- |
| Classic MIS | 110张 | 18.09 / 43.56 ms | 11组线性像素max_abs=0，Console同为0 |
| ReSTIR DI | 85张 | 24.16 / 25.17 ms | 11组线性像素max_abs=0，Console同为0 |

原始观察见[Classic](../../out/build/windows-voxel-editor/test-artifacts/Release/media-pathtrace-cd60c4a3555e483c85b0f3807dea4a14/report.json)与[ReSTIR](../../out/build/windows-voxel-editor/test-artifacts/Release/restir-pathtrace-2f6c7e0905d4410f8916a68df70b9bd9/report.json)；它们证明持续运动可交付完整RGB预览，不代表Sponza生产帧率。
未执行全引擎、Debug、其他GPU及Sponza长链质量/性能矩阵；当前配置未启用Luau，两个capture测试明确传入skip-luau，示例断言已同步但Lua运行不计入通过证据。

## 偏差与决策

任意连续三样本都覆盖RGB，兼容ReSTIR继承非零epoch起点；不是每组三样本重新随机排列，避免跨组起点失衡。单样本边缘分布均匀、跨样本有意相关；误差用独立seed评估。目标1/2保留原样本数语义，仍可能有彩色噪声。

基准的当前采集/聚合冻结到sampler=3，旧sampler=2历史归档应使用其归档脚本复算，不将新旧采样混为同工作量下的逐像素等价或自动沿用旧性能结论。本次不重跑Sponza长链质量/性能矩阵。

## 遗留问题与下一步

本次修复范围完成，无阻塞项。首个可显示批次工作量由1增加至3条路径/像素；此改动不消除光照/散射蒙特卡洛噪声，不增加降噪器。后续大场景实时收敛/重建需单独设计和测量。

## Sponza补充验证（2026-11-17）

用户要求实际场景的移动效果与延迟。增加仅验收用的旧策略复现和真实SDL窗口运动记录，固定机位另采32张颜色对照图；输出保留线性像素、来源、资产/源码/二进制摘要和逐帧时间。`--media-motion-probe`要求一次性工程标记与配置；旧策略在首次path请求前冻结，默认关闭，不修改正常GUI设置或物理积分器。

- [20261117-144742-4cacf776](../../out/verify/20261117-144742-4cacf776/summary.json)：Release构建dk_editor_app、dk_editor_render_benchmark，新Sponza真实窗口required validation 1/1通过、0失败/跳过。
- `scripts/benchmark-media-preview.py --mode all --seconds 6 --rounds 3`：31个GPU进程串行通过，含1个独立validation、24个性能（无云/稀疏云×Classic/ReSTIR×新旧×3轮）、4个真实运动取图、2个固定机位颜色进程。44张线性图像均为有限非负值。正式计时关闭validation和读回，实际全尺寸1629×989、运动192×116。
- [20261117-150028-1b3bb6c1](../../out/verify/20261117-150028-1b3bb6c1/summary.json)：Release构建Editor/benchmark/ctl，既有ReSTIR worker和media GUI/capture 2/2通过、0失败/跳过；未重跑本阶段全部GPU/CPU测试。GUI/capture配置未启用Luau，仍使用既有skip-luau路径。
- [实测报告](../benchmarks/2026-11-17-media-preview.md)：输入交付p50约30–31ms，与旧策略基本相同。稀疏云/ReSTIR更新率78.13→70.50次/秒，停动全尺寸首批317.39→428.53ms，下一批637.07ms；没有把GUI约165Hz当作完整预览帧率。
- 天空开口RGB花斑明显减少；等3 spp对照共同有效区域色度误差降低约13%，但该区域仅占整图约13%。地面与暗部仍有大量彩噪，新3 spp约67–69%像素未获得正辐亮度贡献。实测不能支持“运动花屏已解决”，后续需处理路径方差或设计交互重建；本轮不扩展为新降噪实现。
- 有限96 spp参考与低样本共享部分seed流，不作独立真值/收敛认证。未验证其他GPU、Debug、全引擎、手动主观延迟、显示器扫描输出或高分辨率长期收敛。测试使用版本化demo与预设，不能替代用户截图的未保存环境状态。
- 4份定向Markdown/spec检查及Python语法、git diff --check通过。证据包927份文件逐项SHA-256与ZIP完整性检查通过；独立解压后重新聚合，性能/质量全部数值与两张PNG均逐值/逐字节一致。

## 修改记录

- 2026-11-17T13:56:00+08:00：记录设计与验证边界。
- 2026-11-17T14:25:00+08:00：完成实现、27项定向验证及真实后台GUI观察，记录首轮失败与复验、兼容边界和未运行项。
- 2026-11-17T15:04:00+08:00：补齐Sponza三轮真实窗口延迟/速率、运动与固定机位颜色证据，确认仍存在地面/间接光彩噪；新增3项定向验证通过，归档结果与边界。
