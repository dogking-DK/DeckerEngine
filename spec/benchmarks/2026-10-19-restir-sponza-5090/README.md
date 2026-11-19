# M19.8 RTX 5090 D v2 离线样本

[正式报告](../2026-10-19-restir-sponza.md)记录十场景四模式的最终结论。[汇总](summary.json)、[输入 manifest](manifest.json)、[设备环境](environment.json)与[原验收结果](validation/summary.json)来自同一次测量。

仓库中的 40 个 quality-*.json.gz 是原实验的 4×4 线性样本和参考链。以下命令在 CPU 上重算全部十个场景的误差阶梯、等误差/等时间、均值与收敛状态，逐项对比 summary.json 中的 quality 字段。

```powershell
python scripts/benchmark-restir-sponza.py --directory spec/benchmarks/2026-10-19-restir-sponza-5090 --verify-quality
```

性能逐帧记录和全尺寸图像保存在本地 Git 历史。需要复算全部质量、性能、图像和哈希时，在包含 `f1e5734` 的完整克隆中运行：

```powershell
$sample = 'spec/benchmarks/2026-10-19-restir-sponza-5090'
$replay = Join-Path 'out' ('restir-replay-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $replay | Out-Null
git archive --format=zip "--output=$replay/evidence.zip" f1e57345c1e98aff7456945896dd3205a5275e7c $sample
Expand-Archive -LiteralPath "$replay/evidence.zip" -DestinationPath $replay
python scripts/benchmark-restir-sponza.py --directory "$replay/$sample" --verify-archive
```

[重新采集的配置与步骤](../../guides/path-media-benchmark.md#m198-restir-四模式-sponza-验收)
