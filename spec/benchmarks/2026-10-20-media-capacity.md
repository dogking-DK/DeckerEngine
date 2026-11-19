# Scene media 容量与吞吐优化

Scene media 使用设备可用额度、按需增长的数组与 tile 256。同输入三轮吞吐提高 1.43–2.40 倍；时间与空间复用的 tile GPU p95 略超 8 ms。

RTX 5090 D v2、驱动 610.88，Ryzen 9 9950X3D，Release。同输入比较固定样本和物理模型。

| 模式 | 每spp wall ms（前→后） | 加速 | CPU record-submit ms（前→后） | GPU ms（前→后） | 提交数（前→后） |
| --- | --- | --- | --- | --- | --- |
| Classic MIS | 47.52 → 19.82 | 2.40× | 10.17 → 1.49 | 23.48 → 17.23 | 60 → 6 |
| ReSTIR initial | 69.16 → 48.51 | 1.43× | 21.63 → 3.12 | 31.58 → 30.52 | 120 → 12 |
| ReSTIR temporal + spatial | 141.68 → 83.32 | 1.70× | 40.48 → 4.41 | 81.67 → 76.50 | 240 → 24 |

```powershell
out/build/windows-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case courtyard-clear --algorithm spatial --tile 256 --full-preview --output out/media/spatial.json
out/build/windows-editor/bin/Release/dk-editor-render-benchmark.exe projects/demo out/media/desktop --desktop-media
```

[最终汇总](2026-10-20-media-capacity/summary.json)
