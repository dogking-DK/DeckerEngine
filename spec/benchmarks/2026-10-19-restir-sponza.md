---
created_at: "2026-10-19T18:54:00+08:00"
updated_at: "2026-11-19T10:05:01+08:00"
---

# M19.8 Sponza ReSTIR 质量与性能验收

十场景仅云内与云上方通过参考认证。两者的四种模式均在 16384 spp 达到 2% 误差，ReSTIR 等误差速度比为 0.357–0.581×，未达 2×。默认保持 Classic MIS。

RTX 5090 D v2、Windows Release。640×360 性能各三轮；4×4 质量链每组 8 seed。交互、画质和性能使用各自的测量口径。

| 模式 | 三项交互均达标 | 最坏 tile p95 ms | 最坏取消 p95 ms | owned peak MiB | VMA block 观测峰值 MiB |
| --- | --- | --- | --- | --- | --- |
| classic | 6/10 | 2.436 | 11.775 | 869.217 | 580.017 |
| initial | 7/10 | 1.965 | 7.436 | 890.311 | 836.017 |
| temporal | 2/10 | 1.856 | 2.735 | 890.311 | 836.017 |
| spatial | 0/10 | 1.892 | 3.118 | 897.342 | 836.017 |

```powershell
python scripts/benchmark-restir-sponza.py --directory spec/benchmarks/2026-10-19-restir-sponza-5090 --verify-quality
```

[样本、完整汇总与历史归档复算](2026-10-19-restir-sponza-5090/README.md)
