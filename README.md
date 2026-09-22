# DeckerEngine

用于渲染、物理实验、场景编辑与自动化操作的 C++23 引擎工程。
命名空间为 `dk`，CMake target 使用 `dk_*` / `dk::*`。

当前已提供工程骨架、Core 错误/结果类型、稳定 ID、可选日志、Eigen 基础数学与 Transform、工程路径和二进制 IO、Windows 安全保存、
`dk-run --version` 构建探针、CMake/vcpkg 配置和 spec 开发流程。
渲染、场景、物理、编辑器、IPC 和脚本模块尚未实现。

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
│   ├── foundation/            # core、math、io（已构建）；jobs、metadata 预留
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

仅真实模块建立 CMake target；当前多层关系为根 → engine → foundation → core/math/io，
以及根 → apps → runner。其他空目录通过 .gitkeep 留存，开发模块时再增加 CMakeLists。

## Windows 快速验证

需要 Visual Studio 2026 C++ 桌面开发工具和 CMake 4.2+。
安装 vcpkg 并设置 `VCPKG_ROOT`；基础预设仅安装 Core 必需的 stduuid：

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

`windows-dev` 默认构建 Core、日志、Eigen 数学、IO 与 Catch2 单元测试。
启用日志时自动选择 foundation，启用数学时自动选择 math，启用单元测试时自动选择 tests，
并保留 DK_VCPKG_FEATURES 中额外指定的组。
fmt/spdlog 已由 dk::logging 实际链接，Eigen 由 dk::math PUBLIC 传递；
JSON 供后续模块使用，原规划的 GLM 已从清单移除。

| DK_VCPKG_FEATURES | vcpkg 依赖 |
| --- | --- |
| 始终安装（基础依赖） | stduuid（dk::core 私有使用） |
| foundation | fmt、spdlog、nlohmann-json |
| math | eigen3（当前基线 5.0.1） |
| scene | flecs |
| graphics | vulkan、vulkan-memory-allocator、shader-slang |
| editor | sdl3[vulkan]、imgui[docking-experimental,sdl3-binding,vulkan-binding] |
| scripting | lua、sol2 |
| tests | catch2（已接入 Core、数学和 IO 单元测试） |

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

只构建 Core 和版本探针、安装最小 stduuid 依赖时可以运行：

```sh
cmake -S . -B out/build/local-stduuid -DDK_USE_VCPKG=ON -DDK_VCPKG_FEATURES= -DDK_BUILD_LOGGING=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_UNIT_TESTS=OFF
cmake --build out/build/local-stduuid --config Debug
ctest --test-dir out/build/local-stduuid -C Debug --output-on-failure
```

使用 `DK_USE_VCPKG=OFF` 时须自行提供 stduuid CMake package（例如设置
`CMAKE_PREFIX_PATH`），并提供已开启模块需要的其他依赖。
个人路径和构建覆盖放到被 Git 忽略的 CMakeUserPresets.json。
`DK_BUILD_RUNNER`、`DK_BUILD_TESTS`、`DK_BUILD_UNIT_TESTS`、
`DK_BUILD_LOGGING`、`DK_BUILD_MATH`、`DK_BUILD_IO` 默认开启；bootstrap 关闭日志、数学、IO 和单元测试。
DK_BUILD_TESTS=OFF 会关闭全部测试；DK_BUILD_UNIT_TESTS=OFF 仅保留可用的集成探针。
`DK_WARNINGS_AS_ERRORS` 可按需开启。

## Core 能力

| 使用入口 | 头文件 | 内容 |
| --- | --- | --- |
| dk::core | [Error.hpp](engine/foundation/core/include/dk/core/Error.hpp)、[Result.hpp](engine/foundation/core/include/dk/core/Result.hpp) | ErrorCode、错误上下文、std::expected 别名 |
| dk::core | [StableId.hpp](engine/foundation/core/include/dk/core/StableId.hpp) | EntityId/AssetId/SceneId；生成、解析、比较、哈希 |
| dk::logging | [Log.hpp](engine/foundation/core/include/dk/core/Log.hpp) | 显式 Logger 生命周期；stderr/追加文件；模块、级别和 fmt 格式化 |

稳定 ID 由 stduuid 1.2.3 实现生成、解析、格式化与哈希，保留 dk 强类型接口。
ID 为 128 位值，解析要求 36 字符的 8-4-4-4-12 格式，文本输出统一小写；
默认 nil 由上层业务决定是否允许。日志使用方须链接 dk::logging，
其 PUBLIC 依赖会传递 fmt。实例销毁前结束写入线程，需确认写入结果时检查 flush。
完整接口、错误约定及限制见 [Core 设计](spec/design/foundation-core.md)。

