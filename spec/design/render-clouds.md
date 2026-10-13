---
module: render-clouds
created_at: "2026-10-12T14:38:00+08:00"
updated_at: "2026-10-13T13:12:00+08:00"
status: accepted
---

# 体积云设计

## 目标与边界

本设计包含已采用的云场契约及后续渲染计划；阶段顺序/状态见[Roadmap](../roadmap.md)。依赖
[大气M14](render-atmosphere.md)的太阳与透射接口、3D资源、深度采样、HDR合成和环境编辑链路。
首版提供单层球壳状积云/层积云、固定seed天气分布、风平移、云内/云下/云上观察、
太阳散射/自遮蔽、空气透视、时域稳定和地表云影；编辑器及无窗口截图使用相同内容输入。

不包含流体天气预报、降水/闪电、积雨云风暴、任意局部多体积、分层卷云、地形流送、
完整多次散射路径追踪或异步compute。无GPU时保留参数/持久化和CPU参考验证，不提供软件实时云渲染替代。
质量档和阈值是本引擎拟议目标，不能直接引用其他游戏的帧时作为本机结果。

## 技术方向与当前缺口

借鉴Nubis的可控程序化云场和分级渲染思路，以2D天气图约束3D基础形状/细节密度，
先做全分辨率无历史的有界射线积分，再加入半分辨率和时域重建。
依据：[SIGGRAPH 2017课程原始入口](https://advances.realtimerendering.com/s2017/index.html)、
[作者技术资料](https://www.schneidervfx.com/)。这些资料用于研究算法，不将其贴图、代码或演示性能直接纳入工程。

当前Graph可导入外部资源并管理单队列同步，但没有“跨帧Graph句柄”；ScenePipeline每帧独立分配输出，
不能假设已有TAA、运动矢量或历史池。M15的历史由Renderer按视图拥有，每帧重新导入真实资源。
现有M13只识别布料数值；环境截图/时间序列用有版本的独立渲染用例清单，复用Client、capture、
文件摘要和报告原则，不声称已有云参数扫描/恢复器。

## 模块与数据所有权

`engine/render/clouds`提供两个默认OFF的构建选项：`DK_BUILD_RENDER_CLOUD_FIELDS`构建
纯CPU `dk_render_cloud_fields` / `dk::render_cloud_fields`（PUBLIC Scene），
`DK_BUILD_RENDER_CLOUDS`同时构建GPU `dk_render_clouds` / `dk::render_clouds`（PUBLIC fields、graphics_device）。
当前只提供场数据和纹理资源；后续积分/合成才按真实用途引入大气、Graph、Shaders、Profiling。
Scene/Services持有纯CPU CloudParameters；CloudFieldCache拥有不可变CPU天气/噪声代次，
CloudTextureCache拥有GPU纹理代次。模块不反向依赖Editor/Framework，不新增三方库。

### 参数、持久化与显式时间

EnvironmentDescription增加固定大小clouds；DeckerScene仍写v2，嵌套environment升级v2，
强制包含clouds。旧Scene v1或environment v1加载disabled云；environment v1迁移使用
top=min(4000,H/2)、bottom=min(1500,top/2)，兼容较薄大气。新写出统一environment v2。
命令query/set使用严格v2完整对象，旧v1 set拒绝，避免旧调用者无意覆盖云；迁移仅发生于磁盘读取。
clouds包括enabled、uint32 seed、generator_version=1、weather/base/detail_resolution，
bottom/top_height_m、coverage、density、extinction_per_m、weather/shape/detail_scale_m、
bottom/top_fade_fraction及水平wind_velocity_mps[2]（世界X/Z，每秒米）。
所有值即使disabled也校验：0≤bottom<top<大气顶，coverage[0,1]、density[0,10]、消光[0,1]，
尺度[1,1e7]米、两个fade各[.001,.5]、风各[-1000,1000]。
分辨率必须2的幂：weather[8,256]、base[8,128]、detail[4,64]，默认128/128/32。
生成器版本不支持时拒绝；相函数/光照参数由积分阶段定义，不添加未消费的占位字段。

sample_cloud_density输入完整环境、不可变场、世界位置及显式time_seconds（[-1e9,1e9]）。
密度为无量纲[0,density]，相对行星的球面高度约束垂直剖面；天气平面使用世界X/Z，
3D场使用行星相对坐标，随水平风按p-wind*time平移，无帧累积和隐式时钟。
底/顶边界及壳外为0，coverage=0、density=0、disabled精确为0；消光系数供后续积分使用。
同一时刻重复/乱序调用产生相同结果，负时间有效。采样先检查有限输入及场key匹配。

### 确定性纹理与发布

生成器v1为周期整数格点value-noise的多频加权组合，固定uint32 hash和Q16平滑插值；
天气RG8保存coverage/type，base/detail为R8，线性过滤、repeat、单mip。
所有输出字节由整数计算确定，不依赖标准随机库、平台浮点舍入或线程调度。
字节FNV-1a摘要仅作内容诊断（非密码学）；身份key包含生成器版本、seed及三个分辨率。
密度/高度/coverage/风/时间变化不重建纹理。CPU bilinear/trilinear按GPU texel-center约定采样，
天气coverage阈值、type调节剖面顶端、base形状与detail侵蚀后夹取，具体公式由实现和锁定测试冻结。

两个cache均单线程所有者、不可复制，返回shared_ptr<const generation>保活旧代；
GPU cache额外持有退休代次，只在poll确认队列无在途提交、无其他录制批次且没有外部快照时回收，
防止提交仅保活Image而外部释放generation时漏计预算。借用image/view不延长父代次寿命。
同key返回当前代，不生成/分配纹理或提交GPU工作（仍校验队列身份）。不同key先校验预算、生成/分配，GPU上传提交并等待完成后才交换current。
失败（参数、预算、资源创建、记录/提交/等待）保留旧current及generation编号；异常分配由RAII保护，不承诺进程终止恢复。
默认至多2个活代（含current、外部保活旧代、待建候选）；第三代在生成前有界拒绝，释放旧代可重试。
CPU预算默认8MiB，统计活代纹理字节+候选；GPU预算默认16MiB，统计活代纹理字节+候选纹理+候选upload载荷。
预算是逻辑载荷而非分配器/显存硬上限；驱动对齐和scratch另由队列资源统计核验。
cache不暴露底层可变容器；纹理view按2D/3D类型创建，最终处于shader-read布局，队列提交保活资源。
队列必须属于创建cache绑定的设备/所有者；跨队列使用不支持。无后台任务、无历史、无云光照/合成；
ScenePipeline::render_environment遇到enabled云返回not_supported，防止把仅参数支持误当作有云截图。

验证覆盖固定seed独立重建字节/摘要、不同seed、范围/周期/插值、球壳/coverage/density边界、
显式时间暂停/跳跃/反向、预算和旧代保活、失效key、CPU-only编辑/事务/撤销/迁移/独立进程重载。
GPU要求真实设备、validation与sync validation，逐字节readback及shader过滤采样对照，
提交失败注入/代次额度/释放和关闭检查，不用skip替代通过。

后续积分/视图接口规划（云场当前接口以上文为准）：

| 输入/输出 | 后续约定 |
| --- | --- |
| CloudParameters | enabled、seed、球壳底/顶高度、coverage、density/extinction、形状/细节尺度、垂直剖面、风方向/速度、相函数及光照近似参数 |
| Weather map | 首版由固定版本生成器+seed生成2D coverage/type字段，CPU数据/摘要可追溯；暂不扩资产导入器支持外部气象数据 |
| Base/detail noise | 固定版本的程序化3D纹理，候选128³/32³；只在seed/生成器/分辨率变化时重建，不每帧重生成 |
| Per-view request | 与M14一致的ViewId、相机/投影/尺寸、显式time和frame、质量档、history策略 |
| Cloud integration | 线性散射L、透射T、有效云区间/代表深度和有效性，不把代表深度当成几何表面 |
| Cloud shadow | 光空间2D透射图及覆盖范围/变换，只调制同一太阳直射项，不降低emissive或unlit颜色 |
| Diagnostics | seed/参数/相机/时间/Scene与环境版本、历史使用或丢弃原因、步数、资源代次、shader摘要与GPU测量范围 |

范围校验包括底高<顶高<大气顶、非负消光、coverage∈[0,1]、非零采样尺度、有限风速与时间。
相函数参数远离奇点；零密度/coverage=0是精确定义的无云状态，光学结果T=1/L=0。
content seed和帧采样噪声seed分开，风由显式时间平移，不随实际帧率或线程调度累积。
所有预算（噪声字节、积分步数/距离、光线步数、视图数、在途代次）在提交前验证；无效输入不改环境或历史。

## 积分、光照与大气合成

1. 根据行星球壳、大气边界、相机和场景线性深度求出射线有效区间，正确处理从云内出发、
   切线、朝向地面及被几何截断；对向上无几何的射线采用物理边界/显式最大追踪距离。
2. 参考路径全分辨率、固定高采样、无历史，逐步积分Beer–Lambert透射及散射源项，
   步长改变时用一致的光学积分权重。初版有主光线和朝太阳的阴影光线最大步数，所有循环有界。
3. 云光照包括经大气衰减的同一太阳、云自遮蔽、参数明确的前向相函数、天空漫射近似。
   多次散射采用能量受控的低阶近似并单列开关/误差，不将艺术增亮称为物理精确。
4. 参考合成沿射线将空气与云的消光/源项共同积分；快速路径把无云空气区间与混合云区间
   统一表达为(L,T)，按相机到远处顺序组合：`(L1 + T1 * L2, T1 * T2)`。
   背景/表面辐亮度只在终端进入；不能将云颜色简单盖到已应用完整空气透视的天空/场景上再加一遍雾。
   从LUT取空气分段时，近零透射下避免相除放大误差，必要时走有界参考积分。
5. 日落时光线在大气中的衰减、云对太阳盘的遮挡、远云的空气透视必须同时成立。
   半分辨率合成使用几何深度保边上采样；alpha-mask孔洞沿已有深度结果处理，前景边缘不能漏云。

M15.3先完成无历史的合成正确性，之后才做优化；保留单独关闭云/空气/多散射/历史的诊断输出，
以零密度云严格退化到M14结果。reference只代表本次选定模型的高采样版本，不是完整光谱/多散射真值。

## 时域重建和资源契约

首轮Balanced候选为1080p输出、960×540云积分，主光线至多96步、光线采样至多8步；
reference建议256步/32步并通过加倍采样核对收敛，最终数值与预算在各阶段开工时冻结。
空区跳跃和早停都必须与无优化结果比较，早停阈值有误差预算，不能只统计省下的步数。

每个ViewId独立拥有颜色/透射、代表深度/置信度及前帧相机、时间和环境版本。
采用ping-pong读写，不读写同一历史图像；用代表采样位置、前帧矩阵及已知风平移重投影，
不能把没有运动矢量的现有管线当作已有正确重投影。低置信/深度冲突/反遮挡拒绝历史，
邻域夹取限制残影；快速移动、云内、太阳低角度与前景边缘单独验收。

以下情况必须明确重置/拒绝历史：新ViewId、resize/FOV变化、相机切换/跳跃、场景更换、
内容参数/seed/太阳跳变、质量或shader版本变化、设备重建、时间倒退/跨度过大和前帧提交失败。
连续风动画按显式delta-time补偿；曝光在介质合成之后，历史保存未曝光的线性量，不混入不同曝光结果。
同样的frame编号不能在两个视图间共享缓存，截图与编辑器不能互相污染历史。

历史候选在Graph成功提交并完成后才可作为已完成源发布，失败不推进有效历史序号；
若没有可用历史，当前帧从无历史路径开始。旧Frame/pending submission继续保活资源。
视图槽与在途代次有额度，超额有界拒绝/排队；关闭、resize或设备丢失后等待相关GPU工作再回收。
快速连续调参只发布匹配当前内容版本的结果，旧代完成不得覆盖新代，界面保留旧帧时显示其真实版本。

截图默认使用固定时间、固定seed、reference无历史模式；序列验收若启用历史，显式声明预热帧数、
相机路径和采样序列，每次从空历史开始。不能拿随机预热过的GUI截图与单帧离线结果作确定性比较。

## 云影与交互闭环

M15.5生成有界光空间云透射图，M14.4最小日光接收路径消费；云影不要求几何阴影系统/PBR先完成。
验证太阳方向、风向、距离投影、相机移动下稳定性及coverage=0时单位透射。
首版不包含云对整个大气多次散射场的反馈，也不承诺复杂体积光束；这些与地表云影分开。

M15.6通过统一environment命令修改云；GUI预设、undo/redo、保存/重载、runner截图与Python/Luau保持一致。
界面区分物理参数和质量参数，显示实际生效版本/历史重置原因；预设显式写入seed与生成器版本。
独立渲染清单固定相机、太阳高度、coverage、seed、时间、质量和预算，输出图像、线性数值摘要、
环境/二进制/shader指纹及性能报告，失败项保留错误/缺失，不伪装为零耗时或有效图像。

## 验证、质量和性能候选目标

阶段见M15.1–M15.7。至少覆盖晴空、稀疏/厚云、逆光、日落、地面/云内/云上、前景剪影、
固定seed复现、静止及平移/旋转相机、风动画、暂停/时间跳跃、resize、取消/失败和关闭。
CPU参考验证均匀介质解析解、区间组合、密度范围、确定性与量纲；GPU验证storage写入→采样、
历史导入/读写隔离、对象寿命和同步validation，无可用设备的跳过不能替代验收。

初始候选门槛：零云状态与M14参考一致；均匀介质透射绝对误差≤2e-3；
静态Balanced相对收敛参考线性RGB归一化RMSE≤5%、p99≤12%，归一化尺度同大气设计。
固定镜头预热32帧后的120帧序列，归一化帧差RMS候选≤1%；相机切换后的首帧必须拒绝旧历史。
动态路径以相同时间的无历史参考逐帧比较，并单列边缘残影/反遮挡区域，不能用静态收敛替代动态质量。
具体参考收敛、动态阈值及曝光固定值在M15.2/4开始优化前冻结，变更须留证，不事后调阈值掩盖失败。

同一实测设备/构建、1080p Balanced的候选目标：云积分+重建+合成+云影增量GPU p95≤4ms，
大气与云合计p95≤5.5ms（测组合区间，不能直接相加各Pass p95），环境资源稳态≤128MiB、
含旧代在途的峰值≤192MiB。无实测前不承诺60fps或指定设备达标；基础场景成本另计。
沿M11记录预热/冷启动、CPU建图/提交、GPU timestamps、端到端、显存和采集开销；
至少三轮固定序列，报告原始样本、p50/p95及失败。正确性开validation，性能另记录关闭validation的条件，
不将readback/捕获等待当成云GPU耗时，不累计重叠的父子GPU区间。

## 相关文档

[大气](render-atmosphere.md)、[管线](render-pipeline.md)、[Graph](graphics-graph.md)、
[GPU观测](foundation-profiling.md)、[截图](render-capture.md)、[实验管理](experiment-management.md)、
[0093规划记录](../development/0093-atmosphere-cloud-plan.md)。
