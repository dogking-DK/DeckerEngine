---
created_at: "2026-10-23T10:15:00+08:00"
updated_at: "2026-11-19T10:05:01+08:00"
---

# Editor / Sponza 路径追踪吞吐优化

批量提交和显示交接重叠改善四种模式的交付间隔。Classic 在 1600×1200 的三轮结果为 30.167 / 70.658 / 32.539 ms，不能保证固定帧率。

RTX 4070 Laptop、驱动 596.49，Release。默认 Sponza 相机，三轮独立 worker，交付间隔包含消费者轮询。

| 模式 | 1600×1200前→后 ms | 前/后 | 2560×1440前→后 ms | 前/后 |
| --- | ---: | ---: | ---: | ---: |
| Classic MIS | 41.004 → 32.539 | 1.26× | 60.072 → 45.272 | 1.33× |
| ReSTIR initial | 56.606 → 41.568 | 1.36× | 89.643 → 71.157 | 1.26× |
| ReSTIR temporal | 225.272 → 48.006 | 4.69× | 421.408 → 84.422 | 4.99× |
| ReSTIR temporal + spatial | 389.318 → 56.086 | 6.94× | 768.661 → 98.642 | 7.79× |

```powershell
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case surface --algorithm spatial --tile 256 --tiles-per-submission 4 --performance-samples 16 --output out/path-throughput/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/path-throughput/desktop --desktop-path
```

[最终结果](2026-10-23-path-throughput-summary.json)
