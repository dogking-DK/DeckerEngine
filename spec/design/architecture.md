---
module: architecture
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-09-22T13:41:30+08:00"
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
已实现工程骨架、Core 错误/日志/稳定 ID、Eigen 基础数学、仿射 Transform、
工程路径、二进制文件 IO 和 Windows 同目录安全保存，已通过独立 CPU 示例的集成验收。
M1 完成，M2 已接入 flecs 场景文档、组件层级、工程/资产引用和 JSON 安全保存/重载，
见 [Scene 设计](scene.md)、[工程协议](project-format.md) 和 [资产类型](assets-types.md)。
M3.1 已接入独立 [命令层](commands.md)，供后续服务和自动化共用契约。
M3.2 [场景服务与操作层](application-services.md) 已提供会话 guard、实体编辑、分页查询与保存。
其余模块在开始开发前另写专项设计。

本设计整理自用户引用的“设计引擎架构”讨论（会话
`6ab1c45a-ec94-83ea-82df-a152c0c45cc5`）中可读取的内容，
并采用当前用户明确指定的工程名称和命名空间。

## 模块边界

| 目录 | 职责 | 依赖约束 |
| --- | --- | --- |
| engine/foundation | core、数学、IO、任务、元数据 | 不依赖 Scene、Vulkan、Editor |
| engine/platform | 窗口、输入、SDL3 后端 | 可选，CPU 无窗口运行不依赖它 |
| engine/geometry | AABB、射线、CPU BVH | 仅基础数学/数据 |
| engine/assets | 资产类型、运行时、导入器 | 与设备资源和图资源分离 |
| engine/scene | 组件、层级、序列化、迁移 | 基础层与资产引用，不持有 Vulkan 资源 |
| engine/graphics | device、presentation、shaders、graph | 图核心不依赖具体 Pass 或场景 |
| engine/render | 数据、GPU 缓存、Pass、pipeline | 使用 Graph；不依赖 Editor |
| engine/physics | 接口、CPU/GPU 求解器 | CPU 不依赖 Vulkan；GPU 可用 Device/Graph |
| engine/framework | commands、services、operations、runtime | 应用服务与模块装配；通用命令层保持独立 |
| engine/automation | 协议、传输、客户端、服务端 | 客户端不链接完整 Runtime/Renderer |
| engine/scripting | 脚本 API 和 Lua 绑定 | 经命令/服务操作引擎 |
| engine/editor | 模型、交互、控件、面板 | 经命令/服务修改状态 |

`apps/editor`、`apps/runner`、`apps/ctl` 分别计划为
`dk-editor`、`dk-run`、`dk-ctl`。
`tools/assetc` 和 `tools/shaderc` 为离线工具；
`sdk/python` 为外部客户端，`shaders/common` 为公共 Slang 模块。
`projects/demo` 预留示例资产、场景和脚本。
测试按 unit、integration、gpu、replay 分组。

## 关键约束

1. 编辑器是前端；业务状态通过统一 Commands / Application Services 修改。
   Runtime 负责协调，不能用循环链接解决模块耦合。
2. AssetId 是持久身份，GPU Handle 是运行时资源，Graph Handle 属于图执行期；
   三者分别管理。Scene 保存资产引用，不保存设备指针。
3. `graphics/graph` 统一调度绘制、计算、上传、读回和 GPU 物理。
   Pass 声明局部算法，pipeline 负责组合，Graph 负责依赖、同步和执行。
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

## 目录与 target

分组目录用 `add_subdirectory()`，真实模块建 target，
模块内部用 `target_sources()`。公开头文件放
`include/dk/<module>/`，实现放 `src/`。
根据公开 API 是否暴露依赖选择 PUBLIC/PRIVATE/INTERFACE；
不设置全局 include 路径，不通过他模块 src 访问内部实现。
预留目录不创建假接口或空链接 target；真正开发时再添加模块 CMakeLists。

## 后续设计顺序

工程基础 → Foundation/资产身份 → Scene/序列化 → Commands/服务/CPU Runtime →
资产加载/导入 → Vulkan Device/Shader → GPU Graph → 场景渲染 → Editor/IPC →
脚本自动化 → Physics 实验。
可以按需求调整，但在编写模块实现前完成对应专项设计和编号开发记录。
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
