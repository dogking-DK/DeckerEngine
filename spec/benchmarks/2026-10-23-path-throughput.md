---
created_at: "2026-10-23T10:15:00+08:00"
updated_at: "2026-10-23T10:15:00+08:00"
---

# Editor / Sponza 路径追踪吞吐优化

[0144开发记录](../development/0144-path-throughput-optimization.md)完成统计、tile提交组和Editor显示/采样重叠三个阶段；当前GPU、相机、分辨率、采样与反弹相同。小型[机器可读汇总](2026-10-23-path-throughput-summary.json)长期保留，全部原始报告、冻结shader/程序及复算脚本在忽略的`out/optimization/20261023-path-throughput`，完整复算需要这些本地原始数据。

## 环境与协议

NVIDIA GeForce RTX 4070 Laptop GPU（8 GiB）、驱动596.49、Windows、Release。基线为61770f7；改后为6dac811加已重建的Editor/worker修改，采集时工作区身份和文件SHA256已记录，不能将其表述为纯净6dac811构建。

前后分别3轮默认相机worker、4模式×3轮renderer，共30个独立进程，串行执行，全部退出0。性能关闭validation，renderer启用原生GPU timestamps；正确性单列required RT/Khronos/sync。仅在子进程设置AMD layer JSON支持的`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，不修改系统层或注册表。

Surface only、真实Sponza、4次反弹/RR3。ReSTIR候选8，时间/空间参数保持原设置，空间模式包含时间复用。没有降分辨率、减少采样/反弹、改变估计器或删除统计。Editor采用256 tile和4 tile提交组，单在途票据保持；独立renderer基线组1、改后组4。

Renderer固定640×360、基准相机、seed101，逐1spp累积16批；每轮去掉前4批，取后12批中位数，再取三轮中位数。worker使用默认项目/默认相机，1600×1200每模式4spp、2560×1440每模式2spp，每次模式首交付排除；先求各轮热间隔中位数，再求三轮中位数。

## 默认相机实际worker交付

交付间隔包括tone、GPU图像交接与Windows消费者轮询，不是实际GUI FPS。

| 模式 | 1600×1200前→后 ms | 前/后 | 2560×1440前→后 ms | 前/后 |
| --- | ---: | ---: | ---: | ---: |
| Classic MIS | 41.004 → 32.539 | 1.26× | 60.072 → 45.272 | 1.33× |
| ReSTIR initial | 56.606 → 41.568 | 1.36× | 89.643 → 71.157 | 1.26× |
| ReSTIR temporal | 225.272 → 48.006 | 4.69× | 421.408 → 84.422 | 4.99× |
| ReSTIR temporal + spatial | 389.318 → 56.086 | 6.94× | 768.661 → 98.642 | 7.79× |

1600×1200改后Classic三轮为30.167/70.658/32.539 ms，第二轮显著较慢；保留全部轮次，不能据此承诺跨运行稳定达到某个帧率。2560每轮只有一个热间隔，统计支持比1600更有限。

三轮worker共27次取消观测，中位数12.584→13.611 ms，最大24.595→24.858 ms；没有随吞吐同比下降。冷编译、导入及AS准备未优化。

## Renderer成本

GPU列为提交区间总和，CPU为record-submit；各列独立取中位数，不可相加还原wall。

| 模式 | wall前→后 ms/spp | GPU前→后 ms | CPU准备前→后 ms | 提交数前→后 |
| --- | ---: | ---: | ---: | ---: |
| Classic MIS | 4.798 → 3.683 | 2.820 → 2.523 | 1.401 → 0.795 | 6 → 2 |
| ReSTIR initial | 21.559 → 18.862 | 4.309 → 5.405 | 4.139 → 2.430 | 12 → 4 |
| ReSTIR temporal | 32.386 → 7.985 | 25.568 → 4.532 | 4.621 → 2.206 | 18 → 6 |
| ReSTIR temporal + spatial | 51.950 → 13.488 | 43.446 → 7.076 | 6.294 → 3.030 | 24 → 8 |

时间/空间模式主要受益于device-local分片统计；各模式CPU准备下降约41%–52%，本尺寸提交数减少三分之二。初始RIS的GPU区间增加而wall下降，不能描述为全部GPU阶段均变快；短阶段仍有提交/驱动等待开销。

Classic renderer基线第三轮wall/GPU为13.684/10.794 ms，另两轮wall为4.493/4.798；改后第二轮wall/GPU为14.882/12.515，另两轮wall为3.683/3.539。异常轮不删除、不补跑或挑最快结果。没有抓取硬件利用率时间线，GPU时间/wall不等于任务管理器利用率或SM占用率。

## 正确性与状态

- 三轮四模式的12对32×18/4spp比较图，线性颜色与二阶矩全部逐位相同；2,688项射线/时间/空间/介质统计精确一致。物理输入与采样身份核对通过。它不是全尺寸高spp质量认证。
- 20个不同的required GPU检查通过，0最终失败/跳过。覆盖分片统计、1/4/8 tile组精确等价、尾组/反序、每dispatch tracking及超额、真实低额度组8失败/组1恢复、取消/后续组失败、外持旧帧、双队列与共享队列回退、三槽tone/读回/采样和活动关闭。
- 三个真实`--background` GUI场景通过Classic/Scene media/ReSTIR面板、交互、IPC、固定capture一致性；保留Surface完整帧防饥饿及GUI资源寿命等待。设备支持时预览使用同族queue1；这不保证硬件并行。
- 首轮新增group用例出现重复Graph Pass名和表面测试输入问题，修正后复验通过；后续读回排空修正又复验4项受影响检查。所有失败和成功日志保留，见0144。
- 未运行全工程、Debug、其他GPU或完整M19.8高spp质量/等误差矩阵；默认算法仍为Classic MIS。Scene media本轮做了功能/状态验证，没有重新采集其完整性能矩阵。

## 复算

```powershell
python out/optimization/20261023-path-throughput/summarize.py
```

重新采集改后单个renderer例子：

```powershell
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case surface --algorithm spatial --tile 256 --tiles-per-submission 4 --performance-samples 16 --output out/path-throughput/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/path-throughput/desktop --desktop-path
```

本次结果已完成计划范围。后续优先定位初始RIS短pass与驱动等待、Classic运行波动及冷启动；不以监视器100%作为正确性或实时交互验收条件。
