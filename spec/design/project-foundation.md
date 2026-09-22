---
module: project-foundation
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-09-22T09:09:41+08:00"
status: accepted
---

# 工程基础设计

## 目标与范围

在空目录初始化 main 分支 Git 仓库，建立可扩展模块目录、可重复构建入口、
vcpkg 清单和技术留档流程。当前交付 core 版本查询和 dk-run 构建探针，
以验证 C++23 静态库到可执行程序的真实链接；尚不实现引擎场景运行功能。

## 构建和目录

根目录依次组织 engine、apps、tools、tests。
engine/foundation/core 是首个真实静态库 `dk_core`，别名 `dk::core`。
公开头 `dk/core/Version.hpp` 提供
`[[nodiscard]] std::string_view dk::version() noexcept`；
返回编译生成的项目版本字符串，生命周期为静态期，无分配、线程状态或失败路径。

`dk-run --version` 输出 DeckerEngine 和版本；
无参数或 `--help` 输出当前骨架用法；不支持的参数在 stderr 报错并返回 2。
该程序当前只链接 dk::core。其他应用、模块和工具只预留目录并注明未实现。

CMake 最低 3.28，C++23 target 使用要求向消费者传播，禁用编译器扩展。
项目警告函数只作用于自身 target，不污染依赖。构建目录为 `out/build/<preset>`，
按配置放置 bin/lib。禁止源码内构建。CTest 使用不依赖第三方的 runner 冒烟验证。

## CMake Presets

- `windows-bootstrap`：Visual Studio 18 2026 / x64，关闭 vcpkg，仅验证基础链路；
  此生成器要求 CMake 4.2 或更新。
- `windows-dev`：相同生成器，开启 vcpkg 并选择 foundation feature。
- `windows-desktop-deps`：准备后续桌面功能依赖；安装依赖不表示模块已实现。
- `ninja-debug / ninja-release`：Ninja 构建入口，需调用者提供匹配的编译器环境。

共享 presets 不包含本机绝对路径。个人覆盖用已忽略的
`CMakeUserPresets.json`，vcpkg 根路径通过 `VCPKG_ROOT` 环境变量提供。

## vcpkg 策略

使用 manifest mode，固定 builtin-baseline 为
`62159a45e18f3a9ac0548628dcaf74fcb60c6ff9`（初始化时本机干净的 vcpkg HEAD）。
依赖 feature 分为 foundation、scene、graphics、editor、scripting、tests，
默认清单不安装任何第三方库。当前 core 构建探针只使用 C++ 标准库。

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

## 验证计划与取舍

运行 configure/build/CTest，检查版本与错误参数行为；检查预设 JSON、
vcpkg feature 解析、skill 格式、文档链接及 Git 忽略项。
真实安装受网络、编译器、缓存影响，未完成的依赖构建应如实记录；
基础无依赖构建与完整桌面依赖验证分别记录。

暂不建立大量空静态库、不实现 Vulkan 初始化、不引入完整测试框架。
不定义安装导出/打包协议；模块 API 稳定后再设计。
未进行完整读取的原会话后半部分不作为本次已确认事实。

## 参考

- [CMake Presets](https://cmake.org/cmake/help/v4.2/manual/cmake-presets.7.html)
- [vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [OpenAI 官方 skill 文档](https://learn.chatgpt.com/docs/build-skills)

