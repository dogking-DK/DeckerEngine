# 默认 Sponza 路径预览：预算与吞吐优化

RTX 5090 D v2 / 610.88、Ryzen 9 9950X3D、Release；实现与验收见[0142](../development/0142-path-preview-throughput.md)，完整证据见[归档](2026-10-20-path-preview/README.md)。

## 结果与原因

原编辑器固定 1 GiB，1600×1200 下已有 Classic 累积与场景再加 ReSTIR 的224 bytes/pixel候选无法同时准入。
现在按最大 device-local heap 的1/4、当前可用预算的1/2和8 GiB上限取小，本机选择5.868 GiB；最大实测owned峰值2.332 GiB。
支持时开启VK_EXT_memory_budget；不支持时使用VMA估计。额度不是预分配，也不保证未来驱动分配不会失败。
无时间复用的完成图像释放96 bytes/pixel的首表面/reservoir，1600×1200每个完成owner少占175.78 MiB；线性累积、来源和诊断不变。

表面预览tile上限由64提高到256，工作预算仍可拆分；默认batch1、所有尺寸/反弹/候选数/采样/物理模型不变。
640×360每spp的提交数Classic/initial/temporal/spatial从60/120/180/240降至6/12/18/24。
减少每tile资源准备、Graph编译、提交与完成等待；没有引入忙等、无界在途队列或降低图像质量。
Scene media保留tile64；默认算法仍Classic MIS。

## 三轮同输入性能

640×360、Sponza surface固定基准相机、4反弹、seed101；前后各4模式×3轮，每轮4个逐spp批次、12次相机预览/取消及停动16spp恢复。
关闭validation，开启GPU timestamps；依次运行，无并行GPU测试。基线源为f1e5734，新版测量二进制指纹和最终便携类型书写修正见environment.json。
表中wall/CPU/GPU为全部12个逐spp值的中位数，tile和取消为汇总nearest-rank p95；独立中位数不能相加还原wall。

| 模式 | 每spp wall ms（前→后） | 加速 | CPU record-submit ms（前→后） | GPU ms（前→后） | 新tile GPU p95 ms | 新取消回收p95 ms |
| --- | --- | --- | --- | --- | --- | --- |
| Classic MIS | 42.92 → 15.50 | 2.77× | 10.30 → 1.84 | 14.87 → 10.49 | 4.67 | 1.04 |
| ReSTIR initial | 46.74 → 31.78 | 1.47× | 20.99 → 3.74 | 10.78 → 13.61 | 4.40 | 11.37 |
| ReSTIR temporal | 77.22 → 60.45 | 1.28× | 28.54 → 5.31 | 33.28 → 41.16 | 7.18 | 2.67 |
| ReSTIR temporal + spatial | 121.00 → 62.92 | 1.92× | 39.10 → 6.26 | 60.55 → 51.91 | 7.06 | 2.85 |

全部原始轮次保留。Classic新版三轮wall中位数9.48/16.30/14.94 ms，存在波动，汇总为15.50 ms，未选最快轮。
GPU时长并非每种模式都下降，收益主要来自提交开销减少；新tile p95均低于8ms、取消p95均低于100ms。
Classic的GPU时长/wall中位数比例约35%→68%，spatial约50%→83%；这不是任务管理器利用率或全程GPU利用率，不能承诺100%。
32×18对照的线性颜色和二阶矩逐值完全一致；640×360逐批来源/统计一致。
新增required GPU测试独立比较257×129非整除尺寸、256/64 tile与反向遍历的颜色/二阶矩/命中，包含全部四种复用组合，完全一致。
未重跑M19.8完整高spp参考认证/等误差矩阵，本次数据不代表ReSTIR优于Classic的等误差收益。

## 实际默认相机与桌面尺寸

同一PathPreview worker，正常默认Sponza和相机，验证层关闭，三轮依次切换四模式。
1600×1200每模式4spp、2048²每模式2spp；表为排除首个交付后的交付间隔中位数，包含tone/交接和消费者轮询。
Windows消费者1ms sleep可按调度tick量化，因此不是纯GPU时长或窗口FPS。单独required validation覆盖同流程。

| 模式 | 1600×1200交付间隔 ms | 2048²交付间隔 ms | 2048²截至该模式owned峰值 GiB |
| --- | --- | --- | --- |
| Classic MIS | 30.82 | 61.63 | 1.449 |
| ReSTIR initial | 42.67 | 93.01 | 1.832 |
| ReSTIR temporal | 232.49 | 520.74 | 2.207 |
| ReSTIR temporal + spatial | 473.68 | 1080.29 | 2.332 |

每次都完成续批、实际在途取消、恢复与模式/尺寸切换；旧显示图像同时保留。24个renderer性能进程的CPU/VMA活分配均为0；worker关闭后CPU活分配为0，各进程无诊断，额度内运行。
冷首次交付另含编译/导入/AS准备，详见raw；没有把冷启动计入热帧加速。时间/空间复用在高分辨率仍明显昂贵，尚非实时全尺寸路径追踪。

## 复现与验证

先按[构建指南](../guides/build.md)配置Release和Sponza；本机子进程使用DISABLE_VULKAN_OBS_CAPTURE=1避开OBS旧API层，required Khronos/sync保留。

```powershell
# 前后源码各运行三轮，旧版64，新版256；mode依次为classic/initial/temporal/spatial。
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case surface --algorithm spatial --tile 256 --full-preview --output out/preview/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/preview/desktop --desktop-path
```

定向验收为out/verify/20261020-000351-10afa8f9：16通过、0失败、0跳过，含设备、progressive分割/预算/失败/device-lost、ReSTIR各阶段调度/状态、默认桌面worker及真实后台GUI/独立捕获。
最终uint64_t模板类型匹配修正后重建，out/verify/20261020-001430-49e1a6ce复验预算与桌面两项通过，0失败/跳过，14.64秒。
两次沙箱内构建分别因vcpkg缓存写权限及MSBuild FileTracker访问被拒，未执行测试；提权构建通过后才计有效结果。
未运行全工程、Debug矩阵、其他GPU硬件、无memory-budget扩展硬件或完整高spp质量矩阵。
