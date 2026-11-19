# 光追大气与云：保守密度查询优化（2026-11-17）

默认启用保守局部密度上界和严格零值剔除，同样本图像一致。分段追踪在日落比原算法慢 27.96%，保持关闭。厚云默认路径退化 2.30%。Sponza 长链参考尚未收敛，128 spp 在冻结旧版和关闭优化模式下均触发数值域拒绝。

RTX 4070 Laptop、驱动 596.49，含体素 Release 构建。每个进程预热 256 批后测量 20 批，三轮。

| 场景 | 原算法 GPU ms/spp | 默认优化 GPU ms/spp | GPU耗时变化 | 原/新 wall ms/spp |
|---|---:|---:|---:|---:|
| 日落 | 4.992 | 4.429 | -11.28% | 6.110 / 5.615 |
| 无云空气（控制组） | 3.012 | 3.273 | +8.65% | 4.120 / 4.122 |
| 厚云 | 4.347 | 4.447 | +2.30% | 5.436 / 5.539 |
| 云内 | 8.754 | 6.789 | -22.44% | 10.022 / 7.880 |
| 纯mesh（控制组） | 2.383 | 2.389 | +0.24% | 3.428 / 3.380 |

```powershell
python scripts/benchmark-path-medium-bounds.py --binary out/build/windows-voxel-editor/bin/Release/dk-pathmedia-benchmark.exe --output out/benchmarks/path-medium-repeat
python scripts/benchmark-path-medium-bounds.py --analyze --output out/benchmarks/path-medium-repeat
```

[最终比较](2026-11-17-path-medium-optimization.json)
