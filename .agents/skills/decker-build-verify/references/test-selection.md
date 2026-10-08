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
| JSON-RPC、JSON Lines、任务 | dk_protocol_tests | `^dk\.protocol\.` |
| IPC ticket、断连/超时、分帧、有界队列、owner 分派 | dk_ipc_tests | `^dk\.ipc\.(IPC \|Named pipe \|Disconnected \|Pipe )` |
| runner 与轻量客户端真实进程、重启与 Unicode 参数 | dk_run、dk_ctl | `^dk\.ipc\.runner_client$` |
| Python 客户端参数/错误/截止时间与 CPU IPC 批量/重载 | dk_run、dk_ctl | `^dk\.python\.(unit\|cpu)$`（需要 Python 3.11+） |
| Python 批量编辑、截图等待与产物收集 | dk_run、dk_ctl | `^dk\.python\.capture_gpu$`（需要 Python 3.11+ 和 render capture；gpu label） |
| 记录格式/输入/失败与 Python SDK 单元回归 | dk_run、dk_ctl | `^dk\.python\.unit$`（需要 Python 3.11+） |
| 跨进程记录/重放、身份/事务/文件/漂移拒绝 | dk_run、dk_ctl | `^dk\.replay\.cpu$`（启用 Luau 时从脚本保存的工程开始） |
| 记录/重放截图及 Job/逻辑状态核验 | dk_run、dk_ctl | `^dk\.replay\.gpu$`（需要 Python 3.11+ 和 render capture；gpu label） |
| 独立客户端帮助入口（windows-client，无 Runtime/Renderer） | dk_ctl | `^dk\.ipc\.client_help$` |
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
| GPU 资产输入校验、缓存空/关闭状态（无需 GPU） | dk_gpu_asset_tests | `^dk\.render\.assets\.` |
| GpuMesh/纹理 Graph 上传、字节读回、缓存与在途卸载 | dk_render_resources_probe | `^dk\.render\.gpu_`（gpu label；77 为跳过） |
| Render 管线参数、矩阵 float 转换与空帧（无需 GPU） | dk_render_pipeline_tests | `^dk\.render\.pipeline\.` |
| 场景 depth/opaque/tone 图像、HDR 传输与帧失败/寿命 | dk_render_pipeline_probe | `^dk\.render\.pipeline_gpu_validation$`（gpu label；77 为跳过） |
| 磁盘 Scene/glTF/CPU 产物、身份/预算与只读失败保护 | dk_render_disk_tests | `^dk\.render\.disk\.` |
| 磁盘到 GPU 图像、alpha mask、重载与上传部分失败 | dk_render_disk_probe | `^dk\.render\.disk_gpu_validation$`（gpu label；77 为跳过） |
| 默认 Sponza 磁盘渲染示例 | dk_render_demo | `^dk\.render\.sponza_validation$`（gpu/local-assets；77 为跳过） |
| GPU Graph 声明、编译裁剪、生命周期与诊断（无需 GPU） | dk_graph_tests | `^dk\.graph\.graph ` |
| GPU Graph 同步、执行、导入导出、诊断与失败保护 | dk_graph_probe | `^dk\.graph\.gpu_`（gpu label；77 为跳过） |
| Vulkan 设备策略、缺失环境和失败清理（无需 GPU） | dk_device_tests | `^dk\.device\.unit\.` |
| SDL3 窗口事件、尺寸与寿命（需桌面） | dk_platform_probe | `^dk\.platform\.windows$` |
| Vulkan Surface、呈现选卡与窗口保活 | dk_present_device_probe | `^dk\.presentation\.device_validation$` |
| 交换链 capability 策略与错误分类（无需 GPU） | dk_presentation_tests | `^dk\.presentation\.unit\.` |
| 窗口三角形、帧同步、实际图像读回与恢复 | dk_presentation_probe | `^dk\.presentation\.(frames\|recovery)_validation$` |
| 窗口三角形公开接口示例 | dk_presentation_demo | `^dk\.presentation\.example_smoke$` |
| Vulkan 真设备、验证消息、VMA 分配与双设备销毁 | dk_device_probe | `^dk\.device\.gpu_`（gpu label；77 为跳过） |
| Vulkan 资源描述、范围/对齐/布局与空对象（无需 GPU） | dk_graphics_resource_tests | `^dk\.graphics\.unit\.` |
| VMA 上传/读回、timeline、延迟释放与提交失败 | dk_graphics_resource_probe | `^dk\.graphics\.gpu_`（gpu label；77 为跳过） |
| Slang 三阶段编译、反射、诊断与 Memory 寿命（无需 GPU） | dk_shader_tests | `^dk\.shaders\.` |
| shaderc Unicode/include/import/宏、原子输出与隔离部署 | dk_shaderc | `^dk\.shaderc\.` |
| 离屏 draw/dispatch 参数、SPIR-V/布局和设备限制（无需 GPU） | dk_offscreen_tests | `^dk\.offscreen\.unit\.` |
| Graph 完整管线、离屏绘制/计算读回、M5 基线与同步寿命 | dk_offscreen_probe | `^dk\.offscreen\.gpu_`（gpu label；77 为跳过） |
| Foundation 进程示例 | dk_foundation_demo | `^dk\.foundation\.` |
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
