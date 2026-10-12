---
id: "0092"
created_at: "2026-10-12T14:08:00+08:00"
updated_at: "2026-10-12T14:20:00+08:00"
status: completed
design_refs:
  - ../design/experiment-management.md
---

# 0092 M13.4 对比报告与闭环验收

## 目标与设计依据

按[实验管理](../design/experiment-management.md)补齐可追溯JSON/CSV/HTML报告、
同配置重跑及CPU/GPU数值容差核验；沿用M11计时口径，失败/缺失不填零。
保留spec/README.md和AI文档流程指南的既有改动，不纳入本次提交。

## 实际变更

- [experiment_report.py](../../sdk/python/decker/experiment_report.py)增加write_report和离线CLI，
  锁定合作写者并重验batch/index及全部产物，按当前成功attempt取数；完整参数、来源、环境、
  run/attempt身份、文件摘要及旧尝试都保存在报告。独立重跑按experiment_id并集配对，
  同批CPU/GPU保持seed、参数、步数和输出一致。拒绝自身或同batch副本充当重跑证据。
- [渲染模块](../../sdk/python/decker/_experiment_report_render.py)生成report.json、experiments.csv、
  comparisons.csv及自包含HTML/SVG。参数/指标可展开，产物可追溯；缺失值为空，失败不填零。
  每次发布到新的reports/UUID目录；临时目录写完/fsync后rename，发布失败保留源文件和全部旧报告。
  输出有128MiB总上限，HTML转义错误文本，CSV保护公式前缀。
- [批处理CLI](../../sdk/python/decker/experiment_batch.py)在run/resume后自动生成报告，支持baseline和tolerances。
  数值超差/不完整返回1，报告发布失败返回2并保留执行摘要/report_error；不改变引擎或批次终态。
  SDK执行API保持原行为，按需显式报告。不改C++、命令schema、原plan/index/batch格式或依赖。
- 数值策略分别声明同后端和跨后端位置/速度阈值，逐粒子比较最大分量/RMS和逆质量；
  物理指标逐项列差。性能展示最近批次样本、样本拍数和可比的配对差/比率；
  GPU timestamp明确缺失，CLI单独观察本次调用端到端墙钟，不用持久时间戳推算。
- 新增[14项报告单元](../../sdk/python/tests/test_experiment_report.py)、
  [CPU/GPU扫描重跑夹具](../../tests/integration/ExperimentReportTest.py)，同步CMake、选择表、指南与索引。
  M13.1–4全部验收后在Roadmap关闭M13，没有添加未规划的下一阶段。

## 验证记录

Windows 11 / Python 3.13.2，CPU-only使用windows-scripting Debug；GPU使用windows-editor RelWithDebInfo。
均只构建dk_run、dk_ctl。GPU同步验证启用，要求可用设备，不能以77跳过。

```powershell
$env:TEMP = (Resolve-Path out/test-temp).Path
$env:TMP = $env:TEMP
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Configuration Debug -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(report_unit|report_cpu|batch_unit|recovery_unit|batch_cpu|recovery_cpu)$' -Reason 'M13.4 报告容差与CPU扫描重跑、自动报告影响的批处理及恢复'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(report_gpu|batch_gpu|recovery_gpu)$' -Reason 'M13.4 GPU扫描与300拍重跑报告、图像批处理和恢复回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Configuration Debug -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.report_unit$' -Reason 'M13.4 最终报告：重试历史、异长计时与零分母回归'
```

| 范围 | out/verify证据目录 | 结果 |
| --- | --- | --- |
| CPU报告/批处理/恢复 | 20261012-140843-29a9b529 | 6通过，0失败/跳过，45.60秒 |
| GPU报告/批处理/恢复 | 20261012-141152-effbf8e0 | 3通过，0失败/跳过，83.68秒 |
| 最终14项报告单元 | 20261012-141520-8055e217 | 1 CTest通过，0失败/跳过，8.79秒 |

