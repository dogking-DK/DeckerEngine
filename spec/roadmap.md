---
module: roadmap
created_at: "2026-09-22T09:29:25+08:00"
updated_at: "2026-09-22T18:51:42+08:00"
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
M1.6 CPU 集成验收已完成，见 [0008](development/0008-foundation-integration.md)，M1 全部子阶段完成。
M2.1–M2.4 已完成：flecs 文档、组件层级、工程/资产引用和 JSON 安全持久化；
最终验收见 [0012](development/0012-scene-persistence.md)。
M3.1–M3.5 已完成：统一命令、场景服务、内存事务/撤销重做、CPU CLI/stdio 和同步任务，
交付 A 最终验收见 [0017](development/0017-stdio-delivery-a.md)。

默认先交付 Windows x64；CPU-only 构建始终保留。
优先正确性、可观测性和可复现操作，暂不安排复杂并行与性能优化。
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
| M4 资产加载链路 | 导入、缓存、加载状态、异步任务 | M2、M3 | 设计稿已建立，实施待开始 |
| M5 Vulkan / Slang 底座 | 设备资源、shader 编译、离屏输出、最小呈现 | M1 | 待开始 |
| M6 GPU Graph | 资源声明、依赖编译、同步、执行与诊断 | M5 | 待开始 |
| M7 场景渲染 | 资产上传、场景提取、Pass、pipeline、可等待截图 | M3、M4、M6 | 待开始 |
| M8 编辑器与进程控制 | 可编辑保存的视口；dk-ctl 操作运行中的程序 | M7 | 待开始 |
| M9 脚本与自动化 SDK | Lua 场景脚本、Python 客户端、批处理与重放 | M3；集成验收需要 M8 | 待开始 |
| M10 物理实验闭环 | 固定步长、CPU 参照、首个 GPU 求解器及可视化 | M7、M9 | 待开始 |

默认执行表中顺序。M5 在 M1 后已具备独立探索条件，
M9 的 Lua 命令绑定在 M3 后可提前做；若调整次序，仍需满足对应集成验收依赖。
本路线不要求同时开展多个模块。

三个可单独交付的节点：

- **A：M3 完成，已验收。** 无窗口、无 GPU，通过结构化命令生成场景并保存、重载。
- **B：M7 完成。** 从磁盘资产和场景得到可核验的离屏图像，AI 可以等待任务并检查结果。
- **C：M8 完成。** 编辑器和外部 CLI 操作同一套场景服务，保存后可在 runner 重现画面。

M9、M10 将这套基础扩展为可脚本化的渲染与物理实验平台。

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

**暂缓：** 自研通用容器、完整反射、自定义分配器、无锁任务系统、插件框架。

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
这里使用命令序列驱动，不要求 Lua 提前完成。

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
| M4.1 | 资产身份与导入契约（M4.1.1–2） | M3 | meta/登记候选；多文件提交与改名保留 ID、失败可诊断恢复 | 待开始 |
| M4.2 | 首个网格导入与 assetc（M4.2.1–2） | M4.1 | CPU 网格/材质；纹理与离线进程，非法数据有定位错误 | 待开始 |
| M4.3 | 缓存和依赖失效（M4.3.1–2） | M4.2 | 内容键/发布；失效/删除/损坏重建且 ID 不变 | 待开始 |
| M4.4 | 异步加载与任务生命周期（M4.4.1–4） | M4.3 | 队列；资产状态；服务/命令；CPU 进程闭环和退出 | 待开始 |

