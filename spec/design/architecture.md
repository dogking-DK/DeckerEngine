---
module: architecture
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-10-08T10:13:00+08:00"
status: accepted
---

# DeckerEngine 整体架构

## 目标与命名

建立支持渲染与物理实验、场景编辑和保存、CLI/命令/脚本自动化的引擎。
名称为 **DeckerEngine**，C++ 命名空间为 **dk**；公开头文件路径以
`dk/` 开头，CMake 实体 target 为 `dk_*`，别名为 `dk::*`，
构建选项和宏以 `DK_` 开头，程序以 `dk-` 开头。

技术方向：C++23、Eigen、Vulkan、Slang、SDL3、ImGui、CMake、vcpkg；
ECS 使用 flecs，内嵌脚本计划用 Lua/sol2，外部自动化计划用 Python。
当前 CPU 链路由 Foundation/Memory/Jobs、Scene、Commands/Services/Runtime 和 Assets 组成。
graphics/device 提供独立可选的无窗口 Vulkan 设备、VMA 资源、提交/读回与延迟释放，不进入 CPU-only Runtime 的链接依赖；
graphics/shaders 提供独立 CPU Slang 编译、SPIR-V 和最小反射，同样不进入 CPU-only Runtime 的链接依赖。
graphics/offscreen 组合 Device、Shaders 与 Graph，提供同步离屏 draw/dispatch/readback 验证入口，同样独立于 CPU Runtime。
platform 提供 SDL3 窗口/事件，graphics/presentation 提供独立可选的窗口设备、交换链及帧恢复，
复用 graphics/device 使用层；两者不进入 CPU Runtime 或 Offscreen 依赖。Graph 提供独立可选的 CPU 资源/Pass 声明、校验、依赖编译、裁剪、资源生命周期计划、单队列同步执行及计划/同步诊断。具体边界见[设备设计](graphics-device.md)、[资源设计](graphics-resources.md)、
[Graph 设计](graphics-graph.md)、[Shader 设计](graphics-shaders.md)、[离屏设计](graphics-offscreen.md)、[窗口设计](platform.md) 和[呈现设计](graphics-presentation.md)。
[Vulkan 使用层](graphics-vulkan.md) 统一对象/管线/绑定/录制/同步与传输，Offscreen 通过 Graph 编排，再由使用层录制执行；
无 Slang/Vulkan 依赖的 shader-types 保存编译产物，设备模块无需链接编译器。Graph 共用 device 的纯访问校验；执行复用同一录制、导入导出状态与完成接口。
模块接口与实现入口见[设计索引](README.md)，阶段状态和下一项统一见[Roadmap](../roadmap.md)，
历史验收结果见[开发记录](../development/README.md)。
Runtime 装配 Assets/Jobs heap 与线程上下文；worker 捕获拥有型路由并在安全点退休。
render/data 已提供不可变 SceneSnapshot 提取和显式相机 View，render/resources 通过 Graph 上传 CpuAsset，
管理 GpuMesh/纹理/材质快照、提交后缓存发布和在途卸载，见 [Render 数据](render-data.md) 与
[GPU 资源](render-resources.md)。两者均为独立可选模块，不进入 CPU-only Runtime。
render/pipeline 通过 Graph 执行 depth/opaque/tone/readback，提供绑定场景版本的异步离屏帧，
见 [最小渲染管线](render-pipeline.md)。render/disk 只读连接 M2 工程/场景、M4 导入或 CPU 产物，
准备独立 GPU 缓存，见 [磁盘渲染](render-disk.md)。截图任务已条件接入 Runtime；M8.1 编辑器通过只读快照与同一命令链路接入，脚本仍待后续阶段。

本设计整理自用户引用的“设计引擎架构”讨论（会话
`6ab1c45a-ec94-83ea-82df-a152c0c45cc5`）中可读取的内容，
并采用当前用户明确指定的工程名称和命名空间。

## 模块边界

| 目录 | 职责 | 依赖约束 |
| --- | --- | --- |
| engine/foundation | core、数学、IO、profiling、memory、CPU jobs；metadata 预留 | 不依赖 Scene、Vulkan、Editor；profiling 不反向依赖 memory |
| engine/platform | 窗口、事件、SDL3 后端；完整输入映射后续接入 | 可选，CPU 无窗口运行不依赖它 |
| engine/geometry | AABB、射线、CPU BVH | 仅基础数学/数据 |
| engine/assets | 资产类型、运行时、导入器 | 与设备资源和图资源分离 |
| engine/scene | 组件、层级、序列化、迁移 | 基础层与资产引用，不持有 Vulkan 资源 |
| engine/graphics | device、shaders、offscreen、presentation、graph 声明/编译/执行 | 离屏底座不依赖场景，正式业务后续统一进入 Graph |
| engine/render | 已实现只读数据/视图、GPU 资产缓存、最小离屏管线、磁盘输入 | data 依赖 Scene/Memory；resources 依赖 asset_data/Device/Graph；pipeline 依赖前两者、Graph/Shaders；disk 组合 Scene/资产导入和 runtime，不依赖 Framework/Editor |
| engine/physics | 接口、CPU/GPU 求解器 | CPU 不依赖 Vulkan；GPU 可用 Device/Graph |
| engine/framework | commands、services、operations、runtime | 应用服务与模块装配；通用命令层保持独立 |
| engine/automation | 协议、传输、客户端、服务端 | 客户端不链接完整 Runtime/Renderer |
| engine/scripting | 脚本 API 和 Lua 绑定 | 经命令/服务操作引擎 |
| engine/editor | CPU 工作台模型与 ImGui 面板、窗口渲染桥 | model 只依赖 Runtime；UI 私有依赖 SDL3/ImGui/Presentation/Render；经命令修改状态 |

