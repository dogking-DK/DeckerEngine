---
id: "0002"
created_at: "2026-09-22T09:41:24+08:00"
updated_at: "2026-09-22T09:54:00+08:00"
status: completed
design_refs:
  - ../design/foundation-core.md
  - ../design/project-foundation.md
---

# 0002 Foundation Core：错误、日志和稳定 ID

## 目标与设计依据

完成 [foundation-core 设计](../design/foundation-core.md) 和
[Roadmap](../roadmap.md) 中 M1 的第一项任务。仅包含错误、日志、ID 与相应构建验证。

## 实际变更

- 已检查工作区和 0001 记录，当前基线 672ac08。
- 已先建立模块设计；采用无第三方 dk::core + 可选 dk::logging，保留 bootstrap。
- [Error.hpp](../../engine/foundation/core/include/dk/core/Error.hpp)、
  [Result.hpp](../../engine/foundation/core/include/dk/core/Result.hpp)：显式错误码、
  错误消息与由内到外的上下文；Result 为 std::expected 别名，支持 void/move-only。
- [StableId.hpp](../../engine/foundation/core/include/dk/core/StableId.hpp) 与
  [实现](../../engine/foundation/core/src/StableId.cpp)：强类型 EntityId/AssetId/SceneId，
  nil、严格解析、小写文本、比较/hash，Windows BCrypt UUIDv4 生成及随机错误返回。
- [Log.hpp](../../engine/foundation/core/include/dk/core/Log.hpp) 与
  [实现](../../engine/foundation/core/src/Log.cpp)：Logger 显式所有权、
  stderr/Unicode 原生路径追加文件、模块/级别过滤、fmt 格式化、flush 和析构回收。
  同步 sink 支持并发调用，关闭前由调用者结束使用线程。
- [Core CMake](../../engine/foundation/core/CMakeLists.txt) 增加 dk::logging；
  fmt 为 PUBLIC，spdlog 为 PRIVATE；core 的系统 bcrypt 为 PRIVATE。
- [选项](../../cmake/Options.cmake)增加 DK_BUILD_LOGGING/DK_BUILD_UNIT_TESTS；
  [vcpkg 集成](../../cmake/Vcpkg.cmake)在 project() 前根据已启用目标追加依赖组，
  baseline 未变；bootstrap 显式关闭两项以保留无第三方构建。
- [runner](../../apps/runner/src/main.cpp)在开启日志时将未知参数诊断写到 stderr，
  保持版本 stdout 和退出码；构建时实际链接 fmt/spdlog 动态库。
- [单元测试](../../tests/unit/CMakeLists.txt)：14 项 Catch2 行为用例，
  加独立日志进程探针；保留版本冒烟测试，共 16 项 CTest。
- README、架构与工程基础设计同步当前能力，Roadmap 标记 M1 部分完成，
  下一步为数学/变换设计与实现。

## 验证记录

Windows x64，CMake 4.2.1、项目 MSVC 19.51.36257.0。
vcpkg 仍选择 VS 2022 / MSVC 14.44.35207 构建依赖，
fmt 12.1.0、spdlog 1.17.0、Catch2 3.13.0#1；
本次已实际链接并运行其 Debug/Release 产物。

| 检查 | 命令/方法 | 结果 |
| --- | --- | --- |
| 开发配置 | cmake --preset windows-dev -DDK_WARNINGS_AS_ERRORS=ON | 通过，自动补充并安装 Catch2 |
| 开发 Debug | cmake --build --preset windows-debug；ctest --preset windows-debug | 编译通过，16/16 通过 |
| 开发 Release | cmake --build --preset windows-release；ctest --preset windows-release | 编译通过，16/16 通过 |
| bootstrap 配置 | cmake --preset windows-bootstrap -DDK_WARNINGS_AS_ERRORS=ON | 通过，日志和 Catch2 均关闭 |
| bootstrap Debug | cmake --build --preset windows-bootstrap-debug；ctest --preset windows-bootstrap-debug | 编译通过，1/1 通过 |
| bootstrap Release | cmake --build --preset windows-bootstrap-release；ctest --preset windows-bootstrap-release | 编译通过，1/1 通过 |
| runner 兼容性 | Python 子进程分别捕获 --version / --unknown 的 stdout、stderr 和退出码 | 版本输出不变；非法参数为退出码 2、stdout 空、stderr 含 runner 日志 |

测试覆盖 ID 1024 次生成/容器往返、512 次跨线程生成、
编译期类型隔离、格式错误/nil/字节序、错误上下文/void/move-only，
以及日志过滤、文件失败、Unicode 路径、追加/析构、独立实例、
256 条并发记录和子进程输出分离。概率性生成测试不是无碰撞的数学证明。

首次默认沙箱执行遇到 vcpkg 外部缓存权限和 MSBuild FileTracker 访问限制；
在授权的用户环境重试后通过。vcpkg 的 Catch2 port 报告一个未使用的
CATCH_CONFIG_EXPERIMENTAL_THREAD_SAFE_ASSERTIONS 配置变量警告；
安装成功，项目测试未在工作线程中调用 Catch2 断言。

## 偏差与决策

日志显式实例化，不使用全局默认 logger；强类型 ID 不依赖 Scene/Assets 实现。
日志文件使用原生路径的 ofstream 接入 spdlog，避免额外公开 Windows 宽字符宏。
查阅固定依赖的实现后，文档明确 stderr 在无控制台句柄下遵循平台 sink 行为；
文件落盘通过显式 flush 检查，spdlog 报告的 IO 错误转回 Result。

## 遗留问题与下一步

本任务范围完成；数学/变换与文件 IO 尚未实现，下一任务从 foundation-math 设计开始。
其他平台的随机源与构建未实测；系统随机失败、磁盘写满/写入中断等系统级故障
未做注入验证。当前日志同步输出，不支持异步队列或多实例写同一文件。
测试工件保留在 out/build/windows-dev/tests/unit/test-artifacts，受 Git 忽略。

## 修改记录

- 2026-09-22T09:41:24+08:00：先创建设计并建立开发记录。
- 2026-09-22T09:54:00+08:00：完成实现、依赖接入及 Debug/Release 验证，同步路线和交接信息。
