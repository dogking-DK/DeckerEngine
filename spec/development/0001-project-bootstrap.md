---
id: "0001"
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-09-22T09:19:13+08:00"
status: completed
design_refs:
  - ../design/architecture.md
  - ../design/project-foundation.md
---

# 0001 工程初始化

## 目标与设计依据

依据[整体架构](../design/architecture.md)和[工程基础设计](../design/project-foundation.md)，
为 DeckerEngine 建立 Git、目录、CMake/vcpkg 与 spec 留档流程。命名空间统一为 dk。

## 实际变更

- Git：初始化 main 分支；添加 .gitignore、.gitattributes、.editorconfig。
  当前没有提交和远程配置。首次沙箱创建的空 .git 所有权导致用户 Git 拒绝访问；
  确认无引用、无远程、无索引后，备份到被忽略的
  out/build/git-init-sandbox-backup，并以用户账户重新初始化，已恢复正常访问。
- 目录：建立 Foundation、Platform、Geometry、Assets、Scene、Graphics、Render、
  Physics、Framework、Automation、Scripting、Editor，以及 apps/tools/sdk/shaders/
  projects/tests 分组；未实现模块通过 .gitkeep 保留，不建立空链接 target。
- [根 CMakeLists](../../CMakeLists.txt)、[构建预设](../../CMakePresets.json)、
  [选项](../../cmake/Options.cmake)、[vcpkg 集成](../../cmake/Vcpkg.cmake)、
  [编译选项](../../cmake/CompilerWarnings.cmake)：C++23、分层 subdirectory、
  target 依赖、配置独立输出、Windows 与 Ninja 预设。
- [vcpkg 清单](../../vcpkg.json)：固定基线，建立 foundation/scene/graphics/editor/
  scripting/tests 六个可选依赖组；Slang 使用 shader-slang 包。
  基础依赖已安装到 out/build/windows-dev/vcpkg_installed；
  vcpkg 自身复用本机下载、构建和二进制缓存。
- [core 接口](../../engine/foundation/core/include/dk/core/Version.hpp) 与
  [实现](../../engine/foundation/core/src/Version.cpp)：dk::version() 返回 0.1.0；
  dk_core 静态库导出 dk::core target。
- [runner](../../apps/runner/src/main.cpp)：dk-run 支持 --version、--help，
  未知参数返回 2；这是构建探针，尚不执行场景。
- [CTest](../../tests/integration/CMakeLists.txt)：验证可执行程序实际链接并输出版本。
- 已先建立设计、规范、索引和模板，再添加实现；
  [AGENTS.md](../../AGENTS.md) 与
  [decker-spec-workflow](../../.agents/skills/decker-spec-workflow/SKILL.md)
  固化先设计后开发、持续更新编号记录和时间字段的流程。
- [README](../../README.md) 提供目录说明、构建命令和实现状态。

## 验证记录

本机 Windows x64，CMake 4.2.1，Visual Studio Community 2026 18.10.1，
项目编译器 MSVC 19.51.36257.0，Windows SDK 10.0.26100.0。
vcpkg CLI 2025-09-03，registry HEAD 为
62159a45e18f3a9ac0548628dcaf74fcb60c6ff9。
本次 vcpkg 基础依赖由其自动选择的 VS 2022 / MSVC 14.44.35207 构建；
项目源代码由 VS 2026 编译。core 目前不链接这些第三方库。

| 检查 | 实际命令或方法 | 结果 |
| --- | --- | --- |
| 基础配置 | cmake --preset windows-bootstrap | 通过 |
| 基础 Debug | cmake --build --preset windows-bootstrap-debug | 通过 |
| 基础 Debug 测试 | ctest --preset windows-bootstrap-debug | 1/1 通过 |
| 基础 Release | cmake --build --preset windows-bootstrap-release | 通过 |
| 基础 Release 测试 | ctest --preset windows-bootstrap-release | 1/1 通过 |
| vcpkg 配置/安装 | cmake --preset windows-dev | fmt、glm、nlohmann-json、spdlog 和构建辅助包安装成功 |
| vcpkg Debug | cmake --build --preset windows-debug | 通过 |
| vcpkg Debug 测试 | ctest --preset windows-debug | 1/1 通过 |
| 参数行为 | dk-run --version / --help / --unknown | 版本为 DeckerEngine 0.1.0；帮助正常；未知参数退出码 2 |
| 全部依赖组 | vcpkg install --dry-run，指定六组 --x-feature 和 x64-windows host/target | 17 个包的解析计划成功；未安装整套桌面依赖 |
| 预设与清单 | cmake --list-presets=all；Python JSON 解析；vcpkg dry-run | 通过 |
| skill | skill-creator/scripts/quick_validate.py（Python UTF-8 模式） | Skill is valid |
| 文档 | Python 检查相对链接、时区时间、命名 | 通过 |
| Git | status、symbolic-ref、check-ignore、remote | main 正常；构建/个人配置已忽略；无远程 |

初次配置前沙箱执行器发生 setup refresh 错误；之后在已授权的用户执行环境中
完成上述验证。构建本身未发现错误。

## 偏差与决策

用户明确名称为 DeckerEngine、命名空间 dk，已覆盖背景方案中的示例名称。
原会话可读回复在 20000 字符处截断；本次使用可读取的架构与用户明确要求。
选用无依赖构建探针验证基础链路；桌面依赖单独准备，避免将可安装的包误记为已实现模块。

## 遗留问题与下一步

本次工程初始化范围已完成。未验证 Linux/macOS/Ninja、vcpkg Release、
完整桌面依赖安装及其运行时兼容性；后续实际模块接入时验证对应 target 链接和行为。

后续建议从 Foundation（错误/日志/稳定 ID）或 Scene 的最小闭环开始，
先创建对应模块设计，再分配 0002 开发记录。图形、物理、编辑器、自动化、
脚本、场景保存和 shader 编译命令尚未实现。

## 修改记录

- 2026-09-22T09:09:41+08:00：先创建设计文档，建立本开发记录。
- 2026-09-22T09:19:13+08:00：完成目录、构建、依赖配置与 skill；记录构建/测试结果并完成交接。