**设计稿已建立：** [资产身份/缓存/加载](design/assets-runtime.md)、
[导入器/assetc](design/assets-importers.md)、[任务队列](design/foundation-jobs.md)。
详细的 10 个实施小节、独立验收和开工决策见
[0020 M4 开发计划](development/0020-m4-development-plan.md)。默认下一次开发仅推进 **M4.1.1**；
后续按 M4.x.y 顺序，每节记录实际证据后再关闭父阶段。设计稿不代表代码已实现。
三方库已确定为 fastgltf、stb_image（stb port）、PicoSHA2，当前基线版本与封装约定见
[导入器设计](design/assets-importers.md#已确定的三方库与基线)。对应模块实施时接入 vcpkg/CMake 并记录实际链接验证。

**验收：** 同一资产重复导入 ID 保持；合法重命名后引用仍有效；
修改源内容/参数使缓存失效，删除缓存后能重建。
缺失文件、错误纹理和取消任务有终态，不造成退出挂起。
CPU Ready 与未来 GPU Ready 分开。

## M5：Vulkan、窗口与 Slang 底座

**目标：** 证明设备、资源、shader 与读回链路正确。

交付范围：

- Vulkan instance/device/queue、能力检测、错误报告与验证层接入。
- Buffer/Image、VMA、命令提交、完成跟踪、延迟释放；
  首版一个支持所需操作的队列，按完成状态复用和销毁资源。
- Slang 编译、入口/参数约定、诊断、SPIR-V 产物与最小布局反射；
  dk-shaderc 先提供可重复的离线编译入口。
- 离屏资源路径保持不依赖窗口；单独接入 SDL3 与 swapchain，
  覆盖 resize、最小化和重建。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M5.1 | Vulkan 设备与诊断 | M1 | 能力选择、队列、验证层、缺失设备错误可复现 | 待开始 |
| M5.2 | 资源与提交生命周期 | M5.1 | VMA Buffer/Image、上传/读回、完成跟踪和延迟释放 | 待开始 |
| M5.3 | Slang 编译工具 | M5.2 | 图形/compute shader 编译、SPIR-V/反射与失败诊断 | 待开始 |
| M5.4 | 离屏绘制与计算 | M5.3 | 读回图像/计算结果符合预期，验证层无相关错误 | 待开始 |
| M5.5 | 窗口与呈现 | M5.4 | SDL3 swapchain、resize/最小化/重建；离屏路径独立可用 | 待开始 |

**先写设计：** `graphics-device.md`、`graphics-shaders.md`、
`platform.md`、`graphics-presentation.md`。

**验收：** 编译一个图形 shader 和一个 compute shader；
离屏生成小图像并读回、计算已知数据并校验；窗口路径能呈现和调整尺寸。
记录 GPU/驱动、shader 编译参数和验证层结果；
重复创建、提交、回收不出现同步/生命周期错误。
缺少必需 GPU 能力时清晰失败，CPU runner 仍可运行。

本阶段可用小型设备集成测试直接记录命令；它是验证底座的临时路径，
M6/M7 后正式业务统一进入 Graph，不扩展为第二套渲染架构。
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
- 持久资源由外部所有者管理并导入 Graph，禁止保存已失效的临时图句柄。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M6.1 | 图声明与结构校验 | M5 | 资源/Pass 契约可表达；非法引用与循环在提交前拒绝 | 待开始 |
| M6.2 | 依赖编译与资源生命周期 | M6.1 | 拓扑排序、裁剪及 transient 分配计划可检查 | 待开始 |
| M6.3 | 单队列同步与执行 | M6.2 | barrier、layout、外部状态与完成跟踪通过 GPU 验证 | 待开始 |
| M6.4 | 样例迁移与诊断 | M6.3 | upload/compute/draw/readback 全部进入图，重复运行和 M5 结果回归通过 | 待开始 |

**先写设计：** `graphics-graph.md`。

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
- 实现 render.capture、job.status/wait 等操作。
  截图绑定 scene revision/view，完成返回 frame、输出文件和诊断。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M7.1 | 场景提取与 GPU 资源 | M3、M4、M6 | 只读视图、GpuMesh/纹理上传及卸载遵守生命周期 | 待开始 |
| M7.2 | 最小渲染管线 | M7.1 | opaque/depth/tone mapping 由 Graph 执行，程序化场景可回归 | 待开始 |
| M7.3 | 磁盘场景与资产集成 | M7.2 | M4 导入资产和 M2 场景可加载并离屏渲染 | 待开始 |
| M7.4 | 截图任务与自动化验收 | M7.3 | capture/status/wait 绑定 revision/frame，失败有终态，截图可核验；交付 B | 待开始 |

**先写设计：** `render-data.md`、`render-resources.md`、
`render-pipeline.md`、`render-capture.md`；更新 runtime/automation 设计。

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
| M8.1 | 编辑器工作台 | M7 | 窗口、Hierarchy/Inspector/Viewport、保存加载可用 | 待开始 |
| M8.2 | 拾取、选择与交互 | M8.1 | CPU 射线查询/Gizmo/相机；编辑进入命令与撤销链路 | 待开始 |
| M8.3 | IPC 协议和 dk-ctl | M8.2 | Named Pipe、轻量客户端、断连/超时/重复请求有明确语义 | 待开始 |
| M8.4 | 编辑器与外部操作一致性 | M8.3 | 外部编辑、GUI 和截图对应同一 revision，保存后 runner 可重现；交付 C | 待开始 |

**先写设计：** `editor.md`、`editor-interaction.md`、
`geometry-query.md`、`automation-transport.md`、`automation-client.md`。

**验收：** 打开场景、选中/移动物体、修改属性、撤销/重做并保存；
关闭编辑器后 runner 重载得到相同状态。
外部 dk-ctl 修改同一运行中场景，编辑器和截图反映指定 revision。
检查重复请求、断连和超时；超时不能被解释为“操作肯定没有执行”。

## M9：Lua 与 Python 自动化

**目标：** 用脚本组合已存在的操作语义，支持批处理实验。

交付范围：

- Lua/sol2 绑定受控命令与查询，不暴露裸 ECS/Vulkan 指针；
  场景构造/编辑脚本与未来运行态行为脚本使用不同上下文。
- 设计脚本错误、取消、执行时长和可访问能力约束，
  防止脚本无限执行占用编辑器；不宣称嵌入 Lua 自带安全沙箱。
- Python SDK 封装 dk-ctl 或已定义协议，支持超时、任务等待、错误和批量操作。
- 命令记录/重放保存协议版本、输入资产、随机种子和执行上下文。

小阶段与独立验收：

| 子阶段 | 范围 | 前置 | 验收与证据 | 状态 |
| --- | --- | --- | --- | --- |
| M9.1 | Lua 命令绑定 | M3 | 脚本经同一服务创建和编辑场景，脚本错误可恢复 | 待开始 |
| M9.2 | 脚本运行限制与取消 | M9.1 | 执行预算、取消与退出可测试；不承诺安全沙箱 | 待开始 |
| M9.3 | Python 自动化客户端 | M8、M9.2 | 超时/错误/批量操作/任务等待和截图链路可运行 | 待开始 |
| M9.4 | 记录与重放 | M9.3 | 记录版本、资产、随机 seed、上下文并核验重放逻辑状态 | 待开始 |

**先写设计：** `scripting-lua.md`、`automation-python.md`、
`automation-replay.md`。

**验收：** Lua 创建一组实体并保存；Python 批量修改参数、等待截图并收集产物。
同一操作从 GUI/CLI/Lua 进入相同服务校验。
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
| M10.1 | 模拟世界与固定步长 | M7、M9 | Edit/Play 分离、启动/暂停/单步/Stop 不破坏编辑态 | 待开始 |
| M10.2 | 首个 CPU 参考求解器 | M10.1 | 明确单一算法、数据布局、边界条件与数值指标 | 待开始 |
| M10.3 | GPU 求解与可视化 | M10.2 | 图同步正确，小规模 CPU/GPU 对比满足容差 | 待开始 |
| M10.4 | 脚本化实验验收 | M10.3 | 严格 N 步、配置/指标/图像可复现；检查点若需要另立阶段 | 待开始 |

**先写设计：** `physics-api.md`、`physics-<solver>.md`、
`render-<simulation>.md`，并更新 runtime 的固定步长与世界切换设计。
若要交付检查点恢复，另写 `simulation-checkpoint.md`，不得复用场景文件语义。

**验收：** 同一输入和 seed 下严格执行 N 个 step，导出步数/时间/数值指标和图像；
小规模 CPU/GPU 结果在设计容差内。
暂停期间无额外 step，Stop 不覆盖编辑态。
模拟写入与渲染读取由同一图的资源声明建立顺序；
GPU 未完成前不释放状态，也不默认经 CPU 读回再上传。

## 当前执行边界与下一步

已完成 **M1.6 Foundation 集成验收**，设计见 [foundation-integration.md](design/foundation-integration.md)，
记录见 [0008](development/0008-foundation-integration.md)。M1 在当前 Windows 本地文件范围内完成。
CPU 示例串联 ID、仿射变换与安全保存/重载；15 项进程集成测试在无日志/runner/Catch2 的
Debug/Release 配置通过。既有符号链接测试因权限跳过，其他平台和断电恢复不在已验证范围。

M2.1–M2.4 已按连续授权完成，每节均先设计、单独记录、验证和详细本地提交。
默认 Debug/Release 各 128 通过、1 项既有权限跳过；无日志/runner/示例配置各 104 通过、1 跳过；
无日志/runner/Catch2 CPU 示例配置各 16/16。快照精度、旧 revision 保存和失败状态保护均通过。
用户授权的 M3 全部小节已逐节设计、留档、测试并详细本地提交。
M3.1 已完成：默认 Debug/Release 各 133 通过、1 既有权限跳过；独立命令层各 15/15。
M3.2 已完成：默认 Debug/Release 各 137 通过、1 既有权限跳过；独立命令层各 16/16。
M3.3 已完成：默认 Debug/Release 各 143 通过、1 既有权限跳过，原子事务与有界历史通过。
M3.4 已完成：默认 Debug/Release 各 149 通过、1 既有权限跳过；独立 CPU Runtime 各 3/3，bootstrap 各 1/1。
M3.5 已完成：最终默认 Debug/Release 各 152 通过、1 既有权限跳过；独立 CPU Runtime 各 4/4，
bootstrap 各 1/1。真实持续 stdio 在 stdin 打开时即时响应、保存/关闭/EOF/重启均通过，交付 A 完成。
下一项为 **M4.1.1 元数据与身份目录**；M4 设计规划已完成，代码实施尚未开始。

根目录 VS2026 生成脚本作为工程维护完成，见 [0018](development/0018-vs2026-generation-script.md)，不改变里程碑顺序。
三个开发辅助 skill 与定向验证脚本已作为工程维护完成，见
[0019](development/0019-development-skills.md)，不改变里程碑顺序。
M4 开发安排与三份设计稿见 [0020](development/0020-m4-development-plan.md)，
该编号仅记录设计准备，不作为任何 M4 实现小节的完成证据。
当前下一可用开发编号为 **0021**；实际开工时重新扫描
[开发目录](development/README.md)，取最大编号加一。
重要设计准备或实际开发开始时创建编号记录，不预建未来小节的空日志；
子阶段状态和验收证据在本文件及对应实施记录持续维护。

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

## 暂不进入首轮路线的工作

跨图形 API 的通用 RHI、多后端、复杂多线程渲染、异步 compute、
GPU 内存 aliasing、通用节点编辑器、完整反射、热重载体系和多种物理求解器
留到可运行闭环之后，按实际需求单独立项设计。
Linux/macOS 扩展与发布打包也需要独立的平台验收，不由已有 Ninja 预设推定支持。

## 相关文档

- [架构总览](design/architecture.md)
- [工程基础设计](design/project-foundation.md)
- [spec 流程规范](README.md)
- [设计目录](design/README.md)
- [开发目录](development/README.md)
- [开发留档 skill](../.agents/skills/decker-spec-workflow/SKILL.md)