单元覆盖重排配对、容差等号/超差/逆质量、缺失配对并集、失败/待执行空值、已登记失败attempt不冒充成功、
重试历史与当前attempt选择、0拍/未观察计时、异长样本与零分母、篡改/缺文件、原子发布失败、
合作写锁、自身比较、非法策略、离线CLI超差退出和报告失败不重跑。
真实恢复回归保留M13.3五处强杀、原票据核实和失败/取消重试；新增自动报告不改变恢复结果。
GPU日志无验证错误；已知AMD隐式层1.3/应用1.4 Loader警告仍保留，batch/recovery/report分别8/26/16次。

真实扫描：8×8布片，iterations 8/12，seed42/43，dt10ms，严格300拍、每批8拍。
CPU-only首轮4项及独立重跑4项；图形首轮CPU/GPU共8项及独立重跑8项，均按配置/seed核对。
重跑刻意反转seed与迭代轴顺序。两轮产物来自不同run，报告重生成后旧文件字节不变，Edit/history保持不变。

证据根目录：

- CPU：`out/build/windows-scripting/test-artifacts/Debug/experiment-report/52d3937faa854e23944bfb30f34b5279`。
- GPU：`out/build/windows-editor/test-artifacts/RelWithDebInfo/experiment-report/c5fac305d040481f91feb29a002208fe`。
  原自动重跑报告`rerun/reports/75e9cc24-5c5c-4502-9364-c06b7227487b`，含CLI端到端观察；
  最终排版离线报告`rerun/reports/a8a85089-16c9-4b5c-878a-8dfac24d3f65`，该观察留空。
  两者均有12对数值核验（4组CPU/GPU、8组独立重跑），全部通过；8组重跑位置/速度分量差均为0。

GPU构建内第二轮CPU/GPU比较如下。时间是**末批4拍**的单个墙钟样本，启用validation，
不能与M11关闭validation的稳态8拍p50直接比较，也不表示整轮求解耗时或统计显著提速。

| iterations / seed | 最大位置分量差 m | 最大速度分量差 m/s | CPU末批 ms | GPU末批 ms |
| --- | --- | --- | --- | --- |
| 12 / 43 | 1.149625e-5 | 3.576279e-5 | 0.0972 | 92.4558 |
| 12 / 42 | 1.057982e-5 | 3.129244e-5 | 0.0944 | 103.4891 |
| 8 / 43 | 5.364418e-6 | 3.799796e-5 | 0.0642 | 47.3298 |
| 8 / 42 | 4.410744e-6 | 3.725290e-5 | 0.0620 | 46.2609 |

跨后端位置2e-3m/速度2e-2m/s、同后端1e-6m/1e-5m/s策略均嵌入报告。
GPU runner SHA256为`63556eaddbd9c50b706933714efac156cc9b4cf8bacd43266422da725c927ced`；
设备/驱动未由SDK观测，device为null，不将历史M11设备信息当成本次已确认环境。
最终HTML经本机后台浏览器检查，图表坐标/表格/缺测说明可见；字体调整后离线重新核验/发布通过。
文档检查9个Markdown、555个本地链接及全局索引/测试入口通过，git diff --check通过。
命令为`./scripts/check-spec.ps1 -Path`，限定本次README、SDK README、设计/开发索引、
实验设计、0092、实验指南、Roadmap和测试选择表共9个文件。
未执行全引擎、Tracy采集或M11性能基线，
原因是本次仅增加SDK报告与CLI连接，不改变求解器、图形资源或性能采集实现。

## 偏差与决策

当前导出仅保存最近批次计时；报告不外推整轮耗时，不冒充GPU timestamp。
同后端容差是显式报告策略；记录环境相等不推断未知硬件相同。旧文件摘要不代替逐粒子数值比较。
HTML浏览器初次预览因测试临时目录权限返回404，改用同权限的仅localhost只读服务后正常，
没有改目录ACL。临时服务与浏览器标签在验收后关闭。

## 遗留问题与下一步

M13.4无未完成项。M0–M13本轮路线闭环；图像像素差异、多机/多GPU并行、检查点、其他平台
及完整性能统计仍不在本阶段范围，后续按需求独立立项。

## 修改记录

- 2026-10-12T14:08:00+08:00：创建记录并先更新设计。
- 2026-10-12T14:17:00+08:00：实现与CPU/GPU定向验收完成，补充真实数值/计时证据和边界。
- 2026-10-12T14:20:00+08:00：文档/差异检查通过，关闭临时预览服务，准备本阶段独立提交。
