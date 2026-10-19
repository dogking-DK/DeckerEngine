---
module: roadmap
created_at: "2026-09-22T09:29:25+08:00"
updated_at: "2026-10-19T14:18:00+08:00"
status: accepted
---

# DeckerEngine 开发 Roadmap

## 目标与当前起点

以可独立运行、可由 AI 调用的引擎内核为主线，逐步交付场景编辑、
Vulkan/Slang 渲染与物理实验能力。工程名为 DeckerEngine，命名空间为 dk。

初始工程基线为 `376b288`（工程初始化提交），路线建立于其后。
已有多层 CMake、vcpkg 清单、目录骨架与 spec/skill 流程；
现已完成 Core 错误/结果、日志和强类型稳定 ID。
工程初始化见 [0001](development/0001-project-bootstrap.md)，
Core 初始实现及 Debug/Release 验证见 [0002](development/0002-foundation-core.md)，
稳定 ID 已迁移到 stduuid，回归验证见 [0003](development/0003-stduuid-migration.md)。
Eigen 基础数学 M1.2 已完成，见 [0004](development/0004-eigen-math-foundation.md)；
M1.3 Transform 已完成，见 [0005](development/0005-transform.md)；
M1.4 工程路径与文件 IO 已完成，见 [0006](development/0006-foundation-io.md)。
M1.5 Windows 安全保存已完成，见 [0007](development/0007-atomic-file-save.md)。
M1.6 CPU 集成验收已完成，见 [0008](development/0008-foundation-integration.md)，原 M1.1–M1.6 全部完成。
M2.1–M2.4 已完成：flecs 文档、组件层级、工程/资产引用和 JSON 安全持久化；
最终验收见 [0012](development/0012-scene-persistence.md)。
M3.1–M3.5 已完成：统一命令、场景服务、内存事务/撤销重做、CPU CLI/stdio 和同步任务，
交付 A 最终验收见 [0017](development/0017-stdio-delivery-a.md)。
新增 M1.7 Memory/Tracy 基础设施补充，设计见 [0022](development/0022-memory-profiling-design.md)，
M1.7.1–7 已完成，见 [0023](development/0023-tracy-cpu-profiling.md)、[0024](development/0024-mimalloc-heap.md)、
[0025](development/0025-memory-ownership-routing.md)、[0026](development/0026-scratch-arena.md)、
[0027](development/0027-memory-pools.md)、[0029](development/0029-memory-context-routing.md)、[0030](development/0030-memory-baseline.md)。
原 M1–M3 验收不包含这项新增能力。该补充及 M4.1–4 均已完成。
M5.1 设备与诊断、M5.2 资源与提交、M5.3 Slang 编译、M5.4 离屏绘制与计算已完成，见 [0042](development/0042-vulkan-device.md)、
[0045](development/0045-graphics-resources-submission.md)、[0046](development/0046-slang-shader-compiler.md)、
[0047](development/0047-offscreen-execution.md)。M5.5.1–5 使用层封装及离屏迁移已完成，见
[0053](development/0053-vulkan-offscreen-migration.md)。M5.6 窗口与呈现完成，见
[0056](development/0056-presentation-recovery.md)；M5 已全部验收。M6.1 图声明与结构校验完成，见
[0057](development/0057-graph-declarations.md)。M6.2 依赖编译与资源生命周期完成，见
[0058](development/0058-graph-compilation.md)。M6.3 单队列同步与执行完成，见
[0059](development/0059-graph-execution.md)。M6.4 样例迁移与诊断完成，见
[0060](development/0060-graph-integration.md)；M6 已全部验收。M7.1 场景提取与 GPU 资源已完成，见
[0061](development/0061-render-data-resources.md)；M7.2 最小渲染管线完成，见
[0062](development/0062-render-pipeline.md)。M7.3 磁盘场景与资产集成完成，见
[0063](development/0063-render-disk.md)。M7.4 截图任务与自动化验收完成，见
[0064](development/0064-render-capture.md)；M7 与交付 B 已验收。M8.1 编辑器工作台已完成，见 [0065](development/0065-editor-workbench.md)；M8.2 拾取、Gizmo 与相机已完成，见 [0066](development/0066-editor-interaction.md)；M8.3 IPC 与 dk-ctl 已完成，见 [0067](development/0067-ipc-client.md)；M8.4 跨入口一致性完成，见 [0068](development/0068-editor-consistency.md)。M8 与交付 C 已验收；M9.1 Luau 命令绑定已完成，见 [0070](development/0070-luau-command-bindings.md)，M9.2 执行限制与取消已完成，见 [0071](development/0071-luau-execution-limits.md)，M9.3 Python 客户端已完成，见 [0072](development/0072-python-automation.md)，M9.4 记录/重放已完成，见 [0073](development/0073-automation-replay.md)；M9 已验收；M10.1 模拟世界与固定步长已完成，见 [0074](development/0074-simulation-world.md)，M10.2 CPU XPBD参考求解器已完成，见 [0075](development/0075-cpu-xpbd.md)，M10.3 GPU求解与可视化已完成，见 [0076](development/0076-gpu-xpbd.md)，M10.4 脚本化实验已完成，见 [0077](development/0077-simulation-experiments.md)，M10 全部验收。

默认先交付 Windows x64；CPU-only 构建始终保留。
优先正确性、可观测性和可复现操作；提早建立多线程内存契约与性能测量，复杂并行优化仍由测量驱动。
以下按交付依赖排序；没有人员和工期预算，因此不指定日历日期。
里程碑编号表示路线顺序，不是开发文档编号，也不是版本号。

本文件负责阶段目标和顺序，具体接口由实施前的模块设计确定。
`status: accepted` 表示当前采用这份规划；各阶段是否完成以状态列和验收证据为准。

## 里程碑总览

| 阶段 | 交付目标 | 必要前置 | 状态 |
| --- | --- | --- | --- |
| M0 工程骨架 | 可配置、编译和测试的基础工程；spec 流程 | 无 | 已完成 |
| M1 Foundation 最小基础 | 错误、日志、稳定 ID、数学约定、文件 IO | M0 | 已完成，M1.1–M1.6 均验收；Windows 本地文件范围 |
| M2 场景文档 | 实体、变换、层级、资产引用、保存与重载 | M1 | 已完成（M2.1–M2.4） |
| M3 命令与 CPU Runtime | CLI/stdio 创建、修改、查询、保存场景 | M2 | 已完成（M3.1–M3.5，交付 A） |
| M1.7 Foundation 补充 | Tracy、mimalloc heap、PMR/智能指针、arena/pool、多线程生命周期与性能基线 | 原 M1、M3（现有链路埋点） | 已完成：M1.7.1–7 均验收 |
| M4 资产加载链路 | 导入、缓存、加载状态、异步任务 | M2、M3、M1.7 | 已完成：M4.1–4，证据 0031–0040 |
| M5 Vulkan / Slang 底座 | 设备资源、shader 编译、离屏输出、Vulkan 使用层、最小呈现 | M1、M1.7 | 已完成：M5.1–6 均验收，见 0042–0056 |
| M6 GPU Graph | 资源声明、依赖编译、同步、执行与诊断 | M5 | 已完成（M6.1–4） |
| M7 场景渲染 | 资产上传、场景提取、Pass、pipeline、可等待截图 | M3、M4、M6 | 已完成（M7.1–4，交付 B） |
| M8 编辑器与进程控制 | 可编辑保存的视口；dk-ctl 操作运行中的程序 | M7 | 已完成（M8.1–4，交付 C），见 0065–0068 |
| M9 脚本与自动化 SDK | Luau 场景脚本、Python 客户端、批处理与重放 | M3；集成验收需要 M8 | 已完成（M9.1–4） |
| M10 物理实验闭环 | 固定步长、CPU 参照、首个 GPU 求解器及可视化 | M7、M9 | 已完成 |
| M11 模拟运行响应性与性能基线 | GPU 性能观测、可复现基准、有界模拟任务与暂停/取消 | M1.7、M8、M10 | 已完成，见0078–0081 |
| M12 编辑器模拟工作台 | 参数配置、运行控制、实时模拟视口、指标与导出 | M8、M11 | 已完成，见0082–0085 |
| M13 实验管理与参数扫描 | 批量实验、产物索引、失败恢复与对比报告 | M9、M12 | 已完成，见0089–0092 |
| M14 大气渲染 | 物理天空、太阳漫反射、空气透视及编辑器/截图一致性 | M7、M8/M9、M11 | 已完成，见0094–0100；保留性能目标实测差距 |
| M15 体积云 | 可复现云场、光照/合成、时域重建、云影及质量性能验收 | M14；自动化复用M9/M13原则 | 已完成质量/性能基线；非零云GPU与在途峰值候选未达 |
| M16 渐进式路径追踪 | CPU参考、Vulkan RT表面积分、每视图累积、编辑器与固定spp截图 | M5/M6/M7；集成需M8/M9，观测复用M11 | 已完成质量与基线交付；CPU参考、RT/GGX、渐进视口和固定capture，见[0120](development/0120-pathtracing-delivery.md)；实时性能候选未达 |
| M18 相机模式与投影 | Orbit/Fly/Walk、可调镜头/移速、分层正交与截图一致性 | M8/M12；路径与环境需M14–M17 | 已完成 M18.1–5；Fly/Walk、基础/路径/光栅环境正交及捕获，见[0133](development/0133-raster-orthographic-environment.md) |
| M19 ReSTIR 直接光重采样 | 首表面 DI、时空复用、Scene media 适配与 Editor/capture、固定质量测量 | M16–M18 | M19.1/2 已完成，其余未开始；见[设计](design/render-restir.md) |

默认执行表中顺序。M5 在 Foundation 补充完成后具备独立探索条件，
M9 的 Luau 命令绑定在 M3 后可提前做；若调整次序，仍需满足对应集成验收依赖。
本路线不要求同时开展多个模块。

首轮的三个可单独交付节点：

- **A：M3 完成，已验收。** 无窗口、无 GPU，通过结构化命令生成场景并保存、重载。
- **B：M7 完成。** 从磁盘资产和场景得到可核验的离屏图像，AI 可以等待任务并检查结果。
- **C：M8 完成，已验收。** 编辑器和外部 CLI 操作同一套场景服务，保存后可在 runner 重现指定视图画面，见 [0068](development/0068-editor-consistency.md)。

M9、M10 已将这套基础扩展为可脚本化的渲染与物理实验平台。
M11测量/响应性、M12交互工作台与M13实验管理，以及M14大气/M15体积云质量与性能基线均已交付；性能候选差距见对应报告。
本次仅完成规划，没有新增渲染实现或GPU能力，见[0093](development/0093-atmosphere-cloud-plan.md)。

## 小阶段执行规则

每个里程碑拆为 Mx.y；小阶段编号用于范围和验收，开发记录仍全局连续编号。
表中“前置”表示必须先通过的验收；未来小阶段仅定义边界，开工时再写详细模块设计。
默认同一里程碑按表中顺序实施，不自动并行开展。

用户未指定范围的“继续/下一步开发”默认只推进首个满足前置的未完成小阶段。
明确要求多个阶段时按授权范围推进。每次开工说明子阶段编号、产物和验收；
完成后更新状态、证据和下一项，不因一个子阶段完成就关闭整个里程碑。
若阶段仍过大，在实施前进一步细分并保留已有编号，避免临时扩展任务范围。

## M0：工程骨架（已完成）

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M0.1 | Git、目录和技术留档流程 | 无 | 仓库、spec 模板及 skill 可用；[0001](development/0001-project-bootstrap.md) | 已完成 |
| M0.2 | CMake/vcpkg 与构建探针 | M0.1 | 配置、编译、版本测试通过；[0001](development/0001-project-bootstrap.md)，依赖增量见后续记录 | 已完成 |

## M1：Foundation 最小基础

**目标：** 为 Scene 和命令层提供稳定、轻量的公共能力。

交付范围：

- 在 core 中建立错误码/错误上下文与结果返回约定；以 C++23 标准设施为基础，
  明确可恢复错误、边界异常转换和断言的区别。
