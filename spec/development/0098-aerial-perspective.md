---
id: "0098"
created_at: "2026-10-13T10:30:33+08:00"
updated_at: "2026-10-13T10:51:44+08:00"
status: completed
design_refs:
  - ../design/render-pipeline.md
  - ../design/render-atmosphere.md
  - ../design/architecture.md
---

# 0098 M14.5 视锥空气透视与HDR合成

## 目标与设计依据

完成[Roadmap](../roadmap.md) M14.5。先细化[管线](../design/render-pipeline.md)与[大气](../design/render-atmosphere.md)契约。
视锥RGB L/T、深度真实距离、天空/几何一次合成、统一曝光及受限线性读回。M14.6上层集成不在范围。
无新增依赖；复用M14.3球面传输与M14.4表面日光。

## 实施与验证

阈值、支持范围、状态与失败约定已在实现前写入设计。新增显式 render_atmosphere 和 AtmosphereSettings，
相机位置/环境严格匹配快照；disabled退回旧unlit。共用AtmosphereTransport.slang，CPU双精度校验投影与局部坐标。
两张64×64×128 RGBA32F L/T体积、128段积分；天空单独查询并评估太阳盘，几何仅合成一次 L + T*surface，最后统一tone。
Graph声明深度到compute、storage到sampled及transfer/host同步；各帧拥有独立资源，未改动Scene/GpuAssets/大气缓存。
可选受限曝光前RGBA32F读回，最多65536像素；512×512高采样reference模式最多4096像素。
新GPU探针、投影CPU策略测试、使用指南、设计/索引与测试选择表同步，无命令/场景格式/三方依赖改动。

初轮32×32×64体积在15km接近地面的RGB T误差0.008920705，超过0.002门槛；先将设计改为64×64×128（两表共16MiB），随后提高分辨率重验，不修改阈值。
初轮深度使用了底层未开放的DepthStencilReadOnly布局；改用已验收的ShaderReadOnlyOptimal深度采样契约。

64×64×128仍出现T误差0.005348247。定位为地面截断导致传输函数导数不连续（地上/穿地体素混合），
仅提高分辨率不足；先补充设计：按单元世界空间跨度识别地表边界带，实际像素用128段有限积分，保持RGB T门槛。

## 数值与状态验收

Windows x64，windows-graphics Debug，MSVC14.51/Slang2026.18，RTX4070 Laptop GPU，驱动596.49。
Khronos required与同步validation；进程临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1隔离旧AMD隐式层，运行后恢复。

- CPU管线5项：参数、clip矩阵、法线、空帧及透视物理距离/投影拒绝/读回上限；out/verify/20261013-103746-cde12bc1。
- M14.5三项GPU：numeric/composition/failures全部通过，0跳过、0GPU错误/警告，VMA与Memory分配回零。
  证据out/verify/20261013-104317-6707359b（full-tests.log保留完整数值，不依赖JUnit截断文本）。
- 5幅33×33 HDR：高度100m/10km/100km/150km，轴向距离10m/5km/50km；逐图对512×512 GPU直接参考。
  最坏nRMSE=0.000134668（0.01347%），p99=0.000763697（0.07637%），全图最大0.0030488（0.30488%）；
  边缘/地平线独立最大0.00121125（0.12113%）。阈值为3%/8%，边缘另要求<=8%。
- CPU1024×1024有限段独立参考覆盖每图5点；最大相对误差0.000815879。远几何穿地时T=0，L只积分到行星。
- 625个几何像素RGB T与CPU1024段比较：最大绝对误差0.0001455496，小于0.002。
  near/far从10/50000改为100/100000m，图像T最大绝对差0.00006878376。
- 非对称投影与俯仰旋转相机各5点CPU参考（直接检查正常体积路径，旋转实际改变高度/密度光程）；
  最终numeric补强通过，证据out/verify/20261013-104920-b3191bb6/full-tests.log。真空L=0/T=1；disabled与旧unlit逐字节一致。
- mask独立解析检查前景/露出远景/露出天空；MS开启的完整合成nRMSE=0.0000689384、p99=0.000343467。
  HDR在曝光1/3下逐float一致，RGBA8逐通道对tone参考<=1byte；日光表面加AP对独立有限传输，排除重复散射/透射。
- 太阳盘被opaque遮挡、被mask洞露出，开关生效；纯背景与独立SkyFrame逐float一致。
- 错环境/空快照/错锚点/非法投影、对象/提交/跨队列及外队列LUT失败，预算逐步释放中部分分配失败/恢复、
  域关闭、pending与错误尺寸/未请求线性读回不写目标、新快照不改旧帧、在途销毁保活。

原unlit管线、日光normals/direct/failures、天空reference/vacuum共6项全部通过，0跳过；
证据out/verify/20261013-104530-9c46dd29（完整输出full-tests.log）。合计14项不同测试，0未解决失败。
文档检查9份Markdown、587个本地链接及全局索引/测试入口通过；git diff --check通过。早期失败和修正如上，未用旧二进制充当新实现证据。

实际命令均使用 scripts/verify.ps1 -BuildDir out/build/windows-graphics：

```powershell
-Target @('dk_aerial_perspective_probe','dk_render_pipeline_tests') -TestRegex '^dk\.render\.(aerial_|pipeline\.)'
-Target dk_aerial_perspective_probe -TestRegex '^dk\.render\.aerial_'
-Target @('dk_render_pipeline_probe','dk_surface_sunlight_probe','dk_atmosphere_probe') `
  -TestRegex '^dk\.(render\.(pipeline_gpu_validation|sunlight_(normals|direct|failures)_gpu_validation)|atmosphere\.(reference|vacuum)_gpu_validation)$'
```

每次带明确Reason；对应summary保存目标、实际匹配测试、构建与通过/失败/跳过数。
原有用户修改spec/README.md及未跟踪spec/guides/ai-documentation-workflow.md保留，不纳入本阶段提交。

## 限制与后续

显式C++入口；Editor/capture环境装配为M14.6。仅有限forward-Z刚体透视（far<=1000km、far/near<=10000），
不支持正交/reverse-Z/无限远；其余系数与大气域见设计。MS仍为既有各向同性闭合近似，不是全阶路径追踪真值。
两张体积16MiB，加独立合成HDR/可选读回；地表边界直接积分增加局部成本。未做GPU时序、1080p性能、跨厂商、
Release或全量回归，不宣称M14.7性能门槛达成。无跨帧历史或AP缓存。
