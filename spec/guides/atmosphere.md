# 独立离屏大气天空

本入口生成正午、日落和100km高空的960×540 PPM，使用球面对称大气、单太阳和T/MS/Sky View LUT。
场景几何日光接收、空气透视和Editor/capture集成见下方；固定质量集与1080p性能复现见[基准指南](atmosphere-benchmark.md)。

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
  -TestRegex '^dk\.atmosphere\.([^_]+|(reference|multiple|cache|vacuum|continuity|demo)_gpu_validation)$' -Reason '验证大气参考、LUT和离屏天空'
```

数值门槛、缓存key和提交点见[模块设计](../design/render-atmosphere.md)；实现和验证证据见[0096](../development/0096-atmosphere-luts.md)。


## Lambert 日光接收

```powershell
cmake --build out/build/windows-graphics --config Debug --target dk_sunlight_demo
& ./out/build/windows-graphics/bin/Debug/dk-sunlight-demo.exe ./out/sunlight
```

样例生成 noon/sunset/twilight × unlit/direct/sky 共九张 960×360 PPM：
三个白色物体依次为球体、非均匀缩放椭球和负缩放椭球。direct 只接太阳直射；sky 再加天空漫射。
三个预设都固定曝光5，背景为固定clear色，便于比较；此样例只调用render_sunlight。
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

## 天空、几何与空气透视合成

ScenePipeline 的 render_atmosphere 将几何深度转换为相机至表面的真实光程。
背景取 Sky View/太阳盘，表面按 RGB `L_air + T_air * L_surface` 合成，最后统一曝光和tone。
prepare 必须使用当前相机的精确世界位置，快照环境必须与 RenderView 的场景一致。

```cpp
const Vec3d camera_m = view.description().camera_world.matrix().topRightCorner<3,1>();
auto sky = atmosphere.prepare(queue, view.scene().environment(), camera_m);
if (!sky) return std::unexpected(sky.error());
render::RenderSettings settings;
settings.linear_readback = true; // Only enable for diagnostics: <= 65536 pixels.
render::AtmosphereSettings air;  // Lambert surface, solar disk; normal LUT path.
auto frame = pipeline.render_atmosphere(queue, view, assets, *sky, settings, air);
if (!frame) return std::unexpected(frame.error());
auto ready = frame->wait(queue);
if (!ready) return std::unexpected(ready.error());
if (!*ready) return std::unexpected(Error{ErrorCode::invalid_state, "frame is pending"});
std::vector<std::array<float,4>> pixels(std::size_t(view.description().width) * view.description().height);
auto read = frame->read_linear_rgba32f(pixels);
if (!read) return std::unexpected(read.error());
```

shader目录沿用工程源码布局；aerial.slang相对包含大气模块的共享传输文件，部署时需一起保留该布局。
相机支持刚体、有限 forward-Z Vulkan 透视和非对称偏移/Y翻转，far<=1000000m、far/near<=10000；
完整系数范围见[管线设计](../design/render-pipeline.md#m145-空气透视与线性合成实施契约)。不支持正交、reverse-Z、无限远或缩放相机。
air.surface_lighting=false 用原 unlit 表面参与空气透视；air.sun_disk=false 去掉背景太阳盘。
air.reference=true 限4096像素，以高采样直接积分作诊断（MS仍为同一闭合近似）。
linear_readback 关闭时不会分配线性读回缓冲；开启后 wait 完成才能读，得到曝光前 HDR，RGBA8输出仍可照常读取。
每帧两张64×64×128 RGBA32F L/T体积共16MiB，接近地表截断边界的像素直接积分以避免插值偏差；不跨帧缓存。
disabled环境走原unlit，旧render/render_sunlight不自动添加天空或空气透视。Editor/capture通过render_environment自动按环境开关绑定；C++调用者仍可用显式入口。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target dk_aerial_perspective_probe `
  -TestRegex '^dk\.render\.aerial_.*_gpu_validation$' -Reason '验证M14.5空气透视/深度/HDR合成'
```

探针在构建目录 tests/integration 下输出近远/高空和mask的PPM，数值证据来自HDR而非tone后的图像。
验收与实际限制见[0098](../development/0098-aerial-perspective.md)。


## 编辑器和脚本（M14.6）

编辑器顶部Environment按钮切换到环境面板。修改草稿后Apply environment创建一个撤销单元；
Revert丢弃本地草稿并取当前环境。外部命令修改时保留草稿及原guard，过期Apply显示冲突，
不会覆盖外部结果。Undo/Redo/Save/Reload与命令操作同一Scene；Save也会应用草稿。
环境关闭时仍校验参数，但保持原unlit预览。曝光只属于视口会话，截图需显式传相同值。
环境模式near=1m/far=10000m，关闭模式near=.05m；近于1m的几何会被裁剪。
初始世界原点在球面地表，跨到地表以下的几何不在当前渲染支持域；
render-disk示例将planet_center_m.y设置为-ground_radius_m-1000，使全部模型位于安全高度。

大气准备在后台进行，画面准备好后才发布；状态改变时可能短暂保留旧帧并标为过期。
修改请求会合并，旧结果不覆盖新参数。关闭/取消等待已提交GPU回收，不承诺硬中断驱动。
Simulation布料视口仍使用自身渲染入口。当前固定静态时间0和atmosphere_v1质量，性能采样方式见[基准指南](atmosphere-benchmark.md)。

两个示例对render-disk夹具应用相同环境，使用128×96、eye=[0,0,4]/target=[0,0,0]、曝光1、时间0和同一质量：

```powershell
cmake --preset windows-editor -DDK_BUILD_SCRIPTING_LUAU=ON
cmake --build out/build/windows-editor --config Debug --target dk_editor_app dk_run dk_ctl
# 先将 tests/fixtures/render-disk 复制到新的 out/environment-demo 目录。
# 编辑器打开该副本，并以 --pipe dk-environment-demo 启用IPC。
$env:PYTHONPATH = "$PWD/sdk/python"
python examples/automation/environment_capture.py --pipe dk-environment-demo --ctl out/build/windows-editor/bin/Debug/dk-ctl.exe --project-root out/environment-demo
# 关闭编辑器后，用独立runner操作同一夹具；该脚本自行scene.load。
out/build/windows-editor/bin/Debug/dk-run.exe --project-root out/environment-demo --script examples/scripting/environment-capture.luau --script-timeout-ms 120000
```

Python输出包含Job来源元数据；脚本分别生成environment-python.ppm、environment-luau.ppm，覆盖同名文件，
不隐式保存编辑后的环境。完整capture字段与原子发布/取消语义见[命令参考](../commands/render.md)。

定向验收使用Debug、required GPU与同步validation，GUI自动加--background：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor `
  -Target @('dk_editor_app','dk_run','dk_ctl','dk_environment_preview_tests') `
  -TestRegex '^dk\.editor\.environment_(worker|consistency)_gpu_validation$' -Reason 'M14.6环境集成'
```

真实GUI流程验证面板Apply、外部修改、草稿冲突、Undo/Redo、Save/Reload；
冻结旧版本capture与GUI原图、重载capture与GUI新图逐字节比较，实际运行Python/Luau示例并比较图像。
GUI另验证异步预览期间Gizmo提交与截图；后台测试单独验证错队列恢复、尺寸替换、旧代丢弃、取消、失败恢复和在途关闭。产物位于构建目录test-artifacts/Debug/environment-*。
同设备/驱动的固定夹具验收不代表跨GPU像素恒等。证据见[0099](../development/0099-environment-integration.md)。
