---
module: render-restir
created_at: "2026-10-19T12:50:00+08:00"
updated_at: "2026-10-19T15:29:00+08:00"
status: accepted
---

# ReSTIR 光照重采样设计

本文件是[路径追踪设计](render-pathtracing.md)的采样扩展设计。
CPU 数学、GPU 初始 RIS、时间与空间复用的现行契约见 M19.1–M19.4，CPU 介质条件采样见 M19.5；GPU 介质与消费者按后续阶段推进；
状态、依赖和下一项统一见 [Roadmap M19](../roadmap.md#m19restir-直接光重采样)。
下文区分现行 CPU/GPU 采样契约和后续 GPU 介质/消费者计划，不把独立接口视为编辑器已有能力。

## 目标与范围

先交付显式可选的 **ReSTIR DI**：在相机可见的第一个表面上，用 reservoir 保存一个光源样本及其权重，
通过初始候选、时间复用和空间复用降低直接光方差。后续反弹继续执行现有路径追踪。
先覆盖 Surface only，再支持 Scene media 中符合条件的首表面直接光；原 MIS/BSDF-only 路径保留作独立对照。

首轮支持已有的 Lambert/GGX、baseColor/mask、单/双面、静态三角形、透视/正交相机。
候选光源使用已有发光三角形、常量环境；表面模式的 delta 太阳单独估计，CPU 介质参考将有限太阳圆盘纳入候选。
mask 发光三角形继续由 BSDF 命中估计，不扩大其 NEE 支持范围。
移动相机可复用匹配的静态表面历史；几何、材质、光源、介质内容变化先清空采样历史，
不承诺动态物体运动向量、跨光源重排映射或形变复用。

ReSTIR GI/PT、介质散射顶点/整条体积路径复用、ReGIR、去噪、HDRI、新材质、wavefront 与异步队列另行规划。
天空或云内部的主要噪声可能来自自由程和多次散射，首表面 DI 不能直接解决；M19 必须分别测量这类场景。
不以论文的速度倍数代替本引擎结果，不预先承诺实时或默认开启。

## 当前接入依据与模块边界

| 已有能力及入口 | 对本计划的约束 |
| --- | --- |
| [CPU 场景与灯光表](../../engine/render/pathtracing/src/PathTraceScene.cpp)、[CPU 表面积分](../../engine/render/pathtracing/src/PathTraceReference.cpp) | 复用冻结三角形/材质/纹理与面积乘亮度分布，增加可独立检验的 reservoir 数学；CPU-only 仍可运行 |
| [表面 shader](../../engine/render/pathtracing/shaders/pathtrace.slang) 的 directLight/tracePath | 当前逐路径迭代 NEE/MIS；需要显式拆出首表面直接光，不能在原结果上叠加 DI |
| [介质 CPU](../../engine/render/pathtracing/src/VolumePathReference.cpp)、[介质 shader](../../engine/render/pathtracing/shaders/path-air.slang) | hero RGB 通道、analog 自由程、ratio tracking 阴影与有限太阳保持物理模型；复用须先处理条件概率与随机维度 |
| [Renderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)、[会话实现](../../engine/render/pathtracing/src/PathTraceRenderer.cpp) | 当前两视图、一个在途批次、tile 串行；最后 tile 才发布 completed_spp。已有首样本 hit/normal 诊断不足以充当完整复用记录 |
| [Editor](../../engine/editor/src/PathPreview.cpp)、[面板](../../engine/editor/src/PathPanel.cpp)、[捕获设计](render-capture.md) | 复用 worker/取消/来源与显式模式，不引入 Scene 持久状态；GUI 和独立捕获需各自拥有历史 |

实现仍归 `engine/render/pathtracing`：纯数学/参考进入 `dk::render_pathtracing_reference`，
GPU passes 与会话进入 `dk::render_pathtracing`，公开接口置于 `include/dk/render/`。
Graphics 仅提供资源、RT、Graph 与同步，不承载 reservoir/BSDF/光源策略；Render 不反向依赖 Editor/Framework。
沿用现有 BLAS/TLAS、单队列和 Slang，不创建另一套设备所有者。

用户明确要求后续接入 RTXDI、DLSS。CPU 参考保持独立且无 SDK 依赖，作为未来后端的 oracle；
M19.2 使用 RTXDI-Library 的初始 reservoir shader 数学，并验证 Slang/SPIR-V、应用桥接与资源适配，
不把自有 reservoir 的内存布局或随机序列强加给 SDK。接入时按三方库规则核验并固定版本/commit，
M19.1 不下载或链接 SDK；M19.2 的固定来源和实际接入见下述契约及[三方库说明](../third-party-libraries.md)。

对用户分别提供光照采样（普通 MIS / ReSTIR DI）、介质（Surface only / Scene media）和重建（原生 / DLSS）选择；
RTXDI 是采样算法后端，不是与 ReSTIR 并列的物理模型。SDK 实现类型保持 PRIVATE，不泄漏到 Scene、CPU 参考或捕获持久格式。
后续 DLSS 适配层独立消费线性颜色、渲染/输出尺寸、深度、运动矢量、抖动、曝光与 reset/视图身份；
按所选 SR/RR/FG 功能分别核验所需输入，不将首表面 reservoir 当作运动矢量或重建历史。
当前 RTXDI 仅使用 shader 采样数学，DLSS 仍只确立边界；不添加尚不可用的 UI 开关，采样质量验收始终保留重建前线性输出。

## M19.1 CPU 实施契约

新增独立 `RestirReference.hpp`：`RisReservoir` 流式初始采样、`resample_ris_sources` 支持校正合并数学、
`RestirDiSettings` 与 `estimate_restir_reference` 固定射线参考；旧 `PathTraceSettings`/GPU/命令保持兼容。
首版 `version=1`、候选数默认 8，合法 1..64；CPU 入口仅支持初始 RIS，无时间/空间会话或介质。
`PathTraceSettings.estimator` 必须为 MIS，Scene media 仍须显式 surface_only；非法组合不静默降级。
独立候选评估预算默认 2^24，并预检总样本数乘候选数；几何射线保守界在原路径上增加一次首表面阴影。
错误/取消返回失败，流式 reservoir 先计算校验再提交，输入场景与旧结果保持不变。

候选表的三角形密度为 `class_probability * light_weight / total_weight / area`，
其中光类只包含非零环境/面积灯；delta 太阳从候选分布移出，以独立权重 1 的估计相加。
三角形贡献包含接收余弦、光源余弦及距离平方倒数，环境贡献按立体角；最终可见性单独查询。
内部光样本携带冻结光表身份、种类、三角形/重心或单位方向与原始 proposal 密度，不能把 GPU packed 数据作为 CPU ABI。
随机维度用独立域标签加 seed/pixel/epoch/candidate/purpose 的计数 hash，原 BSDF/RR/相机序列不变。

| 光路分量 | 首表面采用 RIS 时的估计责任 |
| --- | --- |
| 相机直接看见发光面/环境 | 保留完整发光 |
| 首表面到非 mask 面积灯/环境 | RIS 完整直接项；关闭旧 NEE 及这一次 BSDF 延伸对应发光 |
| 首表面 delta 太阳 | 独立 NEE、权重 1；不进入连续 reservoir |
| 首表面 BSDF 延伸命中 mask 发光面 | 完整发光，灯 proposal 为 0，不被 RIS 抑制 |
| 第二表面的反射与后续路径 | 原 NEE/MIS/RR；只抑制前一段已估计的发光，不抑制反射 |

### 支持校正的参考推导

每个来源 i 提供正规化样本 `(Y_i, W_i)`，满足 `E[W_i f(Y_i)] = integral(S_i, f)`。
`S_i` 是其目标的正支持域，不能用“本次选中样本是否可见”代替；未选中任何样本的零 reservoir 也保留来源和 M。
对固定接收点和预先确定的来源集合，取有效计数 `m_i > 0`，流权重为 `p_hat_x(Y_i) * W_i * m_i`。
选中 Y 后，计算 `Z(Y) = sum_i(m_i * indicator(Y in S_i))`，输出权重 `sum_weights / (p_hat_x(Y) * Z(Y))`。
条件于来源候选，抽签期望是各来源项之和；再对来源取期望得到
`sum_i integral(S_i, F_x(y) * m_i / Z(y)) = integral(union(S_i), F_x)`。
因此须有新的完整支持候选覆盖接收点非零贡献，来源筛选不得依赖已抽到的光样本；相关来源也不能绕过正规化前提。
数学接口明确接受来源样本在所有来源上的支持矩阵，重算完整 Z；朴素总 M 在支持不相交时会系统性变暗。
本阶段用离散可枚举案例验证期望、空来源和相关来源；实际屏幕支持查询及可见性校正留给 M19.3/4。

计数压缩时，已经正规化的 W 不变，有效 m 一致用于流权重与 Z；若储存未正规化 sum，则须同步缩放。
有限历史按全部参与来源的最大 ancestry age 加一计龄，整体淘汰并注入新候选，不能按所选光样本的年龄、亮度或遮挡丢弃；
避免无限祖先传播的 age 规则在时域阶段实施。上述证明条件于预先确定的有效 m/来源支持域，不能用于任意数据相关的历史筛选。
不把有穷样本均值测试当作任意递归复用的收敛证明。CPU 初始 RIS 没有历史，参考 spp 仅为新路径数。

### 冻结的正确性与性能协议

数值测试使用 8 个 seed（101 + 7919*i），每通道同时要求绝对偏差不超过 `2%*abs(reference)+1e-4`
且不超过 `5*combined_standard_error+1e-4`；参考 quadrature 的 N/2N 差需小于 `0.5%*abs(reference)+1e-5`。
相互独立的 seed/历史链才可计算标准误差。初始 RIS 用 1024/4096 样本比较跨 seed MSE，面积/环境/GGX 高样本用 16384 起。
极窄高光单列方差限制；不为了通过而改变物理 BSDF、裁掉高权重或放宽阈值。
性能基准固定单灯/100灯/1000灯程序输入、相同总发光功率和接收材质，记录输入指纹、seed、反弹、候选数和射线/候选工作量。
先测 CPU 普通 MIS 与 RIS 的线性均值/误差/耗时基线；其结果不用于声称 GPU 加速。
后续 GPU 等误差门槛为 2% 线性归一化 RMSE，参考认证为 0.5%，三轮关闭 validation 计时；
多灯目标耗时不超过普通 MIS 的 50%，固定相机/尺寸/输入/硬件/驱动对照，不因 RTXDI/DLSS 接入改用重建后误差验收。
GPU 程序灯基线使用同一输入、640×360、eye=(0,0,1)、朝 -Z、60 度垂直 FOV、near=.1/far=10、
max_bounces=8/rr_start=3、seed 集合不变，64/256/1024/4096/16384 spp 阶梯；目标设备为已有 RTX 4070 Laptop，
驱动版本与运行时 SDK 版本必须在测量时记录。参考先用 CPU 选定接收点认证，再由高样本 GPU 图像验证全屏收敛。
独立 CPU 单接收点基线的原始结果与命令见[测量记录](../benchmarks/2026-10-19-restir-reference.md)，不将其外推成全屏速度。

## M19.2 GPU 初始 RIS 实施契约

GPU target 私有使用 RTXDI v3.1.0 对应 RTXDI-Library commit `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`。
2026-10-19 查询官方 vcpkg registry `664933b8d385dfc01f4e0cdff4c7c74e71f6a740` 的完整树，没有 RTXDI port；
改用固定上游 archive + SHA256，CPU-only 配置不下载。保留 NVIDIA RTX SDKs LICENSE，SDK 源码不复制进仓库。
应用桥接负责面积/环境 proposal、BSDF 目标、完整 float UV 与当前可见性；SDK 负责 StreamSample/FinalizeResampling。
不使用 SDK 16-bit UV 重新构建已选样本，避免目标与着色坐标不一致；不引入其示例框架或 C++ 资源管理器。

新增 `submit_restir_rays/view` 和 `configure_restir_view`，沿用 `RestirDiSettings`；图像来源显式记录该配置。
旧调用不变，不扩展 Editor/capture schema。仅 Surface only + MIS；候选数 1..64/version 1，非法组合拒绝。
固定射线/小图无复用入口逐样本在 invocation 内完成 RIS，作为批量数值对照；渐进入口按全图两阶段调度：
无历史入口每像素每次提交最多 2^18 次候选评估，超过时显式拒绝；高 spp 参考按连续 `PathTraceSamples` 区间分批。
该限制防止把 64×65536 候选集中到单 invocation，实测此类长任务曾触发 device lost；不降低总采样数或改变估计器。
首表面与初始 reservoir 全部 tile 完成后，才进入最终可见性、独立 delta 太阳和路径尾部着色。
每 epoch 对应一个新路径样本，batch 1..16 依次推进 epoch，全批完成才发布图像/最后表面/reservoir/spp。
同一计数器域与光能分工用于两个入口。首表面保存命中、位置、法线和观察方向；尾部从保存的首命中继续，避免重复查询主射线。

首表面 64 bytes/pixel，初始 reservoir 32 bytes/pixel，线性累积 128 bytes/pixel；完成 owner 共 224 bytes/pixel。
当前批次候选 224 bytes/pixel，batch>1 另需 128 bytes/pixel 的私有累积 ping-pong。两视图/外持旧帧按真实 owner 另计。
峰值 payload 为 `224*sum(held_complete_pixels) + (224 + (batch>1 ? 128 : 0))*candidate_pixels`，
再加场景/纹理/AS/SBT、每 tile 参数/诊断和显式读回；读回单独为 128 bytes/pixel，固定射线结果为 64 bytes/pixel。
渐进 RIS tile 临时 payload 现为参数 672、dummy ray 48、诊断 64、诊断读回 64 bytes，首次无历史时另有 128 bytes 零输入。
固定射线批次还持有 32 bytes/ray 输入、64 bytes/pixel GPU 输出及同尺寸读回（加 4 bytes 诊断），
以及参数 672、占位采样面 96、零历史 128、诊断 64 bytes；上述均为 payload，实际分配需对齐。
实际 Vulkan memory requirements（含对齐）由同一 AllocationBudget 准入，payload 预检不替代实际额度；不包括 driver 隐藏管线开销。
每路径最多 `2*max_bounces+2` 射线，候选数乘 tile 像素数/样本数受 2^24 预算限制；渐进全目标射线上限保持 2^32。
tile 1..64，候选和射线预算共同限宽。状态报告区分阶段和 epoch；只在完整批次发布时推进完成计数。
每次 submit/poll 检查取消与 generation；阶段中途失败/迟到结果丢弃整批，已完成 owner 及其采样平面不可变。
每阶段通过 Graph 声明访问，使跨提交的写入到读取、下一 epoch 的覆盖均有显式同步。

定向验收：SDK Slang/SPIR-V 真实编译、required RT/sync validation、独立 CPU 数值对照、光能/遮挡/mask/GGX/间接光、
透视与正交、不同 tile/batch 重放、阶段中取消/失败/迟到、双视图/旧 owner/额度；不运行 Editor 或时空复用验收。
数值接收点避开共享三角形边的精度平局；CPU 双精度三角形求交与硬件遍历不承诺边界逐位相同。
纹理先按原 M16.7.1 过滤契约（绝对误差 .005）核对任意 UV 的硬件过滤，
再以同 GPU 的 BSDF-only 白炉作初始 RIS 均值参考，仍同时满足 2% 与 5SE；严格 CPU/GPU 光能测试另用二进制可表示的过滤权重。
这是分离确定性硬件过滤误差和采样误差，不放宽 RIS 统计门槛；非纹理和独立积分协议不变。
固定射线上传将 TMin 的有符号零规范成 +0，保留其他裁剪值；实测 RTX 4070 Laptop 对 -0 TMin 漏命中，
对照只改变 TMin 符号即可恢复。该修正同步普通/介质固定射线入口，避免相机生成的合法 -0 经接口传递后改变可见性。

## M19.3 时间复用实施契约

`RestirDiSettings.temporal` 为可选的 `RestirTemporalSettings`（version 1）；默认关闭，保持 M19.2 结果。
时间模式只允许完整渐进视图，CPU 初始参考和无历史 rays/view 请求显式拒绝该配置。
默认历史有效 M 上限 64、祖先年龄上限 8，合法范围分别 1..4096、1..32；
位置绝对容差默认 .1 米（0..1e6，转换为 float 后仍必须大于 0），法线余弦阈值默认 .95（0..1）。所有数值必须有限。
设置变化清空线性图像与采样历史，预算/显示/调度变化不改变已冻结批次。

每 epoch 按全图初始 RIS → 全图 temporal（pass=3）→ 全图着色（pass=1）推进。
temporal 只改本像素 reservoir，读取独立冻结的前一 epoch 表面/reservoir；不读相邻当前输出。
当前命中投影到前来源视图，先校验 clip w/depth、屏幕边界、primitive、位置距离与法线；
失配/显露使用新候选。静态 primitive 身份同时约束材质，当前遮挡始终重新发阴影射线。
前源保存自己的位置、重心/材质、法线和观察方向；GGX 在当前/前观察方向分别重算目标。

合并使用 RTXDI_CombineDIReservoirs 流数学，并使用 M19.1 的支持校正：
fresh 与 history 的流权重为 `p_current(Y_i)*W_i*m_i`，选中 Y 后
`W=sum_weights/(p_current(Y)*(m_fresh+m_history*indicator(p_previous(Y)>0)))`。
fresh 覆盖当前正支持，history 是前接收点正支持上的正规化样本；由两来源期望的分区恒等式，结果在当前支持上正规化。
递归保持这一性质，前提是几何来源筛选、m 和祖先年龄不依赖选中的光样本。
零权重来源保留 M；history 的正规化 W 不随 M 截断改变，截断后的 M 同时用于流权重与分母。
age=全部参与来源的最大祖先年龄+1，与最终选中哪个来源无关；到上限整体丢弃历史，回到 age=0 的新候选。
不复用/烘焙可见性，不套 SDK 默认总 M 归一化，也不把 SDK 中所选样本的 age 当祖先年龄。

采样序号与图像 spp 分离。相机姿态变化保留最近完整采样 owner/来源视图，清空图像并从其下一 epoch 继续随机流；
投影/尺寸、冻结场景/物理设置、seed/算法/复用配置、session/gesture 或显式 reset 清空两者。
曝光/暂停/目标 spp/tile/batch 保留；cancel 丢弃私有工作且保留最后完整采样状态。
采样序号为 uint32，预检图像目标与起点之和，拒绝溢出，不回绕随机流。
来源报告记录图像的 sampling_first_epoch、开始时的历史 epoch、累计复用/失配/过期次数和最大 age/M；
这些只在整批成功时发布，链内二阶矩不是独立样本误差估计。reused 指通过几何/age 校验参与合并，
不等于最终选中旧样本；rejected 包含无历史、未命中和重投影失配。

完成 owner 仍为 224 bytes/pixel。时间模式多 epoch 批次增加表面/reservoir 的 96 bytes/pixel ping-pong，
峰值候选为 `224+(batch>1 ? 224 : 0)` bytes/pixel；所有旧 owner（包括相机移动保留的采样 owner）独立计费。
当前参数为 42 float4（672 bytes），RIS tile 状态/读回各 64 bytes（含 M19.4 扩展）；无历史前源绑定已初始化参数缓冲作占位，has_history=0 禁止读取。
每像素最多两次额外目标评估（history 在当前、最终样本在前源），候选工作预算按 `initial_candidates+2` 预检；
不增加校正射线，因为支持使用未遮挡目标；射线界仍 `2*bounces+2`。
首次配置或 batch 准入失败不覆盖旧图/历史。批内交换均为私有缓冲，GPU 完成、诊断有效且 generation 匹配后同时提交。

验收包含支持域变化/零来源/截断/刷新递归数学、至少 8 条独立 seed 链的均值和 N/4N 趋势、
透视/正交移动与旋转/停动、显露/GGX、重置矩阵、batch/tile 重放、双视图/外持 owner、各阶段失败和取消恢复。
时间模式默认不开启；可选空间组合见 M19.4，介质/消费者/DLSS 不在时间阶段实施。

## M19.4 空间复用实施契约

`RestirDiSettings.spatial` 是独立可选配置，默认关闭，可单独或与 temporal 同用；
version 1，neighbor_count 默认 4（1..8 次邻域尝试），radius 默认 2（1..32 像素），
max_source_candidates 默认 64（1..4096），position_tolerance 默认 1（有限正数，至多 1e6），
normal_cosine 默认 .9（-1..1）。无历史固定射线/单次 view 和 CPU 初始入口拒绝此配置。
只作用于 Surface only 首表面；配置改变清空图像和采样历史，预算/调度改变仍只影响下一批。

epoch 顺序为初始 → 可选时间 → 可选空间（pass 4）→ 着色。空间输入是整幅已完成初始/时间 reservoir，
独立输出面写完后才交换；首表面始终只读。同一像素必保留自身来源，邻居从半径方形中按独立
purpose 6 计数流抽取（排除中心，越界/重复/未命中/距离/法线失配丢弃，不补抽）；purpose 7 用于替换。
最多 8 次尝试，无循环重试；邻域与筛选不依赖已选光源。允许跨 primitive/材质，通过源位置、法线、
重心材质和观察方向重算各自 BSDF 与目标；不能把材质相同当作支持相同。

每个参与源的正规化 W 不变，m=min(M,max_source_candidates)，包括自身；零 reservoir 保留 m。
输出 M 至多 9*4096=36864，float 能精确保存此整数。合并权重为 p_current(Y_i)*W_i*m_i，选中后在全部来源接收点重算正支持，
Z(Y)=sum(m_i*indicator(p_i(Y)>0))，W=sum_weights/(p_current(Y)*Z(Y))。
自身来源覆盖当前正支持，沿用前文分区恒等式；多个相关来源的边缘正规化成立时仍适用。
保留全部参与源的最大 ancestry age，同一 epoch 空间阶段不另加龄；跨 epoch 时间阶段统一加一，
因此空间不能通过选中年轻样本洗掉老祖先，且不会改变 max_history_age 的时间单位。

可见性仍从目标支持中排除。空间源的完整支持校正只需 BSDF/几何评估，额外校正射线为 **0**；
最终选中样本在当前接收点重新发一条 shadow query（含 mask），不可复用源阴影或按最终遮挡清空 reservoir。
这保持 W*F_visible 的积分，无需把源可见性错误加入 Z。当前阴影查询次数在空间诊断中独立报告，
最多每像素每 epoch 一次连续灯阴影；delta 太阳与路径尾部维持原预算，最坏总界仍 2*bounces+2。
候选工作预检每像素 initial_candidates+(temporal?2:0)+2*neighbor_count+1 次目标评估。

完成 owner 仍 224 bytes/pixel。空间批次额外独立 reservoir 32 bytes/pixel；
候选峰值为 224+(spatial?32:0)+(batch>1?(temporal?224:128):0) bytes/pixel，
再加所有外持/历史 owner、场景、读回与 tile 临时资源。参数为 42 float4（672 bytes），
状态和渐进诊断读回各 64 bytes；按真实 Vulkan requirements 对齐计费，不静默关闭空间或校正。
空间诊断记录参与/拒绝邻居、支持校正次数、最大 M/age 和当前阴影查询数；与图像及历史一起整批提交。
空间 pass 失败/取消、迟到结果或额度拒绝保留旧完成 owner，私有输出不提前发布。

验收用独立 double CPU 空间参考与 GPU 多像素结果、离散相关来源/零支持反例、8 条独立链 N/4N，
并验证薄墙/mask、法线/材质/轮廓变化、空间单开与时空同开、tile/reverse/batch 重放、各阶段失败/取消及额度恢复。
统计门槛沿用 M19.1，不将链内像素/帧当独立样本。性能收益与 Editor/capture/DLSS 仍由后续阶段验收。

## 估计器与光能分工

### 初始 RIS 与样本域

CPU 参考采用以下初始 RIS 契约：

- 光样本含光源种类、冻结光表身份、三角形及重心坐标或世界方向；不能只存一个跨版本可能变化的 GPU 下标。
  三角形采用面积测度，环境/有限太阳采用立体角测度，种类标签组成互斥的积分分量；PDF 含种类和光源选择概率。
  跨接收点仍保存同一个光源点/方向，重新计算几何项、BSDF 与目标函数；禁止混用面积/立体角 PDF。
- `F_x(y)` 是接收点 x 对光样本 y 的 RGB 直接光贡献密度，初始标量目标 `p_hat_x(y)` 取非负亮度，
  首版不含遮挡，必须在有贡献处具有正支持。最终着色重新查询当前几何/mask 可见性。
- 对同一完整支持 proposal q 产生的 M 个候选，权重 `w_i = p_hat_x(y_i) / q(y_i)`，
  reservoir 按 `w_i / sum(w)` 替换所选样本；最终估计为 `F_x(y) * sum(w) / (M * p_hat_x(y))`。
  此式只定义初始 RIS；不同来源的时空合并须另推归一化，不能直接套用。
- 已生成的零权重候选仍计入 M；全零 reservoir 返回零贡献而不做 0/0。非有限、负权重、溢出或工作超限拒绝候选帧。
  单候选退化为相同 proposal 的普通直接光估计。reservoir 的 M 与 age 均不代表图像 spp 或独立样本数。
- 计数式随机维度独立覆盖候选编号、替换抽签、邻居选择和历史轮次；不占用已有 BSDF/RR/介质 null/阴影随机流。
  新采样模式有显式版本，不更改旧模式相同输入的结果。

### 与现有路径追踪组合

首版采用可审计的光路分区：ReSTIR 独立估计首表面到受支持光源的直接项，关闭该项的旧 NEE；
该表面一次 BSDF 延伸直接命中同一受支持发光分量时不再累加它。
相机直见发光/背景仍完整计入，未纳入 DI 的 mask 发光命中保留完整权重，delta 太阳仍单独估计。
到达下一散射顶点后，其 NEE、后续 emission/MIS/RR 沿原算法执行。
抑制标记只作用于这一段的直接发光分量，不能丢掉下一顶点反射的间接光。

这一路线牺牲首表面直接光的 BSDF proposal，粗糙度很低时可能方差较高；M19.1 单列 GGX 掠射/窄高光对照。
若实测需要 BSDF 候选，先把它加入完整 proposal 混合并推导权重，再更新本设计；
不能把所选 reservoir 的目标值、原 proposal PDF 或 W 当成其最终边缘 PDF，直接套现有 power heuristic。
每一光源分量的“由谁估计、命中是否计入、使用何种权重”必须有表格和数值测试后才能进入 GPU。

## GPU passes、历史与资源

### 完整采样轮次

新模式把一个 epoch 定义为每像素一个新的相机/路径样本及其重采样流程，顺序为：

```mermaid
flowchart LR
    A[冻结视图与输入] --> B[首表面记录与初始 RIS]
    B --> C[时间复用]
    H[上一完整 epoch 历史] --> C
    C --> D[空间复用]
    D --> E[当前可见性与直接光着色]
    E --> F[路径剩余贡献与候选累积]
    F --> G[诊断通过后整批发布]
    G --> H
```

M19.2 实施首表面/初始 RIS 与最终着色，M19.3/4 分别增加可选时间和空间阶段。
各阶段仍可按 tile 提交，跨阶段必须有完整输入面及 Graph 依赖/屏障。
空间 pass 从冻结的输入 reservoir 平面读，向独立平面写，不能原地读取邻居刚更新的数据。
旧完整历史、当前初始/时间结果与空间输出的活跃期分别计入内存；M19.2 冻结初始采样布局，新增复用平面在 M19.3/4 分别确立。
首表面记录与该 epoch 的抖动主射线一致，记录位置、几何/着色法线、接收材质/primitive、观察方向与有效性，
不直接使用光栅深度或累积图像的旧首样本 hit。M19.2 保存首命中后确定性继续路径尾部，不重复查询主射线。

`batch_spp` 仅打包多个顺序 epoch，不改变复用拓扑；tile 大小和遍历顺序不影响样本编号或邻接输入。
包内 epoch 作为私有候选推进，整批通过才一起提交；取消后从最后已发布状态重算。
先验证这些性质再支持多 epoch 批次，不能把单 tile 的多个样本循环视为全屏空间复用。

### 时间、空间复用与失效

时间复用对静态几何使用当前命中世界位置和前视图投影寻址，保存前视图与前表面记录。
检查越界、有效深度、世界位置/法线、材质/primitive、投影与 disocclusion；失配使用新候选。
视角变化后以当前观察方向重算 GGX 目标；透视/正交分别验证，正交按各像素近面起点建射线。
邻域阈值、邻居数、历史长度与 age 上限先冻结小范围候选，随后以边缘/高光/运动测试决定默认值。

空间与时间合并同时保存来源接收点/目标支持信息，推导支持感知的归一化和必要的可见性校正。
最终光线可见性始终重新查询；不能仅靠深度/法线相似就宣称没有能量偏差，
也不能在最终遮挡后把 reservoir 清空并继续当正常历史复用。
历史 M 截断、age 淘汰和候选拒绝必须与权重更新成套定义；不靠 firefly clamp 掩盖问题。
首版以有正确性推导的校正模式为交付条件；任何近似模式以后单独命名、报告偏差和验证。

| 变化 | 线性累积 | reservoir 历史 |
| --- | --- | --- |
| 同一冻结输入的下一 epoch | 继续 | 复用上一完整 epoch |
| 仅相机姿态变化，尺寸/镜头不变、静态几何 | 从新视图样本 0 开始 | 满足重投影校验才复用；不得把旧辐亮度混进新视图 |
| 投影种类、FOV/正交高度、裁剪、尺寸或预览分辨率变化 | 重置 | 首版清空 |
| 几何/材质/纹理/光源/环境、介质参数/时间变化 | 重置 | 清空；保持原有几何/云场缓存可复用的边界 |
| estimator/seed/算法版本/复用配置变化或显式 reset | 重置 | 清空 |
| 曝光、暂停、目标 spp、tile/打包调度变化 | 按原调度语义保留 | 保留，下一批消费新调度 |

线性图像身份、可复用采样身份与提交 generation 分离。可复用历史持有自己的来源视图，
不能修改旧 frame 的来源以伪装成当前视图；多视图与多个独立捕获任务互不共享 reservoir。
保留历史的相机移动只重置线性 spp，采样历史的 epoch/随机计数继续推进；显式清空历史才从约定起点重启。
两种计数均只随完整提交推进，来源报告分别保存，避免视图切换重复使用同一抽签序列或无法重放。
有限历史、持续注入新候选和独立历史链统计是验收的一部分；递归复用引入相关性，
不能用普通像素二阶矩直接推断独立样本标准误差或承诺无限积累必然按 1/spp 收敛。

### 提交与失败保护

Renderer 拥有当前/前一完整表面及 reservoir、候选输出、工作计数与 GPU 票据；外部完成帧仍是不可变拥有型结果。
验证、分配、各 pass 完成、诊断有效且 generation 匹配后，才同时发布图像、reservoir、epoch 和 completed_spp。
配置拒绝、分配失败、任一 tile/pass 故障、取消或迟到结果均保留上一完整状态，不推进计数；device lost 沿现有终态处理。
重投影允许相机间复用不意味着允许旧代在途工作发布。

继续遵守实际 owner 计费的 1 GiB 默认额度：旧完成帧、候选、首表面、所有 reservoir 面、AS/纹理、读回及仍在途资源都纳入。
M19.2 给出精确 bytes/pixel 和两视图/外持旧帧的峰值公式，预检所有尺寸与乘法溢出；
不得沿用旧分辨率上限就推定新模式有足够显存，也不静默降分辨率或关闭校正。
最坏射线预算覆盖初始查询、最终阴影、校正射线与路径尾部，介质接入后还包括 tracking；
保留有界 tile 与取消点，完整 pass 屏障不能退化成阻塞 GUI 的全屏等待。

## Scene media 接入边界

M19.5 先在 CPU 推导，再由 M19.6 接入 GPU。只对相机路径在首次真实散射前到达的首表面应用 DI；
若先发生体积散射，该路径继续普通体积积分，后续表面/介质顶点均不使用屏幕 reservoir。
行星首表面需要独立表面种类/身份与重投影规则，不把它伪装成网格 triangle。

首表面候选记录原则上由同一抖动几何射线生成，独立于 hero 通道和自由程抽样，
避免“仅未散射路径生成 reservoir”引入未计入的条件选择概率；这也可能带来额外几何查询成本，必须测量。
目标采用确定性的完整 RGB 未遮挡贡献，选样随机流与 hero 通道、analog 存活、ratio tracking 阴影分离。
最终着色用当前路径通道/吞吐量与全新阴影透射估计，网格/mask、行星、空气、云均参与可见性/透射。
不能缓存随机透射率当作确定的 PDF，也不能从 RGB 权重直接推定 hero 通道估计无偏。

有限太阳圆盘按现有角半径、辐照度及方向密度进入样本域；常量边界环境不包含太阳，
不引入 Sky LUT/M15 云多散射 boost。首段 BSDF 延伸中若发生体积散射，就结束首表面直接事件的抑制标记，
保证后续散射的发光/MIS 完整；无事件直达受支持光源时按前述分工处理。
零介质退化、纯吸收、强彩色消光、云遮蔽、多次散射、太阳圆盘与行星均值检验必须先成立。
M19.5 若发现条件概率/支持域无法满足，先修订推导与范围，再进入 GPU，不通过静默降级关闭介质。

## 接口与消费者计划

### M19.5 CPU Scene media 条件采样契约

`VolumePathReference.hpp` 增加独立 `estimate_restir_volume_reference` / `render_restir_volume_reference`，
沿用 `VolumePathSettings` model 4/sampler 2、`RestirDiSettings` version 1/初始候选 1..64。
CPU 只实现首表面初始 RIS，拒绝 temporal/spatial 和 BSDF-only 组合，不把测试用空间参考扩展成介质历史会话。
固定射线返回 `RestirVolumeEstimate`；图像沿用 `VolumePathImage`，来源增加可选 restir 配置与专用统计，
旧体积入口配置为空、算法/随机序列不变。不改变 SDK 依赖、GPU/Editor/capture 或 DLSS 接口。

每条抖动主射线先查询最近网格与物理行星边界，保存独立 mesh/ground 身份、接收位置/法线/BSDF/观察方向；
在 hero 通道与 analog 自由程抽样前，用 RIS1 独立计数域生成完整 RGB 未遮挡 reservoir。
首次几何查询和 medium segments 供原路径循环复用，不重复主射线；只有“零真实体积事件到达首表面”才消费此 reservoir。
先散射或吸收的路径仍产生几何首表面候选（若存在且 max_bounces>0），随后丢弃；后续表面和体积顶点均沿原 MIS/RR。
没有网格/行星终点或反弹上限为零时不生成候选。行星独立构造 Lambert 接收点，不能用伪三角形下标寻址。

光域为互斥的 environment / finite-sun / non-mask-area 标签：均匀选活动种类，环境和太阳用立体角测度，
面积灯用面积测度；每个 proposal 都包含 class probability。太阳沿已有稳定圆锥采样，L=E/(pi*sin²(alpha))，
其与常量环境在同一方向的贡献按不同标签分别计入，不能合并 PDF。目标为完整 RGB 贡献的非负亮度，
不含 hero、相机存活概率、网格/行星阴影或随机透射；RGB 任一通道非零时目标必须为正。

条件于固定主几何接收点 x，令 C 为均匀 hero 通道，A_c 为相机到 x 无真实事件的 analog 存活事件，
(Y,W) 是独立生成、在完整 RGB 正支持上正规化的 reservoir，T_hat_c(Y) 是新的 ratio-tracking 阴影估计。
首表面 c 通道估计为 `3*1(C=c)*1(A_c)*W*F_c(Y)*V_mesh/planet(Y)*T_hat_c(Y)`。
由于候选/替换与 C、A 独立，阴影随机流条件于 Y、c 独立，期望为
`T_camera,c * integral(F_c(y)*V(y)*T_light,c(y))`，无需额外除以通道/存活概率（hero 的 3 已抵消 1/3）。
候选的亮度目标仅影响方差，不能以随机 RGB 透射替换此确定目标或缓存它为 PDF。
以后时空来源只有满足相同条件独立性与完整支持正规化前提才能使用 M19.3/4 合并公式，本阶段不推定 GPU 已满足。

首表面直接项由 RIS 完整负责，关闭该处普通 NEE；仅其下一次 BSDF 延伸无真实介质事件直达非 mask 面积灯、
环境或太阳时抑制对应 emission。null collision 不解除抑制；若延伸发生真实体积散射，则立即解除，
该体积顶点及后续 MIS/emission 完整保留。相机直见发光/天空/太阳、mask 发光与第二表面反射不丢失。
选中样本的阴影使用新增 VolumeSampleStream::restir_shadow_path（尾部追加，旧 0..10 编号冻结），
不消耗普通 light/free_path/collision/BSDF/phase/RR/shadow_path 维度。

统计区分 prepared_mesh/prepared_ground、候选评估、实际应用首表面、选中阴影查询、主段散射/吸收和延伸真实散射；
候选数不是 spp，准备了 reservoir 不等于路径存活。候选预算预检 pixels*spp*initial_candidates，
geometry 仍为 (2*bounces+1)*pixels*spp（复用首查询，首表面最多一次新阴影），triangle/tracking 和逐路径额度沿原契约。
输入/环境/版本/预算先验证；失败、取消、非有限或 tracking 超限只返回错误，冻结场景/介质/已完成图像保持可用。
图像完成所有像素后才返回 owner，保留环境全量、场 hash、风时间、采样/物理参数和 restir 来源。

验收：离散可枚举的 RGB/hero/存活/随机阴影条件恒等式；8 seed 的真空 Lambert/GGX、面积灯/太阳重叠、
纯吸收/强彩色消光、行星/网格排序、真实云阴影与固定/变化风时间；主段先散射与首表面后散射、多次散射/RR、
mask 发光分工、无光与零目标、分批和透视/正交图像身份、预算/主动取消/分配失败和 owner 寿命。
数值沿用 2% 与 5SE 双门槛，独立 quadrature 先检查 N/2N；GPU/性能验收由后续阶段完成。

M19.1 以独立 `estimate_restir_reference` 入口和 `RestirDiSettings` 冻结 CPU 初始采样及版本；
M19.2 的 GPU 与渐进入口沿用该配置，实际来源置于 `PathTraceGpuImageInfo.restir`。
M19.3 以可选 temporal 配置提供时间复用与历史界限；后续消费者计划采用 `direct_lighting = classic_mis / restir_di`。
M19.4 以可选 spatial 配置提供空间复用与有界邻域。RTXDI backend 及 DLSS reconstruction 分别适配。
与 Surface only/Scene media 正交，不复用介质模式开关表达算法；旧请求默认保持原行为。
BSDF-only 对照与 ReSTIR 同时请求的非法组合显式拒绝。设置不进入 Scene 文件/Undo。

M19.7 扩展 Editor、worker、独立 `render.capture` 请求/来源与版本化 schema，同步命令参考和示例。
显示实际采样算法、完成 spp、历史 age/复用诊断与重置原因；M/候选数不能显示成额外 spp。
固定捕获从空历史按确定 epoch 顺序运行，记录 seed、版本、配置、输入指纹与历史起点；不能借用 GUI 隐藏历史。
暂不提供从任意 first sample 随机跳到一个有历史结果的语义；续算必须拥有完整会话状态或从起点重放。
固定射线列表入口可验证无复用 RIS；请求时空复用必须提供完整视图/像素对应关系，否则明确拒绝。
时间复用相机序列先由专用测试夹具重放。现有 `render.sequence` 是光栅入口，M19 不默认为它增加路径后端。

## 验证与测量计划

按 Roadmap 逐阶段运行定向检查；下面是待执行协议，不是本轮结果。

| 层次 | 最小证据 |
| --- | --- |
| CPU 数学 | 可枚举离散灯分布的期望/选择频率；M=1、零权重/全黑、极端权重、来源支持不重合、合并/归一化；独立面积/半球积分、光路分工与 GGX 均值 |
| GPU 表面 | 初始/时间/空间逐项开关；独立 CPU 均值、黑场/环境/多面积灯、mask/双面、窄高光/遮挡边界；required RT 与同步 validation |
| 时域状态 | 透视/正交平移旋转、遮挡显露/镜头跳转、灯亮灭、材质/资产变化、双视图、曝光、暂停恢复；失败/取消后重放、外持旧帧与预算拒绝 |
| 介质 | 真空退化、独立透射/单次散射参考、彩色消光、空气/云/行星/有限太阳；先散射与先表面分支、固定风时间与变化失效 |
| 端到端 | 同设备相同历史起点下改变 tile/遍历/批次结果一致；GUI/capture 同模式来源一致；后台窗口、取消/退出、非法请求不覆盖旧图/旧文件 |

至少 8 条独立 seed/历史链进行均值验证，以链为统计单位；复用链内部的相关样本不能充当独立观测。
CPU 数值验证同时满足 `2%*abs(reference)+1e-4` 和 `5*combined_standard_error+1e-4` 两项偏差上界，
参考认证、样本阶梯与后续 GPU 固定画幅协议见 M19.1 CPU 实施契约。
统计误差检查配合解析/可枚举的期望与支持域反例；仅“落在误差条内”不能代替估计器推导。
增加长链、有限历史刷新、N/4N 的多链误差趋势及镜头停动后恢复，避免短期低噪声掩盖能量偏差或样本停滞。

复用 [PathBenchmark](../../tests/integration/PathBenchmark.cpp)、[介质基准](../../tests/integration/PathMediaBenchmark.cpp)
与已有[性能记录](../benchmarks/2026-10-16-path-media.md)的冻结输入/线性输出/原始报告原则。
基准包含单灯、约百灯、约千灯的发光三角形程序场景及 Sponza；分别测漫反射、GGX、几何阴影、
空气/稀疏云、云内与天空。比较原 MIS、仅 RIS、加时间、再加空间，保持相机、物理输入、分辨率、反弹与曝光一致。
不加去噪，报告等时间线性误差及达到同误差的 wall/GPU 时间，单列冷准备、热交互、校正射线数、tracking 和峰值显存。
同 spp 只作诊断，不能代表相同成本；CPU/GPU 未收敛参考不认证为真值，达不到阈值的时间记未达。

多灯表面场景的候选收益目标为等误差耗时至少降低一半；M19.1 已冻结场景/阈值/设备协议，
其 [CPU 基线](../benchmarks/2026-10-19-restir-reference.md)仅证明当前质量与成本，不是 GPU 收益验收。
M19.8 至少三轮独立计时，不能验收时降低目标。单灯、天空和厚云允许无收益，但必须公开结果，
未达收益目标时保持显式可选并记录未达，不能据此宣布实时或替代默认 MIS。
沿用热相机首个完整预览/取消 p95 ≤100ms、tile GPU p95 ≤8ms 作为交互候选目标，额外 pass 成本一并计算。

## 决策依据与未决问题

以下资料于 2026-10-19 核对；本文的阶段与工程选择是针对当前代码的规划。

- [Bitterli 等，ReSTIR DI（2020）](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct)：
  直接光候选的时空重采样；论文区分有偏/无偏估计，本项目以能量和均值检验为先。
- [Lin 等，GRIS / ReSTIR PT（2022）](https://research.nvidia.com/publication/2022-07_generalized-resampled-importance-sampling-foundations-restir)：
  相关样本、不同域和路径 shift mapping 需要独立理论，DI 完成不等于全路径复用或自动获得渐进收敛保证。
- [RTXDI 集成说明](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/Integration.md)：
  核对光表、接收面、reservoir 与 passes 的职责；本项目保留自己的资源与调度系统。
- [RTXDI 噪声与偏差说明](https://github.com/NVIDIA-RTX/RTXDI/blob/main/Doc/NoiseAndBias.md)：
  核对目标支持、复用归一化和可见性复用的偏差风险，不将基本归一化等同完整校正。

M19.1 的首表面光能分工、合并数学及有限历史的约束见 CPU 实施契约；窄高光保留 light-only proposal 的高方差限制。
M19.2/3 的 RTXDI/GPU 布局、容量、时间复用支持校正和有限历史见实施契约；M19.4 增加屏幕邻域支持与空间平面，
M19.5 单独解决介质条件采样，DLSS 的 SR/RR/FG 范围与能力协商另行设计。
M19 验收后，再依据直接/间接/介质噪声占比决定下一步采用 ReSTIR GI/PT、ReGIR 或体积重要性采样。

## 相关设计与已有证据

- [路径追踪](render-pathtracing.md)、[Vulkan RT](graphics-raytracing.md)、[Graph](graphics-graph.md)、[相机交互](editor-interaction.md)。
- [0117 渐进状态与额度](../development/0117-pathtracing-progressive.md)、[0128 介质性能](../development/0128-path-media-performance.md)、
  [0132 路径正交与捕获](../development/0132-pathtrace-orthographic-capture.md)。
- [0134 CPU 参考实施记录](../development/0134-restir-reference.md)；各后续 M19.x 分别留档、验证并本地提交。
- [0135 GPU 初始 RIS 与 RTXDI](../development/0135-restir-gpu-initial.md)。
- [0136 时间 reservoir 与采样历史](../development/0136-restir-temporal.md)。
- [0137 空间 reservoir 与支持校正](../development/0137-restir-spatial.md)。
- [0138 Scene media 首表面条件采样](../development/0138-restir-volume-reference.md)。
