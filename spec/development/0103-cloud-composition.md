---
id: "0103"
created_at: "2026-10-13T14:18:00+08:00"
updated_at: "2026-10-13T14:52:00+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-pipeline.md
  - ../design/render-capture.md
  - ../design/architecture.md
---

# 0103 M15.3 云、空气与几何统一合成

## 目标与设计依据

按[混合介质契约](../design/render-clouds.md)实现有序区间消光/源项联合积分、D32裁剪、终端表面/太阳及诊断图。
开工冻结参考预算、零云逐位退化和数值/边缘阈值；沿用M14和M15.2模型，不增加云影或历史。

## 实际变更

- [CPU混合参考](../../engine/render/clouds/src/CloudReference.cpp)按空气边界、最多两段云壳和有限深度/地面切成至多五段，
  消光和源项共同积分，终端表面/太阳只由调用者加入一次。沿用M15.2云照明近似，空气独立参考为单散射。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)新增显式render_clouds，并由共用render_environment装配不可变云场/纹理。
  [混合shader](../../shaders/render/cloud-composition.slang)直接读取D32、未加雾表面HDR和云场，替代完整AP体积/合成pass；
  可选M14空气MS仍来自同一快照。按云/空气独立开关处理背景，太阳盘只乘完整混合透射，tone只执行一次。
- [共享云传输](../../engine/render/clouds/shaders/CloudTransport.slang)复用密度、球壳、太阳光学厚度和相函数，
  原M15.2每射线GPU参考仍使用相同模型；无新依赖、历史、早停或低分辨率重建。
- Frame可选读回云散射/Tc、混合RGB透射/代表距离；尺寸错误、未请求和未完成不写目标。
  Graph绑定真实输入/输出，提交保活资源；队列身份、预算、对象/submit失败保留旧帧，首次缓存owner只在帧成功后保存。
- CaptureService/RenderOperations新增结果quality `cloud_reference_v1`，包含完整shader闭包指纹，保持冻结time=0；
  请求字段/effect/guard/事务和原子文件发布契约不变。零云精确回到M14或旧unlit。
- 新增[GPU合成探针](../../tests/integration/CloudCompositionProbe.cpp)和[真实截图进程](../../tests/integration/CloudCaptureTest.py)，
  补充[CPU解析/预算测试](../../tests/unit/CloudReferenceTests.cpp)，同步构建/测试入口、命令与模块设计、架构和Roadmap。

## 验证记录

Windows x64 Debug，复用windows-scripting纯CPU与windows-editor构建目录。21个不同CTest用例最终通过，0失败、0跳过。
GPU为RTX 4070 Laptop，required Khronos validation + sync validation；云探针errors=0、warnings=0、
VMA allocation、队列pending/recording和Memory live归零。仅进程环境禁用AMD旧隐式层，结束恢复。

