---
module: project-foundation
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-09-22T17:14:09+08:00"
status: accepted
---

# 工程基础设计

## 目标与范围

在空目录初始化 main 分支 Git 仓库，建立可扩展模块目录、可重复构建入口、
vcpkg 清单和技术留档流程。本设计记录工程基础，Core 能力增量见
[Core 设计](foundation-core.md)。工程提供 core 版本查询和 dk-run 构建探针，
以验证 C++23 静态库到可执行程序的真实链接；M3.4 扩展为 CPU 场景批处理入口。

## 构建和目录

根目录依次组织 engine、apps、tools、examples、tests。
engine/foundation/core 是首个真实静态库 `dk_core`，别名 `dk::core`。
公开头 `dk/core/Version.hpp` 提供
`[[nodiscard]] std::string_view dk::version() noexcept`；
返回编译生成的项目版本字符串，生命周期为静态期，无分配、线程状态或失败路径。

`dk-run --version` 输出 DeckerEngine 和版本；
无参数或 `--help` 输出当前骨架用法；不支持的参数在 stderr 报错并返回 2。
该程序始终链接 dk::core；FRAMEWORK 与 Scene 开启时链接 automation_transport → protocol → runtime。
M3.4 runner 诊断直接写 stderr，不依赖可选 logging；最小 bootstrap 仍保留原功能范围。
其他应用、模块和工具只预留目录并注明未实现。

DK_BUILD_EXAMPLES 默认 ON；当前 math/io 同时启用时建立独立 dk-foundation-demo，
不满足依赖时跳过，不反向开启模块。bootstrap 关闭示例。
示例跨进程 CTest 不依赖 runner 或 Catch2，详见 [集成设计](foundation-integration.md)。
M2 开发预设启用 DK_BUILD_SCENE（选项自身默认 OFF），构建 dk::asset_types/dk::scene。
Scene 要求 math/io 同时开启；启用示例时增加 dk-scene-demo 及独立进程验收。
bootstrap 和独立 Foundation 配置显式关闭 Scene；README 保留最新可复现命令。
M3.1 新增 DK_BUILD_FRAMEWORK（默认 OFF，windows-dev ON），构建独立 dk::commands；
commands feature 仅引入 JSON，场景关闭时不反向启用 Scene/Math/IO。
M3.4 Scene 开启时继续装配 services/operations/runtime 与 automation/protocol/transport。
bootstrap 和独立 Foundation/Scene 验证应显式关闭 FRAMEWORK，避免继承开发预设。

CMake 最低 3.28，C++23 target 使用要求向消费者传播，禁用编译器扩展。
项目警告函数只作用于自身 target，不污染依赖。默认构建目录为 `out/build/<preset>`，
bootstrap 的迁移目录见下文；按配置放置 bin/lib。禁止源码内构建。
CTest 保留 runner 冒烟验证，开发预设额外运行 Catch2 行为测试。

## Visual Studio 解决方案分组

解决方案通过 CMake 的 FOLDER 元数据分类，根目录显式开启 USE_FOLDERS，
将 CMake 自动生成的 ALL_BUILD、ZERO_CHECK、RUN_TESTS 等项目归入 CMake。
各分组目录在 add_subdirectory 前设置 CMAKE_FOLDER，由下级真实 target 继承：

- engine → Engine；foundation、assets、automation、framework 分别追加
  Foundation、Assets、Automation、Framework 子组；dk_scene 直接位于 Engine。
- apps → Apps；当前包含 dk_run。
- tests → Tests；包含各单元测试和 dk_log_probe，CTest 的脚本测试继续由 RUN_TESTS 执行。
- examples → Examples；包含 Foundation 和 Scene 示例。
- tools → Tools；当前没有真实 target，不生成空的解决方案文件夹。

后续模块在对应目录创建 target 时自动继承分组；需要进一步细分时，
在该目录以 `${CMAKE_FOLDER}/子组名` 扩展。只控制 IDE 展示，不改变 target 名称、
编译选项、链接关系、默认构建集合或测试注册，不手改生成的 slnx/vcxproj 或 VS 个人配置。
通过根脚本重新生成后，已打开的 VS 接受重新加载提示即可更新树形结构。
验证生成前后的项目/依赖/配置保持一致、所有项目均归组，并检查完整与最小预设。
本次为工程基础维护，合并记录到 [0018](../development/0018-vs2026-generation-script.md)。

