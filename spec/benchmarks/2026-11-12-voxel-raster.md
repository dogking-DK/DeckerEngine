---
created_at: "2026-11-12T11:25:00+08:00"
updated_at: "2026-11-19T10:05:01+08:00"
---

# M23.7 稀疏体素光栅基线

12 输入、48 视图的 GPU submission 中位数为 0.212–1.127 ms，最坏视图 p95 为 1.249 ms。旧/新查询中位比 0.996–1.010，未测得稳定时间收益。

RTX 4070 Laptop、Windows Release。三轮，每视图共 24 个稳态样本；全部用例退出时 tracked CPU live allocations 为零。

[光栅矩阵采集](../guides/voxels.md#光栅质量与性能基线) · [完整最终指标](data/2026-11-12-voxels/summary.json)