实际定向命令（GPU各次置于下列try/finally环境范围）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_cloud_reference_tests') -TestRegex '^dk\.clouds\.(composition |reference )' -Reason 'Verify mixed-reference five-segment work budget and existing CPU cloud reference'
$previousLayer=$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_cloud_reference_probe') -TestRegex '^dk\.clouds\.(.*_gpu_composition|reference_gpu_validation)$' -Reason 'M15.3 GPU composition after correcting numeric probe vector type'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe') -TestRegex '^dk\.clouds\..*_gpu_composition$' -Reason 'Verify M15.3 mixed medium composition after excluding unused Sky LUT binding'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_run','dk_protocol_tests','dk_aerial_perspective_probe','dk_render_pipeline_probe') -TestRegex '^dk\.(clouds\.(numeric_gpu_composition|capture_gpu_composition)|protocol\.capture |runtime\.capture_gpu_validation$|render\.(aerial_.*_gpu_validation|pipeline_gpu_validation)$)' -Reason 'M15.3 full CPU/GPU views and real capture contract; shared pipeline and M14 composition regression'
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=$previousLayer }
```

- CPU初轮8/8通过；五区间工作预算补齐后8/8通过，最终日志`out/verify/20261013-144619-cf7527cc`。
  均匀空气RGB吸收+云吸收径向解析误差<1e-10，有限终点/地面、空气关闭与单云一致、零云空气不重复叠加通过。
- 初次GPU构建`out/verify/20261013-143401-2d1dd4ba`失败：测试将float指针直接用于Vec3d构造，改为显式分量转换。
  次轮`out/verify/20261013-143542-4a17efb6`中M15.2参考1/1通过，新合成3项均失败：
  unlit云表面不使用Sky LUT，但仍导入其external binding，Graph裁剪该资源后拒绝绑定。修正为只导入实际使用的LUT，未修改Graph保护规则。
- 修复后3/3通过，日志`out/verify/20261013-144059-058ce967`，覆盖解析吸收≤2e-3、前景/云内/云后终点、
  mask洞、云遮太阳与不穿透前景、四种零云逐位退化、曝光一次、共用入口和显式入口一致。
  预算/尺寸/投影/快照/跨队列/外来纹理拒绝、对象/submit故障、重试、未完成诊断读回、关闭heap及在途owner销毁通过。
- 补充云下/云上/日落整图CPU对照与真实capture后10/10通过，日志`out/verify/20261013-144449-e384f895`。
  含3个截图CPU契约、3个M14空气透视、原pipeline、原Runtime截图、新云数值和新云截图。
  用本次dk-run核验commands.list/describe中external/undoable=false、请求字段及quality enum；
  冻结环境后编辑、time=0重复恢复、空气关闭仍有云、零coverage退化、65×65预算失败保留旧PPM、无隐式场景保存通过。

固定真实场：generator v1/seed1、默认128²/128³/32³，coverage=.85，g=.6/albedo=.9/m=.35、
sky=(.015,.02,.03)、time=17.25s，太阳归一化(.4,.5,-.6)，日落改为(0,.02,-1)。
15×15云内相机h=2200m、表面轴距3000m；完整9×9对照h=100/6000/2200m、表面6000m，
含几何边缘、无几何背景、地平线和行星遮挡。CPU/GPU对照关闭空气MS，云模型保持不变。

| 比较 | HDR nRMSE | p99 | 边缘/地平线最大误差 |
| --- | --- | --- | --- |
| GPU 256/64/64→512/128/128 | 3.09753e-5 | 1.20680e-4 | 3.30958e-4 |
| GPU 512/128/128→1024/256/256 | 2.00083e-5 | 5.48729e-5 | 2.63094e-4 |
| 云下整图 GPU/CPU | 4.66678e-6 | 2.04672e-5 | 3.68636e-5 |
| 云上整图 GPU/CPU | 2.80473e-6 | 1.25798e-5 | 1.94120e-5 |
| 日落整图 GPU/CPU | 1.64243e-5 | 6.27370e-5 | 9.45293e-5 |

归一尺度为max(参考亮度p95,.001)，分别低于冻结.03/.08/.08；固定5像素最大RGB误差1.22905143e-4。
三整图云Tc最大绝对差3.46577892e-5、混合T最大差3.37263477e-5，低于.005；未放宽阈值。
隔离天空云源时，开启空气后云散射逐通道不增加且有可测衰减，证明远云空气透视；固定时间回退后HDR逐位恢复。
最终PPM、Tc、混合T、距离、mask和三视角产物已检查并复制到最终日志`images/`，
`diagnostics.png`为最近邻放大的检查拼图，非新分辨率渲染；`LastTest.log`保存未截断数值输出。
截图schema/report与原图保存在`out/build/windows-editor/test-artifacts/Debug/cloud-capture-63740531ef7244b0b107e07aca7ba1fa`。

check-spec.ps1限定本次13个Markdown文件，通过654个本地链接及元数据、表格、索引、测试入口与JSON检查；
git diff --check发现CMake尾部多余空行，移除后通过。未运行全量、Release、其他GPU/平台、普通尺寸编辑器和实时性能测试。

## 偏差与限制

逐像素无优化参考最多4096像素和1Gi采样工作；64×64默认参数在预算内，不承诺实时编辑器分辨率；优化属M15.4。
空气源不接受云反馈，表面云影属M15.5；编辑UI和时间序列配置属M15.6。

## 下一步

M15.3验收完成。下一项M15.4：半分辨率、空区跳跃/早停、保边上采样和每视图时域重投影。

## 修改记录

- 2026-10-13T14:18:00+08:00：建立设计、预算和验收基线。
- 2026-10-13T14:52:00+08:00：完成联合积分/合成、诊断与截图契约，登记21项定向验收及实际限制。
