# 独立离屏大气天空

本入口生成正午、日落和100km高空的960×540 PPM，使用球面对称大气、单太阳和T/MS/Sky View LUT。
场景几何日光接收见下方独立样例；空气透视及Editor/capture集成见[Roadmap](../roadmap.md)后续阶段。

```powershell
cmake --preset windows-graphics
cmake --build out/build/windows-graphics --config Debug --target dk_atmosphere_demo
& ./out/build/windows-graphics/bin/Debug/dk-atmosphere-demo.exe ./out/atmosphere
```

样例要求真实GPU及Khronos validation，失败返回非零，不以跳过表示成功。
生成noon.ppm、sunset.ppm、high-altitude.ppm；每个文件经原子替换发布，整个目录不作为一次事务。
输入HDR为线性RGB，样例只在输出时执行一次曝光8、Reinhard和sRGB转换；日志报告tone前能量范围与LUT代次。

## C++接口

[AtmosphereRenderer](../../engine/render/atmosphere/include/dk/render/Atmosphere.hpp)通过显式heap、SubmissionQueue和shader目录创建。
prepare接收EnvironmentDescription和米制世界位置，同步生成/校验受影响LUT后返回不可变AtmosphereSky。
render_sky接收该快照与SkyCamera，返回异步SkyFrame；wait成功后read_hdr读取RGBA32F。
同一renderer/queue需外部串行调用。旧快照和旧帧可继续使用；失败不会发布半成品缓存。

参考模式对每条视线直接积分512段、每段太阳光程512段，限制4096像素，用于离散误差诊断。
它仍使用可选MS闭合表；独立[CPU双精度参考](../../engine/render/atmosphere/include/dk/render/AtmosphereReference.hpp)
另外验证直接光和同一各向同性MS闭合，不宣称完整多阶路径追踪真值。

当前GPU范围：半径100–100000km、大气厚度1km至0.1倍半径、Rayleigh/Mie尺度与吸收半宽至少100m、相机高度0至2倍大气厚度。
默认环境enabled=false时天空帧为黑色；太阳盘仅enabled时参与，半径/辐照度不重建LUT。
普通输出每轴不超过4096。未满足RGBA32F storage/sample/linear filter的设备明确报错，不自动降低精度。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_atmosphere_tests','dk_atmosphere_probe','dk_atmosphere_demo') `
  -TestRegex '^dk\.atmosphere\.' -Reason '验证大气参考、LUT和离屏天空'
```

数值门槛、缓存key和提交点见[模块设计](../design/render-atmosphere.md)；实现和验证证据见[0096](../development/0096-atmosphere-luts.md)。


## Lambert 日光接收

```powershell
cmake --build out/build/windows-graphics --config Debug --target dk_sunlight_demo
& ./out/build/windows-graphics/bin/Debug/dk-sunlight-demo.exe ./out/sunlight
```

样例生成 noon/sunset/twilight × unlit/direct/sky 共九张 960×360 PPM：
三个白色物体依次为球体、非均匀缩放椭球和负缩放椭球。direct 只接太阳直射；sky 再加天空漫射。
三个预设都固定曝光5，背景为固定clear色，便于比较；天空背景和空气透视尚未接入。
每文件原子发布；整体九文件不承诺事务。程序要求真实GPU及Khronos validation，失败返回非零。

调用方创建 AtmosphereRenderer 与 ScenePipeline、上传 GpuAssets；用 view.scene().environment()
和米制锚点调用 prepare，显式等待完成后得到的快照传入：

```cpp
auto sky = atmosphere.prepare(queue, view.scene().environment(), anchor_m);
if (!sky) return std::unexpected(sky.error());
render::RenderSettings settings;
settings.sky_diffuse = true;
auto frame = pipeline.render_sunlight(queue, view, assets, *sky, settings);
```

每步处理 Result；render_sunlight 返回异步 RenderFrame，wait 完成后才 read_rgba8。
环境版本值不匹配会拒绝整帧。原 render 始终保持 unlit；disabled 场景即使调用 render_sunlight 也回到 unlit。
直射用真实接收点的透射，天空漫射共享锚点 Sky View；emissive 不受直射衰减，曝光只在tone执行一次。
单面背面在深度和颜色阶段都剔除，双面翻转法线；奇异变换、地下或超出 GPU 高度域的接收体明确拒绝。
这是一种 Lambert 局部环境近似，不含镜面、PBR、几何阴影、地表间接光或材质 unlit 扩展标记。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_surface_sunlight_probe','dk_sunlight_demo') `
  -TestRegex '^dk\.render\.sunlight_.*_gpu_validation$' -Reason '验证M14.4日光接收与离屏样例'
```

接口、法线/数值边界见[管线设计](../design/render-pipeline.md)，验收证据见[0097](../development/0097-surface-sunlight.md)。
