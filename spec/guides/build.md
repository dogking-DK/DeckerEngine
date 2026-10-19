---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-10-19T22:04:00+08:00"
---

# 构建与依赖配置

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。下列完整预设 build/ctest 命令用于
所选配置的环境验收；日常改动从[测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md)选择 verify 的目标和筛选。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## Windows 快速验证

M16.1纯CPU路径追踪参考可用`cmake --preset windows-dev -DDK_BUILD_RENDER_PATHTRACING_REFERENCE=ON`，
target为`dk_pathtracing_reference_tests`/`dk_pathtracing_reference_demo`，筛选`^dk\.pathtracing\.`；
不启用Graphics/Slang，固定场景与线性输出用法见[CPU参考指南](pathtracing-reference.md)。

M16.6 GPU路径追踪使用`cmake --preset windows-graphics -DDK_BUILD_RENDER_PATHTRACING=ON`，
选项默认OFF（windows-editor预设开启），PUBLIC依赖CPU reference/Device、PRIVATE依赖Graph/Shader；自动提供reference，不自动升级依赖。
目标`dk_pathtracing_probe`、筛选`^dk\.pathtracing\..*_gpu_required$`要求RT设备和validation/sync，缺环境失败。
异步帧、HDR诊断及数值验收见[GPU路径追踪指南](pathtracing-gpu.md)。

