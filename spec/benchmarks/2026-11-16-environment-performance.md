# 大气与体积云性能归因（2026-11-16）

1080p 静态云的主要成本是积分与重建；编辑器重复请求的 33 帧细化增加了交付延迟。低质量对照只用于定位瓶颈。

RTX 4070 Laptop、驱动 596.49，原生 timestamp 开启，Tracy 与 validation 关闭。三轮，表内时间单位为 ms，端到端包含同步等待读回。

| 预设 | 静态 GPU p50 | p50 三轮范围 | 静态 GPU p95 | 运动 GPU p50 | 静态端到端 p50 |
|---|---:|---:|---:|---:|---:|
| clear / 零云 | 2.443 | 2.431–2.616 | 2.655 | 2.545 | 9.580 |
| sparse | 18.720 | 18.698–23.886 | 19.734 | 19.713 | 28.896 |
| overcast | 30.159 | 29.416–30.751 | 33.602 | 31.608 | 40.210 |
| sunset | 46.253 | 42.243–46.735 | 51.012 | 44.329 | 57.267 |

```powershell
python scripts/benchmark-environment-diagnostics.py --build-dir out/build/windows-editor --output out/benchmarks/environment-repeat
```

[汇总数据](2026-11-16-environment-performance.json) · [最终对照图](2026-11-16-environment-performance.png)
