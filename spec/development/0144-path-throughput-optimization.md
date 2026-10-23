---
id: "0144"
created_at: "2026-10-23T09:42:00+08:00"
updated_at: "2026-10-23T10:15:00+08:00"
status: completed
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
- B已完成：新增默认1、范围1..8的tiles_per_submission；同一pass内共享参数/诊断分配、Graph和提交，按设备alignment为每dispatch绑定独立区域；组数按射线/候选总工作进一步收缩。完成后先合并全部错误，成功才归约统计和推进tile；batch和当前submit两个取消token均在录制dispatch前检查。
- 新增两个group GPU用例覆盖表面/介质五种算法组合、1/4/8分组、尾组/反序、全部统计和逐值图像、独立tracking及超额、后续提交失败/取消/恢复，以及真实小额度下8tile失败、单tile恢复。性能程序新增--tiles-per-submission（默认1）并报告实际组数。
- C已完成：Editor使用4tile组，tone/验收读回与下一候选重叠；使用pending ticket避免三槽满时重复begin。支持时路径worker改用同族queue1，单队列回退保留；GUI等待和完整相机帧防饥饿保持。过期/取消的linear票据完成前仍保留，排空各分支最多等待1ms而不空转。
- worker回归扩展三槽重叠像素对自身HDR、queue1→0图像交接、活动producer销毁后图像保活与queue0回退。无新依赖、物理采样或命令变化。性能[报告](../benchmarks/2026-10-23-path-throughput.md)和[精简汇总](../benchmarks/2026-10-23-path-throughput-summary.json)已保存，原始报告/冻结程序与复算脚本在out。

## 验证记录

- 基线已完成：61770f7、本机RTX 4070 Laptop/596.49、Release，3轮默认worker及4模式×3轮renderer共15个进程全部通过，原始数据/冻结shader和二进制保存在out/optimization/20261023-path-throughput/before。Classic存在明显轮间波动，保留全部数据。
- A：`out/verify/20261023-094603-23d1c22b`，Release构建dk_pathtracing_probe，11项required RT/validation/sync检查通过，0失败/跳过。覆盖时空/介质调度图像与全部统计、tracking错误、状态、旧owner、Classic渐进/预算/失败。
- 该子进程设置本机AMD层JSON明确支持的`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，避免旧隐式层版本告警；required Khronos/sync保持开启。未更改系统配置。
- B首轮`out/verify/20261023-095426-d9994464`：既有11项通过，两个新增用例失败，分别暴露测试表面模式未显式surface_only和多个Graph Pass重名；修正输入及为组内Pass编号后，`out/verify/20261023-095759-36345cde`两项复验通过，0失败/跳过。后一次同时补足正消光tracking隔离/超额、非法0/9分组、小额度恢复和双取消token检查。
- C：`out/verify/20261023-100101-01bc7878`，7项通过，0失败/跳过；覆盖图像交接、worker、默认表面/介质desktop及三个真实后台GUI/capture。最终保留过期linear票据的排空修正后，`out/verify/20261023-100541-b49f6086`复验worker及三个GUI共4项通过，0失败/跳过。
- 合并实际范围，共20个不同GPU检查最终通过。前后共30个性能进程全部通过，12对32×18图像颜色/二阶矩逐位一致，2,688项统计精确一致；完整本地汇总脚本复算通过。改后采集为6dac811加已重建Editor修改，manifest保存工作区身份及SHA256。
- 三轮1600×1200热交付间隔中位数：Classic41.004→32.539 ms、initial56.606→41.568、temporal225.272→48.006、spatial389.318→56.086；2560×1440分别45.272/71.157/84.422/98.642 ms。不是窗口FPS。
- 640×360同输入提交数6/12/18/24→2/4/6/8；CPU准备下降41%–52%。Classic运行波动及initial GPU区间增加保留，取消中位数12.584→13.611ms、最大24.8575ms，未宣称同比改善。所有轮次详见报告。
- 文档检查最终通过8份Markdown、591个本地链接及全局元数据/索引/测试入口/JSON，git diff --check通过。未运行全工程、Debug、其他设备或完整高spp质量认证，不改变原等误差默认结论。

## 偏差与决策

- 保留Surface only完整帧相机节奏，直接每UI帧取消会导致长帧永远无法发布；由提交/队列改进缩短这段等待。
- 不引入subgroup新能力、第三方依赖或新的公开命令。

## 遗留问题与下一步

计划A/B/C均已完成。初始RIS短pass的驱动等待、Classic轮间波动和冷启动仍需后续专项测量；本次不承诺100%硬件利用率或60fps，也不扩大至去噪/重建或采样算法变更。

## 修改记录

- 2026-10-23T09:42:00+08:00：建立优化设计与分阶段计划。
- 2026-10-23T10:15:00+08:00：三个阶段实现/定向验收、三轮性能与数值复算完成；保留全部实际限制及失败记录。
