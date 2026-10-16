---
module: render-pipeline
created_at: "2026-10-03T16:28:50+08:00"
updated_at: "2026-10-16T10:30:00+08:00"
status: accepted
---

# 最小场景渲染管线

## 天空漫射缓存

日光接收沿用16×16球面方向的Lambert积分，将与表面法线有关的结果预积分到129×65法线方向表。
每个Sky View纹理代只构建一次，表面片段通过四点插值获取单位太阳辐照度响应；太阳能量仍在片段中应用。
表的方位角包含重复端点，极点连续；保持太阳盘不参与天空漫射、背面/缺法线和原地面裁剪语义。
缓存持有不可变Sky快照并以其实际纹理身份判断复用，避免不同renderer代号碰撞。候选计算和上传成功后替换；在途帧保活旧buffer。
用独立高采样角积分、斜法线/接缝/极点和缓存/失败用例验证数值，Sponza固定视图对比优化前后输出及worker耗时。

空气透视近地面边界仍直接积分，不能改成跨地面插值。对太阳高度余弦>0.05、路径长度不超过最小密度尺度的5%、
保守RGB光学厚度≤0.25的短段，使用8段积分；其他段仍128段，参考模式仍512段且独立积分太阳透射。
这避免近距离室内几何每像素重复128段积分；新增1.8m视高、1/10/50m距离和太阳地平线附近的CPU/高档GPU对照。

## 范围与依赖

M7.2 连接 RenderView、GpuAssets 和 Graph，渲染程序化场景，输出可等待的离屏 RGBA8。
engine/render/pipeline 提供 dk_render_pipeline / dk::render_pipeline；PUBLIC 依赖 render_data、
render_resources，PRIVATE 依赖 graphics_graph、graphics_shaders、profiling；构建大气模块时额外 PRIVATE 链接 render_atmosphere，
大气与云模块均存在时再 PRIVATE 链接 render_clouds，公开头仅前置声明云类型。
DK_BUILD_RENDER_PIPELINE 默认 OFF，windows-graphics 启用。无新增依赖/feature。
最小核心不负责磁盘/Runtime/窗口装配；M14.4 增加显式 Lambert 入口，M14.5 增加天空/空气透视合成。不支持完整 PBR、alpha blend、几何阴影或多队列。

