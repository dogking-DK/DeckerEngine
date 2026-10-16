---
created_at: "2026-10-14T14:37:00+08:00"
updated_at: "2026-10-16T14:54:53+08:00"
---

# GPU表面、空气、云与渐进路径追踪

[返回项目入口](../../README.md)。[M16.6设计](../design/render-pathtracing.md#m166-gpu无历史lambert实施契约)
提供硬件三角形求交、NEE/MIS/RR和拥有型HDR结果，使用与[CPU参考](pathtracing-reference.md)一致的冻结输入与采样模型。
提供≤64×64无历史参考和≤2048×2048渐进接口，支持Lambert/GGX、baseColor纹理/alpha-mask，以及显式GPU空气/云积分。
[编辑器](editor.md#路径追踪预览m169)与capture当前接入表面模式，空气/云联合接入按M17.5推进。

## 构建与验证

```powershell
cmake --preset windows-graphics -DDK_BUILD_RENDER_PATHTRACING=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\..*_gpu_required$' `
  -Reason 'GPU表面路径追踪解析/统计/图像/寿命/分配失败验收'
```

开关默认OFF（windows-editor预设开启），消费者链接`dk::render_pathtracing`，包含`dk/render/PathTraceRenderer.hpp`。
Scene/Math/Memory和Graphics Device/Graph/Shader须已启用；GPU开关同时提供CPU reference，
单独开启reference不链接GPU。required探针要求真实RT与Khronos validation，同步验证由CTest设置，缺失不按77跳过。
本机旧AMD隐式层的进程级兼容处理及实测见[0114](../development/0114-gpu-pathtracing.md)。

## 场景、提交与读回

使用`PathTraceScene::create`冻结RenderScene与被引用的CpuAsset；接收opaque/mask及baseColor纹理。
默认Lambert要求metallic=0；GGX输入使用PathTraceScene::create(heap, renderScene, assets, {}, PathTraceMaterialModel::metallic_roughness)。
材质模式冻结在scene中，prepare_scene自动采用，并由image.info().material_model报告。
GGX边界与粗糙度下限见[CPU指南](pathtracing-reference.md)。
GPU renderer读取冻结的世界三角形，保留完整仿射结果、几何法线、单/双面、发光与面积权重。
浮点转换导致几何退化或无法表示的正面积/光源权重会被拒绝，旧场景保持有效。

带纹理设备在DeviceOptions中设置`sampled_image_indexing=SampledImageIndexingMode::required`（或if_available），
并另设`ray_tracing=RayTracingMode::required`。renderer根据实际启用状态选择固定64槽纹理变体；
未启用纹理能力时prepare_scene明确拒绝贴图，原无纹理路径仍可用。Shader和布局分别检查能力及设备descriptor limits。
最多64张、单边2048、总64MiB RGBA8；使用标准sRGB解码后上传RGBA32F，图像与staging各最多256MiB。
LOD=0使用mag_filter，支持nearest/linear与repeat/clamp/mirrored_repeat；UV不翻转，alpha不做sRGB解码。
mask同时作用于radiance与visibility，发光mask不进入NEE灯分布，仍可被BSDF命中。

以下展示生命周期；`heap/queue/source/view`分别是开放Memory、已启用RT的提交队列、PathTraceScene和匹配RenderView。
为突出调用顺序使用value()，正式调用应检查Result错误。

```cpp
auto renderer = dk::render::PathTraceRenderer::create(
    heap, queue, "engine/render/pathtracing/shaders").value();
auto scene = renderer.prepare_scene(queue, source).value();
dk::render::PathTraceSettings settings;
settings.environment_rgb = {0.1, 0.1, 0.1};
auto frame = renderer.submit_view(queue, scene, view, settings, {0, 256}).value();
// 实际交互应轮询；这里显式等待以取得数值结果。
if (queue.wait(frame.submission()).value()) {
    auto image = frame.read(queue).value().value();
    const auto rgb = image.linear_rgb();
    const auto moments = image.second_moment();
    const auto hits = image.primary_hits();
}
```

同一队列串行操作；无需在场景构建与trace之间CPU等待，Graph记录GPU依赖。
`prepare_scene`发布不可变GPU场景和BUILD票据，空场景不创建AS，其场景票据value为0。
相同已消费几何/材质/纹理可复用两个弱缓存中的owner与原BUILD票据；新wrapper仍保留最新场景来源。
geometry_fingerprint只作为预筛，随后逐内容比较；环境或revision改变不会单独重建AS/纹理，资产内容改变会重建。
`submit_view`发布frame及提交票据，不代表GPU已完成；`frame.read(queue)`不阻塞等待，成功但optional为空表示仍在途。
完成后read返回独立CPU拥有型结果；读回或分配失败不发布部分图像，额度恢复后可重试同一frame。
关闭Memory后旧image仍可读，新准备/提交/read拒绝。丢弃renderer/scene/frame不会取消已提交GPU工作，queue.close负责完成和回收。

固定射线数值对照使用`submit_rays(queue, scene, rays, settings, samples, first_pixel)`。
射线归一化、坐标和距离单位米，最多4096条；first_pixel指定计数采样的起始像素编号，不能溢出。
无历史接口的请求显式给出连续样本区间；渐进接口由renderer维护completed_spp。失败/取消由调用者保留上一成功scene/image。
stop_token只在CPU阶段、提交前及读取发布前检查，已提交GPU不可硬抢占。

## 每视图渐进会话

先用configure_view提交完整输入，随后由串行worker事件循环驱动submit_batch和poll。
poll只读取当前tile的4字节状态，不等待、不提交下一tile；返回true表示整个批次成功发布。
没有新输入时不必重复configure，同输入重复配置也不会重置。两个view_id各自累积，renderer最多一个渐进批次。

```cpp
dk::render::PathTraceSchedule schedule;
schedule.target_spp = 64;
schedule.batch_spp = 4;
schedule.tile_edge = 32;
renderer.configure_view(queue, 1, scene, view, settings, schedule).value();
// 每次worker调度调用；false可能表示仍在途、暂停或已经达到目标。
renderer.poll(queue).value();
renderer.submit_batch(queue, 1).value();
auto progress = renderer.status(1).value();
auto completed = renderer.completed_frame(1);
if (completed) {
    // 只读GPU缓冲，8个float4/像素；曝光在消费者显示阶段应用。
    const auto* linear = completed.linear_buffer();
    // 仅需要CPU数值时调用completed.read_linear(queue)，返回异步PathTraceFrame。
}
```

target为1..65536，batch为1..16，tile边长1..64；max_rays使tile进一步缩小，目标总工作量≤2^32。
已完成样本可能大于降低后的目标，此时暂停新提交并报告实际计数。暂停不取消当前tile，恢复后继续当前批次。
曝光/批量/tile/目标/帧序号不重置；场景版本、相机/投影/尺寸、积分参数、session/gesture变化重置。
资产或手势更新应先冻结并prepare新场景，再配置匹配的RenderView，不能复用旧资产快照。
reset清空当前完成历史；cancel丢弃在途批次但保留已完成历史；release_view释放槽。
这些操作不硬抢占GPU，调用者仍须poll回收迟到票据；不能将旧代结果发布到重用的view_id。
提交或tile诊断失败保留已完成图像；重试从原completed_spp开始。device_lost终止会话，不自动重试。

create末参数memory_limit默认1GiB，可收紧到至少1MiB；memory_stats报告自有分配和全设备VMA池统计。
预算跟随GPU资源owner，在途工作、外部保留的completed及显式读回也占额度；丢弃不用的帧后可重试。
历史128字节/像素，2048²单张为512MiB，两张加场景超过默认额度，因此尺寸上限不保证所有组合可运行。
驱动隐藏分配不计入额度；host-visible与device-local可能重叠，VMA block是全设备共享池。
M16.9已由GPU tone直接读取完成缓冲并交给Editor，不做每批全帧CPU读回。

## 线性结果与诊断

- `linear_rgb()`是曝光前线性RGB均值；`second_moment()`为样本RGB平方的均值。无tone、去噪或亮点截断。
- `primary_hits()`记录每像素本区间第一个样本的主射线命中，含距离、重心、世界法线、front_face和Entity/Asset/primitive/triangle来源；miss为空。
- `info()`记录scene/revision/fingerprint、视图、settings、样本区间、fixed_rays/first_pixel和实际trace射线数。
  texture_count/masked_emitter_count记录冻结纹理数及未纳入NEE的mask发光三角形数，model_version=3、sampler_version=1。
  空场景compute的硬件射线数为0；硬件三角遍历计数不可得，triangle_tests为0而非测得0次遍历。
- NaN/Inf、负辐亮度和非法诊断拒绝整帧。指纹仅作稳定诊断摘要，不是安全校验或缓存相等证明。

无历史接口图像≤64×64、场景≤4096三角形、样本区间位于[0,65536)、反弹1..32，沿用CPU工作上界预检，超额不自动降质。
旧surface入口的太阳是冻结Scene方向太阳、环境为常量RGB；空气/云启用时必须显式surface_only。
空气传输需调用下节独立入口，不能向旧接口传入model=4。
BLAS从世界顶点合并构建，尚无资产BLAS复用、UPDATE/refit、跨队列AS；GPU浮点与CPU双精度不承诺逐位相同。

## 世界空间空气路径

`submit_air_rays`、`submit_air_view`和`configure_air_view`消费同一冻结环境的PathTraceScene与PathMediumSnapshot，
使用VolumePathSettings（model=4、sampler=2），反弹0..32；0仅保留无散射透射和终端发光。
空气专用接口要求clouds.enabled=false；零密度但enabled的云也明确拒绝，含云请使用下面的通用介质接口。
空气关闭时可验证真空退化；开启但系数为零时行星仍不透明。相机地下、环境不匹配或超出支持域明确返回错误。

```cpp
// environment须与创建source的RenderScene环境一致；view也须匹配source。
auto medium = dk::render::PathMediumSnapshot::create(environment).value();
dk::render::VolumePathSettings air;
air.max_bounces = 8;
air.sun = true; // 有限太阳圆盘；不可再叠加方向太阳或含太阳的Sky LUT。
renderer.configure_air_view(queue, 1, scene, medium, view, air, schedule).value();
// 后续共用submit_batch/poll/completed_frame/display；独立参考可用submit_air_view。
auto reference = renderer.submit_air_view(queue, scene, medium, view, air, {0, 64}).value();
```

主网格查询遵守near/far，空气、后续反弹和阴影按物理边界继续。
阴影同时检查几何/mask、行星及空气透射；Rayleigh/Mie真实散射、Lambert/GGX、有限太阳和面积灯共用NEE/MIS/RR。
空气结果info.medium保存完整环境、介质和体积设置；公共settings报告model=4/sampler=2及公共字段。
primary_hits只记录第一个样本直接到达的网格，先发生介质事件或命中行星时为空。
statistics.rays在空气模式计逻辑几何查询（含空几何）；triangle_tests仍不可得，不报告GPU介质事件统计。

max_tracking_steps按一次dispatch计费，渐进时是一个tile；max_steps_per_path约束单路径含阴影的tracking draw。
tracking额度耗尽、无效系数/结果拒绝整个候选，保留旧completed_spp与帧；调整预算后可重试。
预算/曝光/批次/tile/目标变更不重置历史，物理环境或积分参数变更重置；正在执行的批次冻结旧预算。
空气参数、计数/错误缓冲、读回、纹理和AS共享renderer额度；弱缓存不延长无人持有的GPU资源寿命。
同设备相同样本跨batch/tile保持连续累积；CPU双精度与GPU float按数值/统计门槛比较。
此入口不依赖M14/M15的视图LUT，未接Editor/capture，也不构成Sponza交互性能承诺。

## 世界空间云路径

`submit_medium_rays`、`submit_medium_view`和`configure_medium_view`使用同一model=4/sampler=2积分器，
接受空气、非均匀云及二者组合。先由CloudFieldCache生成不可变场，再创建PathMediumSnapshot；时间显式冻结。

```cpp
dk::render::CloudFieldCache fields;
auto field = fields.prepare(environment.clouds).value();
dk::render::PathMediumSettings physical;
physical.time_seconds = 17;
physical.cloud_albedo = .9;
physical.cloud_anisotropy = .6;
auto medium = dk::render::PathMediumSnapshot::create(environment, physical, field).value();
dk::render::VolumePathSettings transport;
transport.max_bounces = 8;
renderer.configure_medium_view(queue, 1, scene, medium, view, transport, schedule).value();
// 固定射线/无历史图像也可使用submit_medium_rays/submit_medium_view。
// 与空气和表面模式共用submit_batch、poll、completed_frame、read_linear、display。
```

云coverage/density/extinction为0时无需field，退化为空气；clouds.enabled仍保留行星边界。
非零云的GPU支持域：R>=1000米、weather/shape尺度>=100米、detail尺度>=10米，
壳厚和正系数在float中不能归零；超域拒绝配置并保留旧视图。其他边界与CPU体积/空气接口相同。
CPU用双精度去中心、扣风位移并缩为周期坐标；GPU传播相位，显式repeat线性插值读取原RG8/R8场。
不依赖M15光照、屏幕历史或近似多散射增亮。任意主路径/反弹和NEE均查询真实消光，
已知无云段使用空气上界、云段使用空气加云密度硬界；发生上界低估或预算耗尽会拒绝整批。

最新成功使用的场保存在renderer，并可复用其他视图/在途代持有的相同场；key/hash变化才上传。
GPU field buffer、候选及在途资源均计入同一额度；旧场可能与候选同时占用，额度不足明确失败。
释放视图后最近的缓存场仍保活，renderer析构时释放；外部完成图保留线性结果与来源，不需要保留噪声场。
时间/风/太阳/密度等改变重置历史但复用场；曝光/调度不重置。失败上传/提交不替换旧缓存/视图。
结果`info.medium`包含完整环境、物理与积分设置及`field_hash`，替代M17.3的`info.air`字段。
required GPU验收覆盖数值、遮蔽、多次散射和失败寿命；记录见[0126](../development/0126-gpu-cloud-pathtracing.md)。
Editor与capture的Scene media选项仍在M17.5接入，当前入口是C++模块API。

## 可执行验收与报告

[PathTraceProbe.cpp](../../tests/integration/PathTraceProbe.cpp)提供原五组required测试：
解析/几何/跨尺度阴影、8 seed光照统计、完整图像与分段、失败/在途寿命、内存分配失败和恢复。
CPU oracle使用独立双精度暴力求交，面积灯再用独立二维积分校核；参考先验证N/4N收敛再对比GPU。
新增texture_numeric/mask/lifetime/disabled/allocations，以及启用Assets时的texture_assets组；
覆盖64张发散访问、过滤/UV/wrap、双面/mask发光、能力拒绝、失败寿命，以及glTF导入/产物重载到GPU。
ggx_numeric/statistics/indirect/lifetime验证材质端点、白炉/面积灯、多次反弹、MIS/RR及纹理组合与在途寿命。
progressive_images/state/failure/budget/lost验证分块/65536spp一致性、重置矩阵、迟到/失败/取消、额度与终端device loss。
air_numeric/statistics/progressive/failure/resources覆盖空气CPU对照、解析吸收/单散射、8 seed收敛、低太阳多散射、
近地/远景、反射/mask、分批连续性、几何复用、失败与1MiB资源额度；实现及结果见[0125](../development/0125-gpu-air-pathtracing.md)。
cloud_numeric/statistics/surface/progressive/failure覆盖云壳/透射、混合散射与白炉、几何遮蔽/反射、固定时间与场复用、额度/失败恢复。
会话实施与实测见[0117](../development/0117-pathtracing-progressive.md)。
材质实施与实测记录见[0115](../development/0115-pathtracing-textures.md)和[0116](../development/0116-pathtracing-ggx.md)。

images用例在构建目录`tests/integration/pathtrace-hdr.json`写入4×4、seed=42、256spp线性数据。
报告的comparison对象单独记录8 seed、4096/16384spp对照协议和nRMSE，不能把这组误差当作256spp图像的参考误差。
真实结果、门槛与未验证范围见[0114](../development/0114-gpu-pathtracing.md)。

## GPU 显示图像

`renderer.display(queue, completed_frame, exposure_ev, encode_srgb)`返回RGBA8 UNORM图像和提交票据，
不重采样、不隐式等待、不读回CPU。曝光使用2^EV，Reinhard输出到sRGB交换链前保持线性；
仅在非sRGB目标前设置encode_srgb=true。图像和参数计入同一个renderer额度。
图像票据完成后，可用export_image/import_image交给同设备同队列族的GUI队列；消费者持有期间继续计费。
共享原生队列的native调用须持Device::queue_gate；每个SubmissionQueue ledger仍只允许一个串行所有者。


## 自动化固定采样

[render.capture](../commands/render.md#固定-spp-路径追踪)提供显式pathtrace后端、精确spp、进度/取消、PPM和单文件线性JSON。
[Python](../../examples/automation/pathtrace_capture.py)与[Luau](../../examples/scripting/pathtrace-capture.luau)示例共用该服务。
命令能力由runtime.capabilities.pathtrace_capture报告；缺RT时失败，不自动退回光栅。
