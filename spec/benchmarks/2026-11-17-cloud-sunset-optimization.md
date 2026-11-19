# 云优化第二批：日落回退与太阳查询（2026-11-17）

默认启用平滑区重建和严格零太阳密度剔除。默认尺寸日落耗时减少 23.55%；clear 不运行云算法。固定参考与两档质量门槛通过。

RTX 4070 Laptop、驱动 596.49。1080p，三轮独立进程，表内为 GPU 时间中位数，单位 ms。

| 天气 | 积分尺寸 | 原算法 | 两项启用 | 耗时减少 |
|---|---:|---:|---:|---:|
| clear | 1/2 | 2.571 | 2.426 | 5.66%（无云，无算法收益结论） |
| sparse | 1/2 | 18.827 | 13.641 | 27.55% |
| overcast | 1/2 | 29.049 | 24.901 | 14.28% |
| sunset | 1/2 | 44.045 | 33.673 | 23.55% |
| sunset | 1/4 | 50.024 | 37.885 | 24.26% |

```powershell
python scripts/benchmark-cloud-sunset.py out/benchmarks/my-new-sunset-run
```

[性能结果](2026-11-17-cloud-sunset-optimization.json) · [质量上界](2026-11-17-cloud-sunset-quality.json)