`apps/runner` 提供 `dk-run`；`apps/editor` 提供可选 `dk-editor`；`apps/ctl` 预留 `dk-ctl`。
`tools/assetc` 和 `tools/shaderc` 为离线工具；
`sdk/python` 为外部客户端，`shaders/common` 为公共 Slang 模块。
`projects/demo` 预留示例资产、场景和脚本。
测试按 unit、integration、gpu、replay 分组。

## 关键约束

1. 编辑器是前端；业务状态通过统一 Commands / Application Services 修改。
   Runtime 负责协调，不能用循环链接解决模块耦合。
2. AssetId 是持久身份，GPU Handle 是运行时资源，Graph Handle 属于单个图声明身份；
   三者分别管理。Scene 保存资产引用，不保存设备指针。
3. `graphics/graph` 统一调度绘制、计算、上传、读回和 GPU 物理。
   Pass 声明局部算法，pipeline 负责组合，Graph 负责依赖、同步和执行。
   M5.5 的 Vulkan 使用层负责对象、局部状态校验、录制与提交寿命；Graph 生成同步计划，
   经同一使用层执行，不复制 Vulkan 底层封装，也不把 Pass 排序下放给使用层。
4. 临时、持久、历史、外部 GPU 资源分开；历史资源按视图区分。
   设备资源释放必须等待相关 GPU 工作完成。首版以单队列正确性为先。
5. 物理求解和可视化分离；粒子/网格由求解器连续数据存储管理。
   固定物理步长不依赖显示帧率。
6. Scene 区分持久 EntityId 和运行时句柄；编辑态与运行态分开。
   场景文档由引擎定义 JSON 版本协议，保存采用临时文件验证后原子替换；
   模拟检查点与场景保存分离。
7. GUI、CLI、Lua 使用同一操作语义。自动化计划采用 JSON-RPC 2.0，
   Windows Named Pipe / Unix socket / stdio；stdout 留给结构化结果，
   日志到 stderr。长任务需可查询、等待和取消，结果关联 revision/step/frame。
8. Lua 做场景脚本，Slang 做 GPU 程序；两者用途独立。
   撤销/重做由命令层支持，并明确可回滚操作的范围。
9. CPU MemorySystem 按 Runtime/tool 实例拥有，mimalloc v3 heap 按域管理；持久资源可跨线程，
   scratch/local pool 归属线程。资源存活晚于所有容器和控制块；GPU 内存继续由 VMA 管理。
   框架入口绑定作用域，业务自动取得持久域和线程 scratch；任务传播拥有型路由，不传播线程局部地址。
   进程共享 Tracy 观测后端，heap backing 与 arena/pool logical 指标分开，不重复计为总内存。

## 目录与 target

分组目录用 `add_subdirectory()`，真实模块建 target，
模块内部用 `target_sources()`。公开头文件放
`include/dk/<module>/`，实现放 `src/`。
根据公开 API 是否暴露依赖选择 PUBLIC/PRIVATE/INTERFACE；
不设置全局 include 路径，不通过他模块 src 访问内部实现。
预留目录不创建假接口或空链接 target；真正开发时再添加模块 CMakeLists。

## 后续设计顺序

工程基础 → Foundation/资产身份 → Scene/序列化 → Commands/服务/CPU Runtime →
Memory/Tracy 补充 → 资产加载/导入 → Vulkan Device/Shader → GPU Graph → 场景渲染 → Editor/IPC →
脚本自动化 → Physics 实验。
可以按需求调整；实现前维护模块设计，开发记录粒度遵循 [spec 规范](../README.md)。
阶段依赖、交付节点和验收条件以 [开发 Roadmap](../roadmap.md) 为准。
每个里程碑细分为可独立验收的 Mx.y 小阶段，默认单次开发只推进一个。

## 相关记录

- [工程基础设计](project-foundation.md)
- [Core 基础设计](foundation-core.md)
- [Eigen 数学设计](foundation-math.md)
- [工程路径与文件 IO](foundation-io.md)
- [0006 文件 IO](../development/0006-foundation-io.md)
- [0007 安全保存](../development/0007-atomic-file-save.md)
- [Foundation 集成设计](foundation-integration.md)
- [0008 Foundation 集成验收](../development/0008-foundation-integration.md)
- [0004 Eigen 与小阶段划分](../development/0004-eigen-math-foundation.md)
- [0005 Transform](../development/0005-transform.md)
- [0001 工程初始化](../development/0001-project-bootstrap.md)

## M7.4 截图接入

可选 DK_BUILD_RENDER_CAPTURE 通过独立 RenderServices/RenderOperations 接入 Runtime；CPU-only Runtime 保持无 GPU 依赖。
详见 [截图设计](render-capture.md)。
