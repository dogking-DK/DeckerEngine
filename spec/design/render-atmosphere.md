---
module: render-atmosphere
created_at: "2026-10-12T14:38:00+08:00"
updated_at: "2026-10-13T12:24:00+08:00"
status: accepted
---

# 大气渲染设计

## 目标与当前基线

CPU环境模型、持久化与命令已接入；Graphics支持3D资源与深度采样，独立大气模块提供CPU参考、T/MS/Sky LUT和离屏天空。最小日光接收、视锥空气透视和天空/几何合成由 ScenePipeline 提供，环境视口与capture使用共用入口。阶段、顺序与验收状态统一维护在
[Roadmap](../roadmap.md)；各后续小阶段开工时再细化具体接口。

默认面向Windows x64、Vulkan/Slang、地球尺度的地表至高空实时观察；支持昼间、日出/日落、
行星遮光造成的暗侧和空气透视，供编辑器与离屏截图使用。采用单太阳、球对称大气、线性RGB近似。
首轮验收相机高度为地表上方1m至100km；大气顶外的稳定处理需要测试，但轨道级行星地形不是交付要求。
不含月亮/星空、天文历法、天气流体模拟、完整光谱渲染、地形系统或完整PBR/IBL。
接入普通ScenePipeline场景视口与render.capture；独立RenderSimulation布料视口不在本轮范围。

初始规划代码基线为744e737，以下表格维护当前基线及后续缺口：

| 已核实的现状 | 本轮需要补齐的能力 |
| --- | --- |
| [ScenePipeline](../../engine/render/pipeline/include/dk/render/ScenePipeline.hpp)提供clear/exposure、默认unlit、Lambert及天空/空气透视合成；视口/capture共用 | 保持原unlit路径及公开显式入口 |
| [资源描述](../../engine/graphics/device/include/dk/graphics/Resources.hpp)在M14.2扩展原生3D image/view、RGBA16F和三轴传输 | 后续LUT选择实际设备支持的格式，不把array_layers当连续z坐标 |
| [深度目标](../../engine/render/pipeline/src/ScenePipeline.cpp)在M14.2增加sampled usage和Graph屏障验收 | M14.5使用有限透视深度重建真实光程并合成 |
| [RenderView](render-data.md)包含双精度相机/投影和环境快照，视口发布校验完整身份、静态时间0 | 云动画另行定义时间语义 |
| [截图](render-capture.md)每个作业独立worker/device、输出PPM；冻结环境/相机/曝光/质量并记录shader摘要 | 云参考/历史模式后续定义 |
| [M13实验格式](../../sdk/python/decker/experiments.py)绑定XPBD/cloth | 复用SDK调用及追溯原则，另设有版本的渲染用例清单；不直接塞入布料计划 |

## 技术路线与依据

