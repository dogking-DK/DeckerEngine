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
| 0047 | [M5.4 离屏绘制与计算](0047-offscreen-execution.md) | graphics-offscreen, graphics-resources, graphics-shaders | completed |
| 0048 | [Vulkan 1.4 运行基线](0048-vulkan-14-baseline.md) | graphics-device | completed |
| 0049 | [M5.5.1 对象工厂与寿命基础](0049-vulkan-object-foundation.md) | graphics-vulkan, graphics-device, graphics-resources, graphics-shaders | completed |
| 0050 | [M5.5.2 管线与绑定](0050-vulkan-pipelines-bindings.md) | graphics-vulkan, graphics-resources | completed |
| 0051 | [M5.5.3 命令录制与同步](0051-vulkan-command-encoding.md) | graphics-vulkan, graphics-resources | completed |
| 0052 | [M5.5.4 批量上传与异步读回](0052-vulkan-transfers-readback.md) | graphics-vulkan, graphics-resources | completed |
| 0053 | [M5.5.5 离屏迁移与集成验收](0053-vulkan-offscreen-migration.md) | graphics-vulkan, graphics-resources, graphics-offscreen, graphics-device, architecture | completed |
| 0054 | [M5.6.1 窗口与设备接入](0054-window-device.md) | platform, graphics-presentation, graphics-device | completed |
| 0055 | [M5.6.2 交换链与帧同步](0055-swapchain-frames.md) | graphics-presentation, graphics-resources, graphics-vulkan | completed |
| 0056 | [M5.6.3 呈现恢复与集成验收](0056-presentation-recovery.md) | graphics-presentation, platform, graphics-device, graphics-resources, architecture | completed |
| 0057 | [M6.1 图声明与结构校验](0057-graph-declarations.md) | graphics-graph, graphics-resources, architecture | completed |
| 0058 | [M6.2 依赖编译与资源生命周期](0058-graph-compilation.md) | graphics-graph, architecture | completed |
| 0059 | [M6.3 单队列同步与执行](0059-graph-execution.md) | graphics-graph, graphics-resources, architecture | completed |
| 0060 | [M6.4 样例迁移与诊断](0060-graph-integration.md) | graphics-graph, graphics-offscreen, architecture | completed |
| 0061 | [M7.1 场景提取与 GPU 资源](0061-render-data-resources.md) | render-data, render-resources, assets-importers, architecture | completed |
| 0062 | [M7.2 最小场景渲染管线](0062-render-pipeline.md) | render-pipeline, graphics-vulkan, graphics-resources, graphics-graph, architecture | completed |
| 0063 | [M7.3 磁盘场景与资产集成](0063-render-disk.md) | render-disk, render-pipeline, assets-importers, render-resources, architecture | completed |
| 0064 | [M7.4 截图任务与自动化验收](0064-render-capture.md) | render-capture, runtime, application-services, render-disk, architecture | completed |
| 0065 | [M8.1 编辑器工作台](0065-editor-workbench.md) | editor, platform, graphics-presentation, runtime, architecture | completed |
| 0066 | [M8.2 编辑器拾取与交互](0066-editor-interaction.md) | editor-interaction, geometry-query, editor, render-data, architecture | completed |
| 0067 | [M8.3 IPC 协议与 dk-ctl](0067-ipc-client.md) | automation-transport, automation-client, automation-protocol, runtime, editor, architecture, application-services, project-foundation | completed |
| 0068 | [M8.4 编辑器与外部操作一致性](0068-editor-consistency.md) | editor, editor-interaction, render-capture, render-pipeline, automation-transport, architecture | completed |
| 0069 | [内嵌脚本改用 Luau](0069-luau-selection.md) | architecture, project-foundation | completed |
| 0070 | [M9.1 Luau 命令绑定](0070-luau-command-bindings.md) | scripting-luau, runtime, architecture, project-foundation | completed |
| 0071 | [M9.2 Luau 执行限制与取消](0071-luau-execution-limits.md) | scripting-luau, runtime, architecture | completed |
| 0072 | [M9.3 Python 自动化客户端](0072-python-automation.md) | automation-python, automation-client, architecture | completed |
| 0073 | [M9.4 自动化记录与重放](0073-automation-replay.md) | automation-replay, automation-python, architecture | completed |
| 0074 | [M10.1 模拟世界与固定步长](0074-simulation-world.md) | physics-api, runtime, application-services, scripting-luau, architecture | completed |
| 0075 | [M10.2 CPU XPBD参考求解器](0075-cpu-xpbd.md) | physics-xpbd, physics-api, runtime, architecture | completed |
| 0076 | [M10.3 GPU XPBD与可视化](0076-gpu-xpbd.md) | physics-xpbd-gpu, render-simulation, physics-xpbd, physics-api, architecture, graphics-graph | completed |

按编号升序维护。创建新记录前检查现有最大编号；同一任务继续更新原文件。