云场可独立纯CPU构建：`cmake --preset windows-scripting -DDK_BUILD_RENDER_CLOUD_FIELDS=ON`，
随后用verify构建`dk_cloud_field_tests`并筛选`^dk\.clouds\.cloud `。
`DK_BUILD_RENDER_CLOUDS=ON`同时构建CPU与GPU纹理和积分参考，windows-graphics及继承的editor预设已启用。
GPU采样验证目标`dk_cloud_field_probe`，筛选`^dk\.clouds\.fields_gpu_validation$`，要求真实设备和validation。
CPU积分参考可独立启用`DK_BUILD_RENDER_CLOUD_REFERENCE=ON`，自动包含fields和atmosphere_reference，
不依赖Graphics/Slang。构建`dk_cloud_reference_tests`并筛选`^dk\.clouds\.(reference |composition |optimization )`。
GPU参考要求Graph/Shaders，验证目标`dk_cloud_reference_probe`，筛选`^dk\.clouds\.reference_gpu_validation$`。
M15.3云/空气/几何合成另需Pipeline与Atmosphere，目标`dk_cloud_composition_probe`，
筛选`^dk\.clouds\.(numeric|composition|failures)_gpu_composition$`；真实截图目标`dk_run`，
筛选`^dk\.clouds\.capture_gpu_composition$`（Windows/Python 3.11+）。全部要求真实GPU及同步validation，无跳过。
逐像素参考最多4096像素、无历史。显式优化入口支持最多1920×1080像素，使用半尺寸重建与每视图历史；
同一`dk_cloud_composition_probe`的优化筛选为`^dk\.clouds\.(optimization|optimization_switches|optimization_boundaries|temporal|history)_gpu_composition$`。
含32预热+120静态帧、相机/风/反遮挡、重置/失败寿命和1080p真实调度；不代表实时性能达标，详见[云优化契约](../design/render-clouds.md#m154-优化与时域实施契约)。
云影CPU仍为`dk_cloud_reference_tests`、筛选`^dk\.clouds\.shadow `；GPU仍为`dk_cloud_composition_probe`、
筛选`^dk\.clouds\.shadow_(numeric|surface|failures)_gpu_composition$`，验证光空间透射、直射接收和快照寿命。
编辑器/capture/sequence共用云模式、显式时间与可选云影。用`dk_run/dk_ctl`筛选`^dk\.clouds\.(cases_unit|sequence_gpu_validation)$`，
GUI另构建`dk_editor_app`筛选`^dk\.editor\.cloud_consistency_gpu_validation$`，见[云操作与用例指南](clouds.md)。

大气双精度参考可单独启用`DK_BUILD_RENDER_ATMOSPHERE_REFERENCE=ON`，只要求Scene/Math，
不引入Graphics或Slang。`DK_BUILD_RENDER_ATMOSPHERE=ON`仍构建参考及GPU实现。
纯CPU与优化性能验证命令见[大气基准指南](atmosphere-benchmark.md)。

仅需外部控制客户端时使用 `cmake --preset windows-client`，随后
`cmake --build out/build/windows-client --config Debug --target dk_ctl`；该配置不启用 Runtime/Scene/Renderer。
用法见 [IPC 指南](ipc.md)。完整 windows-dev/windows-editor 配置同样生成 dk_ctl。

[Python SDK](python.md) 使用外部 Python 3.11+ 解释器和标准库，无额外 pip/vcpkg 库。
同时有 dk_run/dk_ctl 时，CTest 自动发现解释器并注册 dk.python.unit/cpu；开启截图后
另注册 dk.python.capture_gpu。记录/重放另有 dk.replay.cpu，截图配置提供 dk.replay.gpu；
启用 Luau 时 CPU 重放使用脚本保存的初始工程，见 [重放指南](replay.md)。
缺失解释器会提示未注册，不影响原 C++ 构建。
可在配置时使用 `-DPython3_EXECUTABLE=C:/path/to/python.exe` 指定解释器。

需要 Visual Studio 2026 C++ 桌面开发工具和 CMake 4.2+。
设置 `VCPKG_ROOT` 后，在根目录运行 [generate-vs2026.bat](../../generate-vs2026.bat)：

```powershell
.\generate-vs2026.bat
```

脚本复用 `windows-dev` 预设，生成 `out/build/windows-dev/DeckerEngine.slnx`，
用 VS2026 打开即可选择 x64 的 Debug/Release 配置。从其他目录调用脚本也可正常生成。
脚本只配置工程和准备依赖；不会自动编译或打开 IDE，配置失败会返回非零退出码。

解决方案按 `Engine`、`Apps`、`Tests`、`Examples` 和 `CMake` 分组。
`Engine` 下再分 `Foundation`、`Assets`、`Automation`、`Framework`，启用设备模块时增加 `Graphics`，启用渲染模块时增加 `Render`；场景库直接位于 `Engine`。
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

### 新机器接续

先核对 `cmake --version`、VS2026 C++ 工具、`VCPKG_ROOT`、`VULKAN_SDK` 和实际 Python 3.11+ 解释器；
WindowsApps 的 `python.exe` 或 `py.exe` 启动器存在不代表解释器已经安装。
将本机 Python 路径通过 `-DPython3_EXECUTABLE=...` 或忽略的 `CMakeUserPresets.json` 显式传给 CMake。
若 vcpkg 缺少固定 baseline，先获取该提交；只有 Git 对象仍不足以更新旧 checkout 的版本数据库，
确认无本地修改后再快进索引并执行 `bootstrap-vcpkg.bat -disableMetrics`。项目依赖清单与 baseline 保持不变。

`projects/demo/assets` 的素材不随 Git 分发。运行 Sponza 测试前按[素材说明](../../projects/demo/assets/README.md)
恢复 glTF、buffer、纹理及许可文件；有历史 manifest 时逐文件核对 SHA-256。M19.8 只使用 Sponza 与程序化介质，
不读取该目录中另列的 HDR/天空盒。换机、换驱动或重建后必须新目录重采性能，不能拼接旧机器报告。

用 `vulkaninfo --summary` 核对设备与校验层。若已安装 OBS 捕获层产生 API 版本警告，可在测试子进程中设置
`DISABLE_VULKAN_OBS_CAPTURE=1`（来自该层 JSON 的 `disable_environment`）；AMD switchable layer 冲突仅在出现时
使用其 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`。不修改全局注册表或关闭 required Khronos/sync 校验。

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
`windows-scripting` 继承 windows-dev 并启用 `DK_BUILD_SCRIPTING_LUAU`，要求 Scene/Framework Runtime，自动选择 scripting feature，提供 `dk::scripting_luau` 和 runner 的 `--script FILE.luau`；见 [脚本指南](scripting.md)。
`windows-graphics` 继承 windows-dev，显式启用 DK_BUILD_GRAPHICS_DEVICE、DK_BUILD_GRAPHICS_SHADERS、DK_BUILD_GRAPHICS_OFFSCREEN、DK_BUILD_GRAPHICS_GRAPH、DK_BUILD_RENDER_DATA、DK_BUILD_RENDER_RESOURCES、DK_BUILD_RENDER_PIPELINE、DK_BUILD_RENDER_DISK、DK_BUILD_RENDER_CAPTURE、DK_BUILD_PHYSICS_GPU、DK_BUILD_RENDER_SIMULATION、DK_BUILD_RENDER_ATMOSPHERE；
Offscreen 要求 Device、Shaders、Graph，复用已有 feature。设备/资源和离屏探针见 [Graphics 指南](graphics.md)、[离屏指南](offscreen.md)。
Render Data 要求 Scene/Memory，Render Resources 要求 Math/Memory/Device/Graph，仅使用 asset_data 不要求 importer；
Render Pipeline 要求 Render Data/Resources、Graph 和 Shaders；与 Render Atmosphere 同时启用时提供显式 Lambert 日光入口与 dk-sunlight-demo，默认 unlit 不变。Render Disk 要求 Render Data/Resources、Asset Importers/Runtime。Render Capture 要求 Framework/Scene、Jobs、Render Disk/Pipeline，通过独立服务/操作接入 Runtime。五个选项默认 OFF，不增加三方库 feature。用法见 [Render 指南](render.md)。
GPU Physics 要求 Graph/Shaders；Render Simulation 要求 GPU Physics，两者默认 OFF，不增加依赖版本或 feature。示例见 [模拟指南](simulation.md)。
Graph 要求 device；声明/校验/编译不初始化 GPU，无额外依赖 feature，见 [Graph 指南](graph.md)。
`windows-shaders` 单独构建无需 Vulkan 的
离线编译库/工具，见 [Shader 指南](shaders.md)。默认开发预设不加载 Vulkan 或 Slang。
`windows-presentation` 继承 windows-graphics 并显式启用 DK_BUILD_PLATFORM、DK_BUILD_GRAPHICS_PRESENTATION；
独立 platform feature 只引入 SDL3[vulkan]，窗口运行方法见 [呈现指南](presentation.md)。
`windows-editor` 继承 windows-presentation，启用 DK_BUILD_EDITOR；构建目标 dk_editor_app，程序 dk-editor。
运行和验收见 [编辑器指南](editor.md)。
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
| editor | sdl3[vulkan]、imgui[docking-experimental,spectrum,sdl3-binding,vulkan-binding]（DK_BUILD_EDITOR 自动选择） |
| scripting | luau 0.741（DK_BUILD_SCRIPTING_LUAU 自动选择；私有链接 Compiler/VM） |
| tests | catch2（已接入 Core、数学和 IO 单元测试） |
| profiling | tracy[on-demand]（关闭默认 features；启用 DK_ENABLE_PROFILING 时自动选择） |

三方库默认采用 vcpkg 官方收录的最新版本（含 port 修订），完整用途、版本与接入状态见
[三方库说明](../third-party-libraries.md)。当前依赖固定到 2026-09-23 核验的官方基线
`33d78c1ed898a06938f31312167c7abefd229455`，本次 Tracy 接入见
[0023](../development/0023-tracy-cpu-profiling.md)，此前升级见
[0021](../development/0021-vcpkg-baseline-update.md)。已是该索引最新版本的包保持不变；
历史阶段记录中的旧版本是当时的验证结果。

Luau 单独从 [vcpkg-configuration.json](../../vcpkg-configuration.json) 的固定官方 registry 解析，
其余依赖保留上述 builtin-baseline；不默认安装 Luau CLI，也不会因选择 scripting feature 而提供脚本运行功能。
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

GPU timestamp/Tracy 专用预设 `windows-graphics-profiling` 继承 graphics 并启用 profiling；
构建、采集和测量口径见 [GPU 性能观测](gpu-profiling.md)。
