# DeckerEngine

用于渲染、物理实验、场景编辑与自动化操作的 C++23 引擎工程。
命名空间为 `dk`，CMake target 使用 `dk_*` / `dk::*`。

当前已提供工程骨架、core 版本接口、`dk-run --version` 构建探针、
CMake/vcpkg 配置和 spec 开发流程。渲染、场景、物理、编辑器、IPC 和脚本模块均为规划目录。

## 目录

```text
DeckerEngine/
├── AGENTS.md                  # AI 开发入口与约定
├── .agents/skills/             # 仓库开发留档 skill
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json                 # 依赖基线和按模块分组的 features
├── cmake/                     # 选项、toolchain 配置、编译警告
├── engine/
│   ├── foundation/            # core（已构建）、math、io、jobs、metadata
│   ├── platform/              # 窗口和输入接口、SDL3
│   ├── geometry/              # CPU 几何查询和 BVH
│   ├── assets/                # 资产类型、加载、导入
│   ├── scene/                 # 组件、层级、序列化、迁移
│   ├── graphics/              # Vulkan device、presentation、shaders、graph
│   ├── render/                # data、resources、passes、pipelines
│   ├── physics/               # API、CPU/GPU 求解器
│   ├── framework/             # commands、services、operations、runtime
│   ├── automation/            # protocol、transport、client、server
│   ├── scripting/             # API、Lua
│   └── editor/                # model、interaction、widgets、panels
├── apps/                      # runner（当前探针）、editor、ctl
├── tools/                     # assetc、shaderc
├── sdk/python/                # 未来外部自动化客户端
├── shaders/common/            # 公共 Slang 模块
├── projects/demo/             # 示例资产、场景、脚本预留
├── tests/                     # unit、integration、gpu、replay
└── spec/
    ├── roadmap.md             # 阶段路线、依赖与验收条件
    ├── design/                # 每个模块的设计文档
    ├── development/           # 按编号排序的开发记录
    └── templates/             # 两类文档模板
```

仅真实模块建立 CMake target；当前多层关系为根 → engine → foundation → core，
以及根 → apps → runner。其他空目录通过 .gitkeep 留存，开发模块时再增加 CMakeLists。

## Windows 快速验证

需要 Visual Studio 2026 C++ 桌面开发工具和 CMake 4.2+。
基础预设不需要安装 vcpkg 依赖：

```powershell
cmake --preset windows-bootstrap
cmake --build --preset windows-bootstrap-debug
ctest --preset windows-bootstrap-debug
.\out\build\windows-bootstrap\bin\Debug\dk-run.exe --version
```

Release 对应 `windows-bootstrap-release` build/test 预设。
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

`windows-dev` 准备 foundation 依赖。当前构建探针只依赖标准库，
这些依赖供后续模块接入。库应在使用它的模块里通过 find_package 和 target_link_libraries 链接。

| DK_VCPKG_FEATURES | vcpkg 依赖 |
| --- | --- |
| foundation | fmt、spdlog、glm、nlohmann-json |
| scene | flecs |
| graphics | vulkan、vulkan-memory-allocator、shader-slang |
| editor | sdl3[vulkan]、imgui[docking-experimental,sdl3-binding,vulkan-binding] |
| scripting | lua、sol2 |
| tests | catch2 |

需要预下载全部规划中的桌面依赖时，使用
`cmake --preset windows-desktop-deps`。
它会安装更多包，但不会启用尚未实现的引擎功能。按需选择组：

```powershell
cmake --preset windows-dev "-DDK_VCPKG_FEATURES=foundation;scene"
```

Slang 的 vcpkg 包名为 `shader-slang`，CMake package 为 `slang`。
Windows 使用 `x64-windows`，不使用全静态 CRT triplet。
此阶段仅预置 Slang 包，编译反射 API、shader 编译命令和跨平台 host 工具管理
将在对应模块设计后接入。

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

仅验证无依赖骨架时可以运行：

```sh
cmake -S . -B out/build/local -DDK_USE_VCPKG=OFF -DDK_VCPKG_FEATURES=
cmake --build out/build/local --config Debug
ctest --test-dir out/build/local -C Debug --output-on-failure
```

个人路径和构建覆盖放到被 Git 忽略的 CMakeUserPresets.json。
`DK_BUILD_RUNNER`、`DK_BUILD_TESTS` 默认开启；
`DK_WARNINGS_AS_ERRORS` 可按需开启。

## 开发留档

开发前先看 [AGENTS.md](AGENTS.md) 和 [spec 规范](spec/README.md)：
先创建/更新模块设计，然后实现；过程中持续更新编号开发记录。
设计和开发文档都记录创建时间与最后修改时间，创建时间保持不变。

- [整体架构](spec/design/architecture.md)
- [开发 Roadmap](spec/roadmap.md)
- [工程基础设计](spec/design/project-foundation.md)
- [0001 工程初始化记录](spec/development/0001-project-bootstrap.md)
- [decker-spec-workflow skill](.agents/skills/decker-spec-workflow/SKILL.md)

skill 位于仓库 `.agents/skills`，符合
[OpenAI 官方 skill 文档](https://learn.chatgpt.com/docs/build-skills)中的仓库发现规则；
根 AGENTS.md 同时提供固定读取入口。

构建输出和个人环境被 .gitignore 排除。仓库已初始化 main 分支；
当前变更和远程配置分别通过 git status 和 git remote -v 查看。
