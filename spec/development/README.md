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
| 0077 | [M10.4 脚本化物理实验](0077-simulation-experiments.md) | physics-api, physics-xpbd-gpu, render-simulation, physics-xpbd, scripting-luau, automation-python, automation-transport, runtime, architecture | completed |
| 0078 | [M11.1 GPU 性能观测](0078-gpu-profiling.md) | foundation-profiling, graphics-resources, graphics-graph, physics-xpbd-gpu, physics-api | completed |
| 0079 | [M11.2 可复现性能与响应性基线](0079-simulation-performance-baseline.md) | foundation-profiling, physics-api | completed |
| 0080 | [M11.3 有界模拟任务与暂停/取消](0080-bounded-simulation-tasks.md) | physics-api, physics-xpbd-gpu, runtime, scripting-luau | completed |
| 0081 | [M11.4 响应性集成与性能复测](0081-simulation-responsiveness.md) | physics-api, physics-xpbd-gpu, editor, foundation-profiling, graphics-device, graphics-graph, graphics-resources, runtime, automation-transport, graphics-shaders | completed |
| 0082 | [M12.1 模拟面板与运行控制](0082-editor-simulation-controls.md) | editor, physics-api | completed |
| 0083 | [M12.2 实时模拟视口](0083-editor-simulation-viewport.md) | editor, editor-interaction, physics-api, render-simulation, graphics-resources | completed |
| 0084 | [M12.3 指标与实验导出](0084-editor-simulation-metrics-export.md) | editor, physics-api, scripting-luau | completed |
| 0085 | [M12.4 GUI/IPC/脚本一致性](0085-editor-simulation-consistency.md) | editor, editor-interaction, physics-api, render-simulation | completed |
| 0086 | [布料视口持续导航卡顿修复](0086-simulation-camera-progress.md) | editor, editor-interaction, physics-api | completed |
| 0087 | [编辑器后台窗口与自动化免打扰](0087-editor-background-window.md) | platform, editor | completed |
| 0088 | [Adobe Spectrum 编辑器主题迁移](0088-editor-spectrum-theme.md) | editor | completed |
| 0089 | [M13.1 实验计划与产物索引](0089-experiment-plan-index.md) | experiment-management | completed |
| 0090 | [M13.2 批量执行与进度](0090-experiment-batch.md) | experiment-management | completed |
| 0091 | [M13.3 中断恢复与失败重试](0091-experiment-recovery.md) | experiment-management | completed |
| 0092 | [M13.4 对比报告与闭环验收](0092-experiment-reports.md) | experiment-management | completed |
| 0093 | [大气渲染与体积云开发任务规划](0093-atmosphere-cloud-plan.md) | render-atmosphere, render-clouds, architecture, render-pipeline | completed |
| 0094 | [M14.1 CPU环境模型与持久化](0094-environment-model.md) | render-atmosphere、scene、application-services、render-data | completed |
| 0095 | [M14.2 体积图像与深度采样底座](0095-volume-images.md) | graphics-resources, graphics-vulkan, graphics-shaders, graphics-graph, render-pipeline, render-atmosphere | completed |
| 0096 | [M14.3 大气积分、LUT与离屏天空](0096-atmosphere-luts.md) | render-atmosphere, architecture | completed |
| 0097 | [M14.4 最小日光接收](0097-surface-sunlight.md) | render-pipeline, render-atmosphere, architecture | completed |
| 0098 | [M14.5 视锥空气透视与HDR合成](0098-aerial-perspective.md) | render-pipeline, render-atmosphere, architecture | completed |
| 0099 | [M14.6 环境面板、视口与截图闭环](0099-environment-integration.md) | editor, render-capture, render-pipeline, render-atmosphere, scripting-luau | completed |
| 0100 | [M14.7 大气质量、性能基线与阶段交付](0100-atmosphere-baseline.md) | render-atmosphere, foundation-profiling | completed |
| 0101 | [确定性云场与版本化参数](0101-cloud-fields.md) | render-clouds、scene、application-services、graphics-resources、graphics-vulkan、render-pipeline | completed |
| 0102 | [无历史云积分参考](0102-cloud-reference.md) | render-clouds、render-atmosphere | completed |
| 0103 | [云、空气与几何统一合成](0103-cloud-composition.md) | render-clouds、render-pipeline、render-capture、architecture | completed |
| 0104 | [半分辨率云重建与每视图历史](0104-cloud-reconstruction.md) | render-clouds、render-pipeline | completed |
| 0105 | [太阳云透射阴影](0105-cloud-shadows.md) | render-clouds、render-pipeline | completed |
| 0106 | [云编辑、固定时间序列与可追溯用例](0106-cloud-integration.md) | render-clouds、render-pipeline、render-capture、editor、application-services、scripting-luau、automation-python | completed |
| 0107 | [云质量、动态稳定性和性能联合交付](0107-cloud-delivery.md) | render-clouds、render-pipeline、foundation-profiling | completed |
| 0108 | [渐进式路径追踪任务规划](0108-pathtracing-plan.md) | graphics-raytracing、render-pathtracing、architecture、render-pipeline | completed |
| 0109 | [M16.1 CPU路径追踪参考](0109-pathtracing-reference.md) | render-pathtracing、architecture | completed |
| 0110 | [M16.2 RT设备能力与BDA/VMA](0110-raytracing-device.md) | graphics-raytracing、graphics-device、graphics-resources、architecture、render-pathtracing | completed |
| 0111 | [M16.3 拥有型BLAS/TLAS与候选构建](0111-acceleration-structures.md) | graphics-raytracing、graphics-resources、architecture | completed |
| 0112 | [M16.4 RT shader、管线、SBT与trace](0112-raytracing-pipeline.md) | graphics-raytracing、graphics-shaders、graphics-vulkan、graphics-resources | completed |
| 0113 | [M16.5 Graph AS、构建与trace](0113-graph-raytracing.md) | graphics-graph、graphics-raytracing、graphics-resources、architecture | completed |
| 0114 | [M16.6 GPU无历史Lambert路径追踪](0114-gpu-pathtracing.md) | render-pathtracing、architecture | completed |
| 0115 | [M16.7.1 路径追踪纹理与alpha-mask](0115-pathtracing-textures.md) | render-pathtracing、graphics-raytracing、graphics-shaders | completed |
| 0116 | [M16.7.2 metallic-roughness GGX](0116-pathtracing-ggx.md) | render-pathtracing、graphics-shaders | completed |
| 0117 | [M16.8 每视图渐进累积与额度](0117-pathtracing-progressive.md) | render-pathtracing、graphics-resources | completed |
| 0118 | [M16.9 编辑器路径追踪预览](0118-editor-pathtracing.md) | editor、render-pathtracing、graphics-resources | completed |
| 0119 | [路径追踪capture与自动化](0119-pathtracing-capture.md) | render-capture、render-pathtracing、foundation-jobs | completed |
| 0120 | [M16.11 路径追踪质量与性能交付](0120-pathtracing-delivery.md) | render-pathtracing、foundation-profiling | completed |

按编号升序维护。创建新记录前检查现有最大编号；同一任务继续更新原文件。
