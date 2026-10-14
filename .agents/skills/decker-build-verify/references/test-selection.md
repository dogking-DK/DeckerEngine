# 测试选择入口

表中 regex 是查找候选用例的范围，不是每次修改都要执行的固定套件。
先结合 [单元测试注册](../../../../tests/unit/CMakeLists.txt) 与
[集成测试注册](../../../../tests/integration/CMakeLists.txt) 确认当前名称，再缩小到目标行为。

| 相关行为 | 构建 target | CTest 查找范围 |
| --- | --- | --- |
| Error、StableId、Logger | dk_core_tests | `^dk\.core\.` |
| stdout/stderr 日志分离 | dk_log_probe | `^dk\.core\.log_stream_separation$` |
| Eigen 基础数学/Transform | dk_math_tests | `^dk\.math\.` |
| 路径、字节 IO、原子保存 | dk_io_tests | `^dk\.io\.` |
| Memory heap、拥有型接口、arena/pool、context/token | dk_memory_tests | `^dk\.memory\.`（探针/benchmark 另见下列入口） |
| heap 分配/跨线程释放与采集探针 | dk_memory_probe | `^dk\.memory\.probe$` |
| ScratchArena 用量与采集探针 | dk_arena_probe | `^dk\.memory\.arena_probe$` |
| Pool 复用与采集探针 | dk_pool_probe | `^dk\.memory\.pool_probe$` |
| 多系统上下文与 worker 退休探针 | dk_context_probe | `^dk\.memory\.context_probe$` |
| 重复工作负载 smoke、参数/部分失败 | dk_memory_benchmark | `^dk\.memory\.benchmark_` |
| CPU profiling 探针 | dk_profiling_probe | `^dk\.profiling\.smoke$` |
| 固定300拍模拟基准、参数拒绝、聚合防误报、32×32 GPU数值与绘制 | dk_simulation_benchmark | `^dk\.simulation\.benchmark_`；GPU要求RenderSimulation，实际基线用scripts/benchmark-simulation.py |
| GPU timestamp、Tracy开关、数值/像素一致与查询寿命 | dk_gpu_profiling_probe | `^dk\.profiling\.gpu_validation$`（gpu label；77 为跳过）；真实 capture 用 scripts/capture-gpu-profiling.ps1 |
| profiling 关闭时无副作用 | dk_profiling_disabled_test | `^dk\.profiling\.disabled_no_side_effects$` |
| Jobs 排队、取消、完成、关闭与保留上限 | dk_jobs_tests | `^dk\.jobs\.` |
| Scene、Project、JSON 持久化 | dk_scene_tests | `^dk\.scene\.` |
| Asset meta/身份目录、登记/改名/补偿、Project 发布 | dk_asset_tests | `^dk\.assets\.`（restart 探针另见下行） |
| Asset 未完成操作的跨进程恢复（Windows/Services） | dk_asset_recovery_probe | `^dk\.assets\.restart_` |
| CPU glTF 网格/纹理导入与预算 | dk_import_tests | `^dk\.import\.` |
| CPU 产物编解码、meta/发布失败保护 | dk_asset_pipeline_tests | `^dk\.pipeline\.` |
| assetc Unicode/JSON/重导入真实进程 | dk_assetc | `^dk\.assetc\.` |
| 资产缓存 key/命中/发布/失效清理 | dk_asset_cache_tests | `^dk\.cache\.` |
| 异步资产候选、取消/代际、owner 发布与 Ready 寿命 | dk_async_asset_tests | `^dk\.async_assets\.` |
| 资产/作业命令 schema、guard 与服务接入 | dk_asset_command_tests | `^dk\.asset_commands\.` |
| CPU 资产真实 stdio、重启与关闭 | dk_run | `^dk\.runtime\.assets_stdio$` |
| 命令注册、schema | dk_commands_tests | `^dk\.commands\.` |
| Luau 绑定、值转换、预算、取消/关闭和能力模式 | dk_luau_tests | `^dk\.luau\.`（含 Windows 控制台取消；target 自动构建 runner 夹具，CLI 进程测试另见下行） |
| Luau runner 创建/保存重载、预算参数和诊断 | dk_run | `^dk\.luau\.runner_roundtrip$` |
| 服务、事务、历史、Operations | dk_service_tests | `^dk\.services\.` |
| Edit/Play、固定纳秒时钟、有限任务暂停/取消、故障与关闭 | dk_simulation_tests | `^dk\.simulation\.(fixed clock \|simulation \|finite )` |
| 真实有限任务IPC、冷初始化控制、三规模300拍、数值与资源退出 | dk_run、dk_ctl | `^dk\.simulation\.task_(cpu\|gpu)$`（Windows/Python；GPU要求RenderSimulation） |
| 有限任务响应汇总、真实runner/GUI心跳smoke | dk_run、dk_ctl、dk_editor_app | `^dk\.simulation\.response_(aggregation\|runner_smoke\|editor_smoke)$`；正式每控制项至少20样本，使用scripts/benchmark-simulation-response.py |
| GUI共享设备双队列、三规模300拍、导出与暖进程关闭 | dk_editor_app、dk_ctl | `^dk\.simulation\.task_editor_gpu$`（required及同步validation） |
| CPU XPBD解析解、布片、分组、接触、数值与失败回滚 | dk_xpbd_tests | `^dk\.xpbd\.` |
| GPU 命令、Python/Luau 严格步数实验与配置/指标/图像重放 | dk_run、dk_ctl | `^dk\.simulation\.experiment_gpu$`（RenderSimulation + Luau + Python，gpu label） |
| 真实 stdio/pipe 空闲模拟、暂停与关闭 | dk_run、dk_ctl | `^dk\.simulation\.(stdio\|pipe)$`（Windows，Python 3.11+） |
| JSON-RPC、JSON Lines、任务 | dk_protocol_tests | `^dk\.protocol\.` |
| IPC ticket、断连/超时、分帧、有界队列、owner 分派 | dk_ipc_tests | `^dk\.ipc\.(IPC \|Named pipe \|Disconnected \|Pipe )` |
| runner 与轻量客户端真实进程、重启与 Unicode 参数 | dk_run、dk_ctl | `^dk\.ipc\.runner_client$` |
| Python 客户端参数/错误/截止时间与 CPU IPC 批量/重载 | dk_run、dk_ctl | `^dk\.python\.(unit\|cpu)$`（需要 Python 3.11+） |
| 实验计划稳定展开、预算拒绝、索引来源/摘要/失败保护 | dk_run、dk_ctl | `^dk\.experiments\.(unit\|index_cpu)$`（Python；CPU-only 可独立运行） |
| 实验计划与真实 GPU 五文件导出来源核验 | dk_run、dk_ctl | `^dk\.experiments\.index_gpu$`（RenderSimulation，gpu label） |
| 实验批处理串行调度、进度/故障保护、CPU-only 数值收集 | dk_run、dk_ctl | `^dk\.experiments\.batch_(unit\|cpu)$`（Python；CPU验收使用无RenderSimulation构建） |
| 实验批处理CPU/GPU图像导出、真实取消与进程互斥 | dk_run、dk_ctl | `^dk\.experiments\.batch_gpu$`（RenderSimulation；gpu label，要求可用设备） |
| 实验恢复、票据核实、强杀后接续与显式重试 | dk_run、dk_ctl | `^dk\.experiments\.recovery_(unit\|cpu)$`（CPU-only独立验收） |
| GPU实验恢复、导出发布中断与重试证据保留 | dk_run、dk_ctl | `^dk\.experiments\.recovery_gpu$`（RenderSimulation；gpu label，要求可用设备） |
| 实验报告、数值容差/缺失/发布保护、CPU参数扫描与重跑 | dk_run、dk_ctl | `^dk\.experiments\.report_(unit\|cpu)$`（CPU-only独立验收） |
| CPU/GPU扫描、图像证据与300拍容差重跑报告 | dk_run、dk_ctl | `^dk\.experiments\.report_gpu$`（RenderSimulation；gpu label，要求可用设备） |
| Python 批量编辑、截图等待与产物收集 | dk_run、dk_ctl | `^dk\.python\.capture_gpu$`（需要 Python 3.11+ 和 render capture；gpu label） |
| 记录格式/输入/失败与 Python SDK 单元回归 | dk_run、dk_ctl | `^dk\.python\.unit$`（需要 Python 3.11+） |
| 跨进程记录/重放、身份/事务/文件/漂移拒绝 | dk_run、dk_ctl | `^dk\.replay\.cpu$`（启用 Luau 时从脚本保存的工程开始） |
| 记录/重放截图及 Job/逻辑状态核验 | dk_run、dk_ctl | `^dk\.replay\.gpu$`（需要 Python 3.11+ 和 render capture；gpu label） |
| 独立客户端帮助入口（windows-client，无 Runtime/Renderer） | dk_ctl | `^dk\.ipc\.client_help$` |
| 模拟面板草稿/旧run保护、GUI与IPC共享控制 | dk_editor_tests | `^dk\.editor\.workspace simulation` |
| CPU/GPU模拟面板与实时视口、暂停像素、相机/窗口/Stop/关闭及场景隔离 | dk_editor_app | `^dk\.editor\.simulation_gpu_validation$`（required及同步validation） |
| 模拟GUI/IPC交替控制、精确300拍、导出/脚本重放及最新Edit/history保护（M12.4） | dk_editor_app、dk_ctl | `^dk\.editor\.simulation_consistency_gpu_validation$`（CPU/GPU，required及同步validation） |
| 编辑器 IPC 草稿/manifest 刷新（无需 GPU） | dk_editor_tests | `^dk\.editor\.workspace IPC ` |
| 编辑器外部 IPC 与窗口退出 | dk_editor_app、dk_run、dk_ctl | `^dk\.ipc\.editor_gpu_validation$`（gpu label） |
| GUI/IPC 指定 revision、视口/截图像素与保存后 runner 重现（M8.4） | dk_editor_app、dk_run、dk_ctl | `^dk\.editor\.consistency_gpu_validation$`（gpu label） |
| 截图 schema/guard、取消关闭、导入失败（无需 GPU） | dk_protocol_tests | `^dk\.protocol\.capture ` |
| Runtime 截图、版本/像素、原子发布和退出 | dk_run | `^dk\.runtime\.capture_gpu_validation$`（gpu label；77 为跳过） |
| Runtime 的 batch/stdio 进程行为 | dk_run | `^dk\.runtime\.` |
| 编辑器选择/草稿、相机、Gizmo 事务/撤销、保存重载与失败保护（无需 GPU） | dk_editor_tests | `^dk\.editor\.(workspace \|runtime \|camera \|interaction )` |
| CPU 射线、AABB、三角形 BVH 和仿射实例 | dk_geometry_tests | `^dk\.geometry\.` |
| 编辑器拾取、Gizmo、相机真实输入和 runner 重载 | dk_editor_app、dk_run | `^dk\.editor\.interaction_gpu_validation$`（gpu label；77 为跳过） |
| 编辑器真实窗口、ImGui 输入、场景预览与保存重载 | dk_editor_app | `^dk\.editor\.workbench_gpu_validation$`（gpu label；77 为跳过） |
| CLI 版本 | dk_run | `^dk\.bootstrap\.version$` |
| RenderScene/RenderView 提取、版本与 CPU 寿命 | dk_render_data_tests | `^dk\.render\.data\.` |
| CPU路径追踪解析/收敛、MIS/RR、采样/相机/输入快照与预算/取消/内存失败 | dk_pathtracing_reference_tests | `^dk\.pathtracing\.[a-z]+ `（CPU-only，单元测试名首词后的空格排除demo_cpu） |
| CPU路径追踪示例、线性/PPM产物、重复样本与非法输入保护 | dk_pathtracing_reference_demo | `^dk\.pathtracing\.demo_cpu$`（Python 3.11+，CPU-only） |
| GPU 资产输入校验、缓存空/关闭状态（无需 GPU） | dk_gpu_asset_tests | `^dk\.render\.assets\.` |
| GpuMesh/纹理 Graph 上传、字节读回、缓存与在途卸载 | dk_render_resources_probe | `^dk\.render\.gpu_`（gpu label；77 为跳过） |
| Render 管线参数、矩阵 float 转换与空帧（无需 GPU） | dk_render_pipeline_tests | `^dk\.render\.pipeline\.` |
| 场景 depth/opaque/tone 图像、HDR 传输与帧失败/寿命 | dk_render_pipeline_probe | `^dk\.render\.pipeline_gpu_validation$`（gpu label；77 为跳过） |
| Lambert 日光/天空漫射、法线/双面/alpha-mask、失败及在途寿命 | dk_surface_sunlight_probe | `^dk\.render\.sunlight_(normals\|direct\|diffuse\|failures)_gpu_validation$`（required GPU，同步 validation，无跳过） |
| 视锥空气透视/透视深度/HDR读回、遮挡/曝光/失败寿命 | dk_aerial_perspective_probe | `^dk\.render\.aerial_(numeric\|composition\|failures)_gpu_validation$`（required GPU，同步 validation，无跳过） |
| 日光接收正午/日落/暮光球体离屏样例 | dk_sunlight_demo | `^dk\.render\.sunlight_demo_gpu_validation$`（required GPU） |
| 后台环境预览旧代/尺寸替换、取消、失败恢复与在途关闭 | dk_environment_preview_tests | `^dk\.editor\.environment_worker_gpu_validation$`（required GPU） |
| 环境GUI/IPC、冻结截图与实际Python/Luau闭环 | dk_editor_app / dk_run / dk_ctl | `^dk\.editor\.environment_consistency_gpu_validation$`（required GPU；Luau开启） |
| 磁盘 Scene/glTF/CPU 产物、身份/预算与只读失败保护 | dk_render_disk_tests | `^dk\.render\.disk\.` |
| 磁盘到 GPU 图像、alpha mask、重载与上传部分失败 | dk_render_disk_probe | `^dk\.render\.disk_gpu_validation$`（gpu label；77 为跳过） |
| 默认 Sponza 磁盘渲染示例 | dk_render_demo | `^dk\.render\.sponza_validation$`（gpu/local-assets；77 为跳过） |
| GPU Graph 声明、编译裁剪、生命周期与诊断（无需 GPU） | dk_graph_tests | `^dk\.graph\.graph ` |
| GPU Graph 同步、执行、导入导出、诊断与失败保护 | dk_graph_probe | `^dk\.graph\.gpu_`（gpu label；77 为跳过） |
| Vulkan 设备策略、缺失环境和失败清理（无需 GPU） | dk_device_tests | `^dk\.device\.unit\.` |
| RT三态请求、必需能力/limits、BDA/VMA与共享寿命 | dk_raytracing_device_probe | `^dk\.device\.rt_required_validation$`（required RT GPU/validation，同步验证；缺环境失败，非77跳过） |
| BLAS/TLAS输入、地址/尺寸、scratch与实例纯策略 | dk_acceleration_structure_tests | `^dk\.acceleration\.unit\.` |
| 拥有型AS构建、候选失败、pending/关闭寿命 | dk_acceleration_structure_probe | `^dk\.acceleration\.required_validation$`（required RT GPU/validation/sync；缺环境失败） |
| SBT 对齐、ray type/实例索引、payload 与 AS descriptor limits | dk_raytracing_tests | `^dk\.raytracing\.unit\.` |
| RT pipeline/SBT、命中与遮挡数值、AS descriptor、失败与在途关闭 | dk_raytracing_probe | `^dk\.raytracing\.required_validation$`（required RT GPU/validation/sync；缺环境失败） |
| SDL3 窗口事件、尺寸与寿命（需桌面） | dk_platform_probe | `^dk\.platform\.windows$` |
| Vulkan Surface、呈现选卡与窗口保活 | dk_present_device_probe | `^dk\.presentation\.device_validation$` |
| 交换链 capability 策略与错误分类（无需 GPU） | dk_presentation_tests | `^dk\.presentation\.unit\.` |
| 窗口三角形、帧同步、实际图像读回与恢复 | dk_presentation_probe | `^dk\.presentation\.(frames\|recovery)_validation$` |
| 窗口三角形公开接口示例 | dk_presentation_demo | `^dk\.presentation\.example_smoke$` |
| GPU XPBD数值/同图绘制/失败与寿命 | dk_gpu_xpbd_probe | `^dk\.xpbd\.gpu_validation$`（gpu label；77 为跳过） |
| Vulkan 真设备、验证消息、VMA 分配与双设备销毁 | dk_device_probe | `^dk\.device\.gpu_`（gpu label；77 为跳过） |
| Vulkan 资源描述、范围/对齐/布局与空对象（无需 GPU） | dk_graphics_resource_tests | `^dk\.graphics\.unit\.` |
| 已完成图像跨队列交接、同设备限制、提交失败和源队列提前销毁 | dk_image_transfer_probe | `^dk\.graphics\.image_transfer_validation$`（required及同步validation） |
| 大气双精度参考、解析透射、相函数与分段合成 | dk_atmosphere_tests | `^dk\.atmosphere\.[^_]+$` |
| 大气正午/日落/高空离屏样例 | dk_atmosphere_demo | `^dk\.atmosphere\.demo_gpu_validation$`（真实GPU和同步validation必需） |
| 大气 LUT/天空参考、缓存失效、失败与寿命 | dk_atmosphere_probe | `^dk\.atmosphere\.(reference\|multiple\|cache\|vacuum\|continuity)_gpu_validation$`（真实GPU和同步validation必需，不允许跳过） |
| 大气性能采集与证据汇总 | dk_atmosphere_benchmark | `^dk\.atmosphere\.benchmark_(gpu_validation\|aggregation)$`（GPU采集必须required validation；性能另用RelWithDebInfo关闭validation） |
| 3D RGBA16F/32F存储、过滤、传输与深度采样/Graph屏障/失败寿命 | dk_volume_image_probe | `^dk\.graphics\.volume_validation$`（真实GPU和同步validation必需，不允许跳过） |
| VMA 上传/读回、timeline、延迟释放与提交失败 | dk_graphics_resource_probe | `^dk\.graphics\.gpu_`（gpu label；77 为跳过） |
| Slang 基线/RT 阶段编译、反射、诊断与 Memory 寿命（无需 GPU） | dk_shader_tests | `^dk\.shaders\.` |
| shaderc Unicode/include/import/宏、原子输出与隔离部署 | dk_shaderc | `^dk\.shaderc\.` |
| 离屏 draw/dispatch 参数、SPIR-V/布局和设备限制（无需 GPU） | dk_offscreen_tests | `^dk\.offscreen\.unit\.` |
| Graph 完整管线、离屏绘制/计算读回、M5 基线与同步寿命 | dk_offscreen_probe | `^dk\.offscreen\.gpu_`（gpu label；77 为跳过） |
| Foundation 进程示例 | dk_foundation_demo | `^dk\.foundation\.` |
| 确定性云场、密度/风时间与旧代预算（CPU-only） | dk_cloud_field_tests | `^dk\.clouds\.cloud ` |
| 云天气/3D纹理上传、GPU过滤采样、失败与代次寿命（required GPU） | dk_cloud_field_probe | `^dk\.clouds\.fields_gpu_validation$` |
| 云球壳、解析解、混合合成、收敛参考、优化预算与云影投影（CPU-only） | dk_cloud_reference_tests | `^dk\.clouds\.(reference \|composition \|optimization \|shadow )` |
| 无历史云参考GPU解析解、CPU对照、收敛和失败保护（required GPU） | dk_cloud_reference_probe | `^dk\.clouds\.reference_gpu_validation$` |
| 云/空气/几何合成、诊断、CPU对照、遮挡和失败寿命（required GPU） | dk_cloud_composition_probe | `^dk\.clouds\.(numeric\|composition\|failures)_gpu_composition$` |
| 半尺寸云/跳跃/早停/低角度光照、时域反遮挡、多视图重置与失败寿命（required GPU） | dk_cloud_composition_probe | `^dk\.clouds\.(optimization\|optimization_switches\|optimization_boundaries\|temporal\|history)_gpu_composition$` |
| 固定世界云影、独立云柱/地表数值、风/相机稳定、直射隔离、失败/预算/在途寿命（required GPU） | dk_cloud_composition_probe | `^dk\.clouds\.shadow_(numeric\|surface\|failures)_gpu_composition$`（同步validation，无跳过） |
| 云截图冻结状态/时间、schema/quality、零云退化及失败保留文件（required GPU） | dk_run | `^dk\.clouds\.capture_gpu_composition$`（Windows/Python 3.11+） |
| 四预设收敛参考、静态/动态/云内外质量与GPU观测生命周期（required GPU） | dk_cloud_benchmark | `^dk\.clouds\.(quality_.*\|benchmark)_gpu_validation$` |
| 云性能报告完整性、匹配基线、云影/LUT区间与计时缺失拒绝（Python CPU） | dk_cloud_benchmark | `^dk\.clouds\.benchmark_aggregation$` |
| 云用例清单、提交/产物来源、线性比较和缺失证据（Python CPU） | dk_run、dk_ctl | `^dk\.clouds\.cases_unit$` |
| 固定时间序列、时域重放、云影、失败/取消与Python/Luau示例（required GPU） | dk_run、dk_ctl | `^dk\.clouds\.sequence_gpu_validation$` |
| 云预设、GUI/IPC/截图/重载与脚本一致性（required GPU） | dk_editor_app、dk_run、dk_ctl | `^dk\.editor\.cloud_consistency_gpu_validation$`（后台窗口、同步validation） |
| 环境命令发现、CPU示例与v1/v2跨进程保存重载 | dk_run | `^dk\.environment\.process$` |
| Scene 跨进程往返 | dk_scene_demo | `^dk\.scene_demo\.` |

