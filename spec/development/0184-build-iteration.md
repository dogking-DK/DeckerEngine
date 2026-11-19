---
id: "0184"
created_at: "2026-11-19T11:50:00+08:00"
updated_at: "2026-11-19T12:04:40+08:00"
status: in-progress
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
