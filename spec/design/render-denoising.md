---
module: render-denoising
created_at: "2026-10-23T16:58:00+08:00"
updated_at: "2026-10-23T17:51:15+08:00"
status: accepted
---

# 路径追踪降噪设计

## 目标与实施边界

M20 提供可独立选择的 `off / atrous / svgf / asvgf / nrd_reblur / nrd_relax / vk_oidn`。
À-trous 是正式空间降噪选项，也是 SVGF/A-SVGF 的公共空间滤波组件；三种自研算法用 Slang 实现。
NRD 和 vk_oidn 是私有后端。阶段、前置与当前完成状态只由 [Roadmap M20](../roadmap.md#m20路径追踪降噪)维护。
M20.1 确定公共边界与固定依赖；M20.2 实现共享信号类型和路径侧信号 shader/发布，尚无降噪滤波器、SDK 接入或 UI。
`accepted` 表示采用本设计，不表示任一滤波器已实现或已通过 GPU 验收。

当前依据是 [PathTraceRenderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)、
[路径 shader](../../engine/render/pathtracing/shaders/pathtrace.slang)、[累积 shader](../../engine/render/pathtracing/shaders/path-accumulate.slang)、
[ReSTIR 桥接](../../engine/render/pathtracing/shaders/path-restir-bridge.slang)及 [PathPreview](../../engine/editor/src/PathPreview.cpp)。
原始输出仍是合并 radiance、二阶矩和首样本 hit/normal；首样本诊断不作为降噪 G-buffer。
可选信号通过[独立类型](../../engine/render/denoising/include/dk/render/DenoisingSignals.hpp)和
[信号 shader](../../engine/render/pathtracing/shaders/path-signals.slang)生成，原始累积、ReSTIR reservoir 与 worker 完成发布契约保持。

首个可用范围是 Surface only、现有静态三角形、Lambert/GGX、纹理/mask、透视与正交相机。
相机运动支持不隐含动态物体/形变运动向量。Scene media 在 M20.10 独立验收；DLSS、OptiX、官方 OIDN 运行时、
降分辨率重建、异步 compute、训练网络和新材质不属于本轮实现。

## 模块边界和依赖方向

目录为 `engine/render/denoising`，公开文件在其 `include/dk/render/`；目前仅有共享 types。
后续滤波器的内部适配和 shader 放 `src/`、`shaders/`，信号生产 shader 留在 Pathtracing。

| target / 状态 | 职责和依赖 |
| --- | --- |
| `dk_render_denoising_types` / `dk::render_denoising_types`，已实现 | INTERFACE target，仅依赖 Math（传递 Core）；信号布局、来源身份、运动 reference，不含 GPU/SDK 类型；随 CPU/GPU Pathtracing 配置提供 |
| `dk_render_denoising_reference` / `dk::render_denoising_reference`，待实现 | 小图 À-trous、矩/方差与重投影数学参考；PUBLIC types，不链接路径 GPU、NRD 或 vk_oidn |
| `dk_render_denoising` / `dk::render_denoising`，待实现 | 异步会话、GPU 输入/输出、历史和后端；PUBLIC types、graphics_device；PRIVATE graphics_graph、graphics_shaders 及启用的 SDK |

Pathtracing 生产拥有型只读信号包并引用 types；denoising 不反向依赖 PathTraceRenderer，也不驱动正式路径采样。
A-SVGF 的梯度请求/结果通过 types 描述，由调用方安排路径侧重评估，避免 render 模块间循环依赖。
Editor/capture 在上层串联两个模块，不将 SDK 放入 Scene、CPU oracle、Commands 或持久化格式。
Graphics 负责设备、录制、同步和资源寿命，不拥有滤波算法、光源策略或历史有效性判断。

计划构建开关为 `DK_BUILD_RENDER_DENOISING_REFERENCE`、`DK_BUILD_RENDER_DENOISING`、
`DK_RENDER_DENOISING_NRD`、`DK_RENDER_DENOISING_VK_OIDN`，初始均默认 OFF；后端开关要求 GPU 模块开启。
CPU-only 配置不求值 SDK 下载/查找逻辑，不以空 GPU stub 冒充实现。启用某后端构建失败须明确报错，不能静默关闭。

## 公共接口与配置

版本 1 配置使用稳定字符串枚举，SDK 数值枚举不外泄。配置变更仅改变降噪会话，不修改 Scene revision 或原始估计器。
滤波会话 API 在相应实现阶段增加，建议的操作及语义如下；M20.2 信号生产 API 见后文：

| 操作/数据 | 约定 |
| --- | --- |
| `DenoiserSettings` | `version=1`、method、处理信号域、质量/算法参数；默认 `off`，参数有限且有界，拒绝未知/跨后端无效字段 |
| `DenoiserCapabilities` | 分别报告 compiled、设备支持、所需信号、预算可容纳、算法/介质组合支持及原因；SDK 存在不等于 ready |
| `DenoiseFrameInput` | 拥有输入 owner、生产票据、尺寸/格式/步长、来源身份、相机/抖动、样本区间、辅助信号与可选梯度；禁止借用临时资源 |
| `DenoiseMemoryEstimate` | 输入增量、输出、历史、工作空间、SDK、旧代峰值分项及估计上界；区分逻辑字节、实际分配与设备预算 |
| `configure(view_id, settings)` | 校验配置与能力；新实例/资源准备失败不替换已提交配置和结果；需要的设备 feature 必须已在设备创建时启用 |
| `submit(input)` / `poll()` | CPU 侧不隐式等待 GPU；每视图最多一个在途降噪；poll 只查询完成，成功且身份有效才发布 |
| `completed_frame(view_id)` | 拥有型不可变线性降噪结果及 provenance；外持结果不因下一轮覆盖失效，并持续计入预算 |
| `reset / cancel / release_view` | reset 使历史失效；cancel 停止候选发布；release 停止新工作，在途 owner 延迟回收；均不伪造 GPU 取消 |

质量参数分组：À-trous 的迭代/保边权重；SVGF 的历史上限、几何拒绝阈值与矩/方差设置；A-SVGF 的梯度采样预算和响应设置；
NRD 的方法及经验证的调参子集；vk_oidn 的 fast/balanced/high、辅助图模式和按 spp 运行间隔。
具体数值默认值随各后端验收冻结，不直接把上游所有参数透传为公共配置。
请求不支持的方法返回 `not_supported` 及明确原因。交互层可以显式展示原始画面，但必须显示实际 method/status，不能声称已降噪；
固定 capture 请求降噪失败则作业失败，不静默输出另一算法。自动降级不作为 v1 默认行为。

## 信号、采样与坐标约定

全部颜色为曝光前 scene-linear RGB/D65，GPU 原型使用 FP32；压缩为 FP16 等必须另证误差/范围收益。
颜色、几何/材质 guides、梯度携带相同输入快照和采样来源；外部库编码由 adapter 转换。

| 信号 | 必须保存的语义 |
| --- | --- |
| `raw_accumulated` | 原始采样累积及真实 completed_spp；Off/诊断/数值报告的依据，降噪不能回写 |
| `fresh_radiance` | 本轮已完成的互不重叠样本区间的均值和样本数；由采样时显式生成，不用两个高 spp 累积均值相减恢复 |
| diffuse/specular | 按首表面 BRDF lobe 划分路径贡献，含对应直接和间接光；NEE/MIS/ReSTIR 权重保持一致；不能按颜色经验分摊 |
| emission/background | 首命中自发光与未命中背景独立标识；决定是否过滤和如何合成，避免锐利发光细节被重复加入或抹平 |
| albedo/normal/roughness | 线性反射特性、世界空间着色法线和线性 roughness；normal 坐标、有效性及所用采样方式写入描述 |
| depth/identity | 正的相机轴向 view distance，区别于光线 hit distance 和硬件 depth；miss 用有效位，不把无穷值当几何 |
| motion | 当前到上一份被降噪消费输入的屏幕位移，单位为渲染像素、原点左上；前后矩阵及抖动独立记录，adapter 处理 SDK 符号/尺度 |
| hit distance | diffuse/specular 各自代表的后续命中距离及有效性；不能用主射线深度代替；NRD 按固定版本 helper 编码 |
| moments/variance | 区分原始样本矩、时域亮度矩和空间滤波方差；1 spp/短历史不能用零估计冒充确定性无噪声 |

M20.2 表面分量重组要求最大绝对误差不超过 `2e-5 * max(1, max(fresh_sum))`，原有 raw 路径保持原样本/加和顺序。
解调/重调制在 adapter 明确规定，零 albedo、全金属、极低 roughness 和发光面单独测试。

输入存在两种模式：

- `accumulated`：À-trous、vk_oidn 可消费原始渐进均值；辅助 albedo/normal 按相同像素采样分布累积。
  中途启用辅助输出时，不将新 guides 伪称为覆盖已有全部 spp：从共同起点重建所需输入或明确拒绝该来源。
  只执行 `color` 网络可以作为显式配置，不能隐式填造 albedo/normal；`cleanAux` 仅用于确实满足无噪辅助图契约的输入。
- `fresh`：SVGF/A-SVGF/NRD 每次消费完整的新样本批次，单独维护降噪历史；初始交互基线为每轮 1 spp。
  更大 batch 需要各后端明示并验证样本权重，不把历史长度当作 raw spp。
  时域 G-buffer 对应当前轮代表性主射线/抖动；与原始首样本诊断及 OIDN 累积 guides 分开存放。

À-trous 组件也可接收 SVGF 的时域结果，但这不产生用户级“叠加两个后端”。默认一次只选择一个降噪后端。
跨 tile 的空间读取只能在完整输入发布后开始；滤波内部切 tile 必须包含所需 halo，不能以路径 tile 边缘截断邻域。

## M20.2 信号生产实施契约

本阶段仅为渐进 Surface only 的 Classic MIS/BSDF-only 与 ReSTIR initial/temporal/spatial 生产信号，
以 `PathTraceSchedule::signals` 显式开启，默认关闭；固定射线/一次性提交和 Scene media 不开放信号。
共享 `render_denoising_types` 只含 CPU 可读布局/来源/重投影描述，GPU owner 仍由 Pathtracing 管理。
不接入滤波器、SDK 或 Editor/命令字段。

每像素使用独立 FP32 缓冲；fresh 颜色、二阶矩、diffuse/specular/emission/background 保存本批显式补偿和与样本数，
消费时除以 fresh count，不从累计图相减。albedo/世界法线另存从样本 0 起的补偿和、总计数和命中计数；
miss 记零并计入总数，法线均值是否归一化由后端编码决定。首表面 BRDF diffuse/total 比例沿同一路径传播，
NEE/MIS/ReSTIR 贡献在其实际入射方向按该 BRDF 拆分；保留原来的 radiance 运算、随机数和可见性射线。
albedo 明确定义为已采样纹理的线性 baseColor，GGX 的 metallic/roughness 分列；不是预先计算的 NRD 材质因子。

`PathTraceCompletedFrame::signal_buffer()` 返回拥有该 frame 生命周期内有效的 GPU buffer；
`read_linear()` 非阻塞读回 raw 和信号，`PathTraceGpuImage::denoising_signals()` 暴露 CPU 数据。
没有信号时 buffer 为 null、span 为空、`info().signals` 缺席。外部不得仅保存裸 buffer 指针而丢弃 frame owner。
`signal_reference()` 返回显式运动来源描述；同一 renderer/view/session/场景、有效 serial/尺寸/有限矩阵才可用于 configure。
raw settings、view 与源身份一并存于 frame；随机 epoch 与累计 sample index 分列，ReSTIR 移动相机后两者可能不同。

布局版本 1 为 21 个 float4（336 字节/像素），与 `DenoisingSignalPixel` 一致：

| float4 槽 | xyz / 主要字段 | w / 额外约定 |
| --- | --- | --- |
| 0–5 | 本批 radiance、RGB 二阶矩、diffuse、specular、首命中 emission、primary miss background 的补偿和 | 保留零；均值除以 source.sample_count |
| 6–7 | 从样本 0 起累计的 baseColor、世界法线之和 | uint32 位模式：总样本数、命中样本数；均值均除以总样本数，包含 miss 零贡献 |
| 8–10 | 本批首样本世界法线、世界位置、baseColor | 线性 roughness、正轴向 viewZ、metallic；Lambert 为 roughness=1、metallic=0 |
| 11 | motion.xy、首条后续射线命中距离 | 主射线距离；无效/未命中由 flags 区分，不能只检查数值 |
| 12 | uint4：triangle index、代表样本随机 epoch、有效 flags、fresh count | miss triangle 为 UINT_MAX；flags 位依次是 surface/motion/continuation-traced/continuation-hit |
| 13–20 | 前述六个 fresh 和及两个累计 guide 和的补偿项 | 生产者状态，消费端不自行修改 |

Classic 的信号候选增量为 `336 * pixels`，ReSTIR 多 epoch 批次额外需要相同大小 ping-pong；
外持旧帧、raw 平面、读回副本、资源对齐和程序资源另按实际 owner 计入同一 AllocationBudget。
不对 1 spp 的零样本方差作无噪声承诺；此处 RGB 二阶矩不是 SVGF 的时域亮度矩。

每批首样本另外保存当前 normal/roughness、position/正 viewZ、albedo/metallic、triangle identity、
有效位和随机 sample/epoch。代表性首条后续射线命中距离使用独立有效标记；未追踪、miss 和零距离分开，
它不是已编码的 NRD lobe hit-distance，NRD 具体统计与编码仍在 M20.6 验收。
相机运动使用调用方显式提供的上一份已消费完整帧的投影描述，记录其 source serial；
不自动取上一 UI 帧。没有 reference 时 motion 无效；输出为未加全局 jitter 的 curr-to-prev 像素位移，
当前随机像素采样位置可由 seed/首 sample 重建；越界/背面投影拒绝。静态几何，不声称支持动态对象。

信号开启/关闭改变输入语义并重建 raw 与 guides 的共同样本起点；仅更新 reference/曝光/目标/分块不重置累计。
已开始批次冻结配置/reference。原始和信号共用完整批次提交点；source serial 仅成功发布时推进，
失败/取消/过期保留旧输出及其辅助图，外持 owner 持续计费；ReSTIR 多 epoch 的信号 ping-pong 与 raw 独立。
开启时检查设备 storage 范围、整数尺寸及包含信号/暂存/旧代的预算；不够则拒绝，不能悄悄去掉 guides。
信号 shader 独立变体按需创建，Off 不分配信号平面、不运行信号计算。

验收覆盖三种 BRDF 边界、直接/间接/自发光/背景重组、fresh 与累计指南的独立参考、奇数尺寸与多 tile/多 epoch、
透视/正交/显式运动 reference、纹理/mask/天空、Off 原始逐值对照、额度/取消/提交失败/旧 owner 与双视图。

## 后端输入与组合能力

下表是后端待验收目标；已有输入生产不代表已有降噪滤波能力，不能据此增加 UI 开关或注册新命令字段。

| 后端 | 最低输入/历史 | Classic / ReSTIR Surface only | Scene media |
| --- | --- | --- | --- |
| Off | raw | 保持当前功能 | 保持当前功能 |
| À-trous | color、normal、depth、有效性；可选方差/材质分量 | M20.3 分别验证；不要求时间历史 | M20.10 验证空间引导/分量策略 |
| vk_oidn | HDR color；albedo/normal 按所选模型要求 | M20.4 分别验证，不能由 Classic 推定 ReSTIR 噪声适用 | M20.10 验证，不假定表面 guides 对云有效 |
| SVGF | fresh 分量、G-buffer、motion、矩/方差和时间历史 | M20.5 分别验证；ReSTIR 相关样本的方差是滤波启发量，不声称独立样本置信区间 | M20.10 独立设计 |
| NRD REBLUR / RELAX | fresh diffuse/specular、对应 hit distance、normal/roughness/viewZ/motion、相机/抖动和 SDK 历史 | M20.6/7 分别验收两估计器，包含 NRD 所需材质因子和编码 | NRD 只承担经过验证的表面信号，不能直接把整个体积 beauty 当表面输入 |
| A-SVGF | SVGF 输入加稀疏相关时间梯度、重评估来源与置信信息 | M20.8 先 Classic；M20.9 通过梯度协议后开放 ReSTIR | M20.10 单独验证，未通过组合明确拒绝 |

A-SVGF 按[作者方法](https://momentsingraphics.de/HPG2018.html)前向投影前帧表面样本，用相关随机序列重评估着色，
重建稀疏时间梯度并调整逐像素时间累积权重。单纯帧差驱动 alpha 不算完成 A-SVGF。
梯度射线、稀疏分布、重放状态与失败标记由路径侧产生，计入额外工作/显存和总时间。
M20.9 须先确定并验证冻结 reservoir 重评估或独立无复用探针的协议；仅保留随机 seed 不足以重放 ReSTIR 历史。
不让降噪适配器改变正式 reservoir、样本序号或原始估计器统计。

Scene media 在 M20.10 明确表面贡献、体积散射、背景的数学分解、透射/合成顺序和 guides。
只支持表面降噪时 provenance 必须标为部分降噪，体积原始贡献保持可查；不静默切换整个后端或宣称全部去噪。
不扩充当前路径积分器物理范围，不以抹平云细节作为误差改善证据。

## 三类历史与完成身份

原始累计、ReSTIR reservoir、降噪历史分别归各自 renderer/session 所有。完成身份至少包含：
`view_id`、会话身份、原始 generation、输入快照指纹、`source_serial`、sample first/count、sampling epoch、
相机/投影/尺寸/抖动、denoiser configuration generation 和 SDK/算法版本。
`source_serial` 只在一个完整输入成功发布时增加；UI 重绘同一结果不触发降噪、不会增加历史长度。
运动矢量必须相对上一份实际消费的输入构造，不能简单引用上一 UI 帧。跳过输入要重新建立该对应关系或 reset。
原始 generation 改变不必自动丢弃可重投影的降噪历史，但输出只能标属于当前输入的 generation。

| 变化 | 原始累积 / ReSTIR | 降噪历史 |
| --- | --- | --- |
| 曝光、暂停、UI 重绘 | 依现行契约，曝光不重采样 | 保留；不增加 source_serial；重新 tone 不重新滤波 |
| 静态场景中相机移动 | 原始累积重置；ReSTIR 按其匹配规则 | 可重投影，逐像素拒绝无效/遮挡显露；相机明显跳切显式 reset |
| resize、投影模式/镜头变化、手动 reset、换文档/视图 | 依现行规则 | 硬重置，不复用旧尺寸或另一视图历史 |
| 几何/材质/资产重载、后端/参数/guide 语义变化 | 原始与采样身份按各自规则 | 首版保守 reset；仅相同配置的运行间隔变化不改变滤波身份 |
| 灯光连续变化/开关 | raw 和 reservoir 按现有物理输入规则重置 | 后端经验证且几何对应仍有效时可保留，由变化检测/梯度响应；未验收则 reset |
| 介质内容/风时间变化 | 保持 M17/M19 规则 | M20.10 前不开放该时域组合；验收时冻结独立失效策略 |

## 所有权、提交点与失败

一个设备/队列域、每视图独立会话；host 调用外部串行，沿用 worker，不新增全局设备或后台 SDK 线程调度器。
原始帧先完成发布；降噪是独立候选，不能使已成功的原始采样倒退。
输入、SDK instance、工作空间、候选和旧输出均保活至其最后 submission 完成。
两个视图可以交替使用 worker 队列，但不共享可变历史。下一结果写入独立输出，不能覆盖外持 completed frame。

状态路径：`unconfigured -> ready -> pending -> completed`；reset 后历史无效但已完成结果可保留供来源明确的展示。
唯一输出提交点是 GPU 完成、结果有效、输入和配置 generation 仍匹配、且请求未取消。

| 失败位置 | 必须保持/处理 |
| --- | --- |
| 非法参数、缺少输入/能力、预算预检拒绝 | 不提交 GPU，不改变已完成图像或有效历史 |
| 创建 SDK/资源、录制、提交失败 | 丢弃新候选，保留原始和旧完成输出；释放未提交资源；SDK CPU 状态若已改变则使该历史失效 |
| 提交后取消、过期 generation、GPU/结果校验失败 | 不发布候选、不推进逻辑历史年龄；等票据完成后回收；无法证明未污染的 SDK 历史整体失效，下次重新开始 |
| device lost | 报告设备丢失并停止该域提交；不承诺恢复设备资源；保留可用 CPU 状态/诊断，不能发布未完成 GPU 输出 |
| 退出/释放视图 | 停止接单、退休候选并完成已有寿命协议后销毁；已录制 SDK 资源不能提前释放 |

NRD/SVGF 等可能原地更新内部历史：取消不能回滚已经执行的 GPU 写。
因此保证的是“不发布错误候选、保留旧不可变输出”，不是失败后仍保留一份可继续使用的 SDK 历史。
若后端提供双历史提交能力可利用；否则标记失效并 reset，不能只回退 host 计数后继续读被污染的历史。

## Graph、SDK 资源及预算

自研及 NRD dispatch 使用现有 Graph 资源/Pass 声明；NRD adapter 从 SDK 描述建立 pipeline、描述符和纹理池，NRI 关闭。
NRD 的 HLSL 用固定 DXC 编成 SPIR-V，应用 Slang 只包含对应 front/back-end helper，不假定现有 Slang 编译器可替代全部 NRD 构建流程。

vk_oidn 的 `cmdDenoise` 需要原生 `VkCommandBuffer`，但当前
[PassContext](../../engine/graphics/graph/include/dk/graphics/GraphExecution.hpp)没有对外的原生录制逃生口。
M20.4 必须先在 Graphics 设计受约束的外部 compute 录制桥：输入/输出访问和末态由 Graph 明示，
仅回调期借用 command buffer，不允许 SDK 自行提交/等待；内部同步由 SDK 管理，owner/票据和外部 barrier 仍由引擎追踪。
这是待实现的直接受影响链路，不得通过公开任意 Vulkan 句柄绕过 Graph；方案要合回 Graphics 设计并定向验证。

vk_oidn 0.2.0 实际使用 `vkAllocateMemory` 管理内部 buffer；`VkAllocationCallbacks` 仅是 Vulkan host 分配回调，
不等于接管设备显存。`maxMemoryMB` 约束的是中间工作空间，`getMemoryUsage()` 是运行时观测，不是全部峰值的预分配保证。
M20.4 要增加固定版本可验证的内存估计/记账适配，必要时维护最小上游补丁；未有可信上界前不宣称硬额度得到保证。
创建后发现超过预留额度时销毁未提交候选并报错，不能把“事后发现超额”当作通过严格预算验收。

预算由上层统一协调路径和降噪的设备占用；按实际 owner 计入输入增量、输出、持久历史、候选、scratch、SDK 权重/内部资源及退休旧代。
共用 owner 只计一次；配置切换/resize 以旧实例和新实例共存的峰值准入。所有字节计算检查溢出；
外持帧不会绕过预算。动态设备预算是准入参考，不能保证驱动不会 OOM。
报告 CPU heap、引擎 GPU 逻辑字节、实际分配、SDK 估计/实测和设备可用预算，避免以 VMA 统计冒充 SDK 全部显存。

## M20.1 依赖决定

机器可读固定来源见 [依赖锁定清单](../dependencies/render-denoising.lock.json)，用途/许可见[三方库说明](../third-party-libraries.md#m20-降噪依赖基线)。
锁定清单目前是已核验的设计输入，不是 CMake 已接入能力；不修改现有 builtin-baseline、不自动下载至正常构建、不引入运行时链接。

- NRD 使用最新正式 release v4.17.3 对应不可变 commit；不采用主分支的 v4.18 标注。锁定 ShaderMake 与 MathLib 的上游指定提交；
  计划仅 SPIR-V、静态 NRD、NRI/DXIL/DXBC 关闭，SDK 资源由引擎创建。
- NRD 的 DXC 选择已核验官方 registry 的 `directx-dxc` 2026-05-27#0；不沿用 ShaderMake 默认下载的旧 DXC。
  实际引入 host tool 时按三方库规则再次核验并固定 registry；禁止上游隐式下载另一版本或从任意 PATH 漂移。
  ShaderMake 自身 compiler 查找/下载关闭，显式绑定选定 DXC。Windows x64 不走 MathLib ARM 的浮动 sse2neon 分支。
- vk_oidn 使用 v0.2.0 Windows x64 官方预编译 SDK，SHA-256 与发布清单、GitHub asset digest 交叉核对；
  包内含 Debug/Release 静态库、头文件、CMake 导出、权重、LICENSE/NOTICE。使用动态 CRT，实际 MSVC 链接仍待 M20.4。
  打包权重来源是 OIDN 2.5.1 对应的固定 oidn-weights commit，随包冻结，不在运行时下载模型。
- 若 vk_oidn 必须从源码构建，另走同一固定源码版本的显式构建路径：CMake 3.21+、C++17、Python、
  支持 `-denorm-mode-fp16` 的 Slang；上游验证基线为 Slang 2026.17 / Vulkan SDK 1.4.363。
  不能静默升级引擎 Slang 或把预编译包可用推断为源码构建已兼容。

设备创建前分别查询并合并所需 feature；绝不能把上游 enable helper 写入的 true 当成物理设备已支持。
vk_oidn 0.2.0 的声明包括 shaderInt64/Int16、16-bit storage、shaderFloat16、BDA、Vulkan memory model/device scope、
maintenance4、subgroup size control/full subgroups；image 路径另需无格式 storage image 读写及相应绑定能力。
cooperative matrix 是可选加速，要查询支持的类型/尺寸；缺少时只在已验证的普通卷积路径可用时回退。
后端未启用时不强加这些 feature 给原有设备选择；缺少强制能力时保持 Off/其他已支持后端可用。

## 验证与交付

M20.1 验收是设计一致性、正式版本/不可变来源/许可、vcpkg 全索引检查、下载哈希与包内容检查、文档链接/索引检查。
不运行无改动的引擎构建，不把 SDK 下载、接口草案或上游基准当作本引擎 GPU 验收。

后续每小阶段通过[定向验证](../../.agents/skills/decker-build-verify/SKILL.md)选择真实受影响用例：

1. 数学/信号：常量保持、1 spp 短历史、矩/方差有限性、分量重组、边界/mask/GGX、CPU 小图与 GPU 对照。
2. 时间/状态：停动相机、前后投影、遮挡显露、灯光开关、跳帧、双视图、曝光、resize、切换、取消/OOM/旧代/设备丢失/关闭。
3. 质量：同输入多 seed 的高 spp 参考、原始和降噪结果分别留档；线性 nRMSE、固定曝光图像质量、边缘/纹理区域、时间误差与恢复速度。
   不将真实运动造成的帧差算作闪烁，不要求有偏滤波器满足原始估计器的无偏/高 spp 收敛判据。
4. 性能：冷初始化、信号生成、梯度重评估、降噪、合成、GPU 与端到端分别计时；同采样预算和等时间均比较，三轮无 validation 性能串行执行。
   计入所有额外射线、SDK/旧代峰值与控制延迟；不靠改分辨率/反弹预算获得隐含收益。
5. 消费端：真实后台 Editor 使用 `--background`；capture 空历史起点、固定 sample/epoch/参数和版本可重放，旧命令/文件保护通过。

固定 capture 的新字段和 schema 版本在 M20.3/M20.10 实际接入时更新命令参考与发现；本阶段不改 v1/v2/v3 的当前含义。
静态时域 capture 明确从空历史按固定输入序列逐轮消费；暖历史必须有可重放的显式序列，不能导入 GUI 的私有历史。
provenance 记录请求/实际 method、配置、源版本/权重 hash、原始 spp、历史输入数、起点/reset 原因、信号范围及部分降噪状态。
M20.11 在基准冻结前制定各场景质量/时间门槛，按实测决定默认后端；没有通过的组合继续标为不支持。

## 后续阶段需要收敛的问题

M20.2 的通道布局与 lobe 分量推导已冻结，输入验证见 [0146](../development/0146-denoising-signals.md)；
M20.4 完成外部 compute 桥、vk_oidn 显存上界和本机 feature/ABI 验证；
M20.6/7 核验固定 NRD 输入编码与编译产物；M20.9 确定 ReSTIR 梯度协议；M20.10 确定体积数学分解。
这些是已定位的后续验收项，不是 M20.1 已实现的能力，也不影响本阶段接口/状态边界成立。

关联设计：[架构](architecture.md)、[路径追踪](render-pathtracing.md)、[ReSTIR](render-restir.md)、
[Graph](graphics-graph.md)、[设备](graphics-device.md)、[Editor](editor.md)、[捕获](render-capture.md)。
依赖基线证据见 [0145](../development/0145-denoising-foundation.md)，信号实现证据见 [0146](../development/0146-denoising-signals.md)。
