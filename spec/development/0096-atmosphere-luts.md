---
id: "0096"
created_at: "2026-10-12T16:56:20+08:00"
updated_at: "2026-10-12T18:13:18+08:00"
status: completed
design_refs:
  - ../design/render-atmosphere.md
  - ../design/architecture.md
---

# 0096 M14.3 大气积分、LUT与离屏天空

## 目标与设计依据

完成[Roadmap](../roadmap.md) M14.3，先细化[大气设计](../design/render-atmosphere.md)，再实现独立CPU参考/GPU模块。
上游作者公开实现和球面传输公式仅作算法依据，无移植源码或新增依赖。
M14.4日光接收、M14.5空气透视和M14.6编辑器/截图集成不在本次范围。

## 实际变更

- 新增engine/render/atmosphere：dk::render_atmosphere_reference仅链接Scene/Math，双精度米制球面求交、密度/相函数、Beer积分、独立太阳光程、RGB L/T及各向同性MS闭合参考。
- dk::render_atmosphere通过现有Graph/Slang生成RGBA32F T 256×64、MS 32×32、Sky 192×108，GPU内部km；显式检查格式/线性过滤和GPU数值范围。
- 固定T 512段、MS 1024方向×64段、Sky 128段；高采样GPU参考512视线段×512太阳段。MS响应按单位太阳能量保存，不重复曝光。
- prepare同步等待每个候选LUT完成并检查有限值/透射范围/MS反馈后，一次发布快照和代次；旧快照/帧持有旧资源。跨队列、关闭域、创建/submit/预算/不收敛失败均拒绝，保留旧缓存。
- T/MS/Sky分别按实际物理输入失效；太阳能量/盘半径、相机朝向和输出尺寸不重建LUT。世界位置变化仅影响实际高度/局部太阳角；平移行星中心和相机不改变表。
- render_sky返回异步HDR图像/读回/票据；太阳盘独立衰减及行星遮挡。盘能量与弦长阈值在CPU用double预计算，支持最小1e-6rad角半径。
- 新增dk-atmosphere-demo，原子写出正午/日落/100km高空的960×540 PPM，只有输出执行一次曝光8、Reinhard、sRGB。加入能力开关、windows-graphics预设、测试选择表、[使用指南](../guides/atmosphere.md)和设计/架构索引。

## 验证记录

环境：Windows x64、windows-graphics Debug、MSVC 14.51、Slang 2026.18、RTX 4070 Laptop GPU、驱动596.49；Khronos required及同步validation开启。
仅在测试进程环境设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，隔离本机旧AMD隐式层API版本告警；测试结束恢复原值，不改变系统/驱动配置。

共12项不同测试通过：6项CPU参考、5项required GPU大气测试、1项原unlit管线回归。0未解决失败，0跳过。

| 检查 | 最终有效证据（out/verify/） | 结果 |
| --- | --- | --- |
| CPU真空/纯吸收解析、分段合成/能量、相函数、球面边界、太阳盘、收敛 | 20261012-174239-7bac529b | 6/6通过；同批旧太阳盘GPU失败在下列最终批修复 |
| 单/多散射天空、高采样GPU对照、CPU点参考、T插值及高空地平线 | 20261012-173904-5901ce43 | reference_gpu_validation通过；完整数值保存在full-tests.log |
| MS对4096方向CPU闭合参考 | 20261012-172850-feb56a77 | multiple_gpu_validation通过，Debug约453秒；MS积分公式在后续太阳盘修复中未改动 |
| LUT失效、旧快照/在途帧、pending读回、三段submit失败、创建/预算/闭合失败、跨队列与关闭域 | 20261012-173904-5901ce43 | cache_gpu_validation通过，VMA/Memory无泄漏 |
| 真空、盘能量、最小太阳盘、遮挡、disabled；离屏样例；旧管线 | 20261012-174413-9bb7d75f | 3/3通过，errors/warnings/live=0 |
| CPU程序动态依赖 | dumpbin /dependents dk_atmosphere_tests.exe | 仅Windows/CRT，无Vulkan/Slang依赖 |
| 三幅PPM视觉检查 | out/build/windows-graphics/test-artifacts/Debug/atmosphere | 转PNG检查正午/日落/高空；地表可见面仍黑色，不含M14.4地面材质 |

定量结果（太阳盘不烘入Sky表，盘能量单列）：

| 相机/太阳 | 单散射NRMSE | 含MS的NRMSE | 含MS的p99归一化绝对误差 |
| --- | --- | --- | --- |
| 正午1m | 0.01794% | 0.01644% | 0.07240% |
| 日落1m | 0.24882% | 0.21430% | 0.45117% |
| 暮光1m | 0.07931% | 0.07006% | 0.30486% |
| 高空10km | 0.05040% | 0.04760% | 0.18514% |
| 顶部100km | 0.39384% | 0.38488% | 1.40604% |
| 顶外150km | 0.45187% | 0.45907% | 1.16655% |

