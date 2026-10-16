---
created_at: "2026-10-14T10:18:00+08:00"
updated_at: "2026-10-16T12:14:04+08:00"
---

# CPU路径追踪参考

[M16.1设计](../design/render-pathtracing.md#m161-cpu参考实施契约)提供小场景双精度Lambert/GGX参考，
用于[GPU管线](pathtracing-gpu.md)正确性对照。表面模式已接入Editor和capture；
CPU体积路径使用本页末尾的独立库入口，GPU及上层体积接入阶段见[Roadmap](../roadmap.md#m17大气云与路径追踪集成)。

## 构建与定向验证

使用现有CPU开发预设，依赖保持固定，不需要Vulkan loader或Slang：

```powershell
cmake --preset windows-dev -DDK_BUILD_RENDER_PATHTRACING_REFERENCE=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @(
  'dk_pathtracing_reference_tests', 'dk_pathtracing_reference_demo',
  'dk_render_data_tests', 'dk_geometry_tests'
) -TestRegex '^dk\.(pathtracing|render\.data|geometry)\.' -Reason 'M16.1 CPU reference and direct dependencies'
```

开关默认OFF；启用后提供RenderData与asset_data，要求Scene/Math/Memory已启用。
测试中的Geometry BVH是独立求交对照，reference库本身不依赖Geometry实现。

## 可复现场景示例

```powershell
./out/build/windows-dev/bin/Debug/dk-pathtracing-reference-demo.exe out/pathtracing-reference 128
```

示例为24×24固定相机的开放盒，包含灰色墙面、红/绿侧壁和顶部面积灯；固定seed=42、
4次最大BSDF散射、RR从第3次开始、MIS、黑色常量环境。参数是输出目录与可选spp（默认128），
通过完整输入/工作预算后才执行。CPU Debug适合数值检查，不用于判断未来GPU速度。

- `reference.ppm`：曝光1、Reinhard加sRGB编码后的预览。
- `reference.json`：曝光前线性RGB、模型/采样版本、场景/资产消费摘要、相机、seed/spp、
  反弹/RR、工作预算与实际射线/求交计数。`cpu_seconds`仅统计CPU积分，`gpu_ms=null`。

每个文件独立原子替换，不承诺两个文件的整体事务。修改spp重新从样本0计算，
相同输入和样本区间可重现；不同spp不是时域重投影或持久历史。
集成测试运行两次8spp示例，对比线性数据/PPM并验证非法输入保留已有输出。

## 库使用与限制

入口为[PathTraceReference.hpp](../../engine/render/pathtracing/include/dk/render/PathTraceReference.hpp)。
先从SceneSnapshot提取RenderScene，以CpuAsset只读指针span构造PathTraceScene，
再用RenderView调用render_path_reference；`estimate_path_reference`用于固定射线的解析/统计实验。
快照复制消费数据，源资产/文档修改不影响旧结果；同一revision下资产改变会改变诊断指纹。

默认Lambert参考模式要求metallic=0；create末参数显式选择PathTraceMaterialModel::metallic_roughness可启用GGX。
GGX模式消费metallic/roughness，缺材质沿MaterialData默认metallic=1/roughness=1；两种模式都支持opaque/mask和baseColor纹理。
当前model_version=3、sampler_version=1；示例报告renderer为cpu_lambert_reference_v3。
GGX使用alpha=max(roughness²,0.0025)、单次散射/Smith/Schlick和双向Fresnel漫反射衰减；不是完整glTF BRDF。
无理想镜面delta、法线/metallic-roughness贴图或多次散射补偿；具体公式与门槛见[材质契约](../design/render-pathtracing.md#m1672-metallic-roughness-ggx实施契约)。
使用世界三角形平面法线，忽略顶点法线；纹理UV按重心插值，单面背面剔除，双面翻转几何法线。
LOD=0采用mag_filter（缺省linear），RGB按sRGB逐texel解码、alpha保持线性，支持repeat/clamp/mirrored_repeat。
mask在alpha×factor小于cutoff时拒绝主/阴影命中；发光mask面仅由BSDF命中提供光照。
最多64张、单边2048、总64MiB RGBA8；PathTraceScene的texture_info可查询被冻结的资产身份、origin及尺寸。
上述PathTraceReference入口是表面参考，不积分空气或云；场景启用介质时必须显式设置surface_only，报告此范围。

最多64×64、4096个计入预算的三角形；样本区间在[0,65536)，相机为刚体透视针孔、
Vulkan深度0..1、有限near/far。每次同时受2^24射线和2^29候选求交上限约束，
超限报错，不自动降低画质。每样本/反弹检查取消；失败返回错误而不是部分图像。
图像是未曝光的拥有型线性RGB，旧结果可在Memory关闭后读取；新分配要求open heap。

检验误差应读取线性报告；不能用PPM截断/曝光后的像素替代收敛判断。
实测结果与限制见[0109](../development/0109-pathtracing-reference.md)。

## CPU体积路径参考

[VolumePathReference.hpp](../../engine/render/pathtracing/include/dk/render/VolumePathReference.hpp)属于同一可选CPU target。
先构造PathTraceScene，再从同一RenderScene的environment构造PathMediumSnapshot；非零云需由CloudFieldCache.prepare提供匹配的不可变场。
PathMediumSettings显式指定time_seconds、cloud_albedo（默认0.9）和cloud_anisotropy（默认0.6）。
时间不会从frame或系统时钟推断；更换场景环境后必须重建对应快照。

`estimate_volume_path_reference(scene, medium, ray, settings, samples, pixel, stop)`提供固定射线的RGB均值、二阶矩和工作统计。
`render_volume_path_reference(heap, scene, medium, view, settings, samples, stop)`返回≤64×64拥有型线性图，
`info()`保留完整环境、介质设置、云场hash、模型/采样版本、来源、相机和样本区间；`second_moments()`可用于方差分析。
旧图保活其几何/场输入，改变或关闭原始文档/heap不使已完成像素失效；闭合heap拒绝新图。

VolumePathSettings采用model=4/sampler=2，默认sun=true、黑色外边界、max_bounces=8、rr_start=3。
反弹计数包含表面和真实介质散射；null碰撞不计反弹。max_bounces=0、rr_start=0可观察无散射传输，
rr_start=0单独关闭俄罗斯轮盘；bsdf_only估计器关闭NEE，便于统计对照。
有限太阳圆盘、常量边界与opaque面积灯共享MIS；空气、云和表面沿同一光路散射，不加M14/M15的近似增亮。

每路径只采一个RGB通道并乘3补偿，因此低样本图有通道噪声；它是数值参考，不是编辑器实时预览。
样本区间[0,65536)，上限32次真实散射，主射线坐标各轴绝对值≤1e6米；地下相机拒绝。
几何near/far只裁主射线网格，介质持续到实际空气顶/地面，后续反弹和阴影使用完整距离。
空气或云启用就保留Lambert行星边界，即使系数为0；均关闭才退化为无隐式行星的真空。

默认工作上限2^24 rays、2^29 triangle tests、2^26 tracking steps及每路径2^16 steps；
tracking可显式提高到请求2^28、每路径2^20。消光上界按已知无云/混合壳区间选择。
超限、取消、majorant低估或非有限结果均返回错误，不降低样本数、不发布部分图。
几何仍为CPU逐三角形参考；不承诺大场景或高光学厚度的交互性能。

定向验收使用已有CPU配置（Configuration应匹配实际配置）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.volume '
```

夹具、独立积分、8 seed统计、收敛与实际结果见[0124](../development/0124-volume-path-reference.md)。
原示例程序仍调用表面入口，不输出体积图；Editor/GPU/capture尚未调用此体积入口。
