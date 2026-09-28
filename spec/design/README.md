# 设计文档索引

| 模块 | 文档 | 状态 | 范围 |
| --- | --- | --- | --- |
| architecture | [整体架构](architecture.md) | accepted | 长期模块边界和依赖方向 |
| project-foundation | [工程基础](project-foundation.md) | accepted | Git、目录、CMake、vcpkg、构建探针与留档 |
| foundation-core | [Core 基础](foundation-core.md) | accepted | 错误、日志、stduuid 稳定 ID 与验证 |
| foundation-math | [Eigen 数学与 Transform](foundation-math.md) | accepted | M1.2 基础数学；M1.3 TRS、仿射组合和逆变换 |
| foundation-io | [工程路径与文件 IO](foundation-io.md) | accepted | M1.4 路径/字节 IO；M1.5 同目录临时文件与安全替换 |
| foundation-integration | [Foundation 集成验收](foundation-integration.md) | accepted | M1.6 独立 CPU 示例、ID/变换/安全保存和跨进程重载 |
| foundation-memory | [Memory System](foundation-memory.md) | accepted | M1.7.2–7 heap、拥有型接口、scratch/pool、context/token、关闭集成及重复工作负载基线已完成 |
| foundation-profiling | [Tracy 性能分析](foundation-profiling.md) | accepted | CPU、heap 事件、arena/pool 曲线与三配置开销基线已完成；GPU 观测待实现 |
| scene | [场景文档](scene.md) | accepted | M2 flecs 身份、层级、快照、安全保存与重载 |
| assets-types | [资产类型](assets-types.md) | accepted | M2.3 持久引用与种类 |
| assets-runtime | [资产身份、缓存与加载](assets-runtime.md) | accepted | M4.1–4 身份/持久化、CPU 产物、缓存、异步 Ready 和生命周期完成 |
| assets-importers | [静态 glTF 导入与 assetc](assets-importers.md) | accepted | M4.2 静态网格/材质/纹理导入、CPU 产物 v1 和离线工具已完成 |
| foundation-jobs | [CPU 队列与后台作业](foundation-jobs.md) | accepted | M4.4 有界 JobId 队列、取消/等待/退出与命令接入完成 |
| project-format | [工程格式](project-format.md) | accepted | M2.3 版本清单、路径与文件诊断 |
| commands | [命令注册](commands.md) | accepted | M3.1 schema、注册与能力发现 |
| application-services | [场景应用服务](application-services.md) | accepted | M3.2 文档会话/命令；M3.3 事务/撤销重做 |
| runtime | [CPU Runtime](runtime.md) | accepted | M3.4–M3.5 生命周期、同步任务、批处理/stdio |
| automation-protocol | [自动化协议](automation-protocol.md) | accepted | M3.4–M3.5 JSON-RPC、任务终态、关闭与流边界 |

后续模块开始开发时先新建设计并加入此表；架构总览中的规划不等于模块已实现。
阶段目标和先后顺序见 [开发 Roadmap](../roadmap.md)，它不替代模块专项设计。