M15.3提供显式render_clouds(queue,view,assets,sky,textures,cloud_settings,settings)，
环境完整入口render_environment在实际非零云时走独立混合介质合成，接口、预算与失败边界见
[云合成实施契约](render-clouds.md#m153-混合介质与图像合成实施契约)。零云走原M14；显式unlit/atmosphere诊断入口保持各自语义。

## M16 独立路径追踪规划

[路径追踪草案](render-pathtracing.md)规划独立 renderer，复用 RenderView、资产输入和显示色彩约定，
通过[光追底座](graphics-raytracing.md)与 Graph 执行，不把 depth/opaque 管线改名为路径追踪。
编辑器和 CaptureService 在后续阶段显式选择 renderer；现有默认行为、环境模式及质量标签保持语义。
路径追踪按每视图的连续样本区间累积线性 HDR，完成后再曝光/tone；不复用云重投影历史或视点天空 LUT 计算任意反弹。
本次仅定义规划入口，无新 API、命令或 shader；前置任务和验收见[Roadmap M16](../roadmap.md#m16渐进式路径追踪)。

## 公开入口

ScenePipeline::create(heap, queue, shader_directory) 编译随工程提供的固定 Slang 源并创建
深度/alpha-mask-depth/不透明/tone 管线；shader_directory 显式传入，不硬编码部署路径。
render(queue, view, gpu_assets, settings) 同步构建/录制一帧并异步提交，返回 RenderFrame。
Settings 为有限非负线性 HDR clear_rgb、有限非负 exposure、可选 Graph 诊断及仅日光入口消费的 sky_diffuse 开关。
M8.4 提供 `unlit_preview_settings()` 作为编辑器视口和 Runtime capture 的共用预览设置
（线性 clear_rgb=(0.04,0.08,0.16)、exposure=1），通用 RenderSettings 的默认黑色不变。
队列/管线/缓存均由调用者串行使用；管线无裸队列指针，只用于创建它的队列。

按稳定实体 ID 和原资产引用顺序遍历 mesh 引用，每个 primitive 形成一次 draw。
重复 mesh 引用产生实例，GPU 资源以物理对象去重导入；material/texture 实体引用不推断 override。
缺少 mesh、alpha blend 材质、不可表示为有限 float 的世界到裁剪矩阵均拒绝整帧。
使用完整 world_to_clip * world，保持父级剪切；GPU 顶点着色使用显式四行 dot，避免矩阵内存布局歧义。
首版双面无背面剔除，颜色为 base_color.rgb * sRGB 纹理采样的线性 RGB + emissive；
无纹理使用清为白色的 1x1 图资源。render 保持 unlit；render_sunlight 使用 normals，metallic/roughness 仍不参与 Lambert。

RenderFrame 拥有 Submission、颜色 Image、紧密 RGBA8 readback、SceneId/revision/frame/extent/draw_count。
wait(queue, timeout) 返回完成/超时/错误；read_rgba8(span) 要求匹配字节数，未完成拒绝而不改写输出。
plan_text() 在启用诊断时提供拥有型计划文本；输出 alpha 恒为 1，正高度 Vulkan viewport 行顺序。
color() 的 Image 借用 Frame；跨帧使用者可显式 share。每帧独立资源，不覆盖先前 Frame。

## Graph 与数值约定

1. clear targets：完整清除 HDR RGBA32F、D32 depth=1、LDR RGBA8 及 1x1 white。
2. depth prepass：仅深度附件，Less/test/write，绘制 opaque 和 alpha mask primitives，mask 使用片段采样与丢弃。
3. opaque：HDR 颜色及同一 depth，Equal/test、不写 depth；复用同一顶点 ShaderModule、输入布局和 push 字节，保持深度计算一致。
4. tone mapping：全屏三角形，Texture.Load 读 HDR，不要求 float32 线性过滤；
   对每通道执行 Reinhard `1 - 1 / (1 + max(c,0)*exposure)`，再显式线性转 sRGB。
5. readback：LDR → host buffer，final host-read；颜色和 buffer 同时作为 Graph 输出。

图声明精确覆盖 vertex/index/texture，深度附件访问沿现有 encoder 保守声明 read/write。
每个 Pass 使用 device typed API，Render 不调用裸 Vulkan；Graph 管理初始内容、顺序和同步。
RGBA32F、D32、目标尺寸须满足实际设备能力；无替代格式或隐式降级。
投影继续由 RenderView 调用方提供（Vulkan z=[0,1]），不新增 Camera 持久组件。

## 提交与失败

输入解析、必要分配、Graph 编译、诊断及 Frame 容器准备均在提交前完成。
成功 Graph execute 是提交点，之后只移动拥有者。参数/分配/录制/submit 失败不返回半帧，
不修改 Scene、GpuAssets 或先前 Frame；device_lost 仍遵循队列失效契约。
不在 render 内等待；timeout 不丢弃 Frame。Frame/管线/缓存可先于 GPU 完成释放，pending owners
保活资源；最后通过 queue wait/poll/析构 drain 回收。Memory closing 后旧帧查询和读回仍有效，
拒绝新帧。Error 和 Slang/Device 已有内部 noexcept 分配边界不扩大为任意进程 OOM 保证。

## 验证与取舍

程序化重叠几何验证深度与遮挡、实例/相机变换、纹理 sRGB 解码、HDR emissive、曝光/tone，
检查背景/前景数值、完整图像重复性、不同尺寸、空场景及场景版本绑定，并输出 PPM/图计划。
验证缺失资产/alpha blend/溢出拒绝、提交/对象创建失败、等待超时及在途卸载/结果/管线销毁。
底层 HDR footprint/对齐/溢出、深度 clear/仅深度管线增加回归，保留旧颜色+深度路径。
固定几何选择避开像素中心落边界的输入，完整图像与 CPU 参考比较时 RGB 容差为 1 byte、alpha 精确；
同设备重复帧逐字节比较。不做 GPU 性能或跨厂商 bit-exact 承诺。

关联 [数据](render-data.md)、[资产](render-resources.md)、[Graph](graphics-graph.md)、
[Vulkan 使用层](graphics-vulkan.md)、[0062](../development/0062-render-pipeline.md)。

## M7.3 alpha mask

支持 opaque 和 alpha mask，拒绝 blend。mask 的 depth fragment 与颜色 fragment 使用一致的
采样 alpha * baseColor.a < cutoff 丢弃，顶点模块一致；深度 Pass 额外声明纹理采样。
opaque 保留无 fragment 的 depth-only 管线。磁盘装配独立在 [render-disk](render-disk.md)。

## 环境集成与后续边界

[大气](render-atmosphere.md)与[体积云](render-clouds.md)提供冻结环境快照、显式C++参考时间与有界图像合成。
M14.6通过共用环境入口接入视口/capture；M15.3在非零云时改用云/空气联合积分，表面HDR只在终端加入。
旧场景默认关闭环境并保留unlit输出。云参考限4096像素，尚无实时大尺寸/时域历史。
缓存保存静态程序和不可变代次，保持旧RenderFrame的独立寿命，不复用跨帧Graph句柄；显式优化云入口另有下述有界每视图历史。
实施每阶段时同步本设计与真实接口、诊断和测试；阶段顺序见[Roadmap](../roadmap.md)。

## 深度采样底座

场景D32目标增加Sampled usage，创建时核对附件/clear/sampled组合能力。现有unlit图顺序和输出保持原契约；
深度写后采样屏障由Graphics Graph体积探针独立验证，M14.5 使用它重建世界射线并完成大气合成。

## M14.4 最小日光接收实施契约

新增显式 `render_sunlight(queue, view, assets, sky, settings)`；原 render/unlit_preview_settings 保持旧路径。
启用且环境完全匹配的 AtmosphereSky 提供太阳、T 与不含太阳盘的 Sky View；disabled 环境回到原 unlit。
未构建大气时此入口返回 not_supported；pipeline 仅在该 target 存在时 PRIVATE 链接大气。
不自动准备或刷新 LUT，不增加 render 内部等待；调用方先 prepare，再绑定同版本 Scene 环境。
该入口仅接收日光；M14.5 的 render_atmosphere 负责天空背景和空气透视。曝光仍只在 tone 一次执行。

几何输出为 base_color * linear_texture / pi * (E_sun * T_sun(position) * max(N dot sun_direction,0)
+ E_sky(N)) + emissive。直射在每片段真实世界位置查 T 并检查行星遮挡；GPU 使用相对快照锚点的 km
和独立高度计算。仅render_sunlight使用世界轴对齐包围盒保守拒绝地下接收体；完整大气合成允许几何跨地面，
片段照明将负高度截到地表，最终空气合成按相机射线的行星交点遮挡地下几何，不移动场景或行星。
最远角点不超过2H、位置分量不超过±1e9m的限制仍保留。太阳采用中心方向的方向光，不对盘面积重复积分。
天空漫射在快照锚点的 Sky View 上用固定 16×16 等立体角方向积分 L * max(N dot omega,0)，缓存为上文的法线方向表；
这是局部环境漫射近似（所有接收点共享锚点天空），不宣称逐点 GI；可通过 settings.sky_diffuse 关闭以诊断直射。
Sky View 不含太阳盘，避免把太阳重复计入漫射。无镜面/PBR、几何阴影和地表间接反弹。

完整仿射矩阵的逆转置变换法线，片段重新归一化；奇异/数值不可逆变换拒绝 lit 整帧，unlit 保持原约定。
缺失、零或插值抵消法线使用屏幕导数计算几何面法线；退化三角形无有效面积，安全返回零漫射。
负 determinant 修正正反面判定；lit 的单面材质在 depth 和 color 同时丢弃背面，双面翻转背面法线。
约定依据 [glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#double-sided)，
unlit 历史双面可见行为不变。alpha-mask 两 pass 使用相同裁剪规则，emissive 不乘光照或太阳透射。

AtmosphereSky 增加只读锚点和借用 LUT Image 查询。Graph 显式导入并声明 fragment sampled read，
逐 draw uniform 声明 vertex/fragment uniform read。首次显式调用按需创建 lit 深度/颜色管线，完整候选成功后发布为静态缓存；后续绘制失败不撤销已创建的静态管线。提交保留资源，旧快照/帧不因 prepare 或释放调用者而变化。
环境不匹配、非法矩阵/范围、分配/创建/记录/提交失败均不改变旧帧、场景或大气代次。

验收：真空 Lambert 解析 RGB/方向/斜法线、剪切/负缩放、单/双面、缺失/零法线、alpha-mask 和 emissive；
真实大气直射对 CPU 高采样透射，天空漫射对高密度角积分，量化前固定 RGB tone 后容差 2 byte；
太阳能量线性与曝光一致、disabled/显式 unlit 完整图一致；版本错配/对象/提交/预算/跨队列失败和在途寿命。
局部 CPU 矩阵测试与原 pipeline GPU 图像回归，required GPU 探针输出可复现 PPM。

## M14.5 空气透视与线性合成实施契约

显式 render_atmosphere(queue, view, assets, sky, settings, atmosphere_settings) 绑定同环境、同相机位置的不可变快照。
AtmosphereSettings 包含 surface_lighting、sun_disk、reference；前者默认启用 M14.4 日光，可关闭以诊断 unlit 表面。
disabled 环境直接走旧 render；不自动接入 Editor/capture（M14.6）。不修改 Scene、资产、大气缓存代次或旧帧。

首版限定有限 forward-Z Vulkan 透视（z=[0,1]、相机朝 -Z，允许非对称投影与 Y 翻转），相机必须为刚体。
near>0、far<=1000000m、far/near<=10000，|p00|/|p11|各自>=0.1、|p02|/|p12|各自<=2；拒绝 reverse-Z、正交、无限远及缩放/剪切相机。
D32 重建轴向距离 z=p23/(depth+p22)，再乘未归一化射线长度得到真实米制光程；积分从相机开始。
投影系数先以 double 校验，再上传 km；与大气共用局部球面坐标，避免地球半径 float 相减。

每帧生成两张 RGBA32F 64×64×128 视锥 3D 表，分别存单位太阳能量的 RGB L 与 RGB T；
XY 覆盖 NDC 端点，Z 为 far*(slice/127)^2 的轴向距离。128段积分并查 T/MS；零距离切片为 L=0/T=1。
接近行星边界时截断光程的导数不连续，不能对跨边界体素盲目插值：
以视锥单元的保守世界空间跨度检测终点高度，落入该边界带的几何像素直接执行128段有限积分（仍查T/MS），
避免地表附近T因插值截断路径而偏高。其余像素三线性查表；诊断reference始终走512段直接太阳积分。
需要设备支持 3D storage/sampled/linear-filter，无隐式格式降级。图内资源不跨帧缓存，没有历史重投影。
深度为1的背景查 Sky View 并独立加入太阳盘；几何仅计算 L_air*E_sun + T_air*L_surface 一次，
实际射线被行星遮挡时截断且不透出行星后的几何。背景不再应用 AP，太阳盘不进入漫射。
合成写独立 RGBA32F，再统一曝光/Reinhard/sRGB，alpha恒1。depth/mask规则来自同一几何 prepass。
reference诊断最多4096像素，逐像素512视线×512太阳积分，MS沿用同一闭合表，用于区分离散误差和模型近似。

RenderSettings.linear_readback 默认关闭，启用时最多65536像素；RenderFrame.read_linear_rgba32f 只读最终合成、曝光前的紧密RGBA。
适用于 unlit/sunlight/atmosphere/clouds，不暴露可修改 HDR/G-buffer。未启用、尺寸不符、未完成拒绝且不改写目标。
Graph 显式声明深度附件到compute sampled、体积storage到sampled、合成storage到tone/transfer以及host-read同步。
参数、临时资源、计划、返回容器均在提交前准备；成功execute后只移动拥有者。
首次调用完整创建两条compute管线后发布为静态缓存，后续绘制失败不撤销该缓存（与M14.4一致）。失败保留旧帧/场景/代次，pending保活。

验收在实现前固定：T绝对误差<=2e-3；HDR归一化RMSE<=3%、p99<=8%，尺度=max(参考RGB亮度p95,1e-3)。
近远景、地平线/遮挡边缘独立检查；CPU有限段高采样参考与GPU reference、不同near/far/高度/视角、RGB吸收、
alpha-mask、真空/零距离、曝光只执行一次、disabled完整图一致，以及创建/提交/预算/版本/队列/在途寿命。
不达标时调整采样/映射并记录，不放宽阈值；性能和上层集成留给后续阶段。

## M14.6 上层共用环境入口

render_environment(queue,view,assets,settings)根据冻结RenderScene环境选择旧render、render_atmosphere或M15.3的render_clouds。
启用时惰性创建AtmosphereRenderer并同步prepare相机锚点，复用实例最近LUT；调用方必须放在允许等待的worker。
不把这一入口当成无等待的render_atmosphere。CPU-only条件编译保持，未构建大气时明确拒绝enabled。
shader_fingerprint(directory,environment)提供固定相关Slang源码集的诊断指纹，包含共享传输文件和算法标识；不承担密码学完整性。

M14.6集成补充：投影矩阵反算near/far的两个上界允许1e-10相对舍入容差，
以接纳数学上恰好far/near=10000的合法投影；不扩大实际支持域，超过上界1e-6的输入仍拒绝。

共用入口首次prepare使用候选AtmosphereRenderer，整帧成功提交后才保留该实例；
首次传错队列或视图失败不把调用者的错误队列固化进缓存，后续合法调用可恢复。已有实例仍遵循AtmosphereRenderer的队列校验。

## M15.3 云合成与诊断

非零云准备不可变场/纹理，使用独立mixed compute替代整段AP，随后执行既有tone/readback。
云有界参考、空气开关/多散射、深度/太阳裁剪与零云退化遵循[云设计](render-clouds.md#m153-混合介质与图像合成实施契约)。
Graph只导入实际消费的LUT（unlit云表面不导入Sky），depth、HDR、云场、T/MS和诊断均声明真实读写。
read_cloud_rgba32f返回云散射RGB/独立Tc，read_medium_rgba32f返回混合RGB透射/云代表距离米；
只有cloud_settings.diagnostics=true的非零云帧可读，未完成/未请求/尺寸错误不写目标。
所有路径通过创建队列的身份资源拒绝跨队列；static program按完整候选发布，帧提交保活实际对象。
共用入口的首个云owner也在整帧成功后保存，后续prepare失败不破坏旧帧或场景；time固定0。

## M15.4 显式优化与历史入口

render_clouds_optimized使用CloudOptimizedSettings，独立于原render_clouds和共用reference截图。
三段Graph为half transport→completed-history reprojection→depth-preserving reconstruction；
几何D32与表面颜色保持全尺寸。保守跳跃、早停、局部直接积分回退及误差门槛以
[云优化契约](render-clouds.md#m154-优化与时域实施契约)为准。
低角度重建的太阳自遮蔽最低32步；最坏工作预算同步按32步核算，上限128Gi逻辑工作量（不是显存字节）。
shader指纹包含low-sun-32算法标记，避免将旧采样结果与本版本混用；质量与性能证据见[联合报告](../benchmarks/2026-10-13-clouds.md)。

每Pipeline至多4个显式view_id，各保存半尺寸L/T/云源三张RGBA32F的已完成源与在途候选。
候选图像在提交前分配，execute成功只发布pending；poll_cloud_history非阻塞确认完成后才能作为source。
同视图在途候选未完成时有界拒绝，不隐式等待或覆盖；release_cloud_history要求该视图无在途候选。
所有历史逻辑图像载荷合计≤256MiB；历史只保留三张介质图，不保留完整Frame/Graph/几何资源。
旧Frame和队列提交独立保活资源。分配/录制/submit失败不推进completed序号，下一帧拒绝旧历史。

cloud_history返回completed/pending序号与复用状态；Frame.cloud_optimization返回实际积分尺寸、
source/candidate序号和重置原因。diagnostics开启时read_cloud_statistics返回GPU计数，
包含实际主步/云光照次数、跳跃/早停省略、全尺寸回退/重建、历史接受/拒绝数量及64位主密度查询数。
read_cloud/medium_rgba32f保持全尺寸语义，HDR仍在曝光前。统计未请求或GPU未完成则拒绝读回。
显式关闭temporal不读取或发布历史；固定时间reference截图不接入这些视图槽。

## M15.5 可选云透射阴影快照

render_sunlight/render_atmosphere/render_clouds/render_clouds_optimized接受可选CloudShadowMap，
默认无云影保持原行为。日光片段仅以阴影图乘直射项，再独立加入天空漫射和emissive；
单位默认白纹理保持零快照路径。Graph显式导入图并声明opaque的fragment sampled read。
快照必须匹配完整环境，接收体投影/云底范围、队列身份以及云合成time/诊断模型均预检。
Frame保留快照，提交成功更新图的最后使用序号，使预算覆盖消费者在途寿命；失败不发布使用。
该快照仅改变终端表面，M15.4历史保存介质量，换图无需因表面光照改变而重置介质历史。
详细参数、误差与生命周期见[云影契约](render-clouds.md#云影与交互闭环)。

## M15.6 共用环境渲染会话

render_environment增加EnvironmentRenderSettings，按reference/temporal选择已验收云路径，并用相同time和密度模型准备可选固定世界云影。设置独立于Scene；默认reference/time=0，完整区域显式可覆盖，接收域仍遵循M15.5拒绝规则。
候选Atmosphere/CloudEnvironment仍在成功返回Frame后保留；每个CloudEnvironment持有云场/纹理及ShadowRenderer，Frame持有真实快照。调用者负责等待后poll历史；共用入口进入temporal时先收集已完成候选。reference不发布或读取历史，切换回temporal必须重置原视图历史，避免跨模式续用。
