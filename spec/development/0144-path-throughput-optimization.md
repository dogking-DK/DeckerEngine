---
id: "0144"
created_at: "2026-10-23T09:42:00+08:00"
updated_at: "2026-10-23T09:42:00+08:00"
status: in-progress
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
  - ../design/editor.md
---

# 0144 路径追踪统计、提交与Editor吞吐优化

## 目标与设计依据

承接0142/0143及2026-10-20的本机诊断：Editor默认Sponza下Classic与ReSTIR均有明显交付等待，复用统计的全局原子写入在host-preferred status上成本尤其显著。按[路径追踪](../design/render-pathtracing.md)、[ReSTIR](../design/render-restir.md)和[Editor](../design/editor.md)契约优化，不改变分辨率、采样、反弹或估计器。

## 实施与验收计划

1. **A：GPU统计。** device-local诊断、256分片及精确SUM/MAX归约；保留错误和tracking总量。验证时空/介质统计、非整除tile、失败/预算；独立本地提交。
2. **B：有界tile组。** 同pass内1..8 tile共享分配/建图/提交，默认API为1、Editor为4；每dispatch参数/诊断独立且组总工作受预算限制。验证分组数值/统计等价、取消、提交失败、完整帧与owner额度；独立本地提交。
3. **C：Editor重叠。** tone/显示与下批采样重叠、可用时第二队列隔离UI；保留完整相机帧防饥饿与GUI寿命等待。验证真实后台GUI/capture、worker交互/关闭，并三轮同输入复测；独立本地提交。

root负责设计、集成、构建/GPU串行验证和提交；子agent按统计、editor文件边界协作，禁止并行GPU基准。基线先完成默认worker，再冻结shader；原始数据放忽略的out，spec仅保留结论与机器可读摘要。

## 状态与失败保护

Renderer拥有候选线性/表面/reservoir及单在途票据；每组完成后读取诊断，任何错误/取消/过期generation不推进已完成样本。仅最后pass/epoch的全部tile成功后交换completed；外持旧帧保持不可变并继续计费。统计归约仅观测，不参与采样选择。Editor显示持有上一completed，下一候选使用独立buffer；跨队列仅通过既有完成图像交接。关闭先停止新请求、排空/回收并join，再释放设备使用者。

## 实际变更

- 已先更新三个模块设计并建立本记录。
- A已完成：Renderer的status改为device-local，同一Graph中fill初始化；256个16-word统计分片在完成后按原SUM/MAX归约，错误位和tracking总量保持全局。三个shader与四个调度回归同步，统计不参与采样选择。
- B/C仍在实现，尚未验收。

## 验证记录

- 基线已完成：61770f7、本机RTX 4070 Laptop/596.49、Release，3轮默认worker及4模式×3轮renderer共15个进程全部通过，原始数据/冻结shader和二进制保存在out/optimization/20261023-path-throughput/before。Classic存在明显轮间波动，保留全部数据。
- A：`out/verify/20261023-094603-23d1c22b`，Release构建dk_pathtracing_probe，11项required RT/validation/sync检查通过，0失败/跳过。覆盖时空/介质调度图像与全部统计、tracking错误、状态、旧owner、Classic渐进/预算/失败。
- 该子进程设置本机AMD层JSON明确支持的`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，避免旧隐式层版本告警；required Khronos/sync保持开启。未更改系统配置。
- 待运行：按阶段选择required RT/validation/sync、progressive/ReSTIR/介质调度状态、worker/后台GUI/capture；文档与diff检查。
- 未计划全工程、Debug/Release双矩阵、其他设备或完整高spp质量认证；本次不改变原ReSTIR等误差默认结论。

## 偏差与决策

- 保留Surface only完整帧相机节奏，直接每UI帧取消会导致长帧永远无法发布；由提交/队列改进缩短这段等待。
- 不引入subgroup新能力、第三方依赖或新的公开命令。

## 遗留问题与下一步

按A/B/C顺序验收；验证未完成前不宣称性能收益或标记完成。

## 修改记录

- 2026-10-23T09:42:00+08:00：建立优化设计与分阶段计划。
