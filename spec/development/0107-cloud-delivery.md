---
id: "0107"
created_at: "2026-10-13T19:40:10+08:00"
updated_at: "2026-10-13T20:27:27+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-pipeline.md
  - ../design/foundation-profiling.md
---

# 0107 M15.7 云质量、动态稳定性和性能联合交付

## 目标与结论

执行[冻结采集协议](../design/render-clouds.md#m157-质量与性能采集协议)：四个共享预设、收敛参考/动态稳定、三轮 1080p 原始 CPU/GPU/显存，以及 required GPU 失败/关闭回归。
质量与观测验收完成；**云 4ms、云与大气联合 5.5ms、在途峰值 192MiB 候选未达，128MiB 稳态候选达到**。不修改质量阈值来获得性能通过。
完整数字与限制见[联合报告](../benchmarks/2026-10-13-clouds.md)，复现命令见[指南](../guides/cloud-benchmark.md)。

## 实际变更

- 新增 [CloudBenchmark](../../tests/integration/CloudBenchmark.cpp)、[观测辅助](../../tests/integration/CloudBenchmarkSupport.hpp)和[质量用例](../../tests/integration/CloudQualityCases.hpp)，调用公开渲染/云影/观测接口，不增加命令或生产计时状态。
- quality 模式使用 required/sync validation；四预设共 58 组相同相机/时间的线性参考比较，静止 32+120 帧，连续平移/旋转/风时间 12 帧，非零云另测云内/云上、时间倒退/相机切换。1024/256/256 高档裁剪投影分块保持像素中心，512/128/128 检查收敛与分块等价。保留未掩码/非太阳/太阳与云 Tc 指标、原始 RGBA 和失败图像。
- 新增 [benchmark-clouds.py](../../scripts/benchmark-clouds.py)：三轮四预设 timestamp OFF/ON，共 24 进程、5040 个计时帧、288 个独立显存诊断帧。每段从空历史开始；combined/air/unlit 同索引相机/时间匹配。只相加新增 LUT、云影和帧 Submission，拒绝缺失/重复区间、错误缓存或历史、输入变化及泄漏。
- 每进程分列设备/程序/CPU 云场/纹理上传 wall，GPU 上传时间为 null；每帧 CPU 提取、prepare、建图提交、等待、读回、端到端及 GPU Pass；显存覆盖外部旧帧和在途候选同时存活。allocation/block、device-local/host-visible、CPU heap 与观察边界分别保留。
- 新增 [聚合器回归](../../tests/integration/CloudBenchmarkTest.py) 6 个 Python 用例，覆盖失败/缺失/重复/错误 phase、输入和质量变化、非有限计时、父子双计、OFF 缺测与三轮完整性。CMake 登记探针/4 个质量用例及 smoke/聚合入口，同步测试选择表和设计索引。
- 实际日落误差推动 [低角度自遮蔽](../../shaders/render/cloud-reconstruction.slang) 最低 16→32 步；[预算](../../engine/render/clouds/src/CloudReference.cpp)按最坏 32 步核算，默认/上限 64Gi→128Gi 逻辑工作量，维持 1080p 上限。shader 指纹添加 low-sun-32；参考单提交 1Gi 预算不变。新增调用者保留 64Gi 时 1080p 拒绝、960×540 接受的边界回归。
- 归档[质量指标](../benchmarks/2026-10-13-clouds-quality.json)、[性能证据](../benchmarks/2026-10-13-clouds-evidence.json)、四预设预览和复现指南；Roadmap 关闭 M15.7/M15 和节点 E 的基线交付，同时明确候选性能差距。

## 状态、发布与失败行为

生产修改只调整采样和提交前预算验证；不改变 Scene、预设物理参数、持久化格式、撤销或会话所有权。
预算拒绝发生于分配/提交前，旧历史/帧语义沿既有实现；原有 failure/history/shadow 生命周期回归覆盖。
探针默认 JSON status=failed，完整采集、输入/指纹核验和关闭成功后才标 passed。
驱动只使用新目录，先写 running manifest，逐进程保存来源和 SHA256；失败保留原始文件/日志并返回非零，不重试或删除失败轮次。
质量/性能多文件产物不承诺跨文件原子事务；性能候选达标与采集有效性分开。

## 失败发现与修正证据

所有路径位于 out/verify；各次保留 summary/build/tests/results 和复制的 details.log。

| 运行 | 结果 | 原因与处理 |
| --- | --- | --- |
| 20261013-194527-c23260b0 | 2通过、3失败、0跳过 | 首次高档按 8 行裁剪，离轴偏移超过既有投影域；改为最多 16² 四象限，保持 1Gi 工作预算及原像素中心 |
| 20261013-194808-0e565832 | 2通过、1失败、0跳过 | sparse/overcast通过；日落太阳盘约7500亮度相对约.062天空尺度放大float差，未掩码收敛nRMSE=.0225252；原始sunset-first.json保留 |
| 20261013-195426-29cda6e5 | 0通过、1失败、0跳过 | 依据既有太阳独立透射契约增加几何太阳分类；分块等价检查仍未分类而失败；统一该度量的分类，sunset-solar-classified.json保留。普通像素最大 .129421 超过 .12，仍需生产修正 |
| 20261013-195826-133fcfe9 | 2通过、0失败/跳过 | 最低太阳采样32步、最坏预算及指纹修正；日落与聚合器通过，静态最大 .0265579，云内 .0712874 |
| 20261013-200135-1371dde5 | 17通过、0失败/跳过 | 最终 Debug 定向链：预算、聚合、smoke、四预设、优化/边界/时域/历史、阴影数值/表面/失败、合成失败、真实脚本序列和后台GUI一致性 |
| 20261013-200810-78e3e75b | 2通过、0失败/跳过 | RelWithDebInfo 构建、6个Python聚合回归、required GPU smoke；随后正式性能采集 |
| 20261013-201517-e3ea69ee | 5通过、0失败/跳过 | 最后审查显式拒绝非有限云Tc，避免std::max忽略NaN；补充64Gi调用者预算边界。四预设和预算复验通过，生产代码和性能路径未改变 |

质量数据最终归档在 `out/verify/20261013-201517-e3ea69ee/cloud-quality-*.json`，此前17项完整日志和初次失败证据也保留。
最终静态帧差 nRMS=0；58组最大非太阳 RGB nRMSE=.00313278、p99=.0100394、单点=.0712874、Tc=.00523138；太阳盘最大独立归一误差 .000102623。
分块与参考收敛通过；不把几何太阳分类用于隐藏普通云像素的真实超差，12%门槛保持。

## 正式性能及复现

`python scripts/benchmark-clouds.py --build-dir out/build/windows-editor` 于 20:09:45–20:12:59 运行，原始目录 `out/benchmarks/clouds-20261013-200944-7b867323`。
24进程都有效，未重试/删轮；每预设六次固定图像 SHA256 一致。设备 RTX 4070 Laptop、驱动596.49，MSVC 19.51.36260.0、RelWithDebInfo，Tracy/validation OFF，timestamp OFF/ON。
源码、二进制、原始JSON/PPM摘要和全部每轮统计随证据归档。性能结束后只加强质量/Tc与CPU预算测试并清理驱动末尾空行；性能二进制及生产源未改，被测旧版质量辅助头摘要仍保留以准确标识采集版本，最终质量头/Debug exe 摘要另存质量证据。

云增量 motion p95 三轮中位数：sparse 17.802ms、overcast 30.123ms、sunset 47.001ms；联合为19.891/32.215/49.386ms。
厚云第二轮云p95 52.463ms如实保留。非零云稳态增量31.359MiB，外部旧帧与新帧在途247.922MiB；稳态通过，峰值超过192MiB约55.922MiB。
主要GPU区间是半尺寸积分和保边重建；24进程关闭后CPU活分配为0、VMA回到设备初值，无validation错误/警告。

验收命令完整列于指南；17项与2项构建分别显式列出所有所需target及TestRegex。
最终5项复验：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor `
  -Target @('dk_cloud_benchmark','dk_cloud_reference_tests') `
  -TestRegex '^dk\.clouds\.(optimization |quality_.*_gpu_validation$)' `
  -Reason 'M15.7 finite transmission evidence and caller work-budget boundary for low-sun 32-step refinement'
```

全部required GPU测试临时设置AMD隐式层禁用并在finally恢复；Khronos和同步validation保留。
文档首次检查发现测试选择表内正则竖线未转义，修正后 `check-spec.ps1 -Path` 通过12份Markdown、677个本地链接及元数据/表格/索引/测试入口/JSON清单，日志 `out/verify/m15-7-docs.txt`。
独立重新计算24个原始报告与汇总、核对全部原始JSON/PPM及58组质量SHA256通过；性能后行为源差仅为质量头的Tc有限值检查，驱动另有末尾空行清理。暂存检查发现该末尾空行后已清理，`git -c core.safecrlf=false diff --cached --check`通过。用户原有 `spec/README.md` 和 `spec/guides/ai-documentation-workflow.md` 改动保持原样、不纳入本阶段提交。

## 限制与下一步

质量只覆盖固定31²参考场景和既有专项边界，不外推到1080p每像素或任意天气。低阶多散射近似不是路径追踪真值。
性能每轮20样本；硬件时钟/功耗未锁定，timestamp对比含调度漂移。离屏数据不覆盖GUI呈现/IPC，未采Tracy轨迹或纹理上传独立GPU时长。
VMA安全点不是系统显存或精确峰值，冷创建内部瞬时分配未完全捕获；CPU heap不是RSS。clear也预建云纹理，初始化/缓存含夹具成本。
本轮未跑全量、跨GPU/驱动、长时间压力或完整M14独立质量集；未修改的CPU-only与M15.6全部命令/持久化回归沿已有验收。
后续需独立规划积分/重建与在途资源复用优化，并继续用本质量门槛和三轮原始数据验收；不以本阶段基线完成冒充性能候选达标。

## 修改记录

- 2026-10-13T19:40:10+08:00：冻结协议并开工。
- 2026-10-13T19:58:26+08:00：修正投影分块、太阳度量和日落低角度自遮蔽，保留失败证据。
- 2026-10-13T20:22:16+08:00：完成质量/回归、三轮性能和资源证据，明确未达性能/峰值候选，完成M15.7基线交付。
