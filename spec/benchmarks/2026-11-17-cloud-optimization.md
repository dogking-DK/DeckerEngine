# 云优化第一批验收（2026-11-17）

稀疏云与厚云的热历史细化分别缩短 60.2% 与 58.5%。积分尺寸默认 1/2，1/4 显式可选；compact 重建没有稳定净收益，默认关闭。

RTX 4070 Laptop、RelWithDebInfo，关闭 validation / Tracy。下表为 1080p 同一输入三轮 GPU p50 的中位数，单位 ms。

| 天气 | 生产1/2 | 生产1/4 | 实验1/2 | 实验1/4 |
|---|---:|---:|---:|---:|
| clear | 2.424 | 2.421 | 2.418 | 2.422 |
| sparse | 18.884 | 18.227 | 19.259 | 18.573 |
| overcast | 28.846 | 25.402 | 30.109 | 26.298 |
| sunset | 43.768 | 48.814 | 43.228 | 49.485 |

```powershell
python scripts/benchmark-cloud-optimization.py out/benchmarks/cloud-repeat
```

[最终性能对照](2026-11-17-cloud-optimization.json)
