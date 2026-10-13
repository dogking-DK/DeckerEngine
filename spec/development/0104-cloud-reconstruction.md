---
id: "0104"
created_at: "2026-10-13T15:10:00+08:00"
updated_at: "2026-10-13T16:08:18+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-pipeline.md
---

# 0104 M15.4 半分辨率云重建与每视图历史

## 目标与设计依据

按[优化与时域契约](../design/render-clouds.md#m154-优化与时域实施契约)完成保守跳跃、早停、
保边重建和风/相机重投影。保留M15.3无历史参考，开工冻结误差、支持范围与历史提交边界。

## 实际变更

- [CloudReference](../../engine/render/clouds/include/dk/render/CloudReference.hpp)定义显式优化参数与CPU预检：半尺寸、96主步/8太阳步默认，
  长云区间至多256步、>100m步段4次主密度采样，低太阳局部|mu|<.2至少16步自遮蔽。
  输出最多1920×1080像素、单轴≤4096；工作≤64Gi，最坏全回退预算区分5空气/2云及2太阳区间。
- [重建Shader](../../shaders/render/cloud-reconstruction.slang)实现保守格子上界跳跃、双T≤1e-4早停、
  半尺寸联合积分、风/相机重投影、3×3邻域夹取和保深度全尺寸合成。太阳盘直接积分并禁早停；
  非中心像素检查3×3透射范围，深度/梯度不可信则直接积分，不用放宽误差容忍掩盖云边缘。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)增加三段Graph与显式入口；
  4个稳定view_id各保留完成源和pending候选，仅GPU完成后发布，所有代次逻辑图像≤256MiB。
  历史只保留3张半尺寸RGBA32F，不保留整帧Graph。候选分配/录制/submit失败不推进完成序号，
  下次拒绝旧历史；参数预检拒绝保持旧状态。跨队列、关闭和资源超额显式拒绝。
- Frame公开积分尺寸/源与候选序号/重置原因和实际GPU工作统计。主密度计数以两个32位原子加进位形成64位，
  避免1080p最坏长射线计数溢出，不引入新的Shader能力。其余计数按像素和步数上界均在uint32范围。
- M15.3无历史reference、共用环境及capture继续原质量。Shader指纹覆盖新文件和采样算法标识。
  共享太阳积分函数增加默认0的最低步数参数，reference调用的步数和模型不变。无依赖升级、命令schema或UI变化。
- [GPU序列用例](../../tests/integration/CloudReconstructionCases.hpp)覆盖数值/静态/运动/风/反遮挡、
  云下/内/上、日落、lit空气MS、纯吸收/太阳解析边界、零云逐位M14、4视图/重置/故障/在途寿命。
  [CPU预算测试](../../tests/unit/CloudReferenceTests.cpp)、构建指南、模块索引和测试选择表同步更新。

## 验证记录

开工冻结HDR nRMSE≤.05、p99及所有像素最大误差≤.12（尺度=max参考亮度p95,.001）；
真实云Tc误差≤.02，均匀纯吸收T≤.002；32预热+120静态帧相邻归一化RMS≤.01。门槛未放宽。
固定generator v1/seed1、默认纹理分辨率，coverage=.85、显式sky=(.015,.02,.03)、time=17.25s，
31×31数值/运动图、15/17尺寸寿命图；512/128/128同模型收敛参考沿用M15.3验证，
云内高度2200m、相机连续平移12m/旋转.002rad并以.03s推进风；mask反遮挡每次80m，共3帧。
另外执行真实1920×1080调度，未将31×31质量图冒充1080p质量/性能验收。

过程中的失败与修复：

- `152306-d64d7f8f`：Shader GetDimensions产生基线外ImageQuery能力。上传已知不可变场分辨率，移除额外能力要求。
- `152509-3bc81c8d`至`153744-242b9e74`：96步长地平线云Tc最大误差.04496；256步后运动帧仍.02131。
  无历史诊断相同，定位主密度积分；长步4中点修复，不改变冻结阈值。
- `154130-e76b2268`：mask历史Tc残留.08896，当前积分误差仅约8.4e-5。历史Tc接受差异由.1收紧至.01。
- `154809-5e9ded7c`：下一mask帧无历史也有.06659误差，全像素诊断`155014-06daa5e2`通过，
  定位到仅两点透射检查漏掉中间云边缘；非中心像素加入3×3范围检查。
  `155119-74420b5d`中优化、开关和完整时域3/3通过。
- `155306-d86ac2a7`：新增日落默认8步太阳自遮蔽p99=.1311、最大=.2037，超标；
  9项CPU和历史寿命均通过。64步诊断`155555-db261a51`确认光照采样原因；
  16步实验`155653-28c3e741`全部边界通过，日落最大=.02942。最终仅低角度提高到至少16步，
  重算五区间预算仍保持64Gi上限，默认配置在1080p预算内。

最终合计21项不同定向用例的最新结果全部通过：9项CPU、12项GPU/真实截图，无跳过。
GPU为NVIDIA GeForce RTX 4070 Laptop GPU、驱动596.49，Debug，required GPU及同步validation，
各probe最终errors/warnings/live均为0。没有运行全量、Release或其他GPU/平台。

