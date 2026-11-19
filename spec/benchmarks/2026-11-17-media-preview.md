# Sponza Scene media 移动预览实测（2026-11-17）

天空开口的 RGB 花斑减少；受光地面和间接光仍有明显噪点。稀疏云下 ReSTIR 的新视角更新率从 78.13 降至 70.50 次/秒。

RTX 4070 Laptop、Release，Sponza，真实相机输入。两种天气、算法与策略交叉，性能各三轮；96 spp 是有限样本颜色参考。

| 天气 / 算法 | 新视角更新 / 秒 | 输入交付p50 / ms | 输入交付p95 / ms | 停动至全尺寸 / ms |
| --- | --- | --- | --- | --- |
| 无云 / Classic MIS | 79.30 → 77.27 | 30.36 → 30.51 | 36.43 → 36.42 | 273.60 → 290.74 |
| 无云 / ReSTIR DI | 78.94 → 78.29 | 30.30 → 30.21 | 36.17 → 36.31 | 326.78 → 402.12 |
| 稀疏云 / Classic MIS | 78.60 → 75.13 | 30.34 → 30.75 | 36.37 → 37.37 | 273.76 → 329.19 |
| 稀疏云 / ReSTIR DI | 78.13 → 70.50 | 30.31 → 30.50 | 36.32 → 36.65 | 317.39 → 428.53 |

```powershell
python scripts/benchmark-media-preview.py `
  --editor out/build/windows-voxel-editor/bin/Release/dk-editor.exe `
  --worker out/build/windows-voxel-editor/bin/Release/dk-editor-render-benchmark.exe `
  --project projects/demo --output out/benchmarks/media-preview-repeat `
  --mode all --seconds 6 --rounds 3
```

[颜色对照图](2026-11-17-media-preview-color.png) · [最终结果](2026-11-17-media-preview.json)