## CMake Presets

- `windows-bootstrap`：Visual Studio 18 2026 / x64，vcpkg 只安装必需的 stduuid，
  关闭日志/数学/IO/单元测试，构建目录为 out/build/windows-bootstrap-stduuid；
  此生成器要求 CMake 4.2 或更新。
- `windows-dev`：相同生成器，开启 vcpkg 并选择 foundation feature。
- `windows-desktop-deps`：准备后续桌面功能依赖；安装依赖不表示模块已实现。
- `ninja-debug / ninja-release`：Ninja 构建入口，需调用者提供匹配的编译器环境。

共享 presets 不包含本机绝对路径。个人覆盖用已忽略的
`CMakeUserPresets.json`，vcpkg 根路径通过 `VCPKG_ROOT` 环境变量提供。

## VS2026 工程生成入口

根目录提供 generate-vs2026.bat，使用 Windows 自带 cmd，复用 windows-dev configure preset，
不另存一份生成器/架构/依赖选项。以脚本所在目录为工作目录，执行后恢复调用者目录；
配置阶段按现有 vcpkg manifest 准备依赖，输出包含 Debug/Release 的 VS2026 x64 解决方案。
检查 PATH 中的 CMake，失败时给出 CMake 4.2+ 安装提示；配置失败原样返回非零退出码。
成功显示 out/build/windows-dev/DeckerEngine.slnx；兼容生成 .sln 时显示对应路径。
没有自动清理缓存、编译或启动 IDE。必要条件继续是 VS2026 C++ 工具、CMake 和已配置的 vcpkg。
验证从其他工作目录实际调用、生成的解决方案和生成器/架构，以及缺少 CMake 时的诊断。
这是工程基础维护，不推进 M4；记录见 [0018](../development/0018-vs2026-generation-script.md)。

## vcpkg 策略

使用 manifest mode，固定 builtin-baseline 为
`62159a45e18f3a9ac0548628dcaf74fcb60c6ff9`（初始化时本机干净的 vcpkg HEAD）。
依赖 feature 分为 foundation、scene、graphics、editor、scripting、tests，
清单默认安装 Core 必需的 stduuid，可选库仍按 feature 选择。
stduuid 在 Core 实现中使用，不暴露到公开头。
数学模块使用独立 math feature 安装 eigen3，启用 DK_BUILD_MATH 时自动补充该组；
foundation 中移除未使用的 glm，保留日志及 JSON 依赖。M2 的 scene feature
也声明 flecs/nlohmann-json，关闭日志的场景配置不需要 fmt/spdlog。
dk::math 的公开 Eigen 类型要求 PUBLIC 传递 Eigen3::Eigen，详见
[数学设计](foundation-math.md)。bootstrap 显式关闭数学，保持最小依赖构建。
IO 通过默认开启的 DK_BUILD_IO 构建 dk::io，仅链接 dk::core，无额外依赖；
bootstrap 也关闭 IO，接口和验证见 [IO 设计](foundation-io.md)。

`DK_VCPKG_FEATURES` 在首次 `project()` 前映射到
`VCPKG_MANIFEST_FEATURES`，并验证 feature 名。关闭 vcpkg 时不能选择 feature；
已注入 vcpkg 的构建目录不能切换到关闭状态，应使用另一构建目录。
具体模块落地时在其 CMakeLists 中添加 find_package 和 target_link_libraries，
不能将整个依赖集合全局链接到每个模块。

Windows 使用动态库/动态 CRT 的 x64-windows triplet，与 Slang 预编译库适配。
Slang 包名是 `shader-slang`，不是 `slang`。graphics feature 准备目标端
Slang 库；未来跨平台 shaderc 编译工具需单独处理 host 工具，当前未承诺交叉编译。

