# 设计文档索引

| 模块 | 文档 | 状态 | 范围 | 源码入口 | 测试入口 |
| --- | --- | --- | --- | --- | --- |
| architecture | [整体架构](architecture.md) | accepted | 长期模块边界和依赖方向 | [模块目录](../../engine) | [流程/检查](../README.md) |
| project-foundation | [工程基础](project-foundation.md) | accepted | Git、目录、CMake、vcpkg、构建探针与留档 | [CMake/构建](../../cmake) | [文档脚本回归](../../scripts/test-check-spec.ps1) |
| foundation-core | [Core 基础](foundation-core.md) | accepted | 错误、日志、stduuid 稳定 ID 与验证 | [Core](../../engine/foundation/core) | [core / log_probe](../../tests/unit/CMakeLists.txt) |
| foundation-math | [Eigen 数学与 Transform](foundation-math.md) | accepted | M1.2 基础数学；M1.3 TRS、仿射组合和逆变换 | [Math](../../engine/foundation/math) | [math](../../tests/unit/CMakeLists.txt) |
| foundation-io | [工程路径与文件 IO](foundation-io.md) | accepted | M1.4 路径/字节 IO；M1.5 同目录临时文件与安全替换 | [IO](../../engine/foundation/io) | [io](../../tests/unit/CMakeLists.txt) |
| foundation-integration | [Foundation 集成验收](foundation-integration.md) | accepted | M1.6 独立 CPU 示例、ID/变换/安全保存和跨进程重载 | [CPU 示例](../../examples/foundation) | [foundation](../../tests/integration/CMakeLists.txt) |
| foundation-memory | [Memory System](foundation-memory.md) | accepted | M1.7.2–7 heap、拥有型接口、scratch/pool、context/token、关闭集成及重复工作负载基线已完成 | [Memory](../../engine/foundation/memory) | [memory / probes](../../engine/foundation/memory/tests) |
| foundation-profiling | [Tracy 性能分析](foundation-profiling.md) | accepted | CPU、heap 事件、arena/pool 曲线与三配置开销基线已完成；GPU 观测待实现 | [Profiling](../../engine/foundation/profiling) | [profiling / probes](../../tests/integration/CMakeLists.txt) |
| scene | [场景文档](scene.md) | accepted | M2 flecs 身份、层级、快照、安全保存与重载 | [Scene](../../engine/scene) | [scene](../../tests/unit/CMakeLists.txt) |
| assets-types | [资产类型](assets-types.md) | accepted | M2.3 持久引用与种类 | [资产类型](../../engine/assets/types) | [scene / assets](../../tests/unit/CMakeLists.txt) |
| assets-runtime | [资产身份、缓存与加载](assets-runtime.md) | accepted | M4.1–4 身份/持久化、CPU 产物、缓存、异步 Ready 和生命周期完成 | [资产运行时](../../engine/assets/runtime) | [assets / pipeline / cache / async_assets](../../tests/unit/CMakeLists.txt) |
| assets-importers | [静态 glTF 导入与 assetc](assets-importers.md) | accepted | M4.2 静态网格/材质/纹理导入、CPU 产物 v1 和离线工具已完成 | [导入器](../../engine/assets/importers) | [import / pipeline](../../tests/unit/CMakeLists.txt) |
| foundation-jobs | [CPU 队列与后台作业](foundation-jobs.md) | accepted | M4.4 有界 JobId 队列、取消/等待/退出与命令接入完成 | [Jobs](../../engine/foundation/jobs) | [jobs](../../tests/unit/JobQueueTests.cpp) |
| project-format | [工程格式](project-format.md) | accepted | M2.3 版本清单、路径与文件诊断 | [Project](../../engine/scene) | [scene / assets](../../tests/unit/ProjectTests.cpp) |
| commands | [命令注册](commands.md) | accepted | M3.1 schema、注册与能力发现 | [Commands](../../engine/framework/commands) | [commands](../../tests/unit/CommandTests.cpp) |
| application-services | [场景应用服务](application-services.md) | accepted | 场景/资产服务、guard、事务/历史与 Project 映射同步 | [Services](../../engine/framework/services) | [services / asset_commands](../../tests/unit/CMakeLists.txt) |
| runtime | [CPU Runtime](runtime.md) | accepted | 生命周期、同步 TaskId、异步 JobId 装配及 batch/事件驱动 stdio | [Runtime](../../engine/framework/runtime) | [runtime](../../tests/integration/CMakeLists.txt) |
| automation-protocol | [自动化协议](automation-protocol.md) | accepted | JSON-RPC、任务终态、可取消 stdio reader 与关闭边界 | [协议/传输](../../engine/automation) | [protocol / runtime](../../tests/unit/ProtocolTests.cpp) |

后续模块开始开发时先新建设计并加入此表；架构总览中的规划不等于模块已实现。
阶段目标和先后顺序见 [开发 Roadmap](../roadmap.md)，它不替代模块专项设计。

先按模块定位设计与源码，再用[测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)
取得构建 target 和候选范围；表中测试入口不是固定全跑套件。Operations 位于
[操作层](../../engine/framework/operations)，runner 和 assetc 分别位于
[apps/runner](../../apps/runner)、[tools/assetc](../../tools/assetc)。
最近关联记录通过 spec/development 中的 design_refs 查找，具体读取方式见[流程规范](../README.md)。
