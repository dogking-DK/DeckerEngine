# 默认 Sponza 路径预览：预算与吞吐优化

表面预览改用 tile 256，默认相机与桌面尺寸均能交付。四模式测得 1.28–2.77 倍吞吐；Classic 有轮间波动，等误差质量未重测。

RTX 5090 D v2、驱动 610.88，Ryzen 9 9950X3D，Release，三轮同输入。

| 模式 | 每spp wall ms（前→后） | 加速 | CPU record-submit ms（前→后） | GPU ms（前→后） | 新tile GPU p95 ms | 新取消回收p95 ms |
| --- | --- | --- | --- | --- | --- | --- |
| Classic MIS | 42.92 → 15.50 | 2.77× | 10.30 → 1.84 | 14.87 → 10.49 | 4.67 | 1.04 |
| ReSTIR initial | 46.74 → 31.78 | 1.47× | 20.99 → 3.74 | 10.78 → 13.61 | 4.40 | 11.37 |
| ReSTIR temporal | 77.22 → 60.45 | 1.28× | 28.54 → 5.31 | 33.28 → 41.16 | 7.18 | 2.67 |
| ReSTIR temporal + spatial | 121.00 → 62.92 | 1.92× | 39.10 → 6.26 | 60.55 → 51.91 | 7.06 | 2.85 |

```powershell
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case surface --algorithm spatial --tile 256 --full-preview --output out/preview/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/preview/desktop --desktop-path
```

[最终汇总](2026-10-20-path-preview/summary.json)
