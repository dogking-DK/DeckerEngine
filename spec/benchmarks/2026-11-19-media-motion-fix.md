# Sponza 运动路径 GPU 错误修复前后对照

同一台 RTX 4070 Laptop GPU（驱动 596.49）上，修复前三档共启动 12 次，有 3 次中止。修复后共启动 9 次，三档各 3 次全部完成，均未重试。另用原生分辨率逐帧提交 400 个确定姿态，越过了此前两次在同一像素中止的第 342 帧。固定机位 Classic MIS 的低太阳角用例此前在样本 36 拒绝，修复后完成 128 spp。两处 GPU 修复与失败时的操作数见 [0186 开发记录](../development/0186-path-gpu-motion-errors.md)。

## 移动视角实测

沿用[修复前的 Sponza 测量](2026-11-19-media-resolution.md)。后台编辑器实际视口为 1629×989，开启大气、稀疏体积云，冻结云场时间 17 秒，Scene media 完整路径 4 次反弹，首表面直接光用 ReSTIR v2、8 个初始候选以及时间和空间复用。每轮用 SDL 注入相同的 2 秒周期水平转向输入，至少预热 2 秒并观察 20 次新视角，随后计时 10 秒。表中为三轮中位数，每轮只计已经交付到 GUI 的新相机画面。每档长边、输入、预热、计时和轮次顺序均与基线相同。

| 运动尺寸 | 修复前 FPS | 修复后 FPS（三轮范围） | 中位数变化 | 输入至交付 P50，前 → 后 | 中止 / 尝试，前 → 后 |
| --- | ---: | ---: | ---: | ---: | ---: |
| 原生 1629×989 | 3.81 | 3.88（3.86–3.93） | +1.9% | 292 → 288 ms | 1/4 → 0/3 |
| 640×388 | 19.61 | 21.28（20.97–21.53） | +8.5% | 67 → 66 ms | 1/4 → 0/3 |
| 192×116 | 60.62 | 67.18（62.02–68.25） | +10.8% | 36 → 31 ms | 1/4 → 0/3 |

三档均未观察到帧率回退。每个版本每档只有三轮完整计时，这些差值包含运行间波动，不能据此把提速幅度归因于着色器修复。用于定位错误的着色器诊断与逐帧回读均在计时之外。

## 同机位画质

每档在偏航 89°、91°、93° 导出原始 3 spp 运动帧。逐项核对修复前后的相机矩阵、镜头、场景、云场、随机种子和首个采样 epoch。九对运动帧的线性 FP32 辐射值存在微小差异，单通道绝对差最大为 3.58×10⁻⁷，单张线性 RGB RMSE 最大为 4.50×10⁻¹⁰。按编辑器曝光、色调映射、量化及双线性放大重建到原生网格后，九对显示图的差异像素数均为零。

以下误差统一使用修复前同一机位的独立 Classic MIS 96 spp 参考，取三个姿态平均。修复后另行生成了 48/96 spp 参考供对照。图中 640 档的放大滤波平滑了部分 3 spp 噪声，192 档仍明显丢失拱廊和挂布细节。

| 运动尺寸 | 显示 RGB RMSE，修复前后相同 | 8×8 区域均色 RMSE，修复前后相同 | 8×8 亮度 SSIM，修复前后相同 |
| --- | ---: | ---: | ---: |
| 原生 1629×989 | 0.08727 | 0.03241 | 0.32889 |
| 640×388 | 0.07707 | 0.03628 | 0.36414 |
| 192×116 | 0.08651 | 0.06612 | 0.33003 |

[三档与参考的全图和同位置裁切](2026-11-19-media-resolution.png)与本次重新导出的对照 PNG 的 SHA-256 相同。[各轮帧率、逐图像素差、哈希和同参考误差](2026-11-19-media-motion-fix.json)

## 复测命令

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
$bin = 'out/build/windows-voxel-editor/bin/Release'
$result = 'out/benchmarks/sponza-media-v2-motion-fixed-repeat'
python scripts/benchmark-media-resolution.py --editor "$bin/dk-editor.exe" --worker "$bin/dk-editor-render-benchmark.exe" --project projects/demo --output $result
python scripts/benchmark-media-resolution.py --editor "$bin/dk-editor.exe" --worker "$bin/dk-editor-render-benchmark.exe" --project projects/demo --output $result --phase replay
python scripts/summarize-media-resolution.py $result "$result/comparison"
& "$bin/dk-pathmedia-benchmark.exe" --root projects/demo --case low-sun --tile 256 --tiles-per-submission 4 --performance-samples 128 --output "$result/low-sun-128.json"
```

400 姿态重放与 SDL 计时分开执行。原生 worker 每个姿态交付 3 spp 后才提交下一姿态，因此同一组随机数总会落到同一个相机。SDL 计时按真实时间推进，在负载波动时交付帧会落在不同的转向位置。