Core 查找范围包含单独的日志探针：选择该用例时也要构建 dk_log_probe。
Memory 范围同时包含多个探针和 benchmark；Assets 范围包含 restart 探针。
先按候选测试名缩小筛选，或补齐所选程序的构建 target；不能仅构建单元测试后运行整个前缀。
float/double 参数化用例需保留与改动相关的两种类型；不要因为名称相似漏掉实际受影响用例。
只有在对应构建选项开启时才存在相关 target；完整开发预设为 windows-dev，其他配置见[构建指南](../../../../spec/guides/build.md)。
新增/删除测试程序时同步此表；check-spec 静态校验 target 存在及 tests 目录的程序覆盖。
它不解析 CMake 条件、验证所有 regex 或替代实际 CTest 枚举。Tracy capture/readback 的额外工具见[Memory 指南](../../../../spec/guides/memory.md)。

## 按影响选择

- 单个算法修复：对应行为和失败案例；调用契约未变时无需测试所有上层模块。
- 公共 ID、序列化格式或状态语义变化：加入真实受影响消费者，例如持久化往返或命令边界。
- 新增业务命令：注册/服务相关用例，加一条实际调用；协议未改变时无需所有传输测试。
- 协议流行为变化：协议用例和相关进程用例，验证 stdin 未关闭时的响应、退出与输出分离。
- IDE 分组：检查生成的 slnx 项目集合/依赖/分组，必要时构建冒烟；不因 CMake 文件变化固定全量。
- 三方库或工具链变化：核对公开依赖及实际链接消费者，按影响决定独立配置或扩大验证。

`-Full` 只表示当前构建树全部测试，不等于所有可选配置、所有平台都通过。
构建成功后才枚举并执行所选测试；Catch2 的 PRE_TEST 发现可能运行测试程序的列举入口，
这不等于执行其全部测试案例。
