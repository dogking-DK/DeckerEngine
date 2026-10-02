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
| 服务、事务、历史、Operations | dk_service_tests | `^dk\.services\.` |
| JSON-RPC、JSON Lines、任务 | dk_protocol_tests | `^dk\.protocol\.` |
| Runtime 的 batch/stdio 进程行为 | dk_run | `^dk\.runtime\.` |
| CLI 版本 | dk_run | `^dk\.bootstrap\.version$` |
| GPU Graph 声明、句柄、内容/循环、编译裁剪与生命周期（无需 GPU） | dk_graph_tests | `^dk\.graph\.` |
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
| 离屏绘制/计算读回、管线寿命与同步验证 | dk_offscreen_probe | `^dk\.offscreen\.gpu_`（gpu label；77 为跳过） |
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
