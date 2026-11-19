# M16.11 路径追踪质量与性能交付（2026-10-15）

三个输入的 CPU 参考收敛，默认 1 GiB 额度通过。1080p 每批 1 spp 均未达 16.67 ms；GGX 在 1024 spp 仍超过 2% 误差。

固定三个场景、八个 seed。下表是线性 RGB 归一误差百分比。时间基线独立运行三轮。

| 输入 | CPU N/4N | CPU/GPU | 64 spp | 256 spp | 1024 spp | 2%候选 |
| --- | --- | --- | --- | --- | --- | --- |
| area-lambert | 0.11006 | 0.0001821 | 6.2084 | 3.2218 | 1.4511 | 1024 spp达到 |
| ggx-box | 0.16926 | 0.0005156 | 8.6038 | 4.8883 | 2.2824 | 本采样阶梯未达 |
| disk-mask | 0.02470 | 0.0000037 | 1.6874 | 0.6796 | 0.3679 | 64 spp达到 |

```powershell
python scripts/verify-pathtracing-evidence.py spec/benchmarks/2026-10-15-pathtracing
```

[离线样本](2026-10-15-pathtracing/archive.json) · [采集指南](../guides/pathtracing-benchmark.md)
