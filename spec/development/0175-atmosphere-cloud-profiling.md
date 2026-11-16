---
id: "0175"
created_at: "2026-11-16T17:40:40+08:00"
updated_at: "2026-11-16T18:22:00+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-atmosphere.md
  - ../design/foundation-profiling.md
  - ../design/editor.md
---

# 0175 大气与体积云性能归因

## 目标与设计依据

按用户请求实测当前大气/云渲染的性能和资源热点，区分分辨率、采样质量、算法与实现开销，并验证 Tracy 的实际作用。依据云性能归因协议；交付可复现报告和原始证据。

## 实际变更

为现有云基准添加有界诊断参数和工作量计数，保留生产实现/默认质量与旧基准协议。测量原生 GPU timestamp/CPU 阶段/VMA 与编辑器首帧/细化延迟；用启用 profiling 的独立构建实际连接 Tracy 采集。

- `CloudBenchmark.cpp`新增尺寸、步数、场缩放、自阴影/地面云影、模式筛选和等待Tracy选项；统计仅在额外非计时帧中读回，显存样本亦与其隔离。增加一次性CPU场生成zone与场纹理内存快照。
- `benchmark-environment-diagnostics.py`执行42进程三轮对照、校验计时/寿命/源码和二进制摘要、保留遥测与失败；`capture-cloud-profiling.py`采集真实云工作负载并核对原生/Tracy GPU叶pass。
- [性能报告](../benchmarks/2026-11-16-environment-performance.md)归档逐轮JSON、逐帧原始数据、图像、Tracy和验证证据；同步云设计与复现指南。没有生产算法、依赖版本或命令注册变更。

## 验证记录

使用`verify.ps1`，RelWithDebInfo：

| 范围 | 结果与证据 |
|---|---|
| `windows-graphics`，`dk_atmosphere_benchmark` / `dk_cloud_benchmark`，`^dk\.(atmosphere\|clouds)\.benchmark_(gpu_validation\|aggregation)$` | 初次编译参数名与sink冲突；修正后大气聚合/GPU和云聚合3项通过，云GPU因统计未启用失败。目录`20261116-174234-f4d3a9bf`、`20261116-174514-8c92534c`。 |
| `windows-graphics`，`dk_cloud_benchmark`，`^dk\.clouds\.benchmark_(gpu_validation\|aggregation)$` | 统计移至显式diagnostics的额外非计时帧后，2/2通过、0跳过；`out/verify/20261116-175403-3225cd2c`。 |
| `windows-graphics-profiling`，`dk_cloud_benchmark`，`^dk\.clouds\.benchmark_gpu_validation$` | 1/1通过，`out/verify/20261116-180816-e42ca739`。配置刷新既有固定基线，未升级依赖。 |
| `windows-editor`，`dk_editor_render_benchmark`，`^dk\.editor\.render_worker_gpu_validation$` | 1/1通过，`out/verify/20261116-180730-0972b03b`。 |
| 自定义160×90、主48/太阳4、场除2、云影128²/64步，required GPU/sync validation | 通过，0错误/警告；超1080p预算、profiling OFF的`--wait-tracy`均拒绝；`out/benchmarks/environment-diagnostic-validation-summary.json`。 |

正式性能validation OFF：云42进程、大气18进程、编辑器3进程全部通过；相同条件图像三轮确定性检查通过。原生timestamp与Tracy采集分开，后者518 GPU/2059 CPU zones，431个原生GPU叶pass逐项误差≤2ns。首次沙箱内TCP连接超时，保留失败证据并关闭子进程；允许127.0.0.1连接后成功，无性能轮次被重试或丢弃。

执行命令及完整范围见[指南](../guides/cloud-benchmark.md#性能归因与-tracy)和报告；文档定向`check-spec.ps1`与`git diff --check`纳入交付检查。GPU验收使用真实设备，正式性能数据未用smoke、软件模拟或skip替代。
采集后补充Python驱动对非优化配置的显式拒绝，重新汇总全部42份原始JSON通过，并验证Debug拒绝；没有改变计时或渲染代码。证据包保留采集时脚本快照以匹配manifest摘要。
交付检查通过：5份Markdown、208个本地链接及元数据/表格/索引/测试入口；`git diff --check`通过。证据包约5.87MiB，214份原始文件、18份去重图像的SHA256和ZIP完整性检查通过。首次文档表格中的正则竖线未转义，修正后重验通过。

## 偏差与决策

测试参数变化属于性能归因，不直接降低生产默认质量。Tracy 不能替代硬件寄存器/warp occupancy 测量。

厚云1080p GPU p50=30.159ms、日落46.253ms；积分/全尺寸重建是主体。场纹理减半约39%收益并改变内容，不能宣称无损。主步96→48只约7.5%收益，计数证明额外密度采样抵消了许多减少。编辑器重复厚云首图约30ms、33帧细化约464ms，调度与GPU算法应分别优化。默认空气透射使用LUT，不将reference逐点空气积分误报为默认热点。

## 遗留问题与下一步

按报告优先优化请求历史/细化调度、重建回退及积分预算，再验收动态尺寸与纹理质量档。未运行完整HDR质量参考集、最大化实际GUI帧率、路径Scene媒体性能和Nsight硬件计数器；这些均未宣称通过。

## 修改记录

- 2026-11-16T17:40:40+08:00：创建测量方案。
- 2026-11-16T18:22:00+08:00：完成三轮实测、真实Tracy交叉校验和报告，保留失败与未运行范围。
