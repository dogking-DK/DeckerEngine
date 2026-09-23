---
module: third-party-libraries
created_at: "2026-09-23T09:09:35+08:00"
updated_at: "2026-09-23T09:12:42+08:00"
status: accepted
---

# DeckerEngine 三方库说明

本文集中说明三方库的用途、当前版本、接入状态和版本维护规则。
依赖声明以 [vcpkg.json](../vcpkg.json) 为准，实际模块选择见
[CMake 映射](../cmake/Vcpkg.cmake)，构建入口见 [README](../README.md#vcpkg-开发配置)。
模块内部的封装和接口约定继续放在对应设计中；历史升级过程见开发记录。

## 版本规则

**三方库默认使用 vcpkg 官方 registry 发布的最新版本，包括 port 修订版本。**
新增依赖、接入已选型库或进行依赖升级时，先核验官方索引，不直接沿用旧文档里的版本号。
这里的“最新”以 vcpkg 收录为准，不默认使用库上游未收录的 release、开发分支或源码 HEAD。

核验后将官方 registry 提交固定在 builtin-baseline，并将版本和相关验证一起提交。
日常 configure/build 使用固定基线，不自动追踪浮动 master；本机 vcpkg 更新也不会自动改写项目基线。
新基线内没有更高版本的包保持原版本。`#N` 是 vcpkg port 修订号，未写 `#` 表示修订为 0。

默认不添加旧版本 override。若最新版本存在兼容问题，优先修复或明确记录未完成的验证；
确需暂用旧版本时，必须记录具体版本、原因、影响范围和恢复最新版本的条件，不能静默回退。
当前没有旧版本 override。

## 当前版本基线与验证范围

- 版本核验日期：2026-09-23。
- builtin-baseline：`67b9e21f86e3034657a04da429a8bf274de67925`。
- 来源：[官方固定索引](https://github.com/microsoft/vcpkg/blob/67b9e21f86e3034657a04da429a8bf274de67925/versions/baseline.json)。
- Windows host/target triplet：`x64-windows`。
- 最近升级：[0021](development/0021-vcpkg-baseline-update.md)。当前启用依赖已安装并完成定向 Debug 验证；
  全部 feature 加 M4 选型共 22 个包已通过依赖解析，未接入模块未声明安装或功能验证通过。

下表记录该基线对应的版本。标为“已集成”的库有实际 CMake 消费者；
“清单预留”表示仅有依赖 feature；“M4 已选型”尚未加入项目清单。
构建缓存中残留的未使用包不属于当前依赖清单，例如此前已移除的 GLM。

## 当前已集成

| 库 / vcpkg port | 当前版本 | feature | 用途与接入边界 |
| --- | --- | --- | --- |
| stduuid / `stduuid` | 1.2.3 | 基础依赖，始终选择 | Core 的 UUID 生成、解析、格式化；由 dk::core 私有封装为强类型 ID |
| fmt / `fmt` | 12.2.0#1 | foundation | 日志格式化；dk::logging 的 PUBLIC 依赖，供日志模板接口使用 |
| spdlog / `spdlog` | 1.17.0#1 | foundation | 日志级别、输出及文件 sink；dk::logging PRIVATE 依赖，使用默认 fmt/tz-offset features |
| nlohmann-json / `nlohmann-json` | 3.12.0#2 | foundation、scene、commands | 场景/工程 JSON、命令 schema 和 JSON-RPC；Scene 私有使用，Commands 公开 Json 值类型 |
| Eigen / `eigen3` | 5.0.1 | math | 向量、矩阵、四元数和 Transform；dk::math PUBLIC 传递 Eigen3::Eigen |
| flecs / `flecs` | 4.1.6 | scene | SceneDocument 内部 ECS；由 Pimpl 持有，公共接口不暴露 flecs 句柄 |
| Catch2 / `catch2` | 3.16.0 | tests | 单元测试与 CTest 测试发现；测试 target 私有链接，运行时模块不依赖它 |

对应设计：[Core/日志](design/foundation-core.md)、[数学](design/foundation-math.md)、
[Scene](design/scene.md)、[Commands](design/commands.md)。
实际依赖声明：[Core/日志 CMake](../engine/foundation/core/CMakeLists.txt)、
[Math CMake](../engine/foundation/math/CMakeLists.txt)、[Scene CMake](../engine/scene/CMakeLists.txt)、
[Commands CMake](../engine/framework/commands/CMakeLists.txt)、[单元测试 CMake](../tests/unit/CMakeLists.txt)。

## M4 已选型，尚未接入

| 库 / vcpkg port | 当前基线版本 | 计划用途 | 边界 |
| --- | --- | --- | --- |
| fastgltf / `fastgltf` | 0.9.0 | 静态 glTF/GLB 解析、accessor 提取 | 导入器私有依赖，数据转换为引擎拥有的 CPU 值 |
| stb_image / `stb` | 2024-07-29#1 | PNG/JPEG 解码到 RGBA8；该 port 的 stb_image 为 2.30 | 单一实现翻译单元，使用内存输入，限制支持格式 |
| xxHash / `xxhash` | 0.8.4 | 恢复记录文件摘要、资源变化检测与缓存键 | 选用 XXH3-128，内部 ContentDigest 封装；不启用 xxhsum feature |
| simdjson / `simdjson` | 4.6.11 | fastgltf 的传递解析依赖 | 不替换引擎公开的 nlohmann-json 接口，不作为新直接依赖重复声明 |

接入顺序和格式限制见 [M4 导入器设计](design/assets-importers.md)、
[资产运行时设计](design/assets-runtime.md) 及 [M4 开发计划](development/0020-m4-development-plan.md)。
使用对应模块时才增加依赖 feature 和 CMake target；已选型不表示已有导入器或摘要实现。

## 清单预留，模块尚未实现

| 库 / vcpkg port | 当前版本 | feature | 计划用途 |
| --- | --- | --- | --- |
| Vulkan / `vulkan` | 2023-12-17 | graphics | 确保 Vulkan 头文件和 loader 可用的占位包；实际 API 版本见下表 |
| Vulkan Memory Allocator / `vulkan-memory-allocator` | 3.4.0 | graphics | Vulkan Buffer/Image 内存分配 |
| Slang / `shader-slang` | 2026.18 | graphics | Shader 编译、SPIR-V 和布局反射；CMake package 名为 slang |
| SDL3 / `sdl3` | 3.4.16#1 | editor | 窗口与输入，选择 vulkan feature |
| Dear ImGui / `imgui` | 1.92.9 | editor | 编辑器 UI，选择 docking-experimental、sdl3-binding、vulkan-binding |
| Lua / `lua` | 5.5.1 | scripting | 内嵌场景脚本语言 |
| sol2 / `sol2` | 3.5.0#1 | scripting | C++ 与 Lua 绑定 |

这些库当前只做依赖解析，尚无引擎链接/运行时兼容性结论。
M5/M7/M8/M9 的实施安排见 [Roadmap](roadmap.md)。

## 其他传递依赖与构建辅助包

| vcpkg port | 当前版本 | 来源与用途 |
| --- | --- | --- |
| `vulkan-headers` | 1.4.357.0 | Vulkan feature 的传递依赖，提供 API 头文件；尚未接入 |
| `vulkan-loader` | 1.4.357.0 | Windows Vulkan feature 的传递依赖，提供 loader；不等同于显卡驱动 |
| `vcpkg-cmake` | 2025-08-07 | host 构建辅助，供 port 配置/编译/安装，当前已使用 |
| `vcpkg-cmake-config` | 2026-07-21 | host 构建辅助，整理 CMake package 导出，当前已使用 |

传递依赖随所选 feature/平台变化，以实际 vcpkg 解析计划为准；上述版本不是独立手工 pin。

## 后续维护

1. 新增、升级、移除或替换库时核验官方 registry，更新清单基线及本表的版本、用途、feature 和接入状态。
   使用同一基线解析直接/传递依赖，检查目标平台支持、CMake 导出和 port 许可证声明的变化。
2. 按模块 PUBLIC/PRIVATE 边界接入，已选型库开始实施时重新核验最新版本。
   新版依赖影响哪些消费者，就构建和验证哪些链路；预留模块可记录 dry-run，不能当作已集成验收。
3. 同步相关模块设计和开发记录，记录版本差异及通过/失败/未验证范围，更新本文 updated_at。
   完成后连同 builtin-baseline 本地提交；历史记录保留当时版本。

维护粒度遵循 [spec 规范](README.md)，构建策略见 [工程基础设计](design/project-foundation.md#vcpkg-策略)。
本文维护当前清单和默认版本规则；构建脚本负责实际解析，开发记录保存每次验证证据。
