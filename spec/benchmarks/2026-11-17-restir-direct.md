# Sponza 移动视角下的 RGB 表面直接光

v2 表面直接光的运动帧误差低于 Classic 与 v1。原生 1629×989 时相对 v1 更新率下降 4.1%，输入 p50 增加 3.2 ms。暗部仍有噪声。

RTX 4070 Laptop、Release，Sponza、clear、单反弹。640×640 三张 1 spp 运动帧分别对照同机位 512 spp 参考。

| 算法 | 线性 RGB NRMSE | 显示 RGB RMSE | 显示色度 RMSE |
| --- | ---: | ---: | ---: |
| Classic MIS | 0.3893 | 0.1434 | 0.01657 |
| ReSTIR v1 | 0.1520 | 0.0757 | 0.01414 |
| ReSTIR v2 | 0.0964 | 0.0435 | 0.00982 |

```powershell
python scripts/benchmark-restir-direct.py --editor out/build/windows-voxel-editor/bin/Release/dk-editor.exe --worker out/build/windows-voxel-editor/bin/Release/dk-editor-render-benchmark.exe --project projects/demo --output out/restir-direct-repeat --mode all
python scripts/summarize-restir-direct.py out/restir-direct-repeat --output out/restir-direct-repeat/summary
```

[运动帧与参考](2026-11-17-restir-direct.png) · [最终结果](2026-11-17-restir-direct.json)