大气采用Hillaire 2020路线：透射、多次散射近似、视点天空和空气透视分层计算，支持参数更新与不同质量档。
作者提供了论文配套实现及参考比较入口；这里只选择算法方向，不引入其DX11平台或整套代码。
依据：[作者项目](https://github.com/sebh/UnrealEngineSkyAtmosphere)、
[作者论文索引](https://sebh.github.io/publications/)。
Sky View只用于天空，Aerial Perspective保存视锥中的散射和透射；分开处理可避免对天空重复加空气散射。
相关说明见[Epic官方Sky Atmosphere文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)。

保留不依赖低分辨率LUT插值的高采样参考路径，用于量化近似误差；CPU双精度积分验证单位、透射和单次散射。
Bruneton实现的量纲检查、CPU参考及自动测试作为验证方法参考，不把两种多次散射近似强行要求逐像素相等。
依据：[Bruneton作者文档](https://ebruneton.github.io/precomputed_atmospheric_scattering/)。

## 模块边界与接口

Scene与render/data的CPU契约见M14.1；M14.3独立模块的公开入口为[Atmosphere.hpp](../../engine/render/atmosphere/include/dk/render/Atmosphere.hpp)与[CPU参考](../../engine/render/atmosphere/include/dk/render/AtmosphereReference.hpp)。

| 位置 | 职责和依赖 |
| --- | --- |
| 现有Scene/Services/Operations | CPU环境值类型、校验、持久化、guard/事务/撤销；不依赖Render或Vulkan |
| 现有render/data | 从SceneSnapshot提取只读环境；相机、尺寸、ViewId和显式时间为逐帧输入 |
| engine/render/atmosphere，dk_render_atmosphere / dk::render_atmosphere | 参数到T/MS/Sky LUT和离屏天空；PUBLIC reference/Graph，PRIVATE Shaders/Profiling；reference仅依赖Scene/Math，不依赖Framework/Editor；共用有限区间传输函数支持M14.5 |
| 现有render/pipeline | depth/unlit或Lambert/tone，M14.4导入大气快照T/Sky；M14.5显式合成天空/空气透视，Render模块不调用裸Vulkan |
| 现有render services / editor | 装配、截图任务及界面；通过统一命令改环境，不能持有可变GPU缓存作为场景状态 |

DK_BUILD_RENDER_ATMOSPHERE隔离GPU大气模块，默认OFF、windows-graphics开启；启用时明确检查target依赖。
DK_BUILD_RENDER_ATMOSPHERE_REFERENCE默认OFF，可在CPU配置单独开启；GPU选项也构建该参考库。
CPU-only仍能读取、保存和修改环境描述，渲染能力未编译/设备不支持时明确报告不可用。
不新增三方依赖；若实际移植外部代码/素材，另行记录版本、许可和来源，不能将参考链接当作已引入依赖。

## 数据、坐标与持久状态

- Scene保存一个场景级EnvironmentDescription，缺省disabled，旧工程视觉保持原unlit背景。
  包含AtmosphereParameters、SunParameters；后续再加入CloudParameters；编辑完整候选后一次提交。
  云字段到M15.1才定义版本和校验，不预先保存无解释占位数据。
- 所有公开距离/高度用米、时间用显式纳秒或秒并标单位；散射/消光系数用m^-1，光学厚度无量纲。
  Rayleigh/Mie密度与吸收层、地面半径/大气顶、地表反照率和相函数参数有有限值与范围校验。
  RGB太阳能量与已有线性HDR材质采用显式共同标度，曝光只在tone执行一次；具体转换及测试向量见M14.1契约。
- 使用现有Y-up局部场景坐标，CPU双精度保存行星中心/地表锚点与相机。
  GPU接收相机相对坐标及已分离的高度/半径；内部改用千米或归一化时同时转换系数。
  禁止直接用两个地球尺度float位置相减求米级高度，亦不把曲率模型等同于现有平面场景地形。
- sun_direction统一为“采样点指向太阳”的归一化方向；积分中入射/出射方向与相函数cos符号须明确。
  太阳角半径有限且非零；行星遮挡参与光程，地平线/切线求交不使用任意距离钳制掩盖NaN。
- 显式升级场景格式承载环境，旧版本读入补disabled，未知版本/字段拒绝而不静默丢失。
  持久化、SceneSnapshot::same_content/logical_bytes、restore、事务、undo/redo和源身份必须一并覆盖。
  参数无变化不推进revision；无效参数/guard冲突/分配失败保持Scene、dirty和历史不变。
- environment.query / environment.set复用Commands→Operations→Services，详见[环境命令参考](../commands/environment.md)。
  ViewId、quality、exposure、time、frame序号属于渲染请求，动画时钟不逐帧修改场景revision；
  自动化固定时间和seed，不能读取不受控wall clock作为内容输入。

## M14.1 CPU环境契约

`EnvironmentDescription`是Scene拥有的值，包含enabled、AtmosphereParameters和SunParameters；
默认disabled，所有标量及RGB/方向/位置均为double。Scene与Services不引入GPU依赖。
`SceneDocument::set_environment`先校验并规范化完整候选，精确相等不修改revision/dirty；
有效变化一次提交。环境进入snapshot、same_content、logical_bytes、stage、apply_snapshot及历史，
保存来源令牌保持原约定。RenderScene提取环境副本；默认unlit管线不消费它；显式render_sunlight要求它与大气快照完全匹配。

坐标为右手Y-up局部米制；默认半径6360000m、中心(0,-6360000,0)，原点处为地面。
高度以double的`hypot(position-center)-ground_radius_m`计算，返回带符号高度，不钳制地下/大气顶外。
半径范围[1,1e9]m、大气厚度[1,1e7]m，中心和高度查询位置各分量绝对值≤1e9m。
地球默认中心附近1mm、1m、100km和切向位移采用绝对误差≤1e-6m验证。
GPU相对位置及高度拆分留到后续阶段，不进行地球尺度float相减。

Rayleigh和Mie密度为exp(-h/scale_height_m)，各自scale_height_m在[1,atmosphere_height_m]。
Rayleigh散射即消光；Mie散射与消光RGB分别保存，逐通道0≤scattering≤extinction≤1 m^-1。
吸收使用`max(0,1-abs(h-center_height_m)/half_width_m)`三角层，中心在[0,top]、半宽在[1,top]；
吸收与Rayleigh系数在[0,1] m^-1。地表反照率RGB在[0,1]，Mie g在[-0.99,0.99]。
这些是首版数值支持边界，不宣称所有组合都符合真实地球。disabled也校验，避免保存无效潜伏状态。

太阳方向定义为采样点指向太阳；任意有限非零方向先按最大分量缩放再归一化，
单位长度误差≤8 double epsilon时保留系数，确保重复查询/写入/保存不漂移。
角半径在[1e-6,0.1]rad；RGB irradiance在[0,1e6]，是线性sRGB/D65通道中的场景归一化法向辐照度，
不是lux或未经光谱转换的W/m²。白色默认E=(1,1,1)，Lambert正入射输出为albedo*E/pi，
均匀太阳盘radiance=E/(pi*sin(angular_radius_rad)^2)，与材质emissive共享曝光前线性辐亮度标度。
曝光不持久化到环境。M14.1以解析测试冻结换算，M14.3实现积分和太阳盘；M14.4以同一E/pi标度实现Lambert绘制。

DeckerScene写version=2并要求environment={version:1,enabled,atmosphere,sun}；所有子字段必填，
未知字段/版本、重复键、非法参数拒绝。读v1严格维持旧字段集合并补默认disabled；Project仍v1。
迁移保留scene_id/revision，parse为dirty、load为clean，之后保存写v2，失败加载保留旧文档。

`environment.query({})`返回{state,environment}；`environment.set({guard,environment})`完整替换，
返回{state,created_id:null}。环境对象与场景v2中的环境相同。query为query，set为可撤销memory_edit，
复用SceneEdit/SceneService；scene.transaction允许与实体编辑混用，仅真实提交递增一次revision。
参数错误、guard冲突、批内后续失败、revision耗尽或历史预算拒绝保留内容/dirty/undo/redo。
所有暂存分配/历史准备在真实提交之前；不承诺flecs致命错误/进程中断恢复。

验证覆盖方向幂等/能量换算/精度与非法值、v1→v2/未知版本和字段、来源/旧快照保存、
事务回滚/撤销/redo/no-op/预算/上限、进程保存重载/命令发现、只读Render提取及CPU-only链接。

## LUT、管线与最小光照

M14.3与M14.5固定RGBA32F以建立数值基线；尺寸/精度变更须重新验证误差和能力：

| 资源 | 当前实现或后续候选 | 失效输入 |
| --- | --- | --- |
| Transmittance | 256×64，RGB透射，RGBA32F | 大气物理参数、行星半径/高度、算法/精度版本 |
| Multiple Scattering | 32×32，RGBA32F | 前项、Mie散射、地面反照率；单位太阳能量响应 |
| Sky View | 192×108，RGBA32F | MS内容、Mie g、观测高度、太阳局部天顶余弦、MS开关 |
| Aerial Perspective | 64×64×128，两张RGBA32F保存RGB L与RGB T，地表边界直接积分 | 每帧由前项、相机/投影、积分距离生成；无跨帧历史 |

不能把RGB透射无说明压成alpha单值。设备不支持所需storage/sampling/filter格式时明确拒绝，
或选择提前声明且独立验证的RGBA32F/手动插值路径；不偷偷降级。3D资源的mip、copy footprint、
descriptor反射、usage/范围和Graph导入导出先在M14.2验证；初版不扩成通用RHI或异步compute。

组合路径：更新需要的LUT → 深度预通道 → 不透明/alpha-mask几何 → 天空与空气透视 → 原tone → 呈现/按需读回。
depth=1背景按天空射线处理，几何按重建的实际距离应用`L_out = L_air + T_air * L_surface`。
云接入后由[统一区间合成](render-clouds.md)负责介质顺序，不能在同一路径重复应用整段空气透视。

为了让大气日光和后续云影真正作用到物体，M14.4提供可选最小Lambert太阳直射及天空漫反射项，
验证法线逆转置、剪切/负缩放、双面约定和无效法线。保持unlit材质/旧预览可选且回归图像不变。
这不是完整PBR：不含镜面IBL、多灯、几何阴影映射、GI或透明物体体积接收。

## 缓存、提交点与失败

renderer实例拥有最近一组大气缓存；不可变快照可在同队列内共享物理/视图LUT。M14.3使用显式位置和太阳角度区分视图，后续场景集成再引入ViewId管理。
key必须覆盖实际被shader读取的参数、格式/尺寸和shader版本；曝光不改变物理LUT，
太阳移动不应无条件重建与其无关的透射表。不能凭scene revision一次性清空所有共享表。

参数解析、预算预约、资源/图候选和返回元数据先准备；Graph execute成功只表示提交，
对应Submission完成后才发布可用代次。异步新代失败或过期不覆盖旧代；已有Frame继续拥有自身资源。
新请求若不能满足其版本，返回明确未就绪/失败，允许界面继续显示带旧版本标识的上一帧，不伪称已更新。
不原地改写仍被在途帧读取的LUT；占用达到约定上限时采用有界排队/拒绝，不能无限积累旧代。
resize、Scene切换、设备重建和关闭有明确失效/回收路径；device_lost遵循现有队列失效契约。

截图冻结环境/Scene revision、相机、时间、质量和shader摘要，默认无跨请求历史；
GPU失败、取消、IO发布失败沿用现有capture保留旧文件的约定。CPU-only保存场景不创建GPU资源。

## 验证和实施入口

按Roadmap M14.1–M14.7推进；CPU参数/迁移测试加入已有Scene/Services/Render Data target，GPU测试在对应实现阶段注册。
验证分为：CPU量纲/积分/持久化，GPU格式与同步，大气参考图，GUI/命令/保存重载一致性，最后性能采样。
真实用例包含海平面/高空、正午/低太阳/日落后、切线/背太阳/朝地面、不同near/far和曝光；
透射率应在[0,1]，零消光返回T=1/L=0，单段拆分合成与整段参考一致，所有输出有限。
后续自动化启动dk-editor使用`--background`，普通交互启动保持常规窗口行为。

M14.3固定数值门槛：透射绝对误差≤2e-3；固定测试集的线性RGB归一化RMSE≤3%、p99归一化绝对误差≤8%。
归一化尺度使用参考图RGB亮度p95与固定1e-3下限的较大者，不用测试图自身数值；
太阳盘/地平线单列最大误差与边界连续性，不从总图平均中隐藏。多次散射近似偏差与离散/插值误差分别记录。
这些门槛在本次实现前冻结；不得以“看起来正确”替代量化或事后悄悄放宽。

性能候选目标为1080p质量档下大气增量GPU p95≤1.5ms，含每帧Sky/AP更新及合成。
质量门槛不随性能结果调整；候选目标未达到时明确记录差距，不将阶段基线交付等同于性能达标。

### 质量与性能采集协议

固定参考集复用AtmosphereProbe六个高度/太阳用例（单次与MS）、独立CPU闭合比较、
地平线两侧显式射线、太阳盘与真空；AerialPerspectiveProbe五个高度/距离用例及分段合成。
地平线另检查相邻采样的变化相对高采样参考的偏差≤8%，使用该射线集参考RGB最大值与1e-3的较大者归一化；
允许真实行星遮挡边界，不要求跨遮挡颜色相等。
CPU参考提供独立DK_BUILD_RENDER_ATMOSPHERE_REFERENCE选项，不要求Graphics或Slang；GPU选项隐含构建参考库。

基准使用ScenePipeline、同一双面远近矩形场景及晴空/日落/高空三个固定预设，1920×1080、时间0、曝光8。
每组先冷帧，再5帧预热和40帧稳态，分别测缓存命中、逐帧高度变化导致Sky更新、物理参数变化导致全部LUT重建；
另测相同相机/几何的unlit帧。三轮独立进程，轮换预设和timestamp OFF/ON顺序，保存原始样本。
分位数使用nearest-rank；逐帧增量为更新帧的Sky GPU加帧GPU减同索引unlit GPU（串行配对，非同时执行），
另列AP/合成Pass，父Submission与子Pass不相加。报告每轮p50/p95，三轮取中位数并保留范围。

冷设备、管线/Slang创建、首次lit/AP延迟编译、同步LUT准备、CPU录制提交、wait、CPU读回及API端到端分别计时。
端到端包含LUT准备、建图/提交、等待和RGBA8读回；不冒充GUI呈现或IPC排队/文件写入延迟。
冷指进程/renderer无缓存，不清理驱动或OS缓存。优化构建、validation关闭用于性能；required GPU同步validation单独验收。
M11独立timestamp无需Tracy；未连接Tracy时不声称采集了Tracy轨迹。缺失/丢弃GPU时间、非有限值或失败运行拒绝汇总。

AtmosphereSky::lut_profile返回该LUT代构建完成时的只读GpuProfile（仅CPU数据，不持有设备）。
缓存命中保留同一profile；用generations判断此次是否重建，不能将旧代时间当本帧耗时。
profile与已通过数值检查的候选表同时发布；失败保留旧表、代次和profile，关闭timestamp时状态为disabled。
VMA分别记录device-local及host-visible的allocation/block字节安全点采样峰值、缓存驻留和清理后值；
正式计时完成后另发一个不计时诊断帧，在wait/collect前采样以包含Graph的AP/HDR临时资源，避免只测到完成帧驻留。
统一内存类型可重叠，不相加，不将VMA块等同于系统总显存/精确瞬时峰值。另列CPU heap backing峰值。
记录设备/驱动、编译器/config、开关、shader摘要、可执行文件和源文件hash，以便复跑。

本机固定基线见[质量/性能报告](../benchmarks/2026-10-13-atmosphere.md)，复现见[指南](../guides/atmosphere-benchmark.md)。
质量门槛通过；RTX4070 Laptop/596.49的1080p动态Sky增量GPU p95三轮中位数为2.709/2.792/1.593ms，
1.5ms候选目标仍未达到。质量/性能采集能力与优化达标分开维护，具体历史证据见[0100](../development/0100-atmosphere-baseline.md)。

## 相关文档

[架构](architecture.md)、[管线](render-pipeline.md)、[Graph](graphics-graph.md)、
[资源](graphics-resources.md)、[Scene](scene.md)、[GPU观测](foundation-profiling.md)、
[体积云](render-clouds.md)、[0093规划记录](../development/0093-atmosphere-cloud-plan.md)。

## M14.3 积分、LUT与离屏天空实施契约

新增engine/render/atmosphere，DK_BUILD_RENDER_ATMOSPHERE默认OFF，windows-graphics开启。
CPU参考独立静态target dk::render_atmosphere_reference，仅依赖Scene/Math；GPU target dk::render_atmosphere
PUBLIC引用参考和Graphics Graph（经Graph公开Memory），PRIVATE引用Shaders/Profiling，不接Framework、Editor或ScenePipeline。
不复制上游源码；依据上述论文与公开数学公式独立实现，不新增三方依赖。

### 数值与采样

CPU参考使用double米制，世界位置复用Scene的±1e9m边界，直接高度入口上限4e9m；独立高采样直接积分太阳光程（不查LUT）；返回RGB L/T、光程和行星命中标记。
球面求交使用factored平方差；分段中点密度与解析常系数Beer积分，零消光使用ds极限。
Rayleigh相函数3(1+c²)/(16π)，Mie采用归一化Henyey–Greenstein，c=dot(view_direction,sun_direction)，朝太阳为正前向峰。
地面遮挡截断光程，太阳光程穿地则为0；顶外先求大气入口，未相交为真空。太阳盘单独解析评估E/(πsin²α)，不烘入Sky View；GPU使用局部太阳向量与弦长判定边界，避免小角半径的cos舍入为1；盘辐亮度因子和弦长阈值在CPU以double预计算，避免GPU小角sin的相对误差。
CPU还提供各向同性多次散射闭合参考：球面均匀方向积分平均L和散射反馈F，Psi=L/(1-F)。
地表项仅包含直射Lambert一次反弹，不宣称完整地表/大气全阶传输或路径追踪真值。

GPU内部km和km^-1；上传独立高度/半径，沿射线高度用有理化半径增量计算，避免地球尺度float位置相减。
首版明确支持地面半径[100km,100000km]、大气厚度[1km,0.1R]、密度尺度≥100m、相机高度[0,2H]；
M14.1更宽的持久参数范围保持不变，不支持的渲染范围明确拒绝。高光学厚度或闭合不收敛时拒绝候选，不隐藏NaN/改用旧参数。

固定算法版本1/质量档：RGBA32F，Transmittance 256×64（512段），MS 32×32（1024方向×64段），
Sky View 192×108（128段）。T使用球面距离/径向光程参数映射；MS使用太阳天顶余弦和平方高度映射；
Sky使用相对太阳方位角、地平线上下分区的平方仰角映射，分区之间不插值。所有LUT保存单位太阳RGB辐照度响应。
Sky View内不含太阳盘，不含地表可见表面反射；向地面的射线只积分到地面，场景表面由M14.4/M14.5合成。
MS在天空积分中作为各向同性源sigma_scattering*Psi。参考模式使用512视线段和512太阳光程段直接积分，
不依赖T/Sky低分辨率插值；MS近似误差通过独立CPU闭合参考单列，不把它当作全阶物理真值。

### 缓存和提交

AtmosphereRenderer::prepare为明确同步的冷/变更更新入口，内部以Graph提交并等待候选完成，再检查有限值/范围及MS反馈<1，最后无分配发布。
只缓存最近一组物理/视图LUT，内部最多一组候选；不做后台队列或多ViewId管理。外部不可变AtmosphereSky可保留旧代；显存受调用者Memory预算、队列槽和持有快照数约束。
T key仅包含消光密度/半径/厚度及固定算法格式；MS key额外包含Mie散射和ground_albedo；Sky key额外包含Mie g、相机高度、太阳对局部up的余弦和MS开关。
太阳能量/角半径、相机朝向、输出尺寸和曝光均不使LUT失效；位置改变只在高度/up-relative太阳角变化时重建Sky。
新代资源独立分配，不覆盖旧代纹理；创建、记录、submit、wait、数值校验或Memory失败均保留旧缓存/计数。
失败提交后的资源由现有队列生命周期管理；禁止把未完成候选标为ready。跨队列和关闭域调用明确拒绝。

render_sky提交独立Graph离屏帧，返回拥有型线性RGBA32F和readback，完成票据与读取状态显式；
准备快照的环境/位置固定，透视相机forward/up/FOV、尺寸、MS/reference模式在公开接口中有边界检查；普通输出每轴上限4096，昂贵的reference模式最多4096像素。
太阳盘在该帧上按ray/planet遮挡及T添加；只有样例输出PPM时执行曝光/Reinhard/sRGB，CPU参考及HDR读回不tone。
API的readonly LUT诊断数据支持本阶段定量比较；不把快照缓存写入场景，不改变默认unlit管线。

### 独立验收

CPU：真空T=1/L=0、纯吸收解析、有限能量、单段拆分合成、相函数积分、米级高度/地平线/顶外/地下拒绝。
GPU：与高采样参考比较正午、低太阳、日落后、1m/10km/100km/顶外、切线/反太阳，T绝对误差≤2e-3；
单散射及完整天空归一化RMSE≤3%、p99≤8%（尺度=max参考RGB亮度p95,1e-3)；太阳盘和地平线边界另列，不纳入平均掩盖。
MS用选定点同一闭合的高采样CPU参考比较，记录积分误差与近似模型限制。若候选分辨率未达门槛，先提高采样/分辨率并记录成本，不放宽门槛。
缓存测试分别改变物理参数/散射/反照率/g/太阳/相机/尺寸，检查代次和旧输出；注入创建/submit/预算失败验证保留旧代。
required GPU validation覆盖Graph与在途销毁。离屏样例输出正午/日落/高空PPM和线性数值测试证据；性能采集独立于质量验收。

## M14.4 几何光照边界

按[场景管线实施契约](render-pipeline.md#m144-最小日光接收实施契约)提供不可变快照的锚点与借用 LUT 图像。
ScenePipeline 显式选择日光接收，共用快照太阳及 T/Sky 表，不修改大气缓存、不增加太阳盘漫射或空气透视。
天空漫射在锚点近似，逐片段直射透射使用实际位置；显式入口保持默认unlit，M14.6上层根据环境开关自动选择共用入口。

## M14.5 场景空气透视边界

按[管线契约](render-pipeline.md#m145-空气透视与线性合成实施契约)生成视锥 L/T 体积并合成。
AtmosphereSky 只读公开 MS 开关，传输函数由天空与空气透视共用；快照仍不可变，缓存 key/提交点不变。
每帧 AP 由 ScenePipeline 的 Graph 拥有，深度仅来自几何/alpha-mask；曝光前提供受限线性诊断读回。


## M14.6 集成状态

Edit视口通过独立有界worker冻结环境/相机并绑定ScenePipeline；Environment面板复用CPU命令和历史。
Capture按冻结环境选择旧unlit或完整大气合成，结果含参数、静态时间、固定质量与shader来源。
Python/Luau、真实GUI/IPC、保存重载和错误/取消/在途关闭已验收，详见[0099](../development/0099-environment-integration.md)、
[编辑器设计](editor.md)、[截图设计](render-capture.md)与[指南](../guides/atmosphere.md)。性能采集入口见[基准指南](../guides/atmosphere-benchmark.md)。
