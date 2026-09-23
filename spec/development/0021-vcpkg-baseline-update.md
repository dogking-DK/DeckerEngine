---
id: "0021"
created_at: "2026-09-23T08:51:49+08:00"
updated_at: "2026-09-23T09:05:28+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
  - ../design/assets-importers.md
  - ../design/assets-runtime.md
  - ../design/scene.md
---

# 0021 vcpkg 全部依赖基线升级

## 目标与依据

按用户要求统一采用 vcpkg 官方索引最新版本，属于工程依赖维护，不推进 M4 实现。
项目基线为 2554621；工作区开始时干净。依据 [工程基础](../design/project-foundation.md)
和 [M4 库选型](../design/assets-importers.md)，保留 feature、模块边界与 x64-windows triplet。

旧 builtin-baseline：62159a45e18f3a9ac0548628dcaf74fcb60c6ff9。
新 builtin-baseline：67b9e21f86e3034657a04da429a8bf274de67925，来自实际 git ls-remote/fetch，
提交时间 2026-09-22T16:54:42-07:00。这里的最新指本次核验时 vcpkg 收录的版本，
不绕过 port 改为库上游分支；构建仍固定此提交以保证可复现。

## 版本变化

下表来自两个提交的 versions/baseline.json，#N 表示 vcpkg port 修订；未写 # 的修订为 0。
列入当前全部直接依赖，以及 M4 已选型库、相关传递包和构建辅助包。

| 包 | 原基线 | 新基线 | 使用范围 |
| --- | --- | --- | --- |
| stduuid | 1.2.3 | 1.2.3 | Core |
| fmt | 12.1.0 | 12.2.0#1 | 日志 |
| spdlog | 1.17.0 | 1.17.0#1 | 日志 |
| nlohmann-json | 3.12.0#2 | 3.12.0#2 | Scene / Commands |
| eigen3 | 5.0.1 | 5.0.1 | Math |
| flecs | 4.1.4 | 4.1.6 | Scene |
| catch2 | 3.13.0#1 | 3.16.0 | 单元测试 |
| fastgltf | 0.9.0 | 0.9.0 | M4 规划 |
| stb | 2024-07-29#1 | 2024-07-29#1 | M4 规划 |
| xxhash | 0.8.3 | 0.8.4 | M4 规划 |
| simdjson | 4.3.1 | 4.6.11 | fastgltf 传递依赖 |
| vulkan | 2023-12-17 | 2023-12-17 | graphics 预留 |
| vulkan-headers | 1.4.335.0#1 | 1.4.357.0 | Vulkan 传递依赖 |
| vulkan-loader | 1.4.335.0 | 1.4.357.0 | Vulkan 传递依赖 |
| vulkan-memory-allocator | 3.3.0 | 3.4.0 | graphics 预留 |
| shader-slang | 2026.2 | 2026.18 | graphics 预留 |
| sdl3 | 3.4.2 | 3.4.16#1 | editor 预留 |
| imgui | 1.91.9 | 1.92.9 | editor 预留 |
| lua | 5.5.0#1 | 5.5.1 | scripting 预留 |
| sol2 | 3.5.0#1 | 3.5.0#1 | scripting 预留 |
| vcpkg-cmake | 2024-04-23 | 2025-08-07 | host 构建辅助 |
| vcpkg-cmake-config | 2024-05-23 | 2026-07-21 | host 构建辅助 |

M4 simdjson 的 port 许可证声明更新为 (Apache-2.0 OR MIT) AND BSL-1.0 AND BSD-3-Clause；
xxHash 的 CMake 源目录从 cmake_unofficial 移到 build/cmake，已核对 0.8.4 官方源码仍导出
xxHash::xxhash。固定的 XXH3-128 流式与规范编码接口仍存在。
历史开发记录保留当时版本；当前设计更新到新基线。

## 变更与验证计划

- 本机 vcpkg 仓库 fast-forward 到核验提交，并 bootstrap 匹配的 CLI；原 CLI 为 2025-09-03。
- 更新项目清单的 builtin-baseline 和当前设计/README，保持依赖按 feature 选择。
- windows-dev 重新配置并实际安装启用的依赖；全部现有 feature 做 dry-run。
- 构建受 Catch2 更新影响的测试程序；执行日志、Scene/服务及其他测试程序代表性用例，
  再验证实际 runner 的相关进程链路。使用 scripts/verify.ps1 留下定向 Debug 证据。
- M4 尚未添加依赖 feature，只核验新基线与选型接口，不将其标记为导入器已集成。
- 最后执行定向文档检查与 git diff --check，记录实际通过/失败/未执行项。