项目依赖缓存留在构建目录的 vcpkg_installed，忽略 build/cache/log 和 IDE 个人文件。
更新 baseline 必须连同依赖变化、构建验证和编号开发记录一起提交。

## 留档与 skill

流程规范由 [spec/README.md](../README.md) 维护，模板置于 spec/templates。
仓库 skill 路径为 `.agents/skills/decker-spec-workflow/SKILL.md`，
根 AGENTS.md 让后续任务先读取该 skill。
本模块设计先落盘，再实现构建文件；开发事实持续写入
[0001](../development/0001-project-bootstrap.md)。

## 开发辅助 skills

在 `.agents/skills` 增加三个按任务选择的仓库 skill，入口说明触发范围与工作方法，
不复制模块设计或全部命令；保留既有 decker-spec-workflow 的留档粒度规则。

- decker-build-verify：根据改动及直接受影响调用链选择构建目标/测试，默认单配置、局部验证。
  scripts/verify.ps1 接收 BuildDir、Configuration（默认 Debug）、Target 和 TestRegex；
  显式 Full+Reason 才构建/测试全部，不自行切换配置或扩展范围。
  要求已配置构建目录；先构建指定目标，失败停止；再枚举匹配测试，零匹配报错，最后执行 CTest。
  每次独立保存日志、JUnit 和 summary.json 到 out/verify，记录提交/工作区、所选测试及通过/失败/跳过。
  调用者须确认 Target 覆盖所选测试程序及进程夹具；脚本不从文件名猜测依赖，也不自动配置或清理缓存。
- decker-state-contracts：明确修改状态、成功结果、失败保护、提交点和必要案例；
  仅将内存编辑/事务、持久化等已实践模式放入按需参考，具体操作仍归属模块设计。
- decker-command-development：复用 Commands/Operations/Services 和现有传输，
  同步 spec/commands 的目录、字段、guard、副作用和示例，选择受影响命令测试。

scripts/check-spec.ps1 将现有临时文档检查提升为仓库脚本，检查 README/AGENTS/spec/skills
中的本地链接、spec 元数据、开发编号及 JSON 清单；支持限定文件检查。
这三个 skill 不新增常驻 agent、并行委派或逐次小改留档要求。
验证采用辅助脚本的正常/失败/零匹配/跳过案例、真实局部测试及 skill 格式/链接检查。
本组改动合并记录到 [0019](../development/0019-development-skills.md)，不推进 M4。

## 验证计划与取舍

当前工程在初始化骨架上按 [Core 设计](foundation-core.md) 扩展：
dk::core 增加错误和 ID，dk::logging 为可选日志 target。
DK_BUILD_LOGGING、DK_BUILD_MATH、DK_BUILD_IO 和 DK_BUILD_UNIT_TESTS 默认开启；
vcpkg 自动补充所需依赖组。windows-bootstrap 显式关闭四者，保留仅依赖 stduuid 的最小探针；
开发预设使用 Catch2 增加真实行为测试，并实际链接日志依赖。
以下原始骨架验证仍保留，Core 增量验证见 [0002](../development/0002-foundation-core.md)，
stduuid 迁移及最小预设调整见 [0003](../development/0003-stduuid-migration.md)。
Eigen 数学、math feature 与阶段细分见 [0004](../development/0004-eigen-math-foundation.md)。
IO 开关和独立 Core/IO 验证见 [0006](../development/0006-foundation-io.md)。

运行 configure/build/CTest，检查版本与错误参数行为；检查预设 JSON、
vcpkg feature 解析、skill 格式、文档链接及 Git 忽略项。
真实安装受网络、编译器、缓存影响，未完成的依赖构建应如实记录；
最小依赖构建与完整桌面依赖验证分别记录。

暂不建立大量空静态库、不实现 Vulkan 初始化；Catch2 已用于当前 Core 单元测试。
不定义安装导出/打包协议；模块 API 稳定后再设计。
未进行完整读取的原会话后半部分不作为本次已确认事实。

## 参考

- [CMake Presets](https://cmake.org/cmake/help/v4.2/manual/cmake-presets.7.html)
- [vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [OpenAI 官方 skill 文档](https://learn.chatgpt.com/docs/build-skills)
