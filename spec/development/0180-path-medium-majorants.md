---
id: "0180"
created_at: "2026-11-17T11:44:26+08:00"
updated_at: "2026-11-19T10:08:48+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
---

# 0180 光追日落大气与云优化

## 目标与计划

先冻结并测量当前真实GPU低太阳Sponza基线，再实现独立零密度剔除和保守局部上界。使用原生开关进行消融，记录GPU每spp、逐tile、固定工作量/等时间质量和显存；维持现有物理/统计门槛，完成后独立提交并推送。

## 状态与验收契约

上界与原云数据归同一个不可变GPU owner，budget覆盖所有数据和在途旧代。场变更上传新候选，环境参数变化复用场；失败不更换已完成状态。追踪预算包括局部区间遍历，任何低估/无进展均失败。零剔除要求同样本≤1e-6，局部上界使用既有8 seed统计及解析参考，不把改变随机轨迹误判为画质偏差或把未收敛结果写为通过。

## 进度

1. 改动前冻结109559f二进制、相关源码及输入，真实低太阳Sponza三轮通过，原始GPU每spp p50为8.433/8.446/11.758ms；短预热有明显频率波动，不与最终更长预热直接求比值。
2. 实现weather/base粗格完整插值支撑最大值、局部上界零密度粗筛、weather/profile/base逐级严格零剔除。默认保留全局追踪和原随机序列；粗格edge=8，默认128³/128²场增加4608字节。
3. 分段局部majorant也已实现：空气解析上界、天气空格跳过、细格边界计费、短光学区间全局回退、大位移全局回退；独立编译变体。多轮预跑显示额外遍历/着色器开销超过收益，因此原生`gpu_local_majorants`/benchmark `--local-majorants`保持显式实验、默认false。
4. 无有效云且未开启分段实验时直接复用原着色器，避免引入无效变体。策略变化重置Classic/ReSTIR历史；原场/上界共享owner与预算。Capture shader指纹覆盖新增依赖。
5. CPU 312,513个上界断言和真实GPU零剔除同序列/透射/混合低太阳统计通过。实际编辑器及capture构建、mesh/voxel/四DI/后台GUI定向回归完成。
6. 含NanoVDB的Release构建，256批预热+20批测量，33次串行运行全部成功：日落4.992→4.429ms/spp（-11.28%），云内8.754→6.789ms（-22.44%）；厚云+2.30%，无云与纯mesh为未改变算法的控制组。日落全尺寸20spp线性图像及五场景均值/二阶矩同样本最大差0。分段实验6.388ms，比原算法慢27.96%，保持默认关闭。
7. Sponza 4×4、8 seed质量长链完成，但参考N/4N误差2.0894%超过0.5%；quality_spp/time保留null，不降低门槛。独立混合低太阳CPU参考N/4N为0.3422%，GPU/CPU集合误差0.0671%，通过既有数值门槛。

## 验证与交付

| 证据目录（out/verify） | 配置与范围 | 结果 |
|---|---|---|
| 20261117-130131-787428b2 | windows-editor / RelWithDebInfo；CPU上界与8 seed GPU解析/混合参考 | 2/2通过 |
| 20261117-131035-5bdd3d7d | 同配置；mesh/air/cloud、ReSTIR、透视正交、历史/失败、capture及后台GUI；构建dk_pathmedia_benchmark、dk_pathtracing_probe、dk_pathtracing_reference_tests、dk_editor_app、dk_run、dk_ctl | 14/14通过 |
| 20261117-132018-27703d98 | windows-voxel-editor / Release；构建dk_pathtracing_probe、dk_pathmedia_benchmark、dk_editor_app，voxel_medium_numeric/schedule/state | 3/3通过 |
| 20261117-132520-518699c3 | 同配置；测试夹具变量遮蔽警告修正后的schedule定向复跑 | 1/1通过 |

均经scripts/verify.ps1显式Target/TestRegex；GPU required/Khronos/sync开启，无跳过。未执行全量引擎或Debug矩阵。性能/质量命令、精确工作量、冷编译成本、显存口径和限制见[性能报告](../benchmarks/2026-11-17-path-medium-optimization.md)，[结构化结果](../benchmarks/2026-11-17-path-medium-optimization.json)和冻结证据（历史文件 `b8b0e7e:spec/benchmarks/2026-11-17-path-medium-optimization.raw.zip`；[现存结果](../benchmarks/2026-11-17-path-medium-optimization.md)）。测量后仅整理一行C++缩进，已核对所有归档源文件去空白后完全相同；未重新计时或重复功能测试。

实现范围为PathTraceRenderer、共享介质shader、GPU场owner、capture依赖指纹和定向测试/benchmark；无新第三方依赖、持久场景字段或Console命令。后续优先独立诊断既有128spp数值域拒绝；分段追踪需获得净收益后再评估默认策略，不能把本次实验自动启用。

## 发现及处理

- 最初单线程一次65536样本的长水平局部追踪发生device lost；参考测试按256样本分批并保留连续样本编号、总样本数和统计门槛，随后通过。未修改TDR/驱动设置，也不把该现象当作已证明的驱动缺陷。
- AMD switchable隐式层报告API版本警告导致一次required检查失败；沿仓库既有办法仅对子进程设`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，Khronos与同步验证保持开启。
- 长链128spp在冻结旧二进制/旧着色器和新关闭优化模式均触发既有numerical-domain拒绝。正式吞吐保持原20spp范围；预热循环同一0..15样本范围，明确不把128spp写成通过。该问题留后续独立诊断。
- 最初动态uniform算法分支明显放大开销，改成惰性缓存独立编译变体；只把创建完整成功的变体发布到cache。

最终文档检查：check-spec.ps1定向检查5份Markdown及全局入口/索引通过；git diff --check通过。证据包CRC、SHA与归档源码哈希逐项通过；解包到独立目录、将线性图像路径改成不存在的旧根后，计时/同样本/质量报告仍可复算，证明不依赖原out目录。
