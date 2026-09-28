# 开发文档索引

| 编号 | 文档 | 关联模块 | 状态 |
| --- | --- | --- | --- |
| 0001 | [工程初始化](0001-project-bootstrap.md) | project-foundation | completed |
| 0002 | [Core 基础](0002-foundation-core.md) | foundation-core | completed |
| 0003 | [stduuid 迁移](0003-stduuid-migration.md) | foundation-core、project-foundation | completed |
| 0004 | [Eigen 基础数学与里程碑细分](0004-eigen-math-foundation.md) | foundation-math、project-foundation | completed |
| 0005 | [Transform TRS、组合与逆变换](0005-transform.md) | foundation-math | completed |
| 0006 | [工程路径与文件 IO](0006-foundation-io.md) | foundation-io、project-foundation | completed |
| 0007 | [同目录临时文件与安全替换](0007-atomic-file-save.md) | foundation-io | completed |
| 0008 | [Foundation CPU 集成验收](0008-foundation-integration.md) | foundation-integration、project-foundation | completed |
| 0009 | [flecs 文档与实体身份](0009-scene-identity.md) | scene | completed |
| 0010 | [组件与变换层级](0010-scene-hierarchy.md) | scene | completed |
| 0011 | [工程格式与资产引用](0011-project-assets.md) | scene、assets-types、project-format | completed |
| 0012 | [场景序列化与安全重载](0012-scene-persistence.md) | scene、project-format、foundation-io | completed |
| 0013 | [命令注册与能力发现](0013-command-registry.md) | commands | completed |
| 0014 | [场景服务与编辑操作](0014-scene-services.md) | application-services、foundation-core | completed |
| 0015 | [事务与撤销重做](0015-transactions-history.md) | application-services、scene | completed |
| 0016 | [CPU Runtime 与 CLI 批处理](0016-cpu-runtime-cli.md) | runtime、automation-protocol | completed |
| 0017 | [持续 stdio 与交付 A](0017-stdio-delivery-a.md) | runtime、automation-protocol | completed |
| 0018 | [VS2026 工程生成与解决方案分组](0018-vs2026-generation-script.md) | project-foundation | completed |
| 0019 | [开发辅助 skills](0019-development-skills.md) | project-foundation | completed |
| 0020 | [M4 开发计划与设计准备](0020-m4-development-plan.md) | assets-runtime、assets-importers、foundation-jobs | completed（仅文档） |
| 0021 | [vcpkg 全部依赖基线升级](0021-vcpkg-baseline-update.md) | project-foundation、assets-importers、assets-runtime、scene | completed |
| 0022 | [Memory System 与 Tracy 设计](0022-memory-profiling-design.md) | foundation-memory、foundation-profiling、foundation-jobs、assets-runtime | completed（仅文档） |
| 0023 | [M1.7.1 Tracy CPU 性能分析](0023-tracy-cpu-profiling.md) | foundation-profiling、project-foundation | completed |
| 0024 | [M1.7.2 mimalloc heap 与内存事件](0024-mimalloc-heap.md) | foundation-memory、foundation-profiling | completed |
| 0025 | [M1.7.3 拥有型内存接口与持久域路由](0025-memory-ownership-routing.md) | foundation-memory | completed |
| 0026 | [M1.7.4 ScratchArena 与线程临时作用域](0026-scratch-arena.md) | foundation-memory, foundation-profiling | completed |
| 0027 | [M1.7.5 Pool、ObjectPool 与受控 trim](0027-memory-pools.md) | foundation-memory, foundation-profiling | completed |
| 0028 | [magic_enum 与枚举字符串迁移](0028-magic-enum.md) | foundation-core, assets-types, commands, foundation-profiling | completed |
| 0029 | [线程上下文、拥有型路由与关闭集成](0029-memory-context-routing.md) | foundation-memory, foundation-profiling | completed |
| 0030 | [重复工作负载与性能基线](0030-memory-baseline.md) | foundation-memory, foundation-profiling | completed |
| 0031 | [M4.1.1 元数据与身份目录](0031-asset-metadata-catalog.md) | assets-runtime, assets-types, project-format | completed |
| 0032 | [M4.1.2 登记提交与受控改名](0032-asset-commit-recovery.md) | assets-runtime, project-format | completed |
| 0033 | [M4.2.1 CPU 数据与网格导入](0033-cpu-mesh-import.md) | assets-importers, assets-runtime | completed |
| 0034 | [M4.2.2 纹理与离线工具](0034-textures-assetc.md) | assets-importers, assets-runtime | completed |
| 0035 | [M4.3.1 内容键与产物发布](0035-asset-cache-publication.md) | assets-runtime, assets-importers | completed |
| 0036 | [M4.3.2 失效、重建与显式清理](0036-asset-cache-invalidation.md) | assets-runtime, assets-importers | completed |
| 0037 | [M4.4.1 CPU 工作队列](0037-cpu-job-queue.md) | foundation-jobs, foundation-memory | completed |
| 0038 | [M4.4.2 异步资产状态](0038-async-asset-state.md) | assets-runtime, foundation-jobs | completed |
| 0039 | [M4.4.3 服务与命令接入](0039-assets-jobs-commands.md) | runtime, application-services, foundation-jobs | completed |
| 0040 | [M4.4.4 CPU 资产交付验收](0040-cpu-assets-delivery.md) | assets-runtime, foundation-jobs, runtime | completed |
| 0041 | [AI 文档入口与开发协作优化](0041-ai-documentation-workflow.md) | project-foundation, foundation-memory, runtime | completed |
| 0042 | [M5.1 Vulkan 设备与诊断](0042-vulkan-device.md) | graphics-device | completed |
| 0043 | [volk、vk-bootstrap 和 VMA](0043-vulkan-libraries.md) | graphics-device | completed |
| 0044 | [Vulkan-Hpp RAII 所有权](0044-vulkan-hpp-raii.md) | graphics-device | completed |
| 0045 | [M5.2 资源与提交生命周期](0045-graphics-resources-submission.md) | graphics-resources, graphics-device | completed |
| 0046 | [M5.3 Slang 编译工具](0046-slang-shader-compiler.md) | graphics-shaders | completed |

按编号升序维护。创建新记录前检查现有最大编号；同一任务继续更新原文件。