以上归一化尺度=max(参考RGB亮度p95,1e-3)，固定门槛NRMSE≤3%、p99≤8%全部通过。
全图最大单点归一化误差10.14%（150km，按亮度p95尺度），单独记录，不将p99误称全图最大误差。
显式地平线上下/朝太阳/反太阳及天顶点按max(参考RGB最大通道,1e-3)归一化，1m最大6.262%，10/100/150km分别0.325%/0.525%/0.572%，均低于8%。
T表节点最大绝对误差8.53e-6，网格间双线性插值最大2.80e-4，低于2e-3门槛。
GPU直接积分对独立1024×1024 CPU点参考最大归一化差0.0426%。
MS三个选定点对4096方向×128段×256太阳段CPU闭合参考的最大相对差2.151%/0.522%/7.329%，低于独立15%闭合离散误差门槛；这不代表对全阶传输真值的误差。
最小1e-6rad太阳盘中心能量相对误差1.86e-8，盘外为0；行星遮挡和disabled均为0。

复现完整大气范围（已验证各部分后不机械重复耗时MS参考）：

```powershell
cmake --preset windows-graphics
$previousLayerSetting=[Environment]::GetEnvironmentVariable('DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1','Process')
try {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_atmosphere_tests','dk_atmosphere_probe','dk_atmosphere_demo','dk_render_pipeline_probe') `
    -TestRegex '^dk\.(atmosphere\.|render\.pipeline_gpu_validation$)' -Reason 'M14.3大气与原管线定向验收'
} finally {
  [Environment]::SetEnvironmentVariable('DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1',$previousLayerSetting,'Process')
}
```

实际分批命令均为上面的verify.ps1，-Target/-TestRegex分别为：

```powershell
# 20261012-172850-feb56a77：4/4，含高采样MS参考与首次缓存/真空/样例。
-Target @('dk_atmosphere_probe','dk_atmosphere_demo')
-TestRegex '^dk\.atmosphere\.(multiple|cache|vacuum|demo)_gpu_validation$'
# 20261012-173904-5901ce43：9/10；天空、补充缓存和6项CPU通过，最小太阳盘失败后单独修复。
-Target @('dk_atmosphere_tests','dk_atmosphere_probe','dk_atmosphere_demo')
-TestRegex '^dk\.atmosphere\.(reference_gpu_validation|cache_gpu_validation|vacuum_gpu_validation|demo_gpu_validation|[^_]+)$'
# 20261012-174239-7bac529b：6/7；最终CPU位置边界6项通过，盘数值日志确认GPU小角sin误差。
-Target @('dk_atmosphere_tests','dk_atmosphere_probe')
-TestRegex '^dk\.atmosphere\.(vacuum_gpu_validation|[^_]+)$'
# 20261012-174413-9bb7d75f：最终太阳盘/样例/旧管线3/3通过。
-Target @('dk_atmosphere_probe','dk_atmosphere_demo','dk_render_pipeline_probe')
-TestRegex '^dk\.(atmosphere\.(vacuum|demo)_gpu_validation|render\.pipeline_gpu_validation)$'
```


最终文档检查：10份Markdown、636个本地链接及元数据/表格/索引/测试入口/JSON清单通过；git diff --check通过。

```powershell
& ./scripts/check-spec.ps1 -Path @(
  '.agents/skills/decker-build-verify/references/test-selection.md','README.md','spec/design/README.md',
  'spec/design/architecture.md','spec/design/render-atmosphere.md','spec/development/0096-atmosphere-luts.md',
  'spec/development/README.md','spec/guides/atmosphere.md','spec/guides/build.md','spec/roadmap.md'
)
git diff --check
```

## 偏差与决策

- 首次构建缺失模块内allocation_error辅助函数，修复后重建；没有用旧二进制提供通过证据。
- T比较发现CPU/GPU把“未命中球体”的占位根当成地面交点；增加有效根区间判定及高空向下但不碰行星的CPU回归。T误差由接近1降到8.53e-6。
- 首轮64方向MS在高空低太阳角与256方向CPU结果相差209.5%；提高到1024方向，并用更高的4096方向CPU比较，未放宽天空门槛。
- 最小太阳角半径1e-6rad暴露GPU小角sin相对误差（能量高14.07%）；CPU预计算盘常数、GPU以向量弦长判断，不以cos相等判断小盘。
- 固定RGBA32F建立数值基线；prepare明确同步等待并检查后发布，render_sky保留异步票据。CPU诊断镜像与readback是本阶段验证成本，性能优化留到M14.7。

## 遗留问题与下一步

本阶段验收完成。下一项M14.4实现最小日光接收；M14.5再接空气透视，M14.6接普通场景视口/capture。
MS为各向同性闭合并仅包含一次直射地面反弹，不宣称完整全阶物理真值。太阳盘为逐像素点采样，未做抗锯齿；不提供星空、地形或地表可见材质。
未运行Release、全引擎测试、GUI、跨厂商或性能基线；本次新增独立模块且未改默认管线逻辑，相关Debug数值/状态/原管线回归已覆盖直接链路。
用户已有spec/README.md及AI文档迁移指南改动保留，不纳入本次提交。

## 修改记录

- 2026-10-12T16:56:20+08:00：先更新设计并创建记录。
- 2026-10-12T17:49:35+08:00：实现、12项不同定向测试、图像和CPU依赖检查完成；同步Roadmap与索引。
