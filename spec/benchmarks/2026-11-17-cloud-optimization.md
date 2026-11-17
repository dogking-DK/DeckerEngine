# 云优化第一批验收（2026-11-17）

基线为[0175性能报告](2026-11-16-environment-performance.md)。本批依次完成预览调度、独立云分辨率、重建回退优化，每阶段独立验证/提交。此报告随各阶段补充，不能把待验收项作为已完成收益。

## 0176 预览调度

实际EnvironmentPreview worker，`projects/demo`、1280×720、RelWithDebInfo、validation OFF，RTX4070 Laptop / 驱动596.49。三轮独立进程，每预设3个重复小幅相机运动请求，共9个热样本；表为合并样本中位数，包含CPU/GPU同步及发布，不包含GUI最终呈现。原始逐请求数据、输入与二进制/源码摘要见[证据JSON](2026-11-17-cloud-preview.json)。

| 预设 | 原首图 | 新首图 | 原完整细化 | 新完整细化 | 细化耗时减少 |
|---|---:|---:|---:|---:|---:|
| sparse | 29.855ms | 30.043ms | 466.390ms | 185.653ms | 60.2% |
| overcast | 30.095ms | 29.743ms | 463.786ms | 192.407ms | 58.5% |
| clear | 29.179ms | 30.097ms | 29.186ms | 30.104ms | 无收益结论 |

变化是连续移动复用完成历史，首图后允许50ms空闲合并，新请求唤醒并停止旧细化；热历史额外8帧，冷历史仍额外32帧。完成标记表示本次预算完成，不是物理收敛证明。没有修改本阶段shader、场纹理尺寸或积分步数，不是单帧GPU提速。clear无算法改变，其约0.9ms wall波动保留；没有锁频或消除跨日桌面负载差异。

五个预设/视角的第一轮最终冷图像与0175逐字节一致。required GPU/sync测试覆盖连续运动、显式camera cut、resize、取消期间迟到结果、取消后错误请求、未提交Gizmo及退出释放。实际GUI与独立capture一致性另行验收，见[0176记录](../development/0176-cloud-preview-scheduling.md)。

## 0177/0178 最终1080p GPU对照

RTX4070 Laptop，RelWithDebInfo，validation/Tracy OFF、原生timestamp ON。相同固定几何、相机、天气和时间，32帧预热后20个稳态样本，三轮独立进程，轮间交替旧/新路径顺序；共48次运行、每格60个样本。表为**三轮各自p50的中位数，单位ms**，包含几何、介质、重建、色调映射和GPU读回；不包含GUI最终呈现。shader/二进制/输入/逐pass/逐轮数据在[JSON](2026-11-17-cloud-optimization.json)，原始数据、最终渲染源码与8张代表图在[ZIP](2026-11-17-cloud-optimization.zip)。24组完整1080p旧/新图像逐字节一致，摘要保留各组SHA-256；同图只存一份。

| 天气 | 生产1/2 | 生产1/4 | 实验1/2 | 实验1/4 |
|---|---:|---:|---:|---:|
| clear | 2.424 | 2.421 | 2.418 | 2.422 |
| sparse | 18.884 | 18.227 | 19.259 | 18.573 |
| overcast | 28.846 | 25.402 | 30.109 | 26.298 |
| sunset | 43.768 | 48.814 | 43.228 | 49.485 |

生产默认保持每轴1/2；UI Environment → Cloud render quality and time → Cloud resolution 可选每轴1/4，网格、深度及最终输出尺寸不变。capture/sequence 对应 `render_options.cloud_resolution_divisor=4`，同时指定 `mode=temporal`；provenance记录配置，切换会失效旧历史。

本轮厚云1/4比1/2节省约11.9%，稀疏云约3.5%；日落反而增加约11.5%。低分辨率并不保证更快：日落完整回退像素从212,284增至652,023，主射线密度查询从158,196,145增至205,464,426；低角度光照使这些额外完整积分更昂贵。工作计数来自独立诊断帧，不混入计时，也不代表SM occupancy。

显存：厚云生产1/2/1/4的采样在途device-local分配为247.67/188.61MiB，稳定缓存/历史31.11/11.42MiB；不是整机显存占用，也不是保留的VMA block大小。tile实验额外增加7.91MiB在途mask，默认未启用时不分配。

## 重建方案的取舍

先实现全局原子列表，再改为8×8 tile共享列表以保持邻近光线关系。两版保持原回退判定与积分模型，均做真实GPU A/B；[全局版本原始档案](2026-11-17-cloud-global-list-experiment.zip)包含匹配原manifest SHA-256的renderer源快照。

最终tile版本的收益不稳定：例如本轮厚云1/2从28.846增至30.109ms，1/4从25.402增至26.298ms。**因此没有替换生产默认重建。** `compact_reconstruction=false`，实验仅通过原生benchmark的显式 `--compact-reconstruction` 开启；UI/capture继续使用已有重建。此项交付为实现、对照与默认启用决策，不能称为已经获得重建提速。

60组透视/正交、2/4分辨率、三种有云天气及移动/时间变化的旧/新比较通过：HDR NRMSE≤1e-6、p99≤1e-5，云透射差≤1e-6，主要积分/密度查询/回退像素计数相同。奇数尺寸和提交失败均保持正确历史；全尺寸24组图像字节相同。

## 验收与复现

最终生产默认/实验路径、worker生命周期、实际GUI及冻结capture、参数发现、性能汇总：`out/verify/20261117-101439-f8c0d3f5` **7/7通过**，required GPU/sync，零错误/警告/泄漏。0177的四预设1/4参考质量58组通过，见[质量摘要](2026-11-17-cloud-resolution-quality.json)与[原始线性/图像](2026-11-17-cloud-resolution-quality.zip)。最终生产默认1/2四预设参考矩阵 **4/4通过**（102651），另58组HDR/透射/太阳盘及稳定性检查通过；[摘要](2026-11-17-cloud-default-quality.json)、[原始线性/图像](2026-11-17-cloud-default-quality.zip)。

```powershell
python scripts/benchmark-cloud-optimization.py out/benchmarks/my-cloud-comparison
```

输出目录必须是新目录。默认读取windows-editor/RelWithDebInfo；可传 `--build-dir` 与 `--configuration`。计时期间不并行跑其他GPU测试；脚本检查二进制及关键源码没有变化。

没有锁频，采集前后GPU温度64→81°C，端点从空闲P8变为P0；逐轮变化保留在JSON，微小差异不作稳定收益承诺。质量是有限测试场景，不保证所有云场/镜头。Tracy真实采集本批未重跑，已更新旧/新pass归因兼容并通过汇总单测；本报告耗时均为原生GPU timestamp。完整Debug/全引擎矩阵未运行。第一阶段早期GUI偶发差异/超时及后续通过，均保留在0176记录，未声称已证明其根因。
