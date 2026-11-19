---
id: "0184"
created_at: "2026-11-19T11:50:00+08:00"
updated_at: "2026-11-19T12:13:00+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
---

# 0184 Windows 增量构建与多目标验证

先交付与 windows-dev 功能对齐的 Ninja 预设，随后优化 VS 多目标验证。以 Scene 定向构建比较墙钟时间，并验证头文件变更传播到实际二进制。

## Ninja 预设与头文件验证

`windows-dev-ninja` 直接继承 `windows-dev`。在 x64 VS 开发环境中使用项目的 vcpkg checkout 配置，15 个包从已有二进制缓存恢复。两个全新配置的 DK 缓存选项、host/target triplet 逐项相同。共享预设和本机用法见[构建指南](../guides/build.md#ninja--其他平台)。

CMake 4.2.1 / 中文 MSVC 19.51 的默认探测会转码 include 前缀。配置时对实际编译器运行 `/showIncludes` 并保留输出字节。对 Debug SceneIO 对象运行 `ninja -t deps`，查到 399 个依赖，包括 SceneIO.hpp、JsonSupport.hpp 和 Eigen。修改构建树中 VersionConfig.hpp 的版本字符串，验证 Version.cpp 重编、dk_core.lib 包含新字符串、dk_scene_tests.exe 重新链接。恢复头文件后再构建，静态库中的标记消失。

### 单目标构建耗时

本机 VS 2026 x64，CMake 4.2.1，Debug，`dk_scene_tests`，`--parallel 4`。两轮串行，第二轮反转顺序。排除 configure、依赖安装和测试。单文件场景移除 SceneIO 对象后重新构建同一目标。

| 单位为秒 | VS windows-dev | windows-dev-ninja |
| --- | --- | --- |
| 干净构建 | 20.93 / 19.70 | 14.32 / 14.20 |
| 单文件重编并链接 | 5.69 / 5.45 | 4.69 / 4.63 |
| 无改动 | 1.32 / 1.02 | 0.15 / 0.15 |

`verify.ps1 -BuildDir out/build/windows-dev-ninja -Target dk_scene_tests -TestRegex '^dk\.scene\.'` 通过 33 项，跳过 0 项。Release 预设定向构建 dk_core 通过。本机原始记录位于 `out/verify-0184/bench.jsonl`，测试摘要位于 `out/verify/20261119-120413-17e3b635`。完整 Editor 和全配置回归未运行。

## VS 2026 多目标验证

配置时保存 CMake 选择的 MSBuild 路径和目标平台。定向构建开始时检查生成戳，读取最新 .slnx 中的文件夹，以一次 MSBuild 调度全部请求目标。旧树、当前生成器不匹配或目标无法映射时交回原 CMake 入口。每次摘要记录 `build_seconds`。用法见[开发工作台](../guides/development-cli.md)。

在同一 VS 2026 Debug 树、四路并行、三个已是最新的目标 `dk_core_tests dk_math_tests dk_scene_tests` 上各测三轮。旧脚本仅加相同位置的计时，使用相同筛选，各轮选中并通过 Core、Math、Scene 各一项。表中包含生成检查、目标解析和构建工具启动，排除 CTest。

| 构建入口 | 三轮墙钟时间（秒） | 中位数（秒） | 每轮 MSBuild 调用 |
| --- | --- | --- | --- |
| 原 CMake 多目标入口 | 2.430 / 2.143 / 2.129 | 2.143 | 3 |
| 解决方案多目标入口 | 1.418 / 1.339 / 1.377 | 1.377 | 1 |

三目标无改动构建阶段缩短约 36%。首次合并构建还实际编译了 Core 和 Math 测试可执行文件。日常 `python tools/dk.py test run` 在已有 `out/build/windows-dev` 选择同三个目标和三个用例，通过 3 项。最终计时摘要位于 `out/verify/20261119-121110-6299b080`、`20261119-121112-3770b84f`、`20261119-121114-280e1913`，实际开发树的验证位于 `out/verify/20261119-121232-4125ffc9`。

- 在独立 VS 树中使生成戳过期并改动解决方案文件夹，确认 CMake 先重新生成并恢复映射，随后一次 MSBuild 构建成功。嵌套的 Engine/Foundation 与 Tests 可同时选中。
- 在独立树的生成头中注入编译错误，验证停在 build 阶段，无测试结果；恢复后通过。未知目标回退到 CMake 并返回实际构建错误。单目标、缺少新工具信息的旧树和 Ninja 多目标入口均通过定向验证。
- 以上耗时只对应所列样本。全量构建、Editor Release 和 GPU 用例未运行。
