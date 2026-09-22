# DeckerEngine

用于渲染、物理实验、场景编辑与自动化操作的 C++23 引擎工程。
命名空间为 `dk`，CMake target 使用 `dk_*` / `dk::*`。

当前已提供工程骨架、Core 错误/结果类型、稳定 ID、可选日志、Eigen 基础数学与 Transform、工程路径和二进制 IO、Windows 安全保存、Foundation CPU 集成示例、
`dk-run --version` 构建探针、CMake/vcpkg 配置和 spec 开发流程。
已接入 flecs 场景文档、组件与变换层级、工程/资产引用、JSON 快照保存和安全重载。
M3.1 已提供独立命令注册表、参数/结果 schema 校验及 commands.list / commands.describe。
M3.2 通过 dk::scene_services 和 dk::scene_operations 提供会话管理、场景编辑、查询与保存。
M3.3 提供事务与有界历史；M3.4 的 dk-run 支持无窗口 CPU 批处理和 JSON-RPC。
M3.5 已接入持续 stdio、同步任务查询与正常关闭，达到交付 A。
渲染、物理、编辑器、网络/命名管道 IPC 和脚本模块尚未实现。

## 目录

```text
DeckerEngine/
├── AGENTS.md                  # AI 开发入口与约定
├── .agents/skills/             # 留档、定向验证、状态契约与命令开发 skills
├── CMakeLists.txt
├── CMakePresets.json
├── generate-vs2026.bat        # 生成 VS2026 x64 开发工程
├── vcpkg.json                 # 依赖基线和按模块分组的 features
├── cmake/                     # 选项、toolchain 配置、编译警告
├── scripts/                   # 定向构建/测试和文档检查
├── engine/
│   ├── foundation/            # core、math、io（已构建）；jobs、metadata 预留
│   ├── platform/              # 窗口和输入接口、SDL3
│   ├── geometry/              # CPU 几何查询和 BVH
│   ├── assets/                # types 已实现；加载、导入预留
│   ├── scene/                 # 文档、组件、层级、工程与 JSON 持久化
│   ├── graphics/              # Vulkan device、presentation、shaders、graph
│   ├── render/                # data、resources、passes、pipelines
│   ├── physics/               # API、CPU/GPU 求解器
│   ├── framework/             # commands、services、operations、runtime
│   ├── automation/            # JSON-RPC、JSON Lines；网络和 SDK 预留
│   ├── scripting/             # API、Lua
│   └── editor/                # model、interaction、widgets、panels
├── apps/                      # runner CPU CLI；editor、ctl 预留
├── tools/                     # assetc、shaderc
├── sdk/python/                # 未来外部自动化客户端
├── shaders/common/            # 公共 Slang 模块
├── projects/demo/             # 示例资产、场景、脚本预留
├── tests/                     # unit、integration、gpu、replay
├── examples/foundation/       # 独立 ID/变换/安全保存示例
├── examples/scene/            # CPU 场景创建与跨进程重载
└── spec/
    ├── roadmap.md             # 阶段路线、依赖与验收条件
    ├── design/                # 每个模块的设计文档
    ├── development/           # 按编号排序的开发记录
    ├── commands/              # 命令目录、参数、返回值和调用示例
    └── templates/             # 两类文档模板
```

仅真实模块建立 CMake target；engine 管理 foundation、assets/types、scene、framework 和 automation，
apps 管理 runner。其他空目录通过 .gitkeep 留存，开发模块时再增加 CMakeLists。

## 定向验证与开发辅助

默认只构建和验证本次目标功能及直接受影响的调用链。已配置 windows-dev 时，例如：

```powershell
pwsh -NoProfile -File scripts/verify.ps1 -Target dk_commands_tests -TestRegex '^dk\.commands\.discovery is sorted' -Reason '验证命令发现'
pwsh -NoProfile -File scripts/check-spec.ps1
```

verify 默认 Debug 单配置；用 `-BuildDir` 指定其他已配置目录，`-Configuration Release` 选择 Release。
先构建指定目标再枚举/执行匹配测试；构建失败或零匹配都会报错，不回退到全量。
结果和日志在 `out/verify/<本次运行>/`，summary.json 区分通过/失败/跳过和未执行步骤。
完整回归需要显式 `-Full -Reason '具体原因'`；它仅覆盖当前构建树和所选配置。
纯文档可通过 `& ./scripts/check-spec.ps1 -Path @('README.md', 'spec/commands/entity.md')` 限定扫描。