- 接入 fmt/spdlog，按模块区分日志；默认写 stderr，支持文件输出和初始化/退出生命周期。
- 建立可序列化、可解析的稳定 ID，区分 EntityId、AssetId 等类型，
  不把运行时 ECS 句柄或地址当持久身份。
- 定义单位、坐标系、角度、矩阵/四元数和变换组合约定；实现当前需要的数学能力。
- 最小文件读写和原子替换工具，保留 Unicode 路径、错误和工程相对路径语义。
  任务系统与元数据在后续实际需要时添加。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M1.1 | Core 错误、日志、稳定 ID | M0 | 错误传播、ID 往返和日志分流；[0002](development/0002-foundation-core.md)、[0003](development/0003-stduuid-migration.md) | 已完成 |
| M1.2 | Eigen 类型、单位/坐标、角度与基础旋转 | M1.1 | Debug/Release 各 36 项通过；独立数学配置 28 项通过；[0004](development/0004-eigen-math-foundation.md) | 已完成 |
| M1.3 | Transform TRS、组合与逆变换 | M1.2 | 剪切/镜像/奇异与溢出验证；Debug/Release 各 60 项、独立数学 52 项通过；[0005](development/0005-transform.md) | 已完成 |
| M1.4 | 工程路径与文件读写 | M1.1 | Unicode/相对路径、二进制及错误边界；Debug/Release 各 73 项、独立 IO 23 项通过；[0006](development/0006-foundation-io.md) | 已完成 |
| M1.5 | 同目录临时文件与安全替换 | M1.4 | Windows NTFS 故障保护/清理；Debug/Release 各 85 通过、1 跳过，独立 IO 35 通过、1 跳过；[0007](development/0007-atomic-file-save.md) | 已完成 |
| M1.6 | Foundation 集成验收 | M1.3、M1.5 | 默认 Debug/Release 各 100 通过、1 权限跳过；无日志/runner/Catch2 的 CPU 示例各 15/15；[0008](development/0008-foundation-integration.md) | 已完成 |

**先写设计：** `foundation-core.md`、`foundation-math.md`、
`foundation-io.md`。可以逐份设计、逐项开发，不要求同时完成三份。

**验收：** 结果错误可传播；ID 文本往返且不同 ID 类型不能混用；
日志不进入协议 stdout；变换约定有验证案例；
写入/替换失败时保留旧文件。CPU 构建不引入 SDL/Vulkan。
为这些行为接入 Catch2/CTest；确保 Release 下断言关闭也能执行检查。

**暂缓：** 自研通用容器、完整反射、无锁任务系统、插件框架。
原“自定义分配器”暂缓项改为以下明确的 M1.7；不自研替代 mimalloc 的通用 heap 算法。

## M1.7：Memory 与性能分析补充

**目标：** 在资产/异步链路扩展前，建立可注入、多实例、多线程安全的 CPU 内存系统，
并从首个小节提供真实可采集的 Tracy CPU 性能分析。此为新增基础设施，原 M1.1–M1.6 不重编号。

设计：[Memory System](design/foundation-memory.md)、[Tracy 接入](design/foundation-profiling.md)。
选型为 mimalloc v3 和 Tracy，版本默认跟随 vcpkg 最新收录并固定验证基线。
M1.7.1–7 已验收，证据见 [0023](development/0023-tracy-cpu-profiling.md)、[0024](development/0024-mimalloc-heap.md)、
[0025](development/0025-memory-ownership-routing.md)、[0026](development/0026-scratch-arena.md)、
[0027](development/0027-memory-pools.md)、[0029](development/0029-memory-context-routing.md)、[0030](development/0030-memory-baseline.md)。

| 小节 | 范围 | 前置 | 独立验收 | 状态 |
| --- | --- | --- | --- | --- |
| M1.7.1 | dk::profiling、Tracy/vcpkg 开关、CPU zone/线程名、runner/IO 局部埋点 | 原 M1、M3 | OFF 无 Tracy 依赖与参数求值；ON 读回 134 区间/2 线程 capture；无 viewer 正常退出、stdout 兼容，见 0023 | 已完成 |
| M1.7.2 | dk::memory、mimalloc v3、域/ResourceHandle、对齐 heap、预算和关闭闸门、Tracy backing 事件 | M1.7.1 | 15 个 heap 用例通过；真实 capture 36 次全部配对、2 次跨线程释放，内存关闭为 0 事件，见 0024 | 已完成 |
| M1.7.3 | PMR、拥有型 Allocator/Buffer、容器别名、隐式/显式智能指针与持久域作用域 | M1.7.2 | 新增 17 个拥有型/路由用例；原 heap 与 probe 回归、完整 README 示例通过，见 0025 | 已完成 |
| M1.7.4 | ScratchArena、自动取得线程 scratch 的嵌套 ScratchScope、保留上限与用量曲线 | M1.7.3 | 15 个新用例及双线程 probe；真实 Tracy 4 曲线/3 chunk 配对、内存禁用采集通过，见 0026 | 已完成 |
| M1.7.5 | LocalPool/SharedPool、ObjectPool、受控 trim | M1.7.4 | 16 个 Pool 用例与双线程 probe；构造/在途竞争、weak/busy trim；Tracy ON/内存 OFF 采集通过，见 0027 | 已完成 |
| M1.7.6 | ThreadContext、拥有型任务路由/线程复用探针、多系统关闭与延迟释放 | M1.7.5 | 14 个新用例及双系统 worker probe；缓存退休/busy 重试、异常/取消恢复；Tracy 11 次配对及内存禁用采集，见 0029 | 已完成 |
| M1.7.7 | 代表性重复工作负载、定向集成与性能基线报告 | M1.7.6 | 88 场景 × 三配置 × 三次运行；checksum/零存活、CPU/Memory trace 读回与完整峰值/保留量/开销表，见 0030 | 已完成 |

每次默认只推进一个 M1.7.x。资源生命周期和并发闸门从 M1.7.2 开始保证，M1.7.6 验证组合场景，
不是延后处理 heap 正确性。先用标准线程完成探针，不依赖尚未实现的 M4 Jobs，避免循环前置。
Tracy 逐对象 arena 追踪、自研无锁 pool、全局 new/delete 替换、GPU profiling 不纳入该补充验收。
M4 新数据采用持久资源、worker 临时数据采用 scratch；M5–M7 再接 GPU zone 和 frame-slot 完成信号。
日常 API 自动取得框架绑定的上下文，持久与临时使用意图仍显式区分；真正 Jobs 的自动路由接入在 M4.4，
M1.7.6 先独立验收 token/恢复规则，不将尚未实现的调度器当作已有能力。

## M2：场景文档与持久化

**目标：** 建立能够编辑、保存、重载的 CPU 场景状态。

交付范围：

- 接入 flecs；SceneDocument 管理场景身份、revision、dirty 状态和 ECS 工作表示。
- 首批组件为名称、持久 ID、Transform、Hierarchy。
  定义组件的持久化名称/版本和最小显式属性描述，支撑后续命令校验与 Inspector。
- 实现实体创建/删除、父子关系、局部/世界变换传播；
  禁止循环父子关系，明确父节点删除策略。
- 定义最小工程描述、资产引用类型与稳定 AssetId；此时只解析引用，
  完整资产导入和加载留到 M4。
- 场景 JSON 有格式/组件版本；分两遍加载实体与引用。
  保存采用一致快照、临时文件验证及原子替换。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M2.1 | flecs 文档与实体身份 | M1 | 创建/删除实体、持久 ID 去重，所有权和失败回滚；[0009](development/0009-scene-identity.md) | 已完成 |
| M2.2 | 组件与变换层级 | M2.1 | 名称/Transform/Hierarchy、父子传播、循环与删除；[0010](development/0010-scene-hierarchy.md) | 已完成 |
| M2.3 | 工程格式与资产引用 | M2.2 | 格式版本、引用和缺失诊断；[0011](development/0011-project-assets.md) | 已完成 |
| M2.4 | 序列化与安全重载 | M2.3 | 两遍恢复、快照/dirty、临时文件验证、失败保护与进程往返；[0012](development/0012-scene-persistence.md) | 已完成 |

**先写设计：** `assets-types.md`、`project-format.md`、`scene.md`。
在 scene 设计中明确编辑态与未来运行态的所有权，不提前实现整个模拟系统。

**验收：** 含父子关系和资产引用的场景保存后重载，持久 ID、组件值和关系保持；
旧文件在保存失败时可用；拒绝重复 ID、非法层级和不支持的版本。
加载失败不留下半个新场景；保存旧 revision 不清掉后续修改的 dirty 标记。

## M3：命令、服务和 CPU Runtime

**目标：** 达到交付节点 A，让 AI 在无 GUI/GPU 条件下操作场景。

交付范围：

- Commands 负责注册、参数校验、结果/错误；Application Services 负责业务行为；
  Operations 连接二者；Runtime 负责装配、退出和状态修改安全点。
- 初始命令覆盖能力发现、命令描述、场景新建/加载/保存/查询、
  实体创建/删除、属性/变换修改、重新设父级。
- 编辑操作支持 revision 检查；建立首批可撤销命令和事务边界。
  保存文件等外部副作用不假定可撤销。
- 将 dk-run 扩展为 CPU 批处理与持续 stdio 服务入口，
  使用 JSON-RPC 请求/结果和稳定退出码；协议 stdout 与日志 stderr 分离。
- 定义任务 ID 和状态协议；当前同步操作同步返回，
  M4/M7 引入真实异步任务时再实现完整等待/取消语义。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M3.1 | 命令注册与能力发现 | M2 | 参数/结果 schema、错误和未知命令有可执行验证 | 已完成，见 [0013](development/0013-command-registry.md) |
| M3.2 | 场景服务与编辑操作 | M3.1 | 命令完成实体和层级编辑，过期 revision 被拒绝 | 已完成，见 [0014](development/0014-scene-services.md) |
| M3.3 | 事务与撤销重做 | M3.2 | 一组内存编辑原子提交或回滚，撤销/重做保持状态 | 已完成，见 [0015](development/0015-transactions-history.md) |
| M3.4 | CPU Runtime 与 CLI 批处理 | M3.3 | 无窗口进程可创建、保存、重启并查询场景 | 已完成，见 [0016](development/0016-cpu-runtime-cli.md) |
| M3.5 | 持续 stdio 协议 | M3.4 | JSON-RPC 请求/错误/退出码、日志分离及同步任务状态；交付 A | 已完成，见 [0017](development/0017-stdio-delivery-a.md) |

**先写设计：** `commands.md`、`application-services.md`、
`runtime.md`、`automation-protocol.md`。

**验收：** 从空进程开始，通过命令创建父子实体、修改变换、保存，
重启进程后加载并查询相同的持久状态；全程不加载窗口或 GPU 模块。
错误参数、未知命令、过期 revision 返回结构化错误。
场景编辑事务可撤销/重做；stdout 可逐条解析。
这里使用命令序列驱动，不要求 Luau 提前完成。

## M4：资产导入与加载

**目标：** 把磁盘内容变成可由渲染/物理消费的 CPU 资产。

交付范围：

- 首版限定静态 glTF/GLB 的明确子集，每源一个 mesh、可含多个 primitive；
  支持基础材质和 PNG/JPEG base color 纹理，覆盖与拒绝范围见导入器设计。
- .meta 保存 AssetId、导入设置和导入器版本；区分源资产、导入产物和可重建缓存。
- 建立依赖追踪、Unloaded/Loading/Ready/Failed 状态、失败诊断；
  引入有界 CPU 工作队列、JobId、等待、协作取消和退出回收，保留 M3 同步 TaskId 语义。
- dk-assetc 执行离线导入；命令层提供导入和加载状态查询。
  重命名首版限同目录保留扩展名，同步元数据与工程清单；外部破坏元数据时给出诊断。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M4.1 | 资产身份与导入契约（M4.1.1–2） | M3、M1.7 | meta/目录、登记提交/改名、Project 替换、多文件补偿及重启恢复通过，见 0031、0032 | 已完成 |
