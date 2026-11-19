---
id: "0177"
created_at: "2026-11-17T09:25:00+08:00"
updated_at: "2026-11-19T10:08:48+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
---

# 0177 独立云积分分辨率

## 目标与设计依据

第一批第二阶段：按照 render-clouds 的实施计划，提供每轴 1/2 与 1/4 云积分分辨率，默认保持 1/2。网格、深度与最终 HDR/色调映射维持完整尺寸。

## 设计与状态契约

`cloud_resolution_divisor` 为 2 或 4；只影响 temporal 云积分/历史纹理。reference 始终全分辨率。UI 为会话设置；capture/sequence 接受可选参数并在 provenance 返回有效设置。历史质量键包含该值，变化后拒用旧历史。所有尺寸向上取整，历史预算使用实际纹理尺寸，奇数尺寸不越界。失败不发布候选历史，默认参数与旧调用兼容。

## 实际变更

完成统一积分/历史尺寸、重投影、实际历史纹理预算、UI Cloud resolution、capture/sequence 参数与provenance，以及原生诊断程序 `--resolution-divisor`。旧调用默认2，几何/深度/HDR维持完整尺寸。

## 验证记录

RelWithDebInfo 定向运行092914：11项CPU及4项GPU（resolution/optimization/orthographic/history）通过，capture首次失败：整数schema缺少引擎要求的token标记。改为公共integer helper后，093612命令边界、实际UI帮助与capture **3/3通过**，验证commands.list/describe、17×19 quarter capture及冻结provenance。

真实GPU/sync四预设1/4质量：`dk-cloud-benchmark --quality --validation --resolution-divisor 4 --preset <preset> --output <path>`，58组参考比较全部通过，包含32预热+120稳定帧及运动/云内外/太阳盘；沿用NRMSE≤.05、p99/max≤.12、云T≤.02、太阳≤.002。原始线性图像、日志和PPM见ZIP（历史文件 `b8b0e7e:spec/benchmarks/2026-11-17-cloud-resolution-quality.zip`；[现存结果](../benchmarks/2026-11-17-cloud-optimization.md)），[摘要](../benchmarks/2026-11-17-cloud-resolution-quality.json)。文档和diff检查通过。三轮分辨率性能对照与第三阶段同输入一起归档；未运行全量/Debug矩阵。

## 偏差与决策

1/4 为显式画质选项，不预设四倍总帧提速，完整分辨率回退仍可能占主要时间。

## 遗留问题与下一步

第三阶段优化重建回退，保留原算法作为对照。

## 修改记录

- 2026-11-17T09:25:00+08:00：确立分辨率、默认值、历史及命令契约。