## Eigen 基础数学（M1.2）

消费者链接 `dk::math`，包含
[Math.hpp](engine/foundation/math/include/dk/math/Math.hpp)；
[Types.hpp](engine/foundation/math/include/dk/math/Types.hpp)
提供 Vec2/3/4、Mat3/4、Quat 的 f/d 别名，直接使用 Eigen 运算。
约定米、秒、弧度，右手系、+Y 向上、-Z 向前、列向量和列主序。
默认构造不保证初始化，使用 Zero()/Identity() 或显式数值。

```cpp
#include <dk/math/Math.hpp>

const dk::Vec3f axis = dk::Vec3f::UnitZ();
const auto rotation = dk::rotation_from_axis_angle(axis, dk::radians(90.0f));
if (rotation) {
    const dk::Vec3f rotated = *rotation * dk::Vec3f::UnitX(); // 约为 +Y
}
```

`normalize_vector`、`normalize_quaternion`、`rotation_from_axis_angle`
拒绝零向量/零四元数及非有限分量，轴角接口还要求角度有限；
失败返回 Result 错误，并支持极大/极小有限分量的归一化。
具体参数和边界见 [数学设计](spec/design/foundation-math.md)。

## Transform（M1.3）

继续链接 `dk::math`，包含
[Transform.hpp](engine/foundation/math/include/dk/math/Transform.hpp)。
`Trs` / `Transform` 默认 float，显式精度可用 Trsf/Trsd、Transformf/Transformd。
TRS 默认平移为零、旋转为单位四元数、缩放为一；Transform 默认单位矩阵。

- `Transform::from_trs(trs)` 按 T * R * S 构造，自动归一化有效四元数。
- `parent.compose(local)` 按 parent * local 组合，矩阵保留非均匀缩放产生的剪切。
- `transform_point` 包含平移；`transform_direction` 只应用线性部分，保留缩放长度。
- `inverse()` 支持剪切和负缩放；零缩放可正向变换，但求逆返回错误。
- `from_matrix` 只接收有限仿射矩阵，末行须为 [0,0,0,1]；`matrix()` 提供只读访问。

这些构造和运算返回 `Result`：非法参数为 invalid_argument，
奇异/数值秩不足及计算溢出为 invalid_state。
求逆使用线性部分的相对主元阈值 64 * epsilon；完整边界见数学设计。
方向接口不代替法线逆转置；Scene 层级管理和 TRS 分解未包含在本阶段。

```cpp
#include <dk/math/Transform.hpp>

dk::Trs trs;
trs.translation = dk::Vec3f{10.0f, 0.0f, 0.0f};
trs.scale = dk::Vec3f{-2.0f, 1.0f, 1.0f};
const auto transform = dk::Transform::from_trs(trs);
if (transform) {
    const auto point = transform->transform_point(dk::Vec3f{1.0f, 0.0f, 0.0f});
    // point 成功时为 (8, 0, 0)，调用方按 Result 检查错误后使用。
}
```

仅验证 Core/数学、关闭日志和 runner 的独立配置：

```powershell
cmake --preset windows-dev -B out/build/windows-math-only -DDK_BUILD_LOGGING=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-math-only --config Debug
ctest --test-dir out/build/windows-math-only -C Debug --output-on-failure
```

M1.3 验证记录见 [0005](spec/development/0005-transform.md)。

## 工程路径与文件 IO（M1.4）

消费者链接 `dk::io`，包含
[Path.hpp](engine/foundation/io/include/dk/io/Path.hpp) 和
[File.hpp](engine/foundation/io/include/dk/io/File.hpp)，无需额外三方库。

- `path_from_utf8` / `path_to_utf8` 转换 UTF-8 与原生路径；拒绝空串、NUL 和非法 Unicode。
- `ProjectPaths::create(root)` 固定已有工程根目录；`resolve(relative)` 规范化相对路径，
  拒绝绝对路径和词法越界，不受之后工作目录改变影响。
- `read_file_bytes(path, max_bytes)` 返回 ByteBuffer；默认上限 64 MiB，超限返回错误。
- `write_file_bytes(path, bytes)` 创建或截断普通文件，不创建父目录，空数据生成空文件。

所有操作返回 `Result`；路径/参数错误为 invalid_argument，缺失文件或父目录为 not_found，
其他系统错误为 io_error，错误上下文包含操作和可用的路径、系统诊断。
工程路径解析仅处理词法结构，子路径符号链接仍按 OS 解析。
普通写入失败可能留下空文件或部分数据；需要旧文件保护时使用下方 M1.5 安全保存接口。

