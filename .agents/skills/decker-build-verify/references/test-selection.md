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
| Scene、Project、JSON 持久化 | dk_scene_tests | `^dk\.scene\.` |
| 命令注册、schema | dk_commands_tests | `^dk\.commands\.` |
| 服务、事务、历史、Operations | dk_service_tests | `^dk\.services\.` |
| JSON-RPC、JSON Lines、任务 | dk_protocol_tests | `^dk\.protocol\.` |
| Runtime 的 batch/stdio 进程行为 | dk_run | `^dk\.runtime\.` |
| CLI 版本 | dk_run | `^dk\.bootstrap\.version$` |
| Foundation 进程示例 | dk_foundation_demo | `^dk\.foundation\.` |
| Scene 跨进程往返 | dk_scene_demo | `^dk\.scene_demo\.` |

Core 查找范围包含单独的日志探针：选择该用例时也要构建 dk_log_probe。
float/double 参数化用例需保留与改动相关的两种类型；不要因为名称相似漏掉实际受影响用例。
只有在对应构建选项开启时才存在相关 target；完整开发预设为 windows-dev，其他配置查 README。

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
