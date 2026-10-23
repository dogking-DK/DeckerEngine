---
module: physics-fluids
created_at: "2026-10-23T18:16:00+08:00"
updated_at: "2026-10-23T18:16:00+08:00"
status: accepted
---

# 粒子流体与统一物理输出设计

## 目标与本次范围

采用 WCSPH、PBF 两种流体方法，各提供 CPU 参考与 Vulkan Compute 实现，并经现有模拟服务接入
点云预览和实验导出。PBF 是流体密度约束方法，不把现有 XPBD 距离约束/布片当作 PBF 实现。
本页为采用的设计，接口和目录中标注“拟”的内容尚未实现；阶段状态以 [M21 Roadmap](../roadmap.md#m21原生点绘制与粒子流体) 为准。

首期为三维、单相、等温、固定粒子数、等质量粒子、单个流体域、静态盒状容器。
SPH 具体采用弱可压缩 WCSPH；PBF 采用固定迭代数的 Jacobi 密度约束投影。
CPU-only 必须能完成数值实验；可视化通过独立 [粒子渲染模块](render-particles.md) 实现。

MAC 网格、场 ray marching、USD/VDB 适配、恢复检查点、发射器、可变质量、多相、移动边界、
双向固液耦合及真实水面光学不属于本期。未来仍保持粒子/场共用单位、时间、来源和物理量语义，
不要求现在建立空网格接口或安装 OpenUSD。

## 模块边界和依赖方向

以下为拟新增模块，落代码时通过 target 的 PUBLIC/PRIVATE 表达依赖；不使用全局 include/link。

| 位置 / target | 职责 | 依赖约束 |
| --- | --- | --- |
| `engine/physics/particles` / `dk::physics_particles` | 粒子 ID、拥有型 CPU 输出、通道语义和输入校验 | PUBLIC Core/Math；不含 Vulkan、Scene、Editor、USD 类型 |
| `engine/physics/fluids` / `dk::physics_fluids` | 配置、CPU 邻域、核/边界基础、WCSPH/PBF、数值诊断 | PUBLIC 粒子公共类型，PRIVATE Memory/Profiling；不依赖图形模块 |
| `engine/physics/particles-gpu` / `dk::physics_particles_gpu` | 拥有型 GPU 粒子视图、缓冲布局与 Graph 接入 | PUBLIC 粒子公共类型、Device/Graph；不依赖具体求解器 |
| `engine/physics/fluids-gpu` / `dk::physics_fluids_gpu` | GPU 邻域与求解 pass、常驻工作集、完成/健康状态 | PUBLIC Fluids/Particles GPU/Graph，PRIVATE Shaders/Profiling；不依赖 Render/Services |
| `engine/render/particles` / `dk::render_particles` | 通用原生点绘制、物理量显示 | 依赖粒子输出与图形模块；不依赖流体或 XPBD 算法 |
| 现有 SimulationServices/Operations | 有类型的实验分支、owner/worker 生命周期、命令与导出装配 | CPU 分支保持图形隔离，GPU 与渲染适配私有且可选 |

公开头文件位于各模块 `include/dk/`，实现位于 `src/`；实体 target 名使用 `dk_*`。
不为每个粒子创建 ECS entity，不把 solver 工作数组放入 SceneDocument。
现有入口见 [Physics API](physics-api.md)、[SimulationService](../../engine/framework/services/include/dk/services/SimulationService.hpp)
和 [worker](../../engine/framework/services/src/AsyncSimulation.hpp)。保留 XPBD 路径与旧协议行为，分阶段迁移公共输出，
不要求布料在首个流体阶段同时重写。

## 输入、物理量与输出

以下为语义草案，不作为已经公开的 C++ 或 JSON schema：

```text
FluidDefinition = solver + backend + material + initialRegion + boundary + timePolicy + seed
ParticleState   = stableId[] + position[] + velocity[] + mass[] + optional quantities[]
ParticleOutput  = runId + completedStep + simulatedTimeNs + generation + channels + owned storage
```

`solver` 与 `backend` 是独立枚举；solver 参数是 WCSPH/PBF 的有类型分支。
实验顶层区分 cloth/fluid，snapshot/metrics 同样按类型区分，不继续用 `bool gpu` 或强制返回 `XpbdSnapshot` 表示流体。
类型层不以任意键值表承载求解器核心数据；可选通道描述只服务输出/可视化。

所有物理量采用 SI：位置 m、时间 s（公开计时为整数 ns）、速度 m/s、质量 kg、密度 kg/m³、压力 Pa。
初始采样记录粒子间距、核尺度与实际支持半径；物理粒子尺度和屏幕点大小独立。
粒子 ID 在一次运行内稳定，排序仅改变存储顺序；导出和 CPU/GPU 对比按 ID 关联。
重排必须包含全部属性，不能只排列 position。初始种子、填充顺序和边界采样方案是输入来源的一部分。

| 通道 | 语义 / 可用性 |
| --- | --- |
| position、velocity、mass、stableId | 基本数据；长度和 ID 唯一性校验；首期质量恒定但输出仍明确质量语义 |
| density、densityError | 密度及相对参考密度偏差；记录采样步、有效范围及内部/自由表面统计分类 |
| pressure | WCSPH EOS 压力；PBF 默认没有压力输出，不能以 0 冒充有效值 |
| constraintMultiplier、constraintResidual | PBF 求解内部量；单位/归一化随离散定义记录，乘子不能直接标成 Pa |
| speed、其他显示派生量 | 从同一代有效数据计算，标明 derived 及单位；不得把旧代诊断附到新代位置上 |

CPU 输出拥有不可变数组，GPU 输出拥有缓冲资源与完成依赖，不能返回悬空 span 或裸借用 buffer。
GPU 视图带元素数、容量、布局/步幅、ID 对应、通道和 generation；公开 CPU 数据类型不包含 Vulkan。
求解器工作数组可复用，已发布输出在消费者退休前不可被覆盖；旧代保留数量必须受预算约束。
逐粒子可选通道缺失时显示“不可用”，不能静默用另一物理量代替。

## 数值基础与邻域

两种 solver 共用邻域查询、初始采样、边界几何和单位约定，各自拥有工作数组与参数。
不强制两种算法使用相同压力模型、迭代结构或黏性参数。
CPU 邻域采用均匀格网；cell size 与核的实际支持半径对应，枚举范围覆盖整个支持球。
小规模独立 O(N²) 查询作为邻域 oracle，覆盖负坐标、网格边界、重复位置、空 cell 和拥挤 cell。

GPU 采用 cell key、排序/前缀和、cell ranges 和关联属性重排，定义溢出/越界检查和确定的 ID 次序。
首期不使用静默截断的定长邻居表。候选邻居过多或预算不足必须报告错误，不丢失作用项。
共享实现细节不能替代独立暴力参考；GPU 还需覆盖非 workgroup 整倍数及 sort padding。

CPU 实现阶段先固定并落档核函数、三维归一化、支持半径、self contribution、梯度和零距离规则，
再开始压力/约束推进。PBF 的密度核与约束梯度关系须明确；若采用原论文中不同核的组合，
单独记录近似，不能用不匹配的有限差分测试假证梯度正确。
边界同时解决密度支持不足与几何不穿透：采用静态边界样本及明确的有效质量/体积权重，
边界层厚度覆盖支持域，盒角/边缘权重有独立测试；仅把位置 clamp 到盒内不足以完成边界验收。
边界样本不属于活动液体粒子数，预算计入其存储与查询成本。

## WCSPH

子步顺序为邻域构建 → 密度 → EOS 压力 → 压力/黏性/重力 → 积分 → 边界修正与健康检查。
压力使用成对对称离散；内部粒子对的总内力须能由独立小系统核验。
CPU 阶段明确参考密度、数值声速、EOS（首选 Tait）及指数、负压策略、黏性离散及参数单位，
禁止将为稳定性调整的数值声速解释成真实水声速，或把人工黏性系数写成动力黏度。
积分器、边界速度响应及容差与测试输入一同固定，GPU 使用同一数值配置。

保留 [FixedStepClock](../../engine/physics/api/src/FixedStepClock.cpp) 的外部拍长/计数契约：一拍至少 1 ms。
流体内部使用整数 ns 子步，可小于 1 ms；以确定分配覆盖整拍，禁止舍入后漏算时间。
首期采用配置确定的子步数及稳定性校验，覆盖声速/速度、加速度和黏性限制；
不在显示帧内偷偷改变物理时长。若当前状态要求更小步长而超出配置/额度，终止并报告，
不吞掉物理时间继续宣称完成一拍。完整自适应步长另行验收。

## PBF

子步顺序为外力与预测位置 → 密度约束/乘子 → Jacobi 位置修正与边界投影（固定 K 轮）
→ 速度重建 → 可选速度修正与健康检查。约束以 `rho/rho0 - 1` 表示，
分母正则、粒子质量因子、梯度符号和边界项在 CPU 参考中写明，避免混用归一化约定。
每轮读取同一输入位置、写独立输出位置；不采用遍历顺序相关的原地修正。

首期每轮在当前预测位置上重建邻域，确保前一轮移动后进入支持域的邻居被包含。
后续如复用邻域，必须给出 skin/最大位移界限及失效重建验证。
速度从子步起点与最终位置重建；碰撞速度处理和约束修正先后固定。
`s_corr` 和 XSPH 独立开关，基础约束测试先关闭增强项；XSPH 不直接解释为物理黏度。
涡量增强不作为首期完成条件。PBF 允许更大步长不等于可忽略 CFL、碰撞或位移限制。

## GPU 调度、资源和失败边界

使用 Vulkan Compute、Slang 和现有 Graph；按粒子 gather 更新，首期避免依赖无序浮点 scatter 原子累加。
初始化上传一次，后续状态常驻 GPU；正常 query/预览无全量粒子读回。
数值健康可通过有界归约/小状态读回取得，但必须完成检查才发布为健康输出；
显式 sample/export 才请求完整数据。Graph 声明排序/归约/求解/vertex 读写及跨 pass 同步。

容量由粒子数、边界样本、临时排序/scan、邻域、迭代双缓冲、完成输出和在途资源的字节/工作量共同准入，
不继承 XPBD 的 1024 粒子上限，也不承诺无限规模。计算元素数/字节数先检查整数溢出。
一个 worker 持有求解器、队列与可变候选；owner 只接收有界进度和已完成结果，不执行长求解或同步回读。

| 操作或边界 | 成功行为 | 拒绝、取消或失败行为 |
| --- | --- | --- |
| prepare / 新运行 | 校验、额度及候选初始化成功后发布独立 run；可在零拍暂停 | 普通输入错误不替换旧状态；异步初始化失败发布 fault，取消不假称已有粒子结果 |
| 宏拍推进 | 内部子步总时长等于拍长，健康检查后发布 completedStep/time/generation | CPU 候选失败保留上次完成输出；GPU 提交前失败不增加提交数 |
| GPU 已提交 | 提交进度与完成进度分别记录，完成且健康后才发布输出 | 设备/等待/健康失败保留真实提交事实和旧完成输出，冻结 fault；不伪装 GPU 回滚 |
| pause / cancel | 请求受理与安全边界完成分开；到稳定边界后无新批次 | 已提交工作必须退休；取消不撤回已完成拍，不把部分子步算整拍 |
| stop / shutdown | 请求停止、排空、在所属线程释放，owner 最后移除 Play | 不持锁 join、不 detach；不承诺硬中断编译器/驱动 |
| preview / sample | 消费明确 run、完成拍、视图/通道代次 | 旧 run 拒绝；预览失败保留旧图，读回失败不发布伪造数值 |

首期沿用有限任务的“领取批次到安全完成”规则，控制在初始化安全点、宏拍/批次边界处理；
子步可拆成有界 GPU 提交，但公开宏拍只有整拍完成才增长。若中途终止，丢弃未发布候选并结束该运行，
不从不可确认的 GPU 工作状态恢复。调度需限制单批宏拍数、子步数和迭代数，分别测量请求延迟与退休延迟。
本设计不修改旧同步 start/step 的响应含义；流体交互优先使用现有有限任务通道。

## 服务、命令和实验文件

启动后物理配置对该 run 不可变；切换 solver/backend 或材料参数须建立新运行。
相机、色标和点大小属于显示设置，不改变物理时间或重建求解器。
沿用准备、单步、有限步运行、暂停/恢复、取消/停止、查询、显式采样和导出语义。
每个 solver 接入时同步注册/schema、能力发现、[命令参考](../commands/simulation.md)和 Python/Luau 用例；
未实现组合不出现在能力列表，不在本设计阶段提前登记可调用命令。

导出版本化配置、按稳定 ID 对齐的粒子数据、指标、图像及 provenance；manifest 记录单位、通道有效性、
solver/backend、seed、核/边界/时间配置、完成拍/时间、版本及数据摘要。
大数组采用有版本、元素类型/字节序/布局和长度校验的二进制文件，具体格式在输出阶段固定。
CPU-only 支持数值导出；图像依赖可选渲染能力，不能为数值导出强制链接 Vulkan。
沿用同父临时目录写齐后 rename 发布、已有目标保护和路径校验；文件失败不破坏旧文件或 EditWorld。
现有布料导出协议兼容。流体导出用于分析和从零重放，不是中途恢复检查点。

OpenUSD 适合作为后续场景/时序交换层：粒子可映射 UsdGeomPoints，场可通过 UsdVol 引用 OpenVDB。
它不替代引擎运行时数组、求解器状态或完整物理协议；MAC 还需显式保存交错位置、单位和边界约定。
选型依据与限制见 [调查报告](../research/2026-10-23-water-simulation.md)，本期不修改依赖清单或建立 USD schema。

## 验证计划与实施门槛

阶段依赖和交付状态仅维护于 [Roadmap](../roadmap.md)；CPU 求解器阶段均带服务和最低可用点预览，
完整四组合的 GUI/脚本/导出闭环在最后阶段验收。

| 类别 | 可证伪的验收条件 |
| --- | --- |
| 公共基础 | 核归一化/梯度、self/重合点、暴力邻域集合、ID/全部通道重排、边界权重、容量溢出/拒绝 |
| WCSPH | 成对内力、静水趋势/溃坝、近壁与自由表面分开诊断、质量/穿透、稳定子步失败保护 |
| PBF | 小系统约束和单轮修正独立对照、迭代残差趋势、移动后新邻居、速度重建、增强项独立开关 |
| CPU/GPU | 固定 seed/子步/迭代，短期逐 ID 比较位置/速度/密度，长期比较统计量；不要求跨 GPU 位相永远一致 |
| 生命周期 | 精确 N 拍/整数时间、提交与完成差异、初始化/推进取消、过期 run、健康故障冻结、旧代仍可读 |
| 交付 | CPU-only 数值运行/导出、四组合、真实后台 Editor、无全量读回预览、文件失败/关闭/XPBD 定向回归 |

M21.1 固定验证方法；M21.4/5 在 CPU 小规模参考中记录实际场景参数、参考输出及按粒子间距/rho0/速度尺度
归一化的容差后，才允许 GPU 对比与优化。不能用“看起来像水”或“不发散”代替数值验收，
也不能在性能优化后放宽已冻结阈值。自由表面密度亏损不与内部密度误差混为一个指标。
功能场景先约 4k 粒子，之后测量 16k/64k；超额如实报告，不预设实时规模。
分开报告 CPU 求解、GPU 邻域/求解、上传/读回、点绘制、初始化、控制响应和全部旧代峰值资源。

未决实现参数限于 CPU 离散基线、边界权重和实际容差、二进制布局及可测容量；在对应阶段动手前补入本页和记录，
不改变本期算法/后端/原生点绘制范围。新增测试 target 时同步选择表，自动化 Editor 使用 `--background`。

## 相关依据与记录

- [水体调查](../research/2026-10-23-water-simulation.md)、[原始首期计划](../research/2026-10-23-sph-pbf-plan.md)、[设计落档记录](../development/0147-particle-fluid-plan.md)。
- [SPH 原论文](https://matthias-research.github.io/pages/publications/sca03.pdf)、[WCSPH](https://cg.informatik.uni-freiburg.de/publications/2007_SCA_SPH.pdf)、[PBF 原论文](https://mmacklin.com/pbf_sig_preprint.pdf)。
- [整体架构](architecture.md)、[Physics API](physics-api.md)、[粒子点绘制](render-particles.md)、[Graph](graphics-graph.md)。
