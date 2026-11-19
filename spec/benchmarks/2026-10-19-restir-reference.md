---
created_at: "2026-10-19T13:09:00+08:00"
updated_at: "2026-11-19T10:08:48+08:00"
---

# M19.1 CPU 初始 RIS 基线

8 候选的初始 RIS 在三种灯数下均降低误差，同 spp 的 CPU 耗时均增加。

Ryzen 9 7940HX、MSVC Release。一个 Lambert 接收点，每模式 1024 个样本、8 个独立 seed，对照独立求积参考。

| 灯数 | 参考辐亮度 | MIS NRMSE | 初始 RIS NRMSE | MIS wall 中位数 ms | 初始 RIS wall 中位数 ms |
| --- | --- | --- | --- | --- | --- |
| 1 | 0.1662444163 | 0.740% | 0.470% | 0.306 | 0.808 |
| 100 | 0.1662380451 | 1.214% | 0.466% | 2.385 | 3.210 |
| 1000 | 0.1681903809 | 1.496% | 0.524% | 20.758 | 21.664 |

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release -Target @('dk_pathtracing_reference_tests','dk_restir_reference_benchmark') -TestRegex '^dk\.pathtracing\.(RESTIR |restir_reference_benchmark_cpu$)' -Reason 'CPU RIS reference and benchmark entry'
& ./out/build/windows-path-media-cpu/bin/Release/dk_restir_reference_benchmark.exe --samples 1024 --output out/restir-reference.json
```

[固定结果](2026-10-19-restir-reference.json)