三个 [开发辅助 skill](spec/README.md#开发辅助-skills) 分别指导验证范围、状态变更与失败处理、
命令实现和文档同步；已有留档 skill 继续负责按改动规模记录。按任务使用，无需每次全部加载。

## CPU 批处理

完整命令用法见 [命令参考](spec/commands/README.md)，按发现、场景、实体、历史和运行时分类。

先按 windows-dev 构建，然后在现有工程目录执行示例：

```powershell
New-Item -ItemType Directory -Force out/demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --batch examples/automation/create-scene.jsonl --auto-guard
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --batch examples/automation/load-scene.jsonl
```

第一进程用一个事务创建父子实体和变换，分别保存 scene.json/project.json；第二进程重新加载并查询。
输入 UTF-8 JSON Lines，每行一个 JSON-RPC 2.0 请求或 1–128 项协议 batch；通知没有响应。
stdout 每行一个 JSON 响应，result 含 task_id、status 和命令返回值 value，stderr 仅诊断。
--auto-guard 显式允许离线顺序脚本为省略 guard 的命令注入当前状态，显式 guard 始终保留。
默认仍要求编辑 guard，协议 batch 与 scene.transaction 的原子事务不同。

退出码：0 全部成功，1 批处理含可恢复请求错误（继续后续行），2 参数/启动文件错误，3 致命流/资源错误。
单行上限 1 MiB，超长行排空后报告错误；详细错误映射见 [协议设计](spec/design/automation-protocol.md)。

仅构建 CPU Runtime、保留进程验收而关闭日志/示例/Catch2：

```powershell
cmake --preset windows-dev -B out/build/windows-runtime-cpu -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-runtime-cpu --config Debug
ctest --test-dir out/build/windows-runtime-cpu -C Debug --output-on-failure
```

Release 替换配置名。该配置只装配 stduuid、Eigen、flecs、JSON，未链接窗口/GPU。

## 持续 stdio 服务

```powershell
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --stdio
```

从 stdin 逐行发送 UTF-8 JSON-RPC，进程立即刷新每条响应；不必关闭 stdin。
可先发送下面两行，查看能力和创建场景：

```jsonl
{"jsonrpc":"2.0","id":1,"method":"runtime.capabilities"}
{"jsonrpc":"2.0","id":2,"method":"scene.new"}
```

读取第二条响应的 result.value，其中 document_id/revision 构成下一次编辑的 guard。
每次编辑完成后用返回的 state 更新 guard；stdio 不允许 --auto-guard。
commands.list / commands.describe 提供实际注册能力和参数/结果 schema。

所有操作同步完成；成功为 result.status=succeeded，已分派命令失败在 error.data 返回 task_id/status=failed。
tasks.list 枚举最近 256 个已完成任务，tasks.get({id:任务UUID}) 查询元数据；不保留大结果，进程重启后清空。
JSON-RPC 请求 id 用于关联响应，task_id 用于查询执行记录。无异步 wait/cancel 能力。
runtime.shutdown 在输出响应后正常关闭，也可关闭 stdin；stdio 的可恢复请求错误不会改变正常退出码 0。
启动错误仍为 2，致命流/资源错误为 3；输出失败时应查询场景状态，不能据此假定编辑未执行。

完整交互及重启验收见 [RuntimeStdioTest.ps1](tests/integration/RuntimeStdioTest.ps1)，
协议定义和限制见 [automation-protocol](spec/design/automation-protocol.md)。

## 命令层独立验证

启用 FRAMEWORK 和 Scene 时，`register_scene_commands(registry, service)` 注册
scene.new/load/query/save、project.save、entity.create/delete/get/set_name/set_transform/set_parent/set_assets。
编辑和保存参数使用 `guard: {document_id, revision}`；new/load 替换现有场景也必须携带 guard。
从返回的 state 读取最新 guard；保存到磁盘后，重新载入会生成新的 document_id。
scene.query 支持 offset（默认 0）和 limit（默认 128，最多 256），返回 has_more 和稳定 ID 排序的实体页。
变换使用 translation[3]、rotation[x,y,z,w]、scale[3]，查询 world_matrix 按行展开。
场景和工程清单分别保存。详见 [应用服务设计](spec/design/application-services.md)。

`scene.transaction` 接收一个 guard 和 1–128 条 `{method, params}`，内部只允许六种 entity 编辑，
内层不传 guard；失败整体回滚，成功只增加一次 revision。单条编辑也进入同一历史机制。
`history.status` 查询历史，`history.undo/redo` 使用最新 guard；撤销重做保留实体 ID 并继续递增 revision。
历史默认最多 64 单元/32 MiB 逻辑载荷，保存保留历史，new/load 清空；文件保存不支持撤销。

`dk::commands` 的 `CommandRegistry` 注册参数/结果 schema、effect 和同步 handler。
`commands.list` 枚举能力，`commands.describe` 返回完整契约；未知命令、参数错误和
handler 契约错误分别返回结构化 Error。支持的 schema 子集与上限见
[命令设计](spec/design/commands.md)。

```powershell
cmake --preset windows-dev -B out/build/windows-commands-only -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=ON -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-commands-only --config Debug
ctest --test-dir out/build/windows-commands-only -C Debug --output-on-failure
```

Release 替换配置名即可；此配置不构建 Scene、Eigen、IO、窗口或 GPU。

## Windows 快速验证

需要 Visual Studio 2026 C++ 桌面开发工具和 CMake 4.2+。
设置 `VCPKG_ROOT` 后，在根目录运行 [generate-vs2026.bat](generate-vs2026.bat)：

```powershell
.\generate-vs2026.bat
```

脚本复用 `windows-dev` 预设，生成 `out/build/windows-dev/DeckerEngine.slnx`，
用 VS2026 打开即可选择 x64 的 Debug/Release 配置。从其他目录调用脚本也可正常生成。
脚本只配置工程和准备依赖；不会自动编译或打开 IDE，配置失败会返回非零退出码。

解决方案按 `Engine`、`Apps`、`Tests`、`Examples` 和 `CMake` 分组。
`Engine` 下再分 `Foundation`、`Assets`、`Automation`、`Framework`；场景库直接位于 `Engine`。
测试程序及日志探针集中在 `Tests`，`dk_run` 位于 `Apps`，CMake 辅助项目位于 `CMake`。
分组由 CMake 目录继承维护；未来工具 target 归入 `Tools`，空分组不会显示。
已打开解决方案时，重新运行脚本后在 VS 接受重新加载提示；也可关闭并重新打开 `.slnx`。

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

`windows-dev` 默认构建 Core、日志、Eigen 数学、IO、Scene 与 Catch2 单元测试。
`DK_BUILD_SCENE` 默认 OFF，开发预设启用，并自动选择 scene feature。
`DK_BUILD_FRAMEWORK` 默认 OFF，开发预设启用；命令层选择 commands feature，并 PUBLIC 使用 JSON。
启用日志时自动选择 foundation，启用数学时自动选择 math，启用单元测试时自动选择 tests，
并保留 DK_VCPKG_FEATURES 中额外指定的组。
fmt/spdlog 已由 dk::logging 实际链接，Eigen 由 dk::math PUBLIC 传递；
JSON 由 Scene 私有使用，原规划的 GLM 已从清单移除。

| DK_VCPKG_FEATURES | vcpkg 依赖 |
| --- | --- |
| 始终安装（基础依赖） | stduuid（dk::core 私有使用） |
| foundation | fmt、spdlog、nlohmann-json |
| math | eigen3（当前基线 5.0.1） |
| scene | flecs、nlohmann-json |
| commands | nlohmann-json |
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
`DK_BUILD_LOGGING`、`DK_BUILD_MATH`、`DK_BUILD_IO`、`DK_BUILD_EXAMPLES` 默认开启；bootstrap 关闭日志、数学、IO、示例和单元测试。
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
cmake --preset windows-dev -B out/build/windows-math-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES=
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
cmake --preset windows-dev -B out/build/windows-io-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-io-only --config Debug
ctest --test-dir out/build/windows-io-only -C Debug --output-on-failure
```

M1.6 的 Windows 开发构建包含 101 项 CTest（26 项 IO/安全保存、42 项数学/Transform、16 项 Core、
15 项 Foundation 集成、日志流探针和版本探针），bootstrap 保留 1 项版本测试。
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

安全保存测试已加入全量回归；符号链接目标测试在无创建权限的环境跳过。
故障注入与真实共享冲突/只读目标等验证见 [0007](spec/development/0007-atomic-file-save.md)。

## Foundation CPU 示例（M1.6）

`dk-foundation-demo` 串联 ID、父子变换、工程路径、安全保存和重载。
默认在 math/io 均启用时构建；关闭 `DK_BUILD_EXAMPLES` 可禁用。
使用已有工程根目录，`save` 会覆盖指定相对文件并读回验证，`load` 只读：

```powershell
New-Item -ItemType Directory -Force out/demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-foundation-demo.exe save out/demo sample.dkf
.\out\build\windows-dev\bin\Debug\dk-foundation-demo.exe load out/demo sample.dkf
```

两个进程输出相同 ID、世界坐标和点往返结果；日志/错误只到 stderr。
`.dkf` 是当前示例专用格式，不是未来场景协议，读取限制 4096 字节。
M1.6 已通过：默认 Debug/Release 各 **100 项通过、1 项权限跳过**，无失败。

无日志、runner、Catch2、窗口和 GPU 依赖的独立配置：

```powershell
cmake --preset windows-dev -B out/build/windows-foundation -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-foundation --config Debug
ctest --test-dir out/build/windows-foundation -C Debug --output-on-failure
cmake --build out/build/windows-foundation --config Release
ctest --test-dir out/build/windows-foundation -C Release --output-on-failure
```

该配置仅安装 stduuid、Eigen 及 vcpkg 构建辅助包，Debug/Release 各 **15/15** 通过。
当前安全保存验收仍限 Windows 本地文件。
详见 [集成设计](spec/design/foundation-integration.md) 和 [0008](spec/development/0008-foundation-integration.md)。

## SceneDocument（M2.1–M2.4）

链接 `dk::scene`，包含 [SceneDocument.hpp](engine/scene/include/dk/scene/SceneDocument.hpp)。
`create()` 返回持有私有 flecs world 的文档；`create_entity()` 生成持久 EntityId，
也可传入已有 ID。重复/nil ID 和缺失删除会返回错误，成功修改递增 revision。
`entity_ids()` 返回排序副本，`validate()` 检查 ECS 与身份索引一致性。
新文档 dirty=true。`entity()` 返回名称、局部 Trsd 和父 ID 的副本；
`set_name` / `set_local_transform` / `set_parent` 验证后编辑，`world_transform` 返回派生仿射矩阵。
重挂保留局部 TRS，删除有子节点的实体被拒绝；循环、缺失父级、非有限变换均不改变旧状态。
`scene_component_descriptors()` 提供稳定组件名、版本及字段类型。Scene 要求 math/io 同时开启。
`set_asset_references()` 设置 AssetId/AssetKind 列表，验证 nil、重复 ID 和种类。
`dk::asset_types` 仅包含持久资产类型；没有资产解码或设备资源。
设计见 [scene.md](spec/design/scene.md)，验收见 [0009](spec/development/0009-scene-identity.md)、
[0010](spec/development/0010-scene-hierarchy.md)。

[Project.hpp](engine/scene/include/dk/scene/Project.hpp) 提供版本 1 工程清单的
`parse_project` / `serialize_project`、`Project::create` / `open`、资产路径解析和文件校验。
`check_asset_references(scene, project)` 给出包含实体/资产 ID 的引用诊断。
工程根必须存在，清单的 scene/assets 路径是使用 `/` 的规范相对路径。
JSON 限制 16 MiB、64 层嵌套；拒绝未知版本、字段、重复键/ID 和路径越界。
设计与协议见 [project-format.md](spec/design/project-format.md)，记录见 [0011](spec/development/0011-project-assets.md)。

独立 Scene 配置（关闭日志、示例与 runner）：

```powershell
cmake --preset windows-dev -B out/build/windows-scene-only -DDK_BUILD_MATH=ON -DDK_BUILD_IO=ON -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-only --config Debug
ctest --test-dir out/build/windows-scene-only -C Debug --output-on-failure
```

## 场景保存与 CPU 示例

场景持久化入口为 [SceneIO.hpp](engine/scene/include/dk/scene/SceneIO.hpp)：

- `snapshot()` 捕获不可变实体数据和 revision；`serialize_scene` / `parse_scene` 处理 v1 JSON。
- `save_scene(document, project)` 保存当前快照；也可传入此前快照，后续编辑仍保持 dirty。
- `load_scene(project)` 返回干净文档；内存 `parse_scene` 返回 dirty 的导入文档。
- `reload_scene(owner, project)` 先构建并验证候选，成功才交换所有者，失败保留旧对象。
- `save_project(project, relative_manifest)` 安全保存工程清单。

场景和组件均显式 version=1，最多 10000 实体；先登记实体再解析关系，不依赖文件顺序。
保存先读回并验证临时文件，然后原子替换；继承 Windows 本地文件边界。
Scene/Project 文件各自原子，不构成跨文件事务。协议细节与测试见
[Scene 设计](spec/design/scene.md)、[0012](spec/development/0012-scene-persistence.md)。

CPU 示例要求已有根目录和资产引用占位文件（本阶段不解码网格）。create 会覆盖其中的 scene.json/project.json：

```powershell
New-Item -ItemType Directory -Force out/demo-scene | Out-Null
Set-Content -LiteralPath out/demo-scene/mesh.bin -Value "M2 reference fixture"
.\out\build\windows-dev\bin\Debug\dk-scene-demo.exe create out/demo-scene
.\out\build\windows-dev\bin\Debug\dk-scene-demo.exe load out/demo-scene
```

无日志、runner 和 Catch2 的场景示例配置：

```powershell
cmake --preset windows-dev -B out/build/windows-scene-cpu -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-cpu --config Debug
ctest --test-dir out/build/windows-scene-cpu -C Debug --output-on-failure
```

## 开发留档流程

M1.6 与 M2.1–M2.4 已完成并分节本地提交。最终默认 Debug/Release 各 128 通过、
1 项既有符号链接权限跳过；独立 Scene 配置各 104 通过、1 跳过，纯 CPU 示例各 16/16。
下一小阶段为 M3.1 命令注册与能力发现。

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
- [0008 Foundation 集成验收](spec/development/0008-foundation-integration.md)
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
