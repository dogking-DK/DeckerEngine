---
created_at: "2026-10-19T13:09:00+08:00"
updated_at: "2026-10-19T13:09:00+08:00"
---

# M19.1 CPU 初始 RIS 基线

Windows / AMD Ryzen 9 7940HX / MSVC 19.51.36260.0 / Release，使用
`out/build/windows-path-media-cpu`；Graphics Device/Graph/Shaders 与 GPU Pathtracing 均关闭。
本记录只比较 CPU 普通 MIS 和 8 候选初始 RIS，无时空复用、RTXDI、DLSS 或去噪。
接口与误差协议见[设计](../design/render-restir.md#m191-cpu-实施契约)，验证见[0134](../development/0134-restir-reference.md)。

## 固定输入与复现

[程序输入](../../tests/RestirTestSupport.hpp)使用固定 Scene/Entity/Asset ID，Z=0 的 Lambert 接收面，
Z=2 向下发光三角形，按平方网格布置 1/100/1000 灯，面积乘辐亮度之和为 8（受 float 取整影响）。
固定射线从 (0,0,1) 指向 (0,0,-1)，反弹 1、RR 关闭、环境/太阳为零。
每模式 1024 个新路径样本，8 个 seed 为 101+7919*i，共 48 次估计。
每灯均匀重心求积 64×64 与 128×128，独立于生产 BSDF/选灯/遮挡采样。
这是一个接收点的基础协议；不代表全屏、运动或复杂场景覆盖。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release -Target @('dk_pathtracing_reference_tests','dk_restir_reference_benchmark') -TestRegex '^dk\.pathtracing\.(RESTIR |restir_reference_benchmark_cpu$)' -Reason 'CPU RIS reference and benchmark entry'
& ./out/build/windows-path-media-cpu/bin/Release/dk_restir_reference_benchmark.exe --samples 1024 --output out/restir-reference.json
```

原始结果为[固定报告](2026-10-19-restir-reference.json)：包括输入指纹、配置/编译器、seed、均值/二阶矩、
射线/三角形测试/候选数与每次 wall 时间。无历史链时 spp 就是新路径数量，8 个候选不等于 8 spp。
聚合 NRMSE 为 8 个独立估计相对求积参考的均方根误差；下表耗时为这 8 次的中位数，
只作本机 CPU 基线，不是三轮 GPU 等误差加速验收。

## 实测

| 灯数 | 参考辐亮度 | MIS NRMSE | 初始 RIS NRMSE | MIS wall 中位数 ms | 初始 RIS wall 中位数 ms |
| --- | --- | --- | --- | --- | --- |
| 1 | 0.1662444163 | 0.740% | 0.470% | 0.306 | 0.808 |
| 100 | 0.1662380451 | 1.214% | 0.466% | 2.385 | 3.210 |
| 1000 | 0.1681903809 | 1.496% | 0.524% | 20.758 | 21.664 |

三组求积细化绝对差分别为 1.389e-5、2.313e-7、3.336e-8，均通过预定参考门槛。
两种模式的 6 组跨 seed 均值均同时满足相对误差与 5 标准误差门槛；
MIS 最大均值相对差为 1.130%，初始 RIS 为 0.185%。完整性检查确认各场景各模式 8 个 seed、共 48 次结果。

初始 RIS 的低噪声伴随候选评估开销，三组同 spp wall 均未降低；这不构成 GPU 提速结论。
单灯无需指望同样的收益，当前 1000 灯输入也没有空间/时间复用。
GPU 后续使用相同程序灯分布，加多接收点/遮挡/GGX及实际 Sponza，分别报告等时间误差和达到 2% 误差的时间。
完整 ReSTIR 多灯等误差耗时减半仍是待验证目标。
