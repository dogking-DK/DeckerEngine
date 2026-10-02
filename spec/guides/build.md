---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-10-03T07:28:09+08:00"
---

# 构建与依赖配置

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。下列完整预设 build/ctest 命令用于
所选配置的环境验收；日常改动从[测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)选择 verify 的目标和筛选。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## Windows 快速验证

需要 Visual Studio 2026 C++ 桌面开发工具和 CMake 4.2+。
设置 `VCPKG_ROOT` 后，在根目录运行 [generate-vs2026.bat](../../generate-vs2026.bat)：

```powershell
.\generate-vs2026.bat
```

脚本复用 `windows-dev` 预设，生成 `out/build/windows-dev/DeckerEngine.slnx`，
用 VS2026 打开即可选择 x64 的 Debug/Release 配置。从其他目录调用脚本也可正常生成。
脚本只配置工程和准备依赖；不会自动编译或打开 IDE，配置失败会返回非零退出码。

解决方案按 `Engine`、`Apps`、`Tests`、`Examples` 和 `CMake` 分组。
`Engine` 下再分 `Foundation`、`Assets`、`Automation`、`Framework`，启用设备模块时增加 `Graphics`；场景库直接位于 `Engine`。
测试程序及日志探针集中在 `Tests`，`dk_run` 位于 `Apps`，CMake 辅助项目位于 `CMake`。
分组由 CMake 目录继承维护；assetc 等工具 target 归入 `Tools`，空分组不会显示。
已打开解决方案时，重新运行脚本后在 VS 接受重新加载提示；也可关闭并重新打开 `.slnx`。

安装 vcpkg 并设置 `VCPKG_ROOT`；基础预设安装 Core 必需的 stduuid 和 magic-enum：

```powershell
cmake --preset windows-bootstrap
cmake --build --preset windows-bootstrap-debug
ctest --preset windows-bootstrap-debug
.\out\build\windows-bootstrap-stduuid\bin\Debug\dk-run.exe --version
```

Release 对应 `windows-bootstrap-release` build/test 预设。
bootstrap 使用新的 `windows-bootstrap-stduuid` 构建目录，避免复用原无依赖配置的缓存。
核心代码最低要求 CMake 3.28；Windows 共享预设选择了要求 CMake 4.2+ 的
Visual Studio 18 2026 生成器。其他工具链可在个人预设中指定生成器。

## vcpkg 开发配置

安装 vcpkg 后，将 `VCPKG_ROOT` 指向其根目录，并确保该 checkout 包含清单中的
builtin-baseline 提交。配置阶段会自动执行 manifest install；
共享文件不写入开发者本机绝对路径。

```powershell
$env:VCPKG_ROOT = "C:\path\to\vcpkg" # 换成你的实际路径
cmake --preset windows-dev
cmake --build --preset windows-debug
ctest --preset windows-debug
```

`windows-dev` 默认构建 Core、日志、Eigen 数学、IO、Scene、Memory、Jobs、CPU 资产导入/缓存、Framework 与 Catch2 单元测试。
`windows-graphics` 继承该预设，显式启用 DK_BUILD_GRAPHICS_DEVICE、DK_BUILD_GRAPHICS_SHADERS、DK_BUILD_GRAPHICS_OFFSCREEN、DK_BUILD_GRAPHICS_GRAPH；
Offscreen 要求前两个模块，复用其 feature。设备/资源和离屏探针见 [Graphics 指南](graphics.md)、[离屏指南](offscreen.md)。
Graph 要求 device；声明/校验/编译不初始化 GPU，无额外依赖 feature，见 [Graph 指南](graph.md)。
`windows-shaders` 单独构建无需 Vulkan 的
离线编译库/工具，见 [Shader 指南](shaders.md)。默认开发预设不加载 Vulkan 或 Slang。
`windows-presentation` 继承 windows-graphics 并显式启用 DK_BUILD_PLATFORM、DK_BUILD_GRAPHICS_PRESENTATION；
独立 platform feature 只引入 SDL3[vulkan]，窗口运行方法见 [呈现指南](presentation.md)。
`DK_BUILD_MEMORY` 默认 OFF，开发及 profiling 预设启用，自动选择 memory feature；bootstrap 保持关闭。
`DK_BUILD_ASSET_RUNTIME` 默认 OFF，开发及 profiling 预设启用并选择 assets feature；要求 IO/Memory。
关闭 Memory/IO 的配置也须关闭 ASSET_RUNTIME；Scene/Framework 可独立关闭，只有 Project 适配依赖它们。
`DK_BUILD_SCENE` 默认 OFF，开发预设启用，并自动选择 scene feature。
`DK_BUILD_FRAMEWORK` 默认 OFF，开发预设启用；命令层选择 commands feature，并 PUBLIC 使用 JSON。
启用日志时自动选择 foundation，启用数学时自动选择 math，启用单元测试时自动选择 tests，
并保留 DK_VCPKG_FEATURES 中额外指定的组。
fmt/spdlog 已由 dk::logging 实际链接，Eigen 由 dk::math PUBLIC 传递；
JSON 由 Scene 私有使用，原规划的 GLM 已从清单移除。

