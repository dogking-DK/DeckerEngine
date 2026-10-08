# DeckerEngine

用于渲染、物理实验、场景编辑与自动化操作的 C++23 引擎工程。
命名空间为 `dk`，CMake target 使用 `dk_*` / `dk::*`。

当前提供 Foundation、Memory/Tracy、Scene 持久化、命令/事务、CPU Runtime、
资产导入与缓存、有界 Jobs、异步 CPU Ready，以及可选的 Vulkan 使用层（对象/管线/绑定/录制/同步/传输）、Slang 编译、离屏绘制/计算读回，以及 SDL3 窗口与 Vulkan 呈现。
GPU Graph 提供资源/Pass 声明、CPU 编译、裁剪、transient 资源管理及单队列同步执行、状态导入导出和诊断；离屏样例已由 Graph 编排。
Render 已提供只读场景/视图提取、GpuMesh/纹理上传与缓存卸载；已接通磁盘资产、场景 Graph 管线与 Runtime 截图。M8.1 编辑器提供工作台、命令编辑和保存加载；物理、网络 IPC 与脚本尚未实现。
阶段状态、依赖和下一项统一见[开发 Roadmap](spec/roadmap.md)。

## 快速开始

Windows 开发配置需要 Visual Studio 2026 C++ 桌面工具、CMake 4.2+ 和 vcpkg。
将 `VCPKG_ROOT` 指向包含项目固定 baseline 的 vcpkg checkout，在仓库根运行：

```powershell
cmake --preset windows-dev
cmake --build --preset windows-debug --target dk_run
.\out\build\windows-dev\bin\Debug\dk-run.exe --version
```

也可执行 [generate-vs2026.bat](generate-vs2026.bat)，生成
`out/build/windows-dev/DeckerEngine.slnx` 后用 VS 打开；该脚本只配置，不自动编译。
最小 bootstrap、vcpkg 配置及 Ninja 入口见[构建指南](spec/guides/build.md)。

## 按任务阅读

| 任务 | 入口 |
| --- | --- |
| 配置、依赖、生成解决方案 | [构建指南](spec/guides/build.md)、[三方库说明](spec/third-party-libraries.md) |
| batch、持续 stdio、命令层独立配置 | [Runtime 指南](spec/guides/runtime.md)、[命令参考](spec/commands/README.md) |
| 登记、改名、导入、缓存、异步加载与作业 | [资产指南](spec/guides/assets.md) |
| heap、拥有型容器、scratch、pool、线程上下文、Tracy | [Memory 指南](spec/guides/memory.md) |
| Vulkan 使用层、上传/读回、绘制/计算与 GPU 探针 | [Graphics 指南](spec/guides/graphics.md) |
| Slang 离线编译、SPIR-V、反射与 dk-shaderc | [Shader 指南](spec/guides/shaders.md) |
| 离屏三角形、compute 与结果读回 | [离屏指南](spec/guides/offscreen.md) |
| GPU Graph 声明、编译与单队列执行 | [Graph 指南](spec/guides/graph.md) |
| 编辑器工作台、面板与保存加载 | [编辑器指南](spec/guides/editor.md) |
| 场景/视图、GPU 资产、磁盘场景渲染与读回 | [Render 指南](spec/guides/render.md) |
| SDL3 窗口三角形、缩放/最小化与交换链恢复 | [呈现指南](spec/guides/presentation.md) |
| Core、数学、IO、Scene 与 CPU 示例 | [Foundation/Scene 指南](spec/guides/foundation.md) |
| 实现 Mx.y 或修改已有模块 | [AGENTS.md](AGENTS.md)、[流程规范](spec/README.md)、[模块与源码索引](spec/design/README.md) |
| 查找历史决策与验证证据 | [开发记录](spec/development/README.md)、[Memory 性能基线](spec/benchmarks/2026-09-28-memory.md) |

## 定向验证

已有配置正确的 windows-dev 构建目录时，只构建并运行本次行为相关的测试，例如：

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Target dk_commands_tests -TestRegex '^dk\.commands\.discovery is sorted' -Reason '验证命令发现'
pwsh -NoProfile -File scripts/check-spec.ps1
```

[verify.ps1](scripts/verify.ps1) 默认 Debug；先构建目标，再枚举并运行匹配测试。
构建失败或零匹配均报错；日志、JUnit 与摘要保存在 `out/verify/<运行编号>/`。
测试 target、探针及筛选入口见[测试选择表](.agents/skills/decker-build-verify/references/test-selection.md)。
需要全量时显式提供 `-Full -Reason`；Release 或独立配置按实际影响选择。

[check-spec.ps1](scripts/check-spec.ps1) 检查文档链接、元数据、索引和测试入口；
`-Path @('README.md', 'spec/commands/entity.md')` 限定 Markdown 扫描，全局一致性检查仍执行。
纯文档变更不触发引擎构建。脚本维护的独立回归入口为 `scripts/test-check-spec.ps1`。

## 目录与边界

| 目录 | 职责 |
| --- | --- |
| engine/foundation | Core、数学、IO、profiling、memory、jobs；metadata 预留 |
| engine/assets | 持久类型、元数据/身份目录、CPU 导入、缓存与异步加载 |
| engine/scene | 场景文档、组件、层级、工程与 JSON 持久化 |
| engine/framework | commands、services、operations、runtime |
| engine/automation | JSON-RPC 与 JSON Lines/stdio；网络和客户端 SDK 预留 |
| engine/graphics/device、shader-types、shaders、offscreen、presentation、graph | Vulkan 1.4 使用层、独立 shader 产物、Slang 编译、离屏执行、窗口呈现与 Graph 单队列执行 |
| engine/render/data、resources、pipeline、disk | 不可变场景/视图、GPU 资产缓存、磁盘输入、Graph 场景渲染与帧读回 |
| engine/editor、apps/editor | CPU 工作台模型、ImGui 面板与 dk-editor |
| engine/platform | SDL3 窗口、事件与像素尺寸；不进入 CPU runner/离屏依赖 |
| apps/runner、tools/assetc | CPU 命令进程、离线资产工具 |
| tools/shaderc、shaders/common | 独立 shader 编译工具、图形/compute 源码示例 |
| tools/profiling | 独立 Tracy 工具及内存 capture 检查器 |
| examples、tests | CPU/窗口示例、单元与集成测试、独立 GPU 设备探针；replay 预留 |
| spec、.agents/skills | 设计/指南/记录/命令文档、按任务加载的开发方法 |

geometry、physics、scripting、编辑器拾取/交互与 Python SDK
仍按 Roadmap 逐步接入；预留目录和安装依赖不代表已经实现。
公开头位于各模块 `include/dk/`，内部实现位于 `src/`，依赖通过 target 声明。
构建产物和个人环境留在 Git 忽略目录。