## 实际结果

本机 vcpkg 已 fast-forward 到新基线，bootstrap 成功，CLI 更新为
2026-07-27-98d7cb0cf1f4686a3e43aa5672b6230c1d56bce8。
首次 configure 在下载配套 CMake 4.4.3 时发生 curl 56 连接中断，未进入引擎编译。
经显式本机代理下载官方压缩包并核对 vcpkg-tools.json 的 SHA512 后继续；
代理仅设置在本次命令进程，没有写入仓库配置或系统环境。原始日志在 out/vcpkg-update-configure.log，
重试日志在 out/vcpkg-update-configure-retry.log。重试配置/生成已成功，启用的 7 个库及 2 个
构建辅助包实际重新安装，均与新基线一致；vcpkg 和项目此次均使用 VS2026 / MSVC 19.51。

全部依赖解析通过：复制项目清单到 out/vcpkg-baseline-update-plan，临时追加 fastgltf/stb/xxhash，
选择全部 8 个已有 feature，执行 install --dry-run，日志 out/vcpkg-update-all-plan.log 共列出
22 个包，与上表逐项一致。临时清单不进入版本管理、不改变项目实际启用范围。

首次定向 build 在沙箱中被 MSBuild FileTracker 的 E_ACCESSDENIED 阻止，测试未运行，证据为
out/verify/20260923-090104-24c17040。随后在用户执行环境中重跑相同 targets/regex，
不关闭模块或更换编译器。9 个目标编译通过，63 项用例中 62 通过、1 失败、0 跳过，证据为
out/verify/20260923-090134-69b6b720。失败仅在 batch_roundtrip：独立 CMake 脚本未指定输出编码，
依赖当前 Windows 代码页，导致 UTF-8 中文响应被错误解码。使用 OUTPUT_FILE 捕获原始 stdout 后，
JSON 解析及“子节点”名称验证通过，证实 runner 原始输出正确。
按既有 UTF-8 协议修复 RuntimeBatchTest.cmake 的三个 execute_process，显式 ENCODING UTF-8；
往返用例增加预期中文名称断言，避免两次同样乱码仅靠相等比较通过。不修改引擎或命令行为。
仅重跑该脚本覆盖的两个 batch 用例，2/2 通过、0 失败、0 跳过，证据为
out/verify/20260923-090458-e599f592。结合此前未受脚本修复影响的结果，最终覆盖的 63 项均通过；
没有重新执行整套 63 项或引擎全量测试。

实际使用的定向验证命令：

```powershell
& ./scripts/verify.ps1 -Configuration Debug `
  -Target @('dk_core_tests','dk_math_tests','dk_io_tests','dk_scene_tests',
            'dk_commands_tests','dk_service_tests','dk_protocol_tests','dk_log_probe','dk_run') `
  -TestRegex '^dk\.(core\.|scene\.|services\.|runtime\.|commands\.discovery|protocol\.RPC validates|math\.(eigen values|trs applies)|io\.binary files roundtrip)' `
  -Reason 'vcpkg baseline update: fmt/spdlog logging and flecs scene/service consumers; Catch2 discovery and representative cases for every test executable; runner integration'
```

修复后的复验：

```powershell
& ./scripts/verify.ps1 -Configuration Debug -Target dk_run `
  -TestRegex '^dk\.runtime\.batch_(roundtrip|errors)$' `
  -Reason 'Explicit UTF-8 decoding in the batch integration harness; verify Unicode roundtrip and error-stream handling after dependency upgrade'
```

定向 check-spec.ps1 覆盖 README、四份设计、本记录与开发索引，7 个文档/114 个本地链接、
元数据及 JSON 清单检查通过；git diff --check 通过。

## 完成范围与限制

项目 baseline、本机 vcpkg 索引/CLI、当前启用依赖均已更新，VS2026 开发工程已重新生成，
当前代码无需为新库版本修改 C++ 接口。全部预留依赖及 M4 选型的新版本已解析并记录。
未实际安装/链接 graphics、editor、scripting 或 M4 导入器；这部分能力留到对应阶段实施。
未运行 Release、其他平台或其他旧构建目录；这些目录下次配置时按新 baseline 更新。
M4 实现仍从 M4.1.1 开始，本次升级不改变其阶段状态。

## 修改记录

- 2026-09-23T08:51:49+08:00：读取新旧官方索引，先更新设计并记录完整版本差异和验证范围。
- 2026-09-23T09:05:28+08:00：完成依赖安装、22 包解析与定向验证；修复批处理测试的 UTF-8 解码并单独复验。