| DK_VCPKG_FEATURES | vcpkg 依赖 |
| --- | --- |
| 始终安装（基础依赖） | stduuid、magic-enum 0.9.8（各消费者 PRIVATE 使用） |
| foundation | fmt、spdlog、nlohmann-json |
| math | eigen3（当前基线 5.0.1） |
| memory | mimalloc 3.5.3（无 override；dk::memory 私有使用） |
| scene | flecs、nlohmann-json |
| assets | nlohmann-json、xxhash（关闭默认 features） |
| asset-importers | fastgltf、stb、nlohmann-json |
| commands | nlohmann-json |
| vulkan-device | vulkan、volk、vk-bootstrap、vulkan-memory-allocator（DK_BUILD_GRAPHICS_DEVICE 自动选择，要求 Memory） |
| shaders | shader-slang、nlohmann-json（DK_BUILD_GRAPHICS_SHADERS 自动选择，要求 Memory/IO） |
| graphics | vulkan、volk、vk-bootstrap、vulkan-memory-allocator、shader-slang |
| platform | sdl3[vulkan]（DK_BUILD_PLATFORM 自动选择；不含 ImGui） |
| editor | sdl3[vulkan]、imgui[docking-experimental,sdl3-binding,vulkan-binding]（编辑器仍预留） |
| scripting | lua、sol2 |
| tests | catch2（已接入 Core、数学和 IO 单元测试） |
| profiling | tracy[on-demand]（关闭默认 features；启用 DK_ENABLE_PROFILING 时自动选择） |

三方库默认采用 vcpkg 官方收录的最新版本（含 port 修订），完整用途、版本与接入状态见
[三方库说明](../third-party-libraries.md)。当前依赖固定到 2026-09-23 核验的官方基线
`33d78c1ed898a06938f31312167c7abefd229455`，本次 Tracy 接入见
[0023](../development/0023-tracy-cpu-profiling.md)，此前升级见
[0021](../development/0021-vcpkg-baseline-update.md)。已是该索引最新版本的包保持不变；
历史阶段记录中的旧版本是当时的验证结果。
2026-09-24 接入 magic-enum 时重新核验官方最新版本为 0.9.8，与固定基线一致；
转换范围与兼容验证见 [0028](../development/0028-magic-enum.md)。
在 vcpkg 仓库目录确认没有本地修改后执行 `git pull --ff-only`，并运行
`.\bootstrap-vcpkg.bat -disableMetrics` 更新配套工具；完成后回到 DeckerEngine 目录，
重新执行 `cmake --preset windows-dev`。
项目配置依然按清单的固定 baseline 解析，不随本机索引自动漂移。

需要预下载全部规划中的桌面依赖时，使用
`cmake --preset windows-desktop-deps`。
它会安装更多包，但不会启用尚未实现的引擎功能。按需选择组：

```powershell
cmake --preset windows-dev "-DDK_VCPKG_FEATURES=foundation;scene"
```

Slang 的 vcpkg 包名为 `shader-slang`，CMake package 为 `slang`。
Windows 使用 `x64-windows`，不使用全静态 CRT triplet。
M5.3 已接入 Slang 编译反射 API 和 dk-shaderc；跨平台 host 工具管理尚未实现。
Slang 标准模块/API binding 文件必须跟随运行库，构建通过 dk_deploy_slang 部署。

feature 选择在 `project()` 前映射到 `VCPKG_MANIFEST_FEATURES`，
遵循 [vcpkg CMake 集成规范](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)。
更换生成器、triplet 或开关 vcpkg 时使用独立构建目录。

## Ninja / 其他平台

在已设置 C++ 编译器、Ninja 和 VCPKG_ROOT 的环境中：

```sh
cmake --preset ninja-debug
cmake --build --preset ninja-debug
ctest --preset ninja-debug
```

Release 使用 `ninja-release`。Windows 下需在 Developer PowerShell/命令行初始化
MSVC 环境；Linux/macOS 需自行准备支持 C++23 的编译器。
这些入口需在目标平台另行验证；当前不承诺完整引擎的跨平台支持。

只构建 Core 和版本探针、安装基础 stduuid/magic-enum 依赖时可以运行：

```sh
cmake -S . -B out/build/local-stduuid -DDK_USE_VCPKG=ON -DDK_VCPKG_FEATURES= -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_UNIT_TESTS=OFF
cmake --build out/build/local-stduuid --config Debug
ctest --test-dir out/build/local-stduuid -C Debug --output-on-failure
```

使用 `DK_USE_VCPKG=OFF` 时须自行提供 stduuid 和 magic_enum CMake package（例如设置
`CMAKE_PREFIX_PATH`），并提供已开启模块需要的其他依赖。
个人路径和构建覆盖放到被 Git 忽略的 CMakeUserPresets.json。
`DK_BUILD_RUNNER`、`DK_BUILD_TESTS`、`DK_BUILD_UNIT_TESTS`、
`DK_BUILD_LOGGING`、`DK_BUILD_MATH`、`DK_BUILD_IO`、`DK_BUILD_EXAMPLES` 默认开启；bootstrap 关闭日志、数学、IO、示例和单元测试。
DK_BUILD_TESTS=OFF 会关闭全部测试；DK_BUILD_UNIT_TESTS=OFF 仅保留可用的集成探针。
`DK_WARNINGS_AS_ERRORS` 可按需开启。