- `155306-d86ac2a7`的9项CPU均通过；最终预算公式变更后优化CPU用例在`155845-47c9d39a`再次通过。
- `155845-47c9d39a`的13项中12项通过，包括最终优化/时域/边界/历史、原参考、M15.3三项、
  M14空气合成、基础管线和真实capture。开关用例的新增“超过一半像素重建”断言失败：
  斜射平面存在真实深度梯度，应回退而不应要求其多数插值。平坦均匀场的工作收益检查改用无前景背景，
  保留其余所有前景/mask数值用例与质量门槛。
- 同时检查到阈值0遇透射浮点下溢仍会早停，改为显式要求阈值>0；新增disabled终止计数必须为0的断言。
  最终仅重跑受影响的开关与更新Shader指纹后的capture，`160317-d59eb7ee`中2/2通过，含真实1080p调度。

```powershell
$previousLayer=$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_cloud_reference_tests','dk_render_pipeline_tests') -TestRegex '^dk\.(clouds\.(optimization |composition |optimization_boundaries_gpu_composition|history_gpu_composition)|render\.pipeline\.)' -Reason 'M15.4 extended shell, sunset, absorption, zero-cloud, retry and CPU budget boundary coverage'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_cloud_reference_probe','dk_cloud_reference_tests','dk_run','dk_render_pipeline_probe','dk_aerial_perspective_probe') -TestRegex '^dk\.(clouds\.(optimization |.*_gpu_composition$|reference_gpu_validation$)|render\.(pipeline_gpu_validation|aerial_composition_gpu_validation)$)' -Reason 'M15.4 final optimized numeric/lifecycle suite, low-sun refinement, shared reference/pipeline and capture regressions'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_run') -TestRegex '^dk\.clouds\.(optimization_switches|capture)_gpu_composition$' -Reason 'M15.4 prove interpolation savings with flat background, disable early termination even after float underflow, and finalize capture fingerprint'
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=$previousLayer }
```

| 默认优化与512参考 | HDR nRMSE | p99 | 所有像素最大 | 云Tc最大绝对差 |
| --- | --- | --- | --- | --- |
| 云内/静态 | .000461126 | .00213618 | .00289291 | .00276335 |
| 云下 | .000140621 | .000645647 | .00151580 | .00107583 |
| 云上 | .0000407458 | .0000998535 | .00111652 | .00302273 |
| 日落 | .00297070 | .0128384 | .0294203 | .00276335 |
| lit + 空气MS | .00103639 | .00423154 | .00791099 | .00276335 |
| 8帧运动/风各列最坏 | .000557083 | .00227956 | .0109134 | .00794068 |
| 3帧mask反遮挡各列最坏 | .00214672 | .00266127 | .0617648 | .00658190 |

静态32预热+120帧的最大帧差RMS=0，152帧累计半尺寸历史接受5587、拒绝33325；
采用固定采样而非随机抖动，不将此结果声称为随机噪声收敛。相机切换、resize/投影、时间回退/大跳、
seed、Scene身份/revision、质量、相机大幅平移/旋转均触发相应重置；曝光不重置。
submit/object故障不推进已完成序号，无泄漏；submit恢复图逐位等于无历史图，重建Pipeline从空历史开始。

空区开关使主步从150636降至71052，跳过85310个原始步，HDR最大差3.42022e-5；
均匀厚云关闭/开启早停主步50094→988，省略49106步，Tc误差≤9.913e-5，HDR最大=.000150561。
该均匀背景31×31的961输出像素均复用16×16积分，证明真实重建收益；
带边缘真实场仅258/961像素复用、703像素回退，明确保留质量代价，不假设固定4倍加速。

未截断输出保存在各最终日志的`details.log`。八张原始PPM及最近邻放大拼图保存在
`out/verify/20261013-160317-d59eb7ee/images/`，已目视核验；拼图不是新分辨率渲染。
check-spec限定本次8个Markdown，通过579个本地链接及元数据、表格、索引、测试入口和JSON检查；
`git diff --check`通过。
数字前缀均对应 `out/verify/20261013-<前缀>/`，含build/tests/summary等原始记录。

## 限制与下一步

本阶段不增加云影、交互面板或时间序列命令；不声称1080p帧时或显存候选目标已达标。
优化仍为显式C++入口，共用编辑器/capture保持M15.3 reference，接入UI/序列留给M15.6。
大深度/透射梯度可大量回退，历史仅在低差异可信区域复用；Scene变化整体重置，未实现几何运动矢量。
256MiB是历史图像逻辑载荷上限，未包含整帧输出、GPU分配对齐、纹理和在途外部Frame；不等同192MiB性能目标。
完成并验收M15.4，后续M15.5接入太阳云影。

## 修改记录

- 2026-10-13T15:10:00+08:00：建立采样、预算、重建、历史发布与质量门槛。

- 2026-10-13T16:06:56+08:00：完成实现、21项定向验收、失败修复和误差/工作量证据；保留性能及UI接入边界。
