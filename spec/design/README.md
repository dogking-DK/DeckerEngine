# 设计文档索引

| 模块 | 文档 | 状态 | 范围 |
| --- | --- | --- | --- |
| architecture | [整体架构](architecture.md) | accepted | 长期模块边界和依赖方向 |
| project-foundation | [工程基础](project-foundation.md) | accepted | Git、目录、CMake、vcpkg、构建探针与留档 |
| foundation-core | [Core 基础](foundation-core.md) | accepted | 错误、日志、stduuid 稳定 ID 与验证 |
| foundation-math | [Eigen 数学与 Transform](foundation-math.md) | accepted | M1.2 基础数学；M1.3 TRS、仿射组合和逆变换 |
| foundation-io | [工程路径与文件 IO](foundation-io.md) | accepted | M1.4 路径/字节 IO；M1.5 同目录临时文件与安全替换 |
| foundation-integration | [Foundation 集成验收](foundation-integration.md) | accepted | M1.6 独立 CPU 示例、ID/变换/安全保存和跨进程重载 |
| scene | [场景文档](scene.md) | accepted | M2.1–M2.2 flecs 身份、组件、变换层级与原子编辑 |

后续模块开始开发时先新建设计并加入此表；架构总览中的规划不等于模块已实现。
阶段目标和先后顺序见 [开发 Roadmap](../roadmap.md)，它不替代模块专项设计。