```cpp
#include <dk/io/File.hpp>
#include <dk/io/Path.hpp>

const auto root = dk::path_from_utf8("projects/demo");
if (root) {
    const auto project = dk::ProjectPaths::create(*root);
    if (project) {
        const auto file = project->resolve("data.bin");
        if (file) {
            const auto bytes = dk::read_file_bytes(*file);
            // 按 Result 检查读取结果；此示例不会创建或覆盖文件。
        }
    }
}
```

仅验证 Core/IO、关闭数学、日志和 runner，并开启警告即错误：

```powershell
cmake --preset windows-dev -B out/build/windows-io-only -DDK_BUILD_MATH=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-io-only --config Debug
ctest --test-dir out/build/windows-io-only -C Debug --output-on-failure
```

Windows 开发构建现有 86 项 CTest（26 项 IO/安全保存、42 项数学/Transform、16 项 Core、
日志流探针和版本探针），bootstrap 保留 1 项版本测试。
边界与验证见 [IO 设计](spec/design/foundation-io.md) 和 [0006](spec/development/0006-foundation-io.md)。

## 安全保存（M1.5）

继续链接 `dk::io`、包含 File.hpp，调用 `write_file_bytes_atomic(path, bytes)`。
首版支持 Windows 本地普通文件：同目录独占创建临时文件，写完、刷新并关闭后重命名替换。
保存空数据、新建文件和覆盖文件使用同一接口；父目录必须存在。

写入/刷新/关闭/替换失败时保留旧内容并清理本次临时文件；如果清理也失败，
错误上下文保留主错误并列出清理错误和临时路径。临时重名不会覆盖其他文件。
与普通写入一样返回 `Result<void>`，无新增 vcpkg 依赖。

目标重解析点、设备名和备用数据流不接受；UNC/网络驱动器及其他操作系统返回 not_supported。
替换会改变文件身份和元数据，不保留旧 ACL/时间/备用数据流，其他硬链接仍指向旧文件。
原子可见性依赖本地同卷重命名语义，已在本机 NTFS 验证；不保证断电持久化或外部并发修改隔离。

Debug/Release 各 **85 项通过、1 项跳过**；独立 Core/IO **35 项通过、1 项跳过**，无失败。
跳过项为符号链接目标测试，当前环境没有创建符号链接权限。
故障注入与真实共享冲突/只读目标等验证见 [0007](spec/development/0007-atomic-file-save.md)。
下一阶段是 **M1.6 Foundation 集成验收**，串联 ID、Transform 和保存/重载。

## 开发留档

开发前先看 [AGENTS.md](AGENTS.md) 和 [spec 规范](spec/README.md)：
先创建/更新模块设计，然后实现；过程中持续更新编号开发记录。
设计和开发文档都记录创建时间与最后修改时间，创建时间保持不变。
Roadmap 的 M0–M10 均拆为 Mx.y 小阶段，各自包含前置、范围和验收；
默认“下一步开发”只推进一个小阶段，完成后同步状态及下一项。

- [整体架构](spec/design/architecture.md)
- [开发 Roadmap](spec/roadmap.md)
- [工程基础设计](spec/design/project-foundation.md)
- [Core 基础设计](spec/design/foundation-core.md)
- [Eigen 数学设计](spec/design/foundation-math.md)
- [工程路径与文件 IO 设计](spec/design/foundation-io.md)
- [0006 文件 IO 开发记录](spec/development/0006-foundation-io.md)
- [0007 安全保存开发记录](spec/development/0007-atomic-file-save.md)
- [0005 Transform 开发记录](spec/development/0005-transform.md)
- [0004 Eigen 基础数学与阶段细分](spec/development/0004-eigen-math-foundation.md)
- [0003 stduuid 迁移记录](spec/development/0003-stduuid-migration.md)
- [0002 Core 开发记录](spec/development/0002-foundation-core.md)
- [0001 工程初始化记录](spec/development/0001-project-bootstrap.md)
- [decker-spec-workflow skill](.agents/skills/decker-spec-workflow/SKILL.md)

skill 位于仓库 `.agents/skills`，符合
[OpenAI 官方 skill 文档](https://learn.chatgpt.com/docs/build-skills)中的仓库发现规则；
根 AGENTS.md 同时提供固定读取入口。

构建输出和个人环境被 .gitignore 排除。仓库已初始化 main 分支；
当前变更和远程配置分别通过 git status 和 git remote -v 查看。
