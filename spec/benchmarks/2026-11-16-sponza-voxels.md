---
created_at: "2026-11-16T15:25:15+08:00"
updated_at: "2026-11-19T10:05:01+08:00"
---

# OpenVDB 网格体素化：Sponza 实测

提高体素分辨率明显增加转换和多反弹路径成本。256 档两种分辨率与投影下光栅 GPU 中位数为 0.281–0.860 ms；11 spp 路径仍有噪声。

Sponza glTF，RTX 4070 Laptop、MSVC Release、OpenVDB 12.0.1。64/128/256 三档各运行三个独立转换与 GPU 进程。

| 最长轴 | 占用格数 | 调色板 | 转换 ms | 链路 ms | NanoVDB MiB | 同域稠密 Int32 MiB | 进程工作集峰值 MiB |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 64 | 39276 | 109 | 143.95 | 671.38 | 1.902 | 0.299 | 218.7 |
| 128 | 164916 | 153 | 237.60 | 801.86 | 4.170 | 2.209 | 248.5 |
| 256 | 802223 | 187 | 659.10 | 1352.50 | 15.165 | 17.057 | 343.3 |

```powershell
python scripts/benchmark-mesh-voxels.py --assetc out/build/windows-voxel-editor/bin/Release/dk-assetc.exe --benchmark out/build/windows-voxel-editor/bin/Release/dk-voxel-joint-benchmark.exe --output out/sponza-benchmark-new
```

[最终汇总](data/2026-11-16-sponza-voxels/summary.json) · [256 档路径图](data/2026-11-16-sponza-voxels/sponza-256-path.png)
