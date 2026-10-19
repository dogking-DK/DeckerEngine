# Scene media 容量与吞吐优化

RTX 5090 D v2 / 610.88、Ryzen 9 9950X3D、Release；实现见[0143](../development/0143-path-capacity-media-throughput.md)，原始结果、哈希与复算程序见[归档](2026-10-20-media-capacity/README.md)。

## 容量政策

Editor自动额度由最大device-local heap的1/4、可用量的1/2、8 GiB取小，改为该heap的当前预算减usage。本机约5.868→22.658 GiB；不是预分配，也不是未来驱动分配成功的保证。
实际owner继续计费，包括上传、AS、外持完成帧、在途旧代与读回；明确的低额度和真实分配失败仍会拒绝。

路径预览取消1600×1200、GPU渐进2048单边、百万三角形、4096 primitive、64纹理/128 MiB源像素及整目标2^32射线的人为限制。
路径预览的源glTF导入同步使用设备容量；纹理数组按实际需要增长。GPU仍检查设备image、descriptor、AS和buffer能力，以及shader下标/数值表示范围；CPU参考、离线产物格式和算法参数有各自的合法域。
目标spp使用非零uint32编号，保留epoch溢出检查；累计射线统计使用历史预留分量的两个uint32，避免float整数失真或32位回绕，不增加显存。
独立捕获沿用其输入/文件格式限制，GPU预算随共享Renderer改为自动选择，报告提供实际额度。

## 同输入三轮性能

基线为22402f6已验收Release程序，介质tile64；新版tile256。每模式顺序运行三个独立进程，不并行GPU测试。
固定courtyard-clear、640×360、4反弹/RR3、seed101、时间17、逐1spp累积到4；每轮另含12次完整尺寸相机预览/取消和停动16spp恢复。
关闭validation，开启GPU timestamps；保留全部首帧，热统计仅汇总每轮spp2/3/4，共9个值。没有降低分辨率、候选数、反弹或采样量。

| 模式 | 每spp wall ms（前→后） | 加速 | CPU record-submit ms（前→后） | GPU ms（前→后） | 提交数（前→后） |
| --- | --- | --- | --- | --- | --- |
| Classic MIS | 47.52 → 19.82 | 2.40× | 10.17 → 1.49 | 23.48 → 17.23 | 60 → 6 |
| ReSTIR initial | 69.16 → 48.51 | 1.43× | 21.63 → 3.12 | 31.58 → 30.52 | 120 → 12 |
| ReSTIR temporal + spatial | 141.68 → 83.32 | 1.70× | 40.48 → 4.41 | 81.67 → 76.50 | 240 → 24 |

CPU提交准备耗时降低85%–89%；每帧GPU区间总时长/wall的中位数分别49.4%→86.1%、45.5%→63.6%、56.8%→92.6%。这是已测提交区间覆盖比例，不是任务管理器或SM占用率，也不保证100%。
新版tile GPU p95为5.37/8.23/8.63ms；后两项略高于历史8ms候选，不能宣称全部历史交互门槛通过。取消到回收p95为4.99/5.28/2.82ms，均低于100ms。
32×18线性颜色/二阶矩逐值完全一致；全部帧、相机序列与恢复帧的来源及统计一致。required GPU另验证257×129非整除尺寸、反向tile遍历、介质全部复用组合的逐值一致。

## 默认相机与实际worker

三轮关闭validation，默认Sponza、默认相机、默认4反弹；1600×1200每模式4spp、2560×1440每模式2spp。
每次切换都外持上一张显示图像，测试百万spp目标的在途取消和恢复；没有实际完成百万spp。

| 模式 | 1600×1200热交付间隔 ms | 2560×1440热交付间隔 ms |
| --- | --- | --- |
| Classic MIS | 45.67 | 49.16 |
| ReSTIR initial | 137.10 | 246.50 |
| ReSTIR temporal | 319.45 | 589.36 |
| ReSTIR temporal + spatial | 558.84 | 1061.33 |

表为排除每次首交付后的间隔中位数，包含tone/交接和Windows消费者轮询，不是窗口FPS；高分辨率复用仍有明显GPU成本。
冷启动12.58–13.03秒包含导入、编译和AS准备，未优化或混入热加速。owned峰值2.103 GiB，实际worker取消最大12.90ms；三轮关闭后无CPU活分配，所有Renderer性能进程无CPU/VMA活分配和诊断。

## 复现与范围

```powershell
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case courtyard-clear --algorithm spatial --tile 256 --full-preview --output out/media/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/media/desktop --desktop-media
python spec/benchmarks/2026-10-20-media-capacity/verify.py
```

验证层数据与性能数据分开；本机仅对进程设置DISABLE_VULKAN_OBS_CAPTURE=1，required Khronos/sync仍启用。
未运行全工程、Debug、其他GPU硬件或完整M19.8高spp质量/等误差矩阵；本结果不改变Classic默认选择或原等误差结论。
当前配置未启用Luau，捕获验收检查真实registry、IPC和Python示例，单独记录Luau示例未运行。