| M4.2 | 首个网格导入与 assetc（M4.2.1–2） | M4.1 | CPU 网格/材质/纹理、产物 v1、assetc 真进程及失败保护通过，见 0033、0034 | 已完成 |
| M4.3 | 缓存和依赖失效（M4.3.1–2） | M4.2 | 内容键/命中/current、失效/损坏/删除重建与有界清理通过，见 0035、0036 | 已完成 |
| M4.4 | 异步加载与任务生命周期（M4.4.1–4） | M4.3 | 队列、资产状态、33 条命令、CPU 进程闭环及退出通过，见 0037–0040 | 已完成 |

**设计稿已建立：** [资产身份/缓存/加载](design/assets-runtime.md)、
[导入器/assetc](design/assets-importers.md)、[任务队列](design/foundation-jobs.md)。
详细的 10 个实施小节、独立验收和开工决策见
[0020 M4 开发计划](development/0020-m4-development-plan.md)。**M4.1 已完成**，见 [0031](development/0031-asset-metadata-catalog.md)
及 [0032](development/0032-asset-commit-recovery.md)；M4.2 验收见 [0033](development/0033-cpu-mesh-import.md)、
[0034](development/0034-textures-assetc.md)，M4.3 验收见 [0035](development/0035-asset-cache-publication.md)、[0036](development/0036-asset-cache-invalidation.md)；M4.4 验收见 [0040](development/0040-cpu-assets-delivery.md)，M4 完成。后续设备验收见 M5.1。
后续按 M4.x.y 顺序，每节记录实际证据后再关闭父阶段。设计稿不代表代码已实现。
导入/加载使用 Memory 拥有型结果和局部 scratch，接入 Tracy 工作阶段与内存曲线；M4.4 仍先一个 worker。
三方库已确定为 fastgltf、stb_image（stb port）、xxHash（XXH3-128），当前基线版本与封装约定见
[导入器设计](design/assets-importers.md#已确定的三方库与基线)。对应模块实施时接入 vcpkg/CMake 并记录实际链接验证。

**验收：** 同一资产重复导入 ID 保持；合法重命名后引用仍有效；
修改源内容/参数使缓存失效，删除缓存后能重建。
缺失文件、错误纹理和取消任务有终态，不造成退出挂起。
CPU Ready 与未来 GPU Ready 分开。

## M5：Vulkan、窗口与 Slang 底座

**目标：** 证明设备、资源、shader 与读回链路正确，并形成窗口与 Graph 可复用的 Vulkan 使用层。

交付范围：

- Vulkan instance/device/queue、能力检测、错误报告与验证层接入。
- Buffer/Image、VMA、命令提交、完成跟踪、延迟释放；
  首版一个支持所需操作的队列，按完成状态复用和销毁资源。
- Slang 编译、入口/参数约定、诊断、SPIR-V 产物与最小布局反射；
  dk-shaderc 先提供可重复的离线编译入口。
- 封装 view/sampler、shader/pipeline/layout、描述符绑定、draw/dispatch 录制和 staging/读回；
  统一 GPU 对象保活与访问状态，常规调用无需手工组装 Vulkan 创建结构或保留 pending 对象集合。
- 离屏资源路径保持不依赖窗口；单独接入 SDL3 与 swapchain，
  覆盖 resize、最小化和重建。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M5.1 | Vulkan 设备与诊断 | M1、M1.7 | 能力/队列选择、验证消息、缺失设备和失败清理通过；RTX 4070 重复创建/销毁零验证错误，见 [0042](development/0042-vulkan-device.md) | 已完成 |
| M5.2 | 资源与提交生命周期 | M5.1 | VMA Buffer/Image、5 种格式上传/读回、timeline/槽复用、延迟释放和失败保护通过；Tracy GPU 寿命边界已定义，见 [0045](development/0045-graphics-resources-submission.md) | 已完成 |
| M5.3 | Slang 编译工具 | M5.2 | vertex/fragment/compute SPIR-V、反射、诊断和 dk-shaderc 离线发布通过；独立无 Vulkan 配置，见 [0046](development/0046-slang-shader-compiler.md) | 已完成 |
| M5.4 | 离屏绘制与计算 | M5.3 | 三角形逐像素/compute 数据读回、RAII pending 寿命和失败恢复通过；同步验证零警告/错误，见 [0047](development/0047-offscreen-execution.md) | 已完成 |
| M5.5 | Vulkan 使用层封装（M5.5.1–5） | M5.4 | 对象/管线/绑定/encoder/传输封装；离屏迁移输出不变、同步与寿命验收；见[设计](design/graphics-vulkan.md)与[0053](development/0053-vulkan-offscreen-migration.md) | 已完成 |
| M5.6 | 窗口与呈现（M5.6.1–3） | M5.5 | SDL3 swapchain、resize/最小化/重建；使用层复用与离屏/CPU 隔离通过，见 [0056](development/0056-presentation-recovery.md) | 已完成 |

M5.6 实施拆分：

| 子阶段 | 范围 | 前置 | 独立验收 | 状态 |
| --- | --- | --- | --- | --- |
| M5.6.1 | SDL3 窗口与 Surface-aware 设备 | M5.5 | 事件/像素尺寸/窗口保活，present family 与能力选择；18 项检查通过，见 [0054](development/0054-window-device.md) | 已完成 |
| M5.6.2 | 交换链、外部图像与帧同步 | M5.6.1 | 144 帧及读回、外部图像寿命、放弃帧与同步验证；见 [0055](development/0055-swapchain-frames.md) | 已完成 |
| M5.6.3 | 恢复与集成验收 | M5.6.2 | resize/minimize/重建/失败、重复关闭、离屏与 CPU 隔离通过；见 [0056](development/0056-presentation-recovery.md) | 已完成 |

M5.5 实施拆分：

| 子阶段 | 范围 | 前置 | 独立验收 | 状态 |
| --- | --- | --- | --- | --- |
| M5.5.1 | 统一对象工厂、设备寿命与 shader 产物类型提取 | M5.4 | view/sampler/ShaderModule 创建、移动与失败清理；普通对象独立保活设备；device 不链接 Slang，见 [0049](development/0049-vulkan-object-foundation.md) | 已完成 |
| M5.5.2 | 管线布局、graphics/compute 管线与不可变绑定 | M5.5.1 | 多 set/固定数组、跨阶段反射合并、管线复用、纹理/各类 buffer 绑定和冲突拒绝，见 [0050](development/0050-vulkan-pipelines-bindings.md) | 已完成 |
| M5.5.3 | 类型化录制、资源状态与显式同步 | M5.5.2 | draw/indexed draw/dispatch、depth、多 mip/layer、依赖 barrier 与失败回滚；Graph 计划路径可复用，见 [0051](development/0051-vulkan-command-encoding.md) | 已完成 |
| M5.5.4 | 批量上传与异步读回 | M5.5.3 | 多操作单次提交、区域 copy、超时/放弃/队列关闭及 host 可见性；完成前保活 staging，见 [0052](development/0052-vulkan-transfers-readback.md) | 已完成 |
| M5.5.5 | 离屏迁移、调用示例与集成验收 | M5.5.4 | 原有输出/失败行为回归，常规消费者不直接创建/录制 Vulkan 对象；同步验证零相关错误，见 [0053](development/0053-vulkan-offscreen-migration.md) | 已完成 |

M5.5.1–5 已完成，实现证据见上表。M5.1–4 的编号与验收保持；
原待开始的 M5.5 窗口与呈现改为 M5.6，历史记录 0047/0048 中的“下一项 M5.5”保留当时含义，
当前执行顺序以此表为准。每个 M5.5.x 独立实现、定向验证和留档后再推进下一节。

**当前设计：** [graphics-device](design/graphics-device.md) 覆盖 M5.1，[graphics-resources](design/graphics-resources.md) 覆盖 M5.2，
[graphics-shaders](design/graphics-shaders.md) 覆盖 M5.3，[graphics-offscreen](design/graphics-offscreen.md) 覆盖 M5.4。
[graphics-vulkan](design/graphics-vulkan.md) 是使用层契约，对象/管线/绑定/录制/传输及离屏迁移均已验收。
volk、vk-bootstrap 和 VMA allocator 接入见 [0043](development/0043-vulkan-libraries.md)；
Vulkan-Hpp RAII 所有权与消费接口见 [0044](development/0044-vulkan-hpp-raii.md)。
M5.2 的拥有型资源、提交、完成跟踪和延迟释放已通过专门验收；Tracy GPU query/context 寿命约定已明确，
实际 GPU timestamp zone/capture 尚未启用。M5.3 离线编译和布局反射、M5.4 真实绘制/计算读回已验收；
窗口 M5.6 设计见 [platform](design/platform.md) 与 [graphics-presentation](design/graphics-presentation.md)。

**验收：** 编译一个图形 shader 和一个 compute shader；
离屏生成小图像并读回、计算已知数据并校验；窗口路径能呈现和调整尺寸。
记录 GPU/驱动、shader 编译参数和验证层结果；
重复创建、提交、回收不出现同步/生命周期错误。
M5.5 需使常规离屏消费路径通过封装完成对象创建、绑定、录制和保活，
保留原图像/计算回归；验证多次复用与 pending 期间提前释放用户包装的安全性。
缺少必需 GPU 能力时清晰失败，CPU runner 仍可运行。

底层设备/互操作测试可直接调用 Vulkan；M5.5 迁移后的常规使用路径通过使用层录制。
M6/M7 后正式业务统一进入 Graph，Graph 复用使用层的对象、encoder、显式 barrier 和提交，
负责跨 Pass 调度；不重新建设一套 Vulkan 录制或对象寿命系统。
shader 热重载、跨平台交叉编译、多 GPU 暂缓。

## M6：GPU Graph

**目标：** 用一个调度核心组织渲染、计算、上传和读回。

交付范围：

- 声明 transient/external 资源和 Pass 读写；
  表达 stage、access、layout、子资源范围及内容保留需求。
- 编译依赖顺序、验证无效访问/循环、裁剪无输出且无副作用的工作；
  计算生命周期并规划单队列 barrier。
- 执行图、导入/导出资源状态、跟踪执行完成；
  提供 Pass/资源/依赖和同步计划诊断。
- 使用 M5.5 的资源/管线/绑定、encoder、显式 barrier 与提交接口；
  Graph 规划依赖，使用层执行并校验状态，两层共享资源状态边界。
- 持久资源由外部所有者管理并导入 Graph，禁止保存已失效的临时图句柄。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M6.1 | 图声明与结构校验 | M5 | 资源/Pass 声明、句柄/内容/循环校验与失败保护通过，见 [0057](development/0057-graph-declarations.md) | 已完成 |
| M6.2 | 依赖编译与资源生命周期 | M6.1 | 确定性排序、内容裁剪、独立快照与 transient 生命周期计划通过，见 [0058](development/0058-graph-compilation.md) | 已完成 |
| M6.3 | 单队列同步与执行 | M6.2 | barrier、layout、外部状态与完成跟踪通过 GPU 验证，见 [0059](development/0059-graph-execution.md) | 已完成 |
| M6.4 | 样例迁移与诊断 | M6.3 | 完整管线重复执行、M5 结果回归与诊断通过，见 [0060](development/0060-graph-integration.md) | 已完成 |

**当前设计：** [GPU Graph](design/graphics-graph.md)。M6.1–4 提供声明/编译、同步执行、状态导入导出、完成跟踪和诊断；Offscreen 与完整组合样例通过图执行。

**验收：** upload → compute → draw → readback 的小图正确执行；
写后读、布局转换、外部状态、重复执行和生命周期有测试。
无效图在提交前报告可定位错误；合法 GPU 测例验证层无相关错误。
M5 的图像/计算样例迁移后输出保持符合原验证条件。

**暂缓：** 异步 compute、多队列 ownership、内存 aliasing、
复杂历史资源复用和可视化节点编辑器。

## M7：场景渲染与可等待截图

**目标：** 达到交付节点 B，打通场景和资产到图像的完整链路。

交付范围：

- RenderScene/RenderView 只读提取；渲染缓存管理 GpuMesh、纹理和材质，
  明确上传完成、卸载与再加载的生命周期。
- 首批 Pass 为简单 opaque、depth、tone mapping；
  pipeline 负责连接，Graph 不包含业务 Pass。
- 从工程加载场景和资产，先提供程序化简单物体回归样例，
  再接入 M4 导入网格。
- 实现 render.capture、jobs.get/wait/cancel 等操作。
  截图绑定 scene revision/view，完成返回 frame、输出文件和诊断。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M7.1 | 场景提取与 GPU 资源 | M3、M4、M6 | 只读视图、GpuMesh/纹理 Graph 上传及卸载寿命，见 [0061](development/0061-render-data-resources.md) | 已完成 |
| M7.2 | 最小渲染管线 | M7.1 | opaque/depth/tone mapping 由 Graph 执行，程序化场景可回归 | 已完成（[0062](development/0062-render-pipeline.md)） |
| M7.3 | 磁盘场景与资产集成 | M7.2 | M4 导入/CPU 产物和 M2 场景可加载并离屏渲染；Sponza 无光照预览与 alpha mask | 已完成（[0063](development/0063-render-disk.md)） |
| M7.4 | 截图任务与自动化验收 | M7.3 | capture/jobs 绑定 revision/frame，失败终态、像素变化和原子截图可核验；交付 B | 已完成（[0064](development/0064-render-capture.md)） |

**设计：** [render-data](design/render-data.md)、[render-resources](design/render-resources.md) 已采用；
[render-pipeline](design/render-pipeline.md)、[render-disk](design/render-disk.md) 已实现；
[render-capture](design/render-capture.md) 已接入 Runtime；完整字段见 [截图命令](commands/render.md)。

**验收：** 在无窗口 runner 中加载固定场景，等待资产/GPU 上传完成后渲染并保存图像；
改变变换再截图，能确认返回结果对应目标 revision。
图像验证有容差和环境记录，不要求跨设备逐像素完全一致。
截图任务完成前不报“文件已可用”，GPU 工作失败可追踪到任务与 Pass。

透明、阴影、PBR、点云、矢量场和流体可视化在该链路稳定后按需求逐项添加。

## M8：编辑器与外部进程控制

**目标：** 达到交付节点 C，提供可用的场景编辑工作台。

交付范围：

- dk-editor 接入 ImGui：Hierarchy、Inspector、Viewport、资源浏览、
  Console、保存/加载和基础诊断面板。
- 编辑器模型管理 Selection/Workspace；属性修改与 Gizmo 操作进入
  M3 命令/事务，拖拽生成一个撤销单元。
- geometry 提供射线查询与首版 CPU 拾取；
  编辑器相机与场景 Camera 分离。
- 将自动化协议扩展到 Windows Named Pipe；dk-ctl 只链接协议/传输/客户端，
  不链接完整 Runtime、Renderer 或 Physics。
- IPC 线程只接收/解析/排队，场景修改仍在 Runtime 安全点执行。
  先提供 Graph 诊断查看，不建设节点式图编辑器。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M8.1 | 编辑器工作台 | M7 | 窗口、Hierarchy/Inspector/Viewport、保存加载可用 | 已完成，见 [0065](development/0065-editor-workbench.md) |
| M8.2 | 拾取、选择与交互 | M8.1 | CPU 射线查询/Gizmo/相机；编辑进入命令与撤销链路 | 已完成，见 [0066](development/0066-editor-interaction.md) |
| M8.3 | IPC 协议和 dk-ctl | M8.2 | Named Pipe、轻量客户端、断连/超时/重复请求有明确语义 | 已完成，见 [0067](development/0067-ipc-client.md) |
| M8.4 | 编辑器与外部操作一致性 | M8.3 | 外部编辑、GUI 和截图对应同一 revision，保存后 runner 可重现；交付 C | 已完成，见 [0068](development/0068-editor-consistency.md) |

**先写设计：** `editor.md`、`editor-interaction.md`、
`geometry-query.md`、`automation-transport.md`、`automation-client.md`。

**验收：** 打开场景、选中/移动物体、修改属性、撤销/重做并保存；
关闭编辑器后 runner 重载得到相同状态。
外部 dk-ctl 修改同一运行中场景，编辑器和截图反映指定 revision。
检查重复请求、断连和超时；超时不能被解释为“操作肯定没有执行”。

## M9：Luau 与 Python 自动化

**目标：** 用脚本组合已存在的操作语义，支持批处理实验。

内嵌语言已按用户要求改为 Luau，依赖预留与规划调整见 [0069](development/0069-luau-selection.md)。
此记录不代表 M9.1 实现；脚本使用 `.luau` 源码，由宿主编译后加载，不接受外部字节码。

交付范围：

- Luau 通过官方 Compiler/VM 接口绑定受控命令与查询，不暴露裸 ECS/Vulkan 指针；
  场景构造/编辑脚本与未来运行态行为脚本使用不同上下文。
- 设计脚本错误、取消、执行时长和可访问能力约束，
  防止脚本无限执行占用编辑器；采用受限标准库和宿主能力边界，不将 Luau 的隔离机制等同于完整引擎安全沙箱。
- Python SDK 封装 dk-ctl 或已定义协议，支持超时、任务等待、错误和批量操作。
- 命令记录/重放保存协议版本、输入资产、随机种子和执行上下文。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M9.1 | Luau 命令绑定 | M3 | 源码编译、受控命令、错误恢复、保存重载与 CPU runner 验证通过，见 [0070](development/0070-luau-command-bindings.md) | 已完成 |
| M9.2 | 脚本运行限制与取消 | M9.1 | 时限/安全点/命令/VM额度、能力模式、取消恢复与控制台退出通过，见 [0071](development/0071-luau-execution-limits.md)；不承诺安全沙箱 | 已完成 |
| M9.3 | Python 自动化客户端 | M8、M9.2 | 超时/错误/批量/事务、Job 等待、CPU 重载与 GPU 截图产物通过，见 [0072](development/0072-python-automation.md) | 已完成 |
| M9.4 | 记录与重放 | M9.3 | 版本/输入/seed/上下文、逐步状态核验、Luau/CPU 与 GPU 重放通过，见 [0073](development/0073-automation-replay.md) | 已完成 |

**当前设计：** [Luau 命令绑定](design/scripting-luau.md) 覆盖绑定、预算、能力和取消/退出；
[Python 客户端](design/automation-python.md) 覆盖超时、错误、批量/事务、Job 等待和截图产物；[记录与重放](design/automation-replay.md) 覆盖输入/版本预检、身份映射、逻辑核验与失败边界。

**验收：** Luau 创建一组实体并保存；Python 批量修改参数、等待截图并收集产物。
同一操作从 GUI/CLI/Luau 进入相同服务校验。
重放能核验逻辑场景状态；取消和脚本错误后进程仍可响应。
不把命令重放承诺为跨 GPU 完全确定的模拟结果。

## M10：物理实验闭环

**目标：** 完成一个可配置、可观察、可复现输入的物理实验。

交付范围：

- 定义物理配置、状态、固定步长、启动/暂停/单步接口；
  EditWorld 创建独立 PlayWorld，Stop 后恢复编辑场景语义。
- 先选一个明确求解器与演示场景，制作小规模 CPU 参考；
  再将对应计算步骤接入 GPU Graph。具体选择（如 SPH 或 XPBD）
  在本阶段设计时确定，不同时铺开多个求解器。
- 粒子/网格使用求解器连续数据，不为每个粒子创建完整场景实体。
  模拟输出通过明确 buffer/data 契约交给独立渲染 Pass。
- 将 simulation.step/pause/query、指标导出接入命令和脚本；
  明确场景配置保存与运行态检查点的区别。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M10.1 | 模拟世界与固定步长 | M7、M9 | 独立 Play、整数固定步长、命令/脚本控制、暂停/Stop 保护与空闲推进通过，见 [0074](development/0074-simulation-world.md) | 已完成 |
| M10.2 | 首个 CPU 参考求解器 | M10.1 | XPBD距离约束、连续粒子/分色、布片地面边界与解析/300拍指标验收，见 [0075](development/0075-cpu-xpbd.md) | 已完成 |
| M10.3 | GPU 求解与可视化 | M10.2 | GPU XPBD/同图布片绘制、300拍CPU/GPU容差、失败/寿命和密集图回归通过，见 [0076](development/0076-gpu-xpbd.md) | 已完成 |
| M10.4 | 脚本化实验验收 | M10.3 | GPU 命令、Python/Luau 精确300拍与统一导出重放、CPU/GPU容差、暂停/Stop保护通过，见 [0077](development/0077-simulation-experiments.md) | 已完成 |

**先写设计：** [physics-api.md](design/physics-api.md) 与 [physics-xpbd.md](design/physics-xpbd.md) 已建立；
M10.3已建立 [GPU XPBD](design/physics-xpbd-gpu.md) 与 [模拟可视化](design/render-simulation.md)，规定数据/提交和渲染读取契约。
若要交付检查点恢复，另写 `simulation-checkpoint.md`，不得复用场景文件语义。

**验收：** 同一输入和 seed 下严格执行 N 个 step，导出步数/时间/数值指标和图像；
小规模 CPU/GPU 结果在设计容差内。
暂停期间无额外 step，Stop 不覆盖编辑态。
模拟写入与渲染读取由同一图的资源声明建立顺序；
GPU 未完成前不释放状态，也不默认经 CPU 读回再上传。

## M11：模拟运行响应性与性能基线

**目标：** 测清现有 CPU/GPU 模拟成本，使长时间实验期间的查询、暂停、取消和退出保持可响应。

交付范围：

- 接入 GPU 时间戳与 Tracy GPU 区间，区分 shader 编译、Graph 构建、提交、等待、求解和绘制成本；
  GPU 查询资源随提交完成回收，性能采集开关不改变求解结果。
- 使用固定输入、seed、步数及多种布片规模建立冷启动和稳定运行基线，记录设备、驱动、构建配置、
  数值误差和采集开销；报告 CPU/GPU 耗时与控制请求延迟，不将整个脚本耗时当成求解器耗时。
- 将长时间模拟组织为有界批次，提供任务进度和 GPU 完成轮询；明确已提交与已完成步数、
  在途资源上限、暂停/取消生效点、故障冻结和关闭排空规则，保持 owner 线程的状态发布职责。
- 保留严格 N 步、固定步长及 EditWorld/PlayWorld 隔离；根据基线选择必要的缓存或批次优化，
  同时覆盖首次初始化/编译和持续运行的响应边界。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收目标 | 状态 |
| --- | --- | --- | --- | --- |
| M11.1 | GPU 性能观测 | M1.7、M10 | timestamp/Tracy提交与求解/绘制、CPU阶段关联、开关一致性、查询复用/失败/关闭及CPU-only通过，见 [0078](development/0078-gpu-profiling.md) | 已完成 |
| M11.2 | 可复现性能与响应性基线 | M11.1 | 三规模/三轮、冷/热CPU与GPU、Tracy采集开销、真实IPC控制及300拍数值通过；后续响应阈值与条件确定，见[报告](benchmarks/2026-10-09-simulation.md)及[0079](development/0079-simulation-performance-baseline.md) | 已完成 |
| M11.3 | 有界模拟任务与暂停/取消 | M11.2 | 有限run/query/cancel、单在途批次、暂停/取消/Stop边界、故障冻结及真实CPU/GPU三规模300拍/退出通过，见[0080](development/0080-bounded-simulation-tasks.md) | 已完成 |
| M11.4 | 响应性集成与性能复测 | M11.3 | 完整响应矩阵与定向回归通过；按用户确认的退出p95≤400ms验收，三规模300拍数值和同基准性能复测通过，见[报告](benchmarks/2026-10-09-simulation-response.md)及[0081](development/0081-simulation-responsiveness.md) | 已完成 |

**设计入口：** 实施前更新 [性能分析](design/foundation-profiling.md)、
[GPU 资源与提交](design/graphics-resources.md)、[GPU Graph](design/graphics-graph.md)、
[模拟服务](design/physics-api.md) 与 [GPU XPBD](design/physics-xpbd-gpu.md) 的相关契约；
任务控制还需核对 [Jobs](design/foundation-jobs.md)、[IPC](design/automation-transport.md)
和 [Luau 执行限制](design/scripting-luau.md)。新增或变更命令同步命令参考及客户端用法。

**验收：** 在同一设备和构建配置下提供可重跑基准、GPU 采集证据与控制延迟统计；
性能变更不得破坏数值正确性、暂停稳定性及 Stop 对编辑态的保护。
客户端超时或断连仍不等于任务取消；取消不假装撤销已提交 GPU 工作。
具体阈值由 M11.2 测量后写入设计，再据此验收 M11.3–4，不承诺跨设备固定帧率或硬抢占。

**暂缓：** 多队列/异步 compute、GPU 内存 aliasing、通用并行调度框架和未由测量支持的优化。

## M12：编辑器模拟工作台

**目标：** 在编辑器内完成物理实验的配置、运行、观察与导出，GUI 和外部命令共享同一模拟服务。

交付范围：

- 增加模拟面板，提供 CPU/GPU 选择、布片参数草稿、Play/Pause/Step/Stop 与运行状态；
  明确运行中参数修改的应用时机、旧 run_id 拒绝和服务故障显示。
- 接入持续更新的模拟视口和相机取景，处理模拟 buffer/图像的跨帧寿命及计算到绘制同步；
  GPU 预览直接使用 GPU 输出，避免默认读回粒子或图像再上传的桥接。
- 显示完成步数、模拟时间、任务进度、性能与数值指标；区分当前值、旧采样和不可用值，
  将数值读回做成显式或有界采样操作。
- 接入现有实验导出，并核对图像、配置与指标对应的 run_id/步数；保持 EditWorld 独立编辑与撤销历史。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收目标 | 状态 |
| --- | --- | --- | --- | --- |
| M12.1 | 模拟面板与运行控制 | M8、M11 | CPU/GPU参数草稿、异步零步准备/单步及运行控制、旧请求/编辑态保护、真实GUI/IPC状态同步通过，见[0082](development/0082-editor-simulation-controls.md) | 已完成 |
| M12.2 | 实时模拟视口与取景 | M12.1 | CPU/GPU持续预览、直接GPU图像交接、独立相机/自动取景、暂停稳定及缩放/最小化/Stop/关闭通过，见[0083](development/0083-editor-simulation-viewport.md) | 已完成 |
| M12.3 | 指标显示与实验导出 | M12.2 | 完成步数/时间/批次性能、显式数值及采样状态、CPU/GPU精确导出与失败保留通过，见[0084](development/0084-editor-simulation-metrics-export.md) | 已完成 |
| M12.4 | GUI/IPC/脚本一致性验收 | M12.3 | CPU/GPU同窗GUI/IPC精确300拍、暂停/导出/脚本重放及最新Edit/history保护通过；响应smoke与GPU同步/寿命回归通过，见[0085](development/0085-editor-simulation-consistency.md) | 已完成 |

**设计入口：** 实施前更新 [编辑器](design/editor.md)、[编辑器交互](design/editor-interaction.md)、
[模拟服务](design/physics-api.md) 与 [模拟可视化](design/render-simulation.md)；
视口呈现继续遵守 [窗口呈现](design/graphics-presentation.md) 的生命周期。
新面板及跨入口状态协调的详细设计在对应小阶段开工时补齐。

**验收：** 用户在工作台完成一次配置、运行、暂停、单步、观察和导出；
同一输入与固定视图可通过脚本重跑并比较数值/图像。
Stop 后保留最新编辑成果及撤销历史；GUI 不另建绕过服务的模拟状态。

**暂缓：** 模拟结果烘焙回场景、通用节点编辑器、检查点恢复和新增求解器。

## M13：实验管理与参数扫描

**目标：** 基于现有 Python SDK 和实验导出，批量执行可追溯实验并自动生成对比报告。

交付范围：

- 定义版本化实验计划，覆盖基础配置、参数组合、seed、求解器、固定步数及输出要求；
  执行前展开并校验组合数量和工作预算，记录可追溯的实验身份与环境信息。
- 提供批量执行器、进度与产物索引；首期对同一 Runtime 串行调度，避免多个实验争用同一 PlayWorld。
  保留每个实验的配置、指标、图像及来源信息，CPU-only 数值任务可独立执行。
- 记录成功、失败、取消和结果未确认状态；对超时先核实已接受任务，避免盲目重复执行。
  支持重启批处理后跳过已核验成功项，以及显式重试失败项；重试从零运行并保留各次尝试证据。
- 输出机器可读汇总及可查看的指标表、对比图和报告，区分求解成本、端到端成本及数值误差，
  明确失败/缺失数据和比较条件。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收目标 | 状态 |
| --- | --- | --- | --- | --- |
| M13.1 | 实验计划与产物索引 | M9、M12 | 稳定展开/身份、float32与非法组合/预算拒绝、真实CPU数值/GPU导出来源和摘要核验、索引失败保护通过；[0089](development/0089-experiment-plan-index.md) | 已完成 |
| M13.2 | 批量执行与进度 | M13.1 | 单次CLI多组合、同Runtime串行/跨进程互斥、持久进度/产物对应及失败保留通过；CPU-only数值与CPU/GPU图像分别验收；[0090](development/0090-experiment-batch.md) | 已完成 |
| M13.3 | 中断恢复与失败重试 | M13.2 | CPU/GPU五处真实强杀后恢复、成功项跳过、原票据/run核实、失败/取消显式重试与旧证据保留通过；[0091](development/0091-experiment-recovery.md) | 已完成 |
| M13.4 | 对比报告与闭环验收 | M13.3 | JSON/CSV/HTML可追溯报告、CPU/GPU参数扫描与300拍独立重跑容差、失败/缺失与发布保护通过；M11计时口径分开呈现，见[0092](development/0092-experiment-reports.md) | 已完成 |

**设计入口：** [实验管理](design/experiment-management.md)基于 [Python SDK](design/automation-python.md)、
[模拟服务](design/physics-api.md)和 [现有实验流程](guides/simulation.md)，明确计划/索引格式、
实验身份与产物核验、任务恢复和报告比较边界；用法见[实验管理指南](guides/experiments.md)。
[M9 场景记录与重放](design/automation-replay.md) 和物理实验输入保持各自的版本语义。

**验收：** 一条批处理入口完成参数组合、多个 seed 和 CPU/GPU 对照实验，
自动收集产物、报告数值与性能差异；真实进程中断后可继续未完成的批处理，成功产物不被覆盖。
批处理恢复只恢复任务清单，单次模拟仍从零重跑，不等同于运行态检查点。
CPU-only 验收不要求图像；图像和 GPU 对照单独验收，不把 GPU 跳过记为通过。

**暂缓：** 多机调度、多 GPU 并行、远程实验服务、模拟检查点和通用科研平台。

## M14：大气渲染

**目标：** 在现有Windows/Vulkan场景管线上提供可参数化天空、太阳日光和空气透视；
地表至高空、昼间至日落连续，GUI/命令/保存重载与离屏截图使用相同输入。
设计入口：[大气渲染设计](design/render-atmosphere.md)。默认球对称地球尺度、单太阳、线性RGB近似，
不新增三方依赖，不要求完整PBR/IBL/几何阴影或轨道地形。关闭环境时保持旧unlit路径。

M14.1已补齐持久环境数据，M14.2已实现3D资源与深度采样，M14.3已完成独立大气LUT与天空。
M14.4日光接收、M14.5空气透视、M14.6环境编辑/截图闭环和M14.7质量/性能基线均已交付。
本机1080p增量GPU的1.5ms候选目标未达，实测差距见[基线报告](benchmarks/2026-10-13-atmosphere.md)。

| 子阶段 | 范围与交付物 | 前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M14.1 | 环境CPU模型、单位/坐标与持久化；单太阳/大气参数、默认disabled、场景格式迁移、环境查询/修改命令 | M2/M3、M7数据层 | 地球尺度高度/参数校验、v1→v2迁移、guard/事务/历史/重载、CPU-only依赖和命令发现均通过；[0094](development/0094-environment-model.md) | 已完成 |
| M14.2 | 体积图像与深度采样底座：3D image/view、必要float格式、采样/storage/复制、Slang反射与Graph访问 | M14.1、M5/M6 | 3D坐标/过滤/读回正确，越界与格式能力拒绝；storage→sample及depth→read屏障；2D路径回归、失败/在途寿命保护均通过；[0095](development/0095-volume-images.md) | 已完成 |
| M14.3 | 大气积分参考、Transmittance/Multiple Scattering/Sky View LUT、太阳盘与行星遮光，离屏天空样例 | M14.2 | 真空T=1/L=0、有限能量、正午/日落/高空/切线参考比较；参数与相机更新只使相关LUT失效；失败不替换旧代；定量参考/缓存/失败寿命与样例均通过，[0096](development/0096-atmosphere-luts.md) | 已完成 |
| M14.4 | 最小日光接收：可选Lambert太阳直射和天空漫射，与大气透射共用太阳 | M14.3 | 法线/剪切/负缩放及太阳方向正确；大气衰减作用于直射；保留unlit和emissive语义，不宣称PBR/几何阴影；数值、镜像/剪切、失败与样例均通过，[0097](development/0097-surface-sunlight.md) | 已完成 |
| M14.5 | 视锥空气透视、深度距离重建、天空/几何HDR合成及受限线性诊断读回 | M14.4 | 近远景/alpha-mask遮挡、不同near/far与高度、RGB透射和曝光一致；无重复散射；关闭环境与旧基线一致；HDR数值/遮挡/曝光、失败寿命与旧链路回归通过，[0098](development/0098-aerial-perspective.md) | 已完成 |
| M14.6 | 编辑器环境面板、快照绑定、render.capture及Python/Luau用例 | M14.5、M8/M9 | GUI与命令改同一环境，撤销/保存重载一致；固定相机/时间/质量的截图可追溯；调参旧代丢弃、resize/取消/关闭安全；环境GUI/IPC、固定截图、Python/Luau、后台寿命和CPU-only验收通过，[0099](development/0099-environment-integration.md) | 已完成 |
| M14.7 | 大气质量/性能基线与阶段交付 | M14.6、M11观测 | 固定参考集、地平线连续性、CPU-only与required GPU通过；三预设18进程的冷/热LUT、CPU/GPU/API端到端与在途显存归档；1.5ms候选未达，详见[0100](development/0100-atmosphere-baseline.md)与[基线](benchmarks/2026-10-13-atmosphere.md) | 已完成 |

**交付节点D（已交付）：** 提供晴空/日落/高空三个可复现预设，支持远景空气透视和环境编辑/截图闭环；[复现入口](guides/atmosphere-benchmark.md)。
首轮接入ScenePipeline场景视口及render.capture；RenderSimulation的独立布片视口不隐含接入。

## M15：体积云

**目标：** 在M14上增加可控制且可复现的单层体积云，覆盖云下/云内/云上、逆光/日落、风动画和地表云影。
设计入口：[体积云设计](design/render-clouds.md)。采用程序化天气/3D噪声、分段介质合成；
先无历史参考正确性，后半分辨率/时域优化。允许复用原SDK，但M13布料计划格式不视为已支持云实验。

| 子阶段 | 范围与交付物 | 前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M15.1 | 云CPU参数/版本化持久化、seed天气图、基础/细节3D噪声及显式风时间 | M14完成 | 固定seed重建一致；零coverage、密度/高度范围；纹理生成/采样核验；预算拒绝及旧代保留；CPU-only编辑/重载；22项定向用例及required GPU通过，[0101](development/0101-cloud-fields.md) | 已完成 |
| M15.2 | 无历史高采样云参考：球壳区间、Beer–Lambert积分、太阳散射、自遮蔽和低阶多散射近似 | M15.1 | CPU/GPU解析解、三档收敛、云内/外/切线、有限能量及失败保护通过；模型近似与离散误差分开，17项定向用例含required GPU通过，[0102](development/0102-cloud-reference.md) | 已完成 |
| M15.3 | 云/空气/几何统一合成、深度裁剪及诊断图 | M15.2、M14.5 | 零云逐位退化M14、前景/mask/太阳遮挡、远云空气透视、联合积分/曝光一次、固定时间与截图重现；21项定向用例通过，参考上限4096像素，[0103](development/0103-cloud-composition.md) | 已完成 |
| M15.4 | 半分辨率、空区跳跃/早停、保边上采样、每视图时域重投影 | M15.3 | 相对收敛参考、静止/运动/风/反遮挡、太阳边界与零云退化通过；4视图完成后发布、重置与失败保护通过；21项定向验收，含1080p调度，性能未验收，[0104](development/0104-cloud-reconstruction.md) | 已完成 |
| M15.5 | 云透射阴影图与日光接收链路 | M15.4、M14.4 | 固定世界光空间云柱/地表数值、太阳/风方向与相机稳定、零云单位透射和仅直射作用通过；预算、失败及在途寿命正确；21项定向检查通过，[0105](development/0105-cloud-shadows.md) | 已完成 |
| M15.6 | 云面板/预设、命令/脚本与固定时间截图序列，版本化渲染用例清单和对比产物 | M15.5、M14.6 | GUI/IPC/脚本/重载同输入；reference与独立时域序列、来源/线性比较及失败保护通过，25项独立定向测试和清单CLI验收见[0106](development/0106-cloud-integration.md) | 已完成 |
| M15.7 | 体积云质量、动态稳定性、性能与大气联合交付 | M15.6、M11观测 | 四预设58组参考/动态质量、17项定向回归和补充复验通过；三轮24进程CPU/GPU/显存归档；云4ms/联合5.5ms及192MiB峰值候选未达，稳态128MiB达标，见[0107](development/0107-cloud-delivery.md)与[报告](benchmarks/2026-10-13-clouds.md) | 已完成基线 |

**交付节点E已完成基线：** 晴空、稀疏积云、厚云与日落预设固定seed/time可重现，
动态视角与云影通过质量检查，编辑器和离屏报告共享同一版本化物理配置。质量及三轮性能见[联合报告](benchmarks/2026-10-13-clouds.md)；非零云GPU及在途峰值候选未达，不声明实时性能达标。

## M14/M15 预算、证据与推进规则

没有人员/日历预算，不给虚构工期。默认串行推进M14.1→M14.7→M15.1→M15.7；
每节开工前细化设计和本节测试，过大的工作先拆Mx.y.z；每个验收小阶段独立留档和本地提交。
规划记录0093的completed只表示规划完成，不能用于关闭M14/M15或跳过GPU验收。

初始质量目标和测试矩阵见两份设计：透射绝对误差≤2e-3；线性RGB归一化RMSE大气≤3%、云≤5%，
太阳盘/地平线/反遮挡单列；动态历史另测重置和残影。参考实现收敛之后、优化之前冻结门槛，
不把不同物理近似之间的差异隐去，也不拿单张低动态范围截图替代数值测试。

性能候选目标以1080p、声明的Balanced质量档为条件：大气增量GPU p95≤1.5ms、
云（含重建/合成/云影）≤4ms，组合区间≤5.5ms；环境稳态资源≤128MiB、含在途旧代峰值≤192MiB。
首轮记录实际GPU/驱动/构建/开关和参考图后冻结；这不是已测结果或60fps承诺。
冷编译/LUT重建、CPU建图/提交、GPU区间、端到端和读回分开；至少三轮，保留原始样本和质量误差，
不能通过降低未声明质量、漏算更新/同步或叠加父子计时获得通过。

纯CPU参数/保存/命令与真实GPU同步/图像分开验证；图形验证要求设备和validation可用。
新测试在实际实现时加入CMake/选择表，不提前登记不存在的测试；阶段结束只跑相关链路。
本轮不引入完整PBR/IBL、几何阴影、气象模拟、多云层/风暴、异步compute或多GPU。

## M16：渐进式路径追踪

**目标：** 先建立可验证的 CPU/无历史 GPU 参考，再交付静态三角形场景的渐进式表面路径追踪视口与固定样本截图。
用户已选择该方向；底层采用 Vulkan KHR RT pipeline，沿用现有 Device/Shader/Graph 和单队列提交契约。
设计入口：[光追底座](design/graphics-raytracing.md)、[路径追踪管线](design/render-pathtracing.md)。
逐项状态以下表为准；0108的completed只表示规划完成，实现验收引用各阶段开发记录。

| 子阶段 | 范围与交付物 | 前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M16.1 | CPU输入/采样/积分契约、程序化Lambert小场景参考，固定相机与灯光/误差/预算协议 | M7数据、M8.2 Geometry（独立测试对照） | 无GPU依赖运行；解析黑场/常量环境/白炉、暴力求交校核；方向光/面积灯及混合光源、MIS/RR及收敛成立；连续样本编号/预算拒绝/旧输入不变 | 完成，见[0109](development/0109-pathtracing-reference.md) |
| M16.2 | RT设备能力组、扩展/feature/limits、BDA及VMA配置 | M16.1、M5 | disabled/if_available/required、显式选卡与缺项可核验；required RT probe成功；非RT路径和CPU-only保持，缺硬件不记通过 | 完成，见[0110](development/0110-raytracing-device.md) |
| M16.3 | 拥有型BLAS/TLAS、构建输入/scratch、尺寸/对齐与候选构建 | M16.2 | 三角/多实例构建、地址和上限拒绝；失败保留旧代；pending卸载/关闭保活；validation/sync validation通过 | 完成，见[0111](development/0111-acceleration-structures.md) |
| M16.4 | Slang raygen/miss/closest-hit/any-hit、AS反射、RT pipeline/SBT/trace encoder | M16.3、M5 shader/使用层 | 阶段与payload/布局错误拒绝；SBT handle/stride/alignment/index核验；真实已知射线命中/重心/距离/visibility正确，shaderc参考同步 | 完成，见[0112](development/0112-raytracing-pipeline.md) |
| M16.5 | Graph AS类型、构建/trace依赖与同步、类型化Pass回调 | M16.4、M6 | upload→BLAS→TLAS→trace→readback；裁剪/初始化/导入导出/跨图与队列拒绝；失败不提交状态；required GPU同步验证及原buffer/image回归 | 完成，见[0113](development/0113-graph-raytracing.md) |
| M16.6 | GPU无历史Lambert路径追踪、不可变场景/灯光表、HDR诊断 | M16.5、M16.1 | 主/阴影/多次反弹、NEE/MIS/RR；空场景、缩放/剪切、发光/遮挡；CPU/解析参考误差与收敛过门槛；失败保留旧帧 | 完成，见[0114](development/0114-gpu-pathtracing.md) |
| M16.7.1 | 纹理/alpha-mask/双面语义、固定descriptor数组与非一致索引能力 | M16.6 | CPU采样与GPU一致，sRGB/alpha/UV/wrap、radiance和visibility mask、负缩放正反面；容量/能力拒绝，当前资产路径可追溯 | 完成，见[0115](development/0115-pathtracing-textures.md) |
| M16.7.2 | metallic-roughness GGX子集，CPU eval/sample/PDF与GPU实现 | M16.7.1 | metallic/roughness端点、掠射/法线、能量与MIS均值检验；先冻结CPU模型/阈值再验GPU；不混同Lambert与默认metallic材质 | 完成，见[0116](development/0116-pathtracing-ggx.md) |
| M16.8 | 每视图渐进累积、计数式样本、reset/poll、tile与工作/显存额度 | M16.7.2 | 批次/分块不重漏样本，固定spp与一次参考一致；相机/资产/手势重置；曝光不重采样；迟到/失败/取消不推进completed_spp；有界在途资源 | 完成，见[0117](development/0117-pathtracing-progressive.md) |
| M16.9 | 编辑器模式切换、后台预览、进度/暂停/重置和完成图像交接 | M16.8、M8/M12视口机制 | 实际GUI/IPC编辑同输入；拖动/resize/切场景不混历史；能力不足可见；owner不做GPU等待，单队列串行与关闭安全；启动测试使用--background | 完成，见[0118](development/0118-editor-pathtracing.md) |
| M16.10 | render.capture显式后端/固定spp、Job进度/取消、provenance与线性报告、Python/Luau例子 | M16.8、M7.4/M9；一致性需M16.9 | commands.list/describe与文档一致；从0精确到目标spp；场景/输入版本绑定；GUI与capture线性一致；取消/失败保留旧文件，CPU-only明确无GPU能力 | 完成，见[0119](development/0119-pathtracing-capture.md) |
| M16.11 | 固定质量/多seed收敛、动态重置、三轮性能/显存基线和交付 | M16.9、M16.10、M11观测 | 程序化与受支持磁盘场景、CPU参考及required RT GPU通过；冷/热/达到固定质量的耗时、取消/回收及显存峰值留原始证据；性能缺口不伪装成达标 | 完成，见[0120](development/0120-pathtracing-delivery.md) |

M16.7材质任务组的M16.7.1/2均已验收；每行有独立开发记录与本地提交。
M16.1–16.11均已验收，交付节点F完成；质量与三轮性能/显存证据见[实测报告](benchmarks/2026-10-15-pathtracing.md)。1080p实时性能和GGX在1024 spp达到2%质量的候选未达，后续优化需另行规划；不虚构工期。

**交付节点F（已验收）：** 用户可选路径追踪视口，静止逐步收敛，改场景/相机后重置，
以明确 seed/反弹数/spp 通过GUI、IPC或脚本获得可追溯截图；失败和不支持的输入有明确结果。
阶段交付要求原始线性质量证据；PPM预览不能代替积分验证。初始阈值/工作额度见管线设计，
CPU参考收敛后、GPU优化前冻结；CPU-only、required RT/同步验证及真实GUI/capture通过。三轮原始性能数据与未达候选均保留，不承诺60fps。

首轮范围不含大气/云体积路径追踪、完整PBR/IBL、透射、去噪、形变/布片、AS refit/压缩、
异步compute和实时性能承诺。M14/M15现有质量门槛和未达性能目标保持，M16不替代其优化任务。
只在实施时登记真实target、命令与测试；新增/升级依赖另按固定baseline规范核验。

## M17：大气、云与路径追踪集成

**目标：** 让大气、云参与主射线、表面反弹、介质多次散射和光源遮蔽，最终通过Editor及固定spp capture使用。
设计与模型边界见[路径介质设计](design/render-pathtracing.md#m17-大气云与表面路径的统一传输)。
本次授权为分阶段方案加第一阶段实现；后续阶段仍待执行，不把M14/M15合成或M16表面模式当作集成完成。

| 子阶段 | 范围与交付物 | 前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M17.1 | 冻结世界空间空气/云输入、系数/保守上界、有序区间和CPU RGB透射参考 | M14/M15字段、M16数据 | 8项介质及6项旧表面回归通过，CPU隔离配置9项通过；解析/真实云参考、边界/旧代/取消/预算均覆盖 | 已完成，见[0123](development/0123-path-media-foundation.md) |
| M17.2 | CPU完整体积路径、相函数/自由程、太阳圆盘、表面/介质NEE与MIS、多次散射 | M17.1 | 真空退化、纯吸收/单次散射、白炉/能量、多次散射收敛、独立seed统计、随机维度及预算拒绝 | 完成，见[0124](development/0124-volume-path-reference.md) |
| M17.3 | GPU晴空/空气透射、任意反弹/阴影的介质与行星边界、渐进历史 | M17.2 | CPU/GPU数值及统计、低太阳/反射/远景、失败与资源额度、required RT/sync及上层表面回归共50项通过 | 完成，见[0125](development/0125-gpu-air-pathtracing.md) |
| M17.4 | GPU云密度/自由程/多次散射、自遮蔽与场景几何遮蔽、世界空间保守上界 | M17.3 | 云壳/零云/固定时间、空气云多散射、纹理反射/几何遮蔽、保守上界/额度/历史及上层兼容共53项通过 | 完成，见[0126](development/0126-gpu-cloud-pathtracing.md) |
| M17.5 | Editor Scene media模式、固定capture与Console/IPC、会话重置和来源报告 | M17.4 | 真实后台GUI/Console/IPC与捕获线性一致、连续错误恢复/会话及Refresh重置、schema/Python/Luau和旧表面回归12项通过 | 完成，见[0127](development/0127-editor-media-pathtracing.md) |
| M17.6 | Sponza固定质量下缓存/计数/调度优化及三轮交互与显存基线 | M17.5 | 42项定向检查、四个CPU参考及同样本比较、三轮性能/显存与真实GUI通过；8个Sponza质量候选未达并保留null | 完成，见[0128](development/0128-path-media-performance.md) |

交付节点G须M17.1–6逐项验收。先建立数值参考，随后GPU，最后交互优化；M17.1完成不意味着Editor可以联合渲染。
不以新增伪接口或声明新模式代替实际集成，不提前更改命令能力；不在本轮引入去噪、wavefront或新依赖。

## M18：相机操作模式与投影扩展

**目标：** 扩展编辑/模拟视口的会话相机，提供 Orbit、Fly、水平 Walk 与可调 FOV/移速，逐层支持正交。
当前支持可调镜头的透视 Orbit/Fly/水平 Walk、正交 Orbit/六视图和 Unlit inspection；编辑/模拟分别保存会话设置。基础光栅/模拟、路径、光栅大气/云与固定捕获均支持各自域内的正交。
检查依据、数据/输入/状态与兼容设计见 [编辑器交互 M18](design/editor-interaction.md#m18-相机扩展计划)。
M18.1–5 已完成，记录见 [0129](development/0129-camera-settings.md)、[0130](development/0130-camera-fly-walk.md)、[0131](development/0131-camera-orthographic.md)、[0132](development/0132-pathtrace-orthographic-capture.md)、[0133](development/0133-raster-orthographic-environment.md)。

| 子阶段 | 范围与交付物 | 前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M18.1 | 相机姿态/镜头/导航设置拆分；可调 FOV、裁剪、灵敏度、移速面板；公共投影计算与截图真实参数 | M8.2、M12.2、现有渲染入口 | 7项数学/相机与7项真实窗口通过，另CPU-only复验7项；FOV/拾取/Gizmo/截图一致、参数拒绝、纯偏好保留累积 | 已完成，见[0129](development/0129-camera-settings.md) |
| M18.2 | Fly / 水平 Walk、连续键盘移动、相对鼠标、Edit/Simulation 共用控制器 | M18.1 | 10项CPU相机与5项真实窗口通过；固高/帧率/切换/释放、持续图像与无Scene/Undo副作用 | 已完成，见[0130](development/0130-camera-fly-walk.md) |
| M18.3 | 正交 Orbit/Pan/缩放、六视图、基础光栅与模拟、显式 Unlit inspection、拾取/Gizmo | M18.2 | 22 项 CPU 与 5 项 GPU/窗口通过（含 CPU-only 18 项）；焦平面/平行射线/深度尺寸/裁剪、双视口、Gizmo/历史、显式禁用及旧图恢复 | 已完成，见[0131](development/0131-camera-orthographic.md) |
| M18.4 | CPU/GPU Surface only 与 Scene media 正交主射线；固定截图/序列 projection 与 ortho_height、来源与示例 | M18.3、M17.5 | CPU/GPU 正交近面起点与介质、历史失效、旧透视请求、schema/来源与序列失败保护通过；required GPU、16 个 GUI/capture 对照点一致，见 [0132](development/0132-pathtrace-orthographic-capture.md) | 已完成 |
| M18.5 | 光栅大气/云正交参考与合成、深度/AP/天空/云时域适配 | M18.4、M14/M15 | 44项定向验证通过；独立CPU参考、正交光程/天空/云/遮挡/时域/云影、GUI/capture一致与失败保护，见[0133](development/0133-raster-orthographic-environment.md) | 已完成 |

M18 各阶段已分别完成定向验收与本地提交。
Walk 首期为固定世界 Y 的水平漫游，无重力/地形碰撞/台阶/跳跃；完整角色控制器另行立项。
会话偏好不写 Scene/历史，跨启动保存和场景 Camera 组件也不隐含包含在本轮。
每个实现小阶段开始时更新直接相关模块设计、创建实际开发记录，按范围定向验证并独立提交；本表不预分配开发编号。

## M19：ReSTIR 直接光重采样

目标是以显式可选的 ReSTIR DI 降低首表面直接光方差，普通路径继续估计后续反弹。
先 Surface only，再处理 Scene media 中首个真实散射前到达的表面；介质散射顶点和全路径复用另行规划。
估计器、模块边界、光源测度、光能分工、历史与资源契约见[ReSTIR 设计](design/render-restir.md)。
用户确定后续接入 RTXDI/DLSS：采样、介质、重建分别选择，CPU 参考保留为独立 oracle，GPU 阶段优先核验 RTXDI-Library。
DLSS 单独设计重建输入/能力，不改变本阶段原始线性质量验收；三方库接入状态见[说明](third-party-libraries.md)。

| 小阶段 | 交付物 | 必要前置 | 独立验收条件 | 状态 |
| --- | --- | --- | --- | --- |
| M19.1 | CPU reservoir/RIS 参考、光样本/PDF/目标/随机维度、首表面光路分工及基准协议 | M16.1/7、M18.4 | 离散可枚举期望、M=1/全零/极值、支持域/合并校正推导；独立直接积分及 Lambert/GGX、mask/delta/环境光能不重漏；冻结均值、收敛与性能协议；CPU-only | 已完成：48 个唯一定向测试及单灯/100灯/1000灯 CPU 基线通过，见[0134](development/0134-restir-reference.md) |
| M19.2 | 优先核验 RTXDI-Library 适配；GPU 首表面记录与初始 RIS、分阶段 tile/epoch 调度、精确内存/工作预算 | M19.1、M16.5/8 | SDK 版本/许可、Slang/SPIR-V/桥接验证；无时空复用 CPU/GPU 均值通过；透视/正交、首表面/间接光分工一致；不同 tile/批次重放一致，跨 pass 同步、超额/取消/迟到保留旧帧 | 已完成：固定 RTXDI v3.1.0、17 GPU 与 18 CPU 定向测试通过，见[0135](development/0135-restir-gpu-initial.md) |
| M19.3 | 时间 reservoir、静态表面重投影、图像与采样历史分离 | M19.2 | 相机平移旋转/停动、disocclusion、GGX 视角变化；投影/尺寸/物理输入变化清空，曝光保留；支持校正/历史界限与长链均值、双视图及失败发布通过 | 未开始 |
| M19.4 | 空间复用、独立输入/输出平面、来源支持归一化与可见性校正 | M19.3 | CPU 空间参考与 GPU 对照；轮廓/薄墙/法线/材质突变无系统性漏光或变暗；邻接/遍历不受 tile 顺序影响，校正射线和峰值额度有界 | 未开始 |
| M19.5 | Scene media 首表面 DI 的 CPU 条件采样参考 | M19.4、M17.2 | hero 通道/自由程/阴影随机流与 reservoir 支持一致；真空、纯吸收/彩色消光、有限太阳/行星/云阴影及先体积散射分支均值通过；不复用体积散射点 | 未开始 |
| M19.6 | GPU 空气/云首表面 DI、当前透射评估与介质预算/失效 | M19.5、M17.4 | 独立 CPU/常规体积路径对照，透视/正交、多次散射与遮蔽成立；固定/变化风时间、场/AS 复用、tracking 超限及取消不发布候选 | 未开始 |
| M19.7 | Editor/worker 算法选项、诊断与独立固定捕获、版本化来源和命令文档 | M19.4、M19.6、M17.5/18.4 | 旧请求兼容；新捕获从空历史可重放、来源完整且与 GUI 同模式一致；非法组合拒绝、旧文件保护、后台窗口/取消/关闭通过；不扩展光栅 render.sequence | 未开始 |
| M19.8 | 质量/性能、相机运动与资源验收，默认策略结论 | M19.7 | 独立历史链均值/长链收敛；单灯/多灯/Sponza/天空/厚云等时间与等误差、三轮计时/峰值资源/热交互报告；收益未达明确记录，不能用降质关闭指标 | 未开始 |

每阶段独立设计细化、开发记录、定向验证和本地提交。没有人员/工期输入，不指定日历日期。
首版使用完整校正与当前可见性，不默认加入有偏近似、去噪或新的三方运行时依赖。
候选收益目标和交互指标见设计草案；最终是否适合默认开启取决于 M19.8 实测，规划不承诺实时。
M19 之后根据噪声与时间分布决定 ReSTIR GI/PT、ReGIR 或体积重要性采样，不提前分配实施记录。

## 当前执行边界与下一步

M0–M6、M1.7 Memory/Tracy 补充均已完成。当前可运行的 CPU 链路包括
Foundation、Scene 持久化、命令/事务、batch/stdio、资产导入/缓存与异步 CPU Ready。
各小阶段的验收条件与证据入口见上表；历史测试计数保留在[开发记录](development/README.md)，不作为日常固定回归套件。
M4 的进程重启、取消和退出验收见 [0040](development/0040-cpu-assets-delivery.md)，
Memory 的测量条件与限制见[基线报告](benchmarks/2026-09-28-memory.md)。

M5.1 的无窗口设备、验证诊断及 CPU-only 隔离验收见 [0042](development/0042-vulkan-device.md)。
M5.2 的 VMA Buffer/Image、单队列提交/读回与延迟释放验收见 [0045](development/0045-graphics-resources-submission.md)。
M5.3 的 Slang 编译、SPIR-V/反射、诊断和离线工具验收见 [0046](development/0046-slang-shader-compiler.md)。
M5.4 的离屏 draw/dispatch/readback、同步验证与对象寿命验收见 [0047](development/0047-offscreen-execution.md)。
[M5.5 Vulkan 使用层封装](design/graphics-vulkan.md) 的 M5.5.1–5 已全部完成并独立提交，
最终验收见 [0053](development/0053-vulkan-offscreen-migration.md)。
M5.6 窗口呈现、交换链同步与恢复、独立离屏和 CPU 回归见 [0056](development/0056-presentation-recovery.md)。
M6.1 图声明与结构校验完成，见 [0057](development/0057-graph-declarations.md)。
M6.2 编译计划与生命周期完成，见 [0058](development/0058-graph-compilation.md)。
M6.3 同步、执行与状态导入导出完成，见 [0059](development/0059-graph-execution.md)。
M6.4 样例迁移与诊断完成，见 [0060](development/0060-graph-integration.md)；M6 全部验收。
M7.1 场景提取与 GPU 资源完成，见 [0061](development/0061-render-data-resources.md)。
M7.2 最小渲染管线与程序化图像回归完成，见 [0062](development/0062-render-pipeline.md)。
M7.3 磁盘到图像及默认 Sponza 验收完成，见 [0063](development/0063-render-disk.md)。
M7.4 截图自动化验收完成，见 [0064](development/0064-render-capture.md)；M7 与交付 B 全部完成。
M8.1 编辑器工作台完成，见 [0065](development/0065-editor-workbench.md)；窗口、面板、命令编辑与保存加载已验收。
M8.2 拾取、选择与交互完成，见 [0066](development/0066-editor-interaction.md)；CPU 三角形 BVH、统一选择、本地 TRS Gizmo、独立相机及保存后 runner 重载已验收。
M8.3 IPC 协议和 dk-ctl 完成，见 [0067](development/0067-ipc-client.md)；Named Pipe、轻量客户端、owner 分派及断连/超时/重复请求语义已验收。
M8.4 编辑器与外部操作一致性完成，见 [0068](development/0068-editor-consistency.md)；真实窗口、dk-ctl、指定版本截图和保存后 runner 的状态/像素一致性已验收，M8 与交付 C 全部完成。
M9.1 Luau 命令绑定完成，见 [0070](development/0070-luau-command-bindings.md)：脚本经同一服务创建/编辑/保存场景，错误可恢复，runner 跨进程重载已验收。
M9.2 执行限制与取消完成，见 [0071](development/0071-luau-execution-limits.md)：有限默认预算、VM额度、宿主取消和 Windows 控制台退出已验收，保留已提交状态。
M9.3 Python 自动化客户端完成，见 [0072](development/0072-python-automation.md)：超时/错误、批量/事务、Job 等待和 CPU/GPU 真实链路已验收。
M9.4 记录与重放完成，见 [0073](development/0073-automation-replay.md)：版本/输入/seed/上下文、逐步状态核验及跨进程 CPU/GPU 链路已验收，M9 全部完成。
M10.1 已完成，见 [0074](development/0074-simulation-world.md)：独立 PlayWorld、固定纳秒时钟、运行控制、Luau 与真实 stdio/pipe 空闲调度通过。
M10.2已完成，见 [0075](development/0075-cpu-xpbd.md)：CPU XPBD布片、连续数组、边界、解析解与300拍数值/脚本/IPC验收通过。
M10.3已完成，见 [0076](development/0076-gpu-xpbd.md)：分色GPU XPBD、同图布片绘制、候选状态发布、数值与同步/寿命验收通过；以独立C++模块/示例交付。
M10.4 已完成，见 [0077](development/0077-simulation-experiments.md)：精确 N 步、配置/指标/图像重放及真实 Python/Luau 链路通过，M10 全部验收。
首轮 M0–M10 路线已完成。后续已纳入 M11 模拟运行响应性与性能基线、M12 编辑器模拟工作台、
M13 实验管理与参数扫描。M11.1 GPU性能观测已完成，见 [0078](development/0078-gpu-profiling.md)：原生时间戳、Tracy区间与CPU关联、开关/查询寿命及CPU-only隔离通过。
M11.2已完成，见[0079](development/0079-simulation-performance-baseline.md)：三规模固定300拍与三轮基线、采集开销及控制延迟已记录，冷初始化阻塞仍需改善。
M11.3已完成，见[0080](development/0080-bounded-simulation-tasks.md)：有限任务进度/取消、单在途上限、安全边界和CPU/GPU数值通过。
M11.4已完成，见[0081](development/0081-simulation-responsiveness.md)：GUI/IPC响应、冷阶段取消、共享设备安全回收及最终性能/数值通过；退出p95目标按用户要求调整为400ms。
M11已完成。M12.1已完成，见[0082](development/0082-editor-simulation-controls.md)：参数与控制面板复用统一服务，异步零步准备/单步、真实GUI按钮及IPC协同通过。
M12.2已完成，见[0083](development/0083-editor-simulation-viewport.md)：CPU/GPU布片实时预览、独立相机和自动取景，
暂停图像稳定、直接GPU图像交接、窗口恢复及Stop/关闭资源安全通过。
M12.3已完成，见[0084](development/0084-editor-simulation-metrics-export.md)：数值采样/批次耗时、明确新旧状态、精确导出、
可见失败与有效产物保留通过；Python/Luau实验重放保持一致。
M12.4已完成，见[0085](development/0085-editor-simulation-consistency.md)：同一真实窗口中GUI/IPC交替控制CPU/GPU实验，
精确300拍导出与Python脚本重放一致，Stop保留最新Edit/Undo/Redo；响应性子集和GPU同步/寿命回归通过。
M12全部完成。M13.1已完成，见[0089](development/0089-experiment-plan-index.md)：版本化计划稳定展开、
预算预检、实验身份和真实CPU/GPU产物来源核验通过。M13.2已完成，见[0090](development/0090-experiment-batch.md)：
串行批处理、持久进度、CPU数值/GPU图像收集、失败/取消/unknown保留通过。
M13.3已完成，见[0091](development/0091-experiment-recovery.md)：批处理强杀后接续、成功项核验跳过、
原票据/run核实及显式重试保留历史通过。M13.4已完成，见[0092](development/0092-experiment-reports.md)：
参数/seed扫描自动生成可追溯报告，CPU-only与GPU图像分别验收，300拍重跑和跨后端容差通过。
**M0–M15质量与基线交付已完成。M14环境模型、体积底座、天空/日光/空气透视、编辑器与截图闭环见[0094](development/0094-environment-model.md)至[0099](development/0099-environment-integration.md)；质量/性能基线与三预设交付见[0100](development/0100-atmosphere-baseline.md)。1.5ms性能候选目标未达，保留原质量门槛与实测差距。M15.1云CPU模型、environment v2迁移、确定性天气/3D噪声、风时间与有界GPU纹理见[0101](development/0101-cloud-fields.md)。M15.2无历史CPU/GPU积分、太阳光照及收敛参考已完成，见[0102](development/0102-cloud-reference.md)。M15.3云/空气/几何联合合成、诊断和有界截图参考已完成，见[0103](development/0103-cloud-composition.md)。M15.4半分辨率/跳跃/保边与有界每视图时域优化已完成，21项定向验收见[0104](development/0104-cloud-reconstruction.md)，性能候选结果见M15.7。M15.5固定世界云透射阴影、直射接收及资源寿命已完成，21项定向验收见[0105](development/0105-cloud-shadows.md)。M15.6云预设/面板、固定时间序列、Python/Luau和版本化用例报告已完成，见[0106](development/0106-cloud-integration.md)。M15.7四预设固定质量、动态/失败回归与三轮性能/显存交付见[0107](development/0107-cloud-delivery.md)。云增量4ms、联合5.5ms和在途峰值192MiB候选未达；后续优化需另行规划，并维持已冻结质量门槛。**
M16渐进式路径追踪已完成CPU参考、RT/Graph底座及GPU无历史Lambert积分、不可变场景/异步帧和HDR诊断，设计与实测见[0109](development/0109-pathtracing-reference.md)至[0114](development/0114-gpu-pathtracing.md)。
M16.7.1纹理/alpha-mask/双面、固定64槽数组与独立非一致索引能力已完成，含CPU/GPU数值、失败寿命与glTF产物链路，见[0115](development/0115-pathtracing-textures.md)。
M16.7.2显式Lambert/GGX、可见法线混合采样和能量/MIS检验已完成；同时修复高样本GPU补偿求和，55项定向验收见[0116](development/0116-pathtracing-ggx.md)。
M16.8双视图GPU累积、连续样本、tile/reset/poll与实际owner额度完成；47项定向验收见[0117](development/0117-pathtracing-progressive.md)。
M16.9编辑器模式、后台渐进预览、GPU显示交接与共享队列同步完成；真实GUI/IPC、模块关闭回退和定向回归见[0118](development/0118-editor-pathtracing.md)。
M16.10固定spp路径capture、Job进度/取消、输入摘要和线性报告完成；Python/Luau、GUI逐像素一致与CPU-only边界见[0119](development/0119-pathtracing-capture.md)。
**M16.11及交付F完成：三个输入/8 seed固定质量与参考收敛、50项定向检查及三轮性能/显存基线通过，见[0120](development/0120-pathtracing-delivery.md)。1080p整批16.67ms候选、GGX在1024 spp达到2%误差候选未达。M17.1–M17.6及交付G完成：介质CPU/GPU、Editor/Console/IPC/capture与Sponza缓存/计数/调度验收完成，见[0128](development/0128-path-media-performance.md)。实测热GUI预览p95约61ms，三轮额度内；8个Sponza高质量候选仍未达，完整体积路径不承诺60fps。**
编辑器 Play 视口已在 M12 验收，批量实验在 M13 验收；检查点、其他平台与发布打包仍未纳入本轮扩展。
M18.1 镜头与导航设置、共享透视和 GUI/capture 一致性已完成，见[0129](development/0129-camera-settings.md)；M18.2 Fly/水平 Walk、共享控制器与连续渲染验收完成，见[0130](development/0130-camera-fly-walk.md)；M18.3 基础正交、六视图、Unlit inspection 及双视口交互完成，见[0131](development/0131-camera-orthographic.md)。M18.4 路径与固定捕获正交已完成，见[0132](development/0132-pathtrace-orthographic-capture.md)。M18.5 光栅大气/云正交已完成，见[0133](development/0133-raster-orthographic-environment.md)；M18 整体验收完成。
M19.1 CPU reservoir/RIS 与首表面光能契约已完成，48 个唯一定向测试及固定 CPU 基线通过，见[0134](development/0134-restir-reference.md)；
M19.2 GPU 初始 RIS、RTXDI v3.1.0 shader 接入与整批调度/额度已完成，17 GPU 与 18 CPU 定向测试通过，见[0135](development/0135-restir-gpu-initial.md)。
时间/空间复用、介质 RIS、Editor/capture 切换和 DLSS 尚未开始；下一阶段为 M19.3。
默认只推进一个满足前置的小阶段；用户明确指定父阶段或多个小阶段时按授权范围逐节验收。
M5 验收不代表场景渲染、Tracy GPU capture、其他平台或跨文件断电原子性已经验证。

工程基础维护记录包括 [0018](development/0018-vs2026-generation-script.md)、
[0019](development/0019-development-skills.md) 和 [0041](development/0041-ai-documentation-workflow.md)，不改变里程碑顺序。
设计准备记录与实现验收分开：[0020](development/0020-m4-development-plan.md)、
[0022](development/0022-memory-profiling-design.md)、[0093](development/0093-atmosphere-cloud-plan.md)、[0108](development/0108-pathtracing-plan.md)
本身不作为对应功能实现完成证据。
开发编号在实际创建记录时扫描目录取最大值加一，不在导航文档预写下一编号。

## 全阶段完成标准

一个阶段只有满足以下条件才标记完成：

- 相关模块设计已先行建立，代码的依赖方向、接口和生命周期与设计一致。
- 开发记录包含实际文件变更、执行命令、环境、结果及未验证项；
  创建时间不改写，更新最后修改时间和索引。
- 有一个覆盖用户行为的可执行例子或回归场景；
  测试覆盖该阶段真实失败路径，不只检查对象能否构造。
- 保持 CPU-only 能力；GPU 测试单独标记。
  无 GPU 时明确跳过相关测试，不把跳过记作 GPU 验证通过。
- 新依赖按模块显式链接，核验项目与依赖工具链/CRT，并记录实际兼容性；
  vcpkg/项目的不同 MSVC 版本已在 0002 中通过实际链接和测试；
  该结果只覆盖当前配置，依赖或工具链变更时重新检查。
- 涉及异步/资源所有权时验证失败、取消和关闭路径；
  性能优化前先记录基准场景、设备、输入与正确性结果。

每完成一个阶段，更新本文件状态与 updated_at，并链接对应开发记录；
实现范围或顺序变化先同步本路线和相关设计。

## 尚未纳入本轮扩展的工作

跨图形 API 的通用 RHI、多后端、复杂多线程渲染、异步 compute、
GPU 内存 aliasing、通用节点编辑器、完整反射、热重载体系和多种物理求解器
仍按实际需求单独立项设计，不由 M11–M13 的响应性和实验管理工作隐含引入。
模拟检查点、布料弯曲/自碰撞等物理扩展、完整PBR/IBL与光栅混合光追效果另行确定阶段；
M16只规划表面路径追踪的几何可见性/阴影、受限金属粗糙度材质和常量环境，不代表完整光照系统；
M14只引入单太阳/天空漫反射接收，M15只引入云透射阴影，不等于完整光照系统。
M13 的批处理恢复不替代模拟检查点。
Linux/macOS 扩展与发布打包也需要独立的平台验收，不由已有 Ninja 预设推定支持。

## 相关文档

- [架构总览](design/architecture.md)
- [工程基础设计](design/project-foundation.md)
- [spec 流程规范](README.md)
- [设计目录](design/README.md)
- [开发目录](development/README.md)
- [开发留档 skill](../.agents/skills/decker-spec-workflow/SKILL.md)

M4.4 实际验收：[0037](development/0037-cpu-job-queue.md) 队列、[0038](development/0038-async-asset-state.md) 状态、[0039](development/0039-assets-jobs-commands.md) 命令、[0040](development/0040-cpu-assets-delivery.md) CPU 交付。仅运行受影响测试；GPU Ready/硬抢占/超长路径不在本次范围。
