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
M1.7.1–6 已接入可选 Tracy 分析、mimalloc heap、PMR、拥有型接口、持久域路由、线程 scratch 和局部/共享 Pool。
内存域支持预算、关闭闸门和跨线程释放；arena 支持嵌套回退，Pool 提供 ObjectPool、安全 trim 和用量曲线。
RoutingToken 与 ThreadContextCache 支持跨线程重绑定、线程复用及安全点退休；Jobs/Runtime 自动装配留在后续阶段。
M1.7.7 已提供重复工作负载、三种 profiling 配置的真实采集对照与[性能基线](spec/benchmarks/2026-09-28-memory.md)，M1.7 全部完成。
M4.1 已提供 meta v1、身份目录、登记提交、Project 替换、同目录改名及未完成操作恢复。
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
│   ├── foundation/            # core、math、io、profiling、memory/持久路由；jobs、metadata 待实现
│   ├── platform/              # 窗口和输入接口、SDL3
│   ├── geometry/              # CPU 几何查询和 BVH
│   ├── assets/                # types、runtime 元数据/身份目录；加载、导入预留
│   ├── scene/                 # 文档、组件、层级、工程与 JSON 持久化
│   ├── graphics/              # Vulkan device、presentation、shaders、graph
│   ├── render/                # data、resources、passes、pipelines
│   ├── physics/               # API、CPU/GPU 求解器
│   ├── framework/             # commands、services、operations、runtime
│   ├── automation/            # JSON-RPC、JSON Lines；网络和 SDK 预留
│   ├── scripting/             # API、Lua
│   └── editor/                # model、interaction、widgets、panels
├── apps/                      # runner CPU CLI；editor、ctl 预留
├── tools/                     # profiling 独立工具和内存 capture 检查器；assetc、shaderc 预留
├── sdk/python/                # 未来外部自动化客户端
├── shaders/common/            # 公共 Slang 模块
├── projects/demo/             # 示例资产、场景、脚本预留
├── tests/                     # unit、integration、gpu、replay
├── examples/foundation/       # 独立 ID/变换/安全保存示例
├── examples/scene/            # CPU 场景创建与跨进程重载
└── spec/
    ├── roadmap.md             # 阶段路线、依赖与验收条件
    ├── third-party-libraries.md # 三方库用途、版本、状态与更新规则
    ├── design/                # 每个模块的设计文档
    ├── development/           # 按编号排序的开发记录
    ├── commands/              # 命令目录、参数、返回值和调用示例
    └── templates/             # 两类文档模板
```

仅真实模块建立 CMake target；engine 管理 foundation、assets/types/runtime、scene、framework 和 automation，
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
cmake --preset windows-dev -B out/build/windows-runtime-cpu -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-runtime-cpu --config Debug
ctest --test-dir out/build/windows-runtime-cpu -C Debug --output-on-failure
```

Release 替换配置名。该配置装配 stduuid、magic-enum、Eigen、flecs、JSON，未链接窗口/GPU。

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
cmake --preset windows-dev -B out/build/windows-commands-only -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=ON -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
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

`windows-dev` 默认构建 Core、日志、Eigen 数学、IO、Scene、Memory、资产元数据/目录与 Catch2 单元测试。
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
| assets | nlohmann-json（元数据/身份目录 PRIVATE 使用；不安装 glTF/图片/散列库） |
| commands | nlohmann-json |
| graphics | vulkan、vulkan-memory-allocator、shader-slang |
| editor | sdl3[vulkan]、imgui[docking-experimental,sdl3-binding,vulkan-binding] |
| scripting | lua、sol2 |
| tests | catch2（已接入 Core、数学和 IO 单元测试） |
| profiling | tracy[on-demand]（关闭默认 features；启用 DK_ENABLE_PROFILING 时自动选择） |

三方库默认采用 vcpkg 官方收录的最新版本（含 port 修订），完整用途、版本与接入状态见
[三方库说明](spec/third-party-libraries.md)。当前依赖固定到 2026-09-23 核验的官方基线
`33d78c1ed898a06938f31312167c7abefd229455`，本次 Tracy 接入见
[0023](spec/development/0023-tracy-cpu-profiling.md)，此前升级见
[0021](spec/development/0021-vcpkg-baseline-update.md)。已是该索引最新版本的包保持不变；
历史阶段记录中的旧版本是当时的验证结果。
2026-09-24 接入 magic-enum 时重新核验官方最新版本为 0.9.8，与固定基线一致；
转换范围与兼容验证见 [0028](spec/development/0028-magic-enum.md)。
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
此阶段仅预置 Slang 包，编译反射 API、shader 编译命令和跨平台 host 工具管理
将在对应模块设计后接入。

feature 选择在 `project()` 前映射到 `VCPKG_MANIFEST_FEATURES`，
遵循 [vcpkg CMake 集成规范](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)。
更换生成器、triplet 或开关 vcpkg 时使用独立构建目录。

## 资产元数据、登记与改名（M4.1）

`dk::asset_runtime` 提供 [Metadata.hpp](engine/assets/runtime/include/dk/assets/Metadata.hpp) 和
[Catalog.hpp](engine/assets/runtime/include/dk/assets/Catalog.hpp)；Project 适配位于
`dk::asset_services` 的 [AssetRegistration.hpp](engine/framework/services/include/dk/services/AssetRegistration.hpp)。
底层可独立于 Scene/Framework 构建。完整开发预设已启用：

```powershell
cmake --preset windows-dev -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -Target @('dk_asset_tests','dk_asset_recovery_probe') -TestRegex '^dk\.assets\.' -Reason '元数据、身份提交及重启恢复'
```

调用入口先绑定 MemorySystem 的持久 Assets 域；新元数据/目录/候选中的 `dk::String/Vector` 拥有分配资源。
以下是已打开 `Project project`、已有 `ExecutionScope` 的宿主入口片段（完整运行行为见上述测试）：

```cpp
auto catalog = dk::make_asset_catalog(project);
if (!catalog) return std::unexpected(catalog.error());
const dk::AssetOutputSpec outputs[] = {
    {"mesh/0", dk::AssetKind::mesh}, {"material/0", dk::AssetKind::material}
};
dk::RegistrationRequest request{
    "assets/model.gltf", outputs, {}, dk::MissingMetaPolicy::create_or_adopt
};
auto candidate = dk::prepare_asset_registration(project, *catalog, catalog->guard(), request);
if (!candidate) return std::unexpected(candidate.error());
// candidate->project is a validated replacement value; no files or current state changed.
auto checked = catalog->validate_registration(candidate->registration);
```

meta v1 为 `DeckerAssetMeta` / version 1，固定 importer `gltf-static` / 1，有限正数 `unit_scale` 默认 1；
`outputs` 使用 `mesh/0`、`material/N`、`texture/N` 到稳定 AssetId 的映射，root_id 对应 mesh/0。
JSON 严格拒绝重复键、未知字段、非法 UTF-8/ID、错误类型和版本；上限为 2 MiB、16 层、10000 outputs。
编解码不解析源内容，登记仅要求工程内普通 glTF/GLB 文件。旧 Project 的普通文件校验仍不限制格式或要求 meta。

`inspect_source` 要求已有合法 meta。`prepare_registration` 默认在 meta 缺失时拒绝；
首次登记/迁移需显式 `create_or_adopt`，最多采用同路径唯一旧 mesh ID，多条或非 mesh 旧记录拒绝歧义。
有 meta 时始终验证，重复请求沿用 ID/settings，新增 selector 获得新 ID，未请求的旧 mappings 保留。
同一未提交新候选需由调用者保留；重复 prepare 不是持久登记，也不保证复用尚未落盘的随机 ID。

目录会话/版本与 Scene revision 分离；候选含 base/next guard、新只读 Project、meta 和原 sidecar 字节。
`validate_registration` 只检查目录 guard、源文件与 sidecar 是否仍匹配，不能代替磁盘提交或隔离外部并发修改。
语义相同的候选不增加 next revision。失败/异常不改旧 Project/目录/Scene/文件；ContextError 和 bad_alloc 沿用 Memory 约定。
以上失败保证针对纯候选。M4.1.2 提供持久提交与恢复；glTF 解码及 CPU Ready 留在后续阶段。没有新增命令。

完整提交使用 [AssetService.hpp](engine/framework/services/include/dk/services/AssetService.hpp)。
在已绑定 Assets 域、已有保存的 project.json 和普通 glTF 源文件时：

```cpp
auto service = dk::AssetService::open(root, "project.json");
if (!service) return std::unexpected(service.error());
auto saved = service->register_source(service->catalog().guard(), request);
if (!saved) return saved;
return service->rename_source(service->catalog().guard(), "assets/model.gltf", "assets/renamed.gltf");
```

成功提交才替换 Project/目录，目录 revision 增加 1；重复 no-op 不写文件、不增加版本。
服务返回的借用引用在成功变更后失效。所有子资产 ID 保持，Scene 内容/revision/dirty/历史不变。
改名仅支持同目录、同扩展名，拒绝已有目标、大小写等价名称、与 Scene 或其他资产占用路径重叠。
首次登记仍需显式 create_or_adopt；新 Project 先用既有 save_project 保存再打开服务。

Windows 本地盘、同步单写者下，操作记录保存在 `.decker/asset-operations/pending.json`。
文件故障先补偿；补偿失败返回原错误、恢复诊断和记录路径，并设置 `catalog().needs_recovery()`。
重启发现记录时拒绝打开。用户/宿主显式调用 `dk::recover_asset_operations(root)`，
成功后重新打开服务；恢复仅回滚可识别的 before/after 文件，外部修改或未知文件保留并报错。
记录写入后的异常也关闭写闸门，交给显式恢复。目录和父路径拒绝 reparse point、硬链接和短名称别名。
不承诺跨文件原子性、任意断电持久化或并发写隔离；`.decker` 恢复记录不能当作缓存删除。

不含 Scene/Framework 的独立验证：

```powershell
cmake --preset windows-dev -B out/build/windows-assets-only -DDK_BUILD_ASSET_RUNTIME=ON -DDK_BUILD_MEMORY=ON -DDK_BUILD_SCENE=OFF -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-assets-only -Target dk_asset_tests -TestRegex '^dk\.assets\.' -Reason '独立资产底层'
```

设计、错误边界和证据见 [assets-runtime](spec/design/assets-runtime.md)、[0031](spec/development/0031-asset-metadata-catalog.md)
及 [0032](spec/development/0032-asset-commit-recovery.md)。摘要采用 xxHash 0.8.4，作为资产底层私有依赖。

## Memory heap（M1.7.2）

模块链接 `dk::memory`。底层接口显式持有资源；日常代码可使用下节的拥有型容器和自动持久域路由。
以下函数演示预算、关闭期间释放及关闭重试，返回值供调用方处理：

```cpp
#include <dk/memory/MemorySystem.hpp>

std::expected<dk::memory::CloseResult, dk::memory::AllocationError> heap_example()
{
    namespace mem = dk::memory;
    auto system = mem::MemorySystem::create();
    if (!system) return std::unexpected(system.error());
    auto heap = system->create_heap({"assets", mem::DomainCategory::assets, 1024});
    if (!heap) return std::unexpected(heap.error());
    auto block = heap->try_allocate(256, 64);
    if (!block) return std::unexpected(block.error());

    system->begin_close(); // 拒绝新申请；现有块仍有效
    const auto busy = system->try_close(); // closing，尚有 1 个活块
    (void)busy;
    heap->deallocate(*block, 256, 64); // 原资源及原 size/alignment；允许跨线程
    return system->try_close(); // closed
}
```

原始指针不自动持有资源，必须保留一个 `ResourceHandle` 直到释放；资源可晚于创建线程和
MemorySystem 包装对象析构。`snapshot()` 返回域身份、活块、backing 请求量、峰值、预算和失败次数。
并发快照为近似采样，静止时精确；预算包含在途申请，限制请求字节而非进程 RSS。
零字节申请按 1 字节计费；错误返回固定枚举，失败不产生 alloc 事件。
完整接口与契约见 [Memory 设计](spec/design/foundation-memory.md)。

定向验证（先配置 windows-dev）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_memory_probe') -TestRegex '^dk\.memory\.' -Reason 'Memory heap 与关闭生命周期'
```

真实内存采集与关闭事件的对照见 [Profiling 工具说明](tools/profiling/README.md)；
M1.7.2 记录见 [0024](spec/development/0024-mimalloc-heap.md)。

## 拥有型内存与持久域路由（M1.7.3）

框架入口绑定一次 ThreadContext 和持久资源，业务函数里的 `dk::Vector`、`dk::String`、
`dk::memory::make_unique/make_shared` 自动捕获该资源。下面是可单独编译链接 `dk::memory` 的完整示例：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>

struct Mesh { dk::Vector<int> indices; };

std::shared_ptr<Mesh> build_mesh()
{
    auto mesh = dk::memory::make_shared<Mesh>();
    mesh->indices = {0, 1, 2};
    return mesh;
}

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto assets = memory->create_heap({"assets", mem::DomainCategory::assets});
    auto scene = memory->create_heap({"scene", mem::DomainCategory::scene});
    if (!assets || !scene) return 2;
    std::shared_ptr<Mesh> result;
    {
        mem::ThreadContext thread{*memory};
        mem::ExecutionScope entry{thread, *assets};
        result = build_mesh(); // 对象/control block 和 indices 都归 Assets
        {
            mem::DomainScope domain{*scene};
            dk::String label(80, 'x'); // 新对象归 Scene
            result->indices.reserve(1024); // 已有容器扩容仍归 Assets
        }
    } // 路由恢复；结果和 allocator 继续持有资源
    result.reset();
    return memory->try_close().closed() ? 0 : 3;
}
```

当前提供上述入口 API，尚未将既有 Runtime/Jobs 自动装配到内存系统。未绑定时隐式接口抛
`ContextError`，`try_current_resource()` 返回固定错误码；不会静默转到全局 heap。
ExecutionScope 支持嵌套另一系统；DomainScope 只切换同系统的持久域。context/scope 必须同线程、
按栈序析构，context 晚于 scope；存活 context 会使系统关闭返回 active_contexts/busy。
容器/Buffer/智能指针的释放无需 TLS，但容器自身的并发读写仍遵守标准库规则。

| 需要 | 接口与边界 |
| --- | --- |
| 显式选择资源 | `Allocator<T>{handle}`、`make_unique_in<T>`、`make_shared_in<T>`；不改写 TLS |
| 资源敏感类型的成员 | 声明 allocator_type，按 uses_allocator 构造；嵌套标准容器使用 scoped_allocator_adaptor，示例见 [测试](engine/foundation/memory/tests/OwnershipTests.cpp) |
| 跨域复制容器 | `dk::Vector<int> clone{source, mem::Allocator<int>{target}}`；普通复制保留源 allocator |
| 借用 PMR | `std::pmr::vector<int> values{owner.pmr_resource()}`；owner 必须晚于容器析构，跨域复制显式传目标 resource |
| 拥有字节缓冲 | `try_allocate(handle, bytes, alignment)` 返回 expected&lt;Buffer&gt;；try_resize 失败保留原内容，增长需容纳新旧块的预算 |

拥有型 allocator 的 copy/move/swap 均传播资源，移动后源容器可继续使用。普通成员不声明 allocator
感知时，显式 `_in` 只决定对象/control block 的资源，成员默认构造仍从当前作用域捕获；不会反射并改写任意类。
对象构造异常原样传播并释放存储。shared 的最后一个 weak 控制块仍持有资源；unique 的 reset/null 赋值
保留 deleter，销毁或用空 UniquePtr 替换后才释放其句柄。PMR 仅借用，不提供这一自动所有权保证。
`Buffer` 只管理字节，不对任意 C++ 对象执行 realloc；新增长字节未初始化，零长度 Buffer 仍规范化申请 1 字节。

定向验证使用上节 memory 命令；实现与验收见 [0025](spec/development/0025-memory-ownership-routing.md)。

## 临时内存与自动 ScratchScope（M1.7.4）

入口为线程配置一次 scratch 上游，业务函数使用 `ScratchScope` 和 `scratch_vector<T>()`，无需传入线程/heap。
返回结果仍使用持久域；临时容器必须先于 scope 析构。完整示例：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>

struct Mesh { dk::Vector<int> indices; };

std::shared_ptr<Mesh> build_mesh()
{
    dk::memory::ScratchScope scratch; // 先声明，最后析构并 rewind
    auto temporary = dk::memory::scratch_vector<int>();
    temporary = {0, 1, 2};
    auto result = dk::memory::make_shared<Mesh>();
    result->indices.assign(temporary.begin(), temporary.end());
    return result; // 只移交持久结果
}

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto assets = memory->create_heap({"assets", mem::DomainCategory::assets});
    auto scratchHeap = memory->create_heap({"scratch", mem::DomainCategory::jobs});
    if (!assets || !scratchHeap) return 2;
    std::shared_ptr<Mesh> mesh;
    {
        mem::ThreadContext thread{*memory, *scratchHeap};
        mem::ExecutionScope entry{thread, *assets};
        mesh = build_mesh();
        auto usage = thread.scratch().snapshot();
        if (usage.used_bytes != 0) return 3; // 空闲 chunk 可以留待复用
    } // context 归还缓存；mesh 继续持有 Assets
    if (mesh->indices.size() != 3 || mesh->indices.back() != 2) return 4;
    mesh.reset();
    return memory->try_close().closed() ? 0 : 5;
}
```

`ScratchOptions{chunk_bytes, max_retained_bytes}` 默认 64 KiB/1 MiB。嵌套 scope 只回收内层申请，
普通 chunk 在上限内复用，超过普通 chunk 大小的申请退出时立即归还。`try_reset()` 仅在无活动 scope 时
清空缓存并更新 generation；`try_checkpoint()/try_rewind()` 提供显式、同线程、严格 LIFO 的底层接口。
`ScratchScope{arena}` 不修改 TLS，显式 PMR 容器使用 `scope.resource()`；直接 raw/PMR 申请也要求活动 checkpoint。
`DomainScope` 不切换 scratch 上游；新的 `ExecutionScope` 不继承外层 ScratchScope。
保留原 `ThreadContext{system}` 用于仅持久分配；它不会自动创建 scratch。
同一 arena 的分配归当前最内层 checkpoint；不要在内层 scope 扩容一个需要在外层继续使用的 scratch 容器，
因为新缓冲会随内层回退。需要跨作用域保留的数据先复制到持久容器。

`snapshot()` 的 used 包含对齐 padding，retained 只计完全空闲 chunk，backing 计全部 chunk 容量；
三者不能相加当总占用，也不等于 RSS。关闭系统后拒绝新申请，清理和释放仍可执行。
Tracy 只记录 chunk 的 alloc/free，用量曲线在作用域边界、增长、reset 和显式 `sample()` 处采样。
`dk/scratch/sampled-peak` 是进程内采样总 used 的历史峰值；精确的单 arena 峰值保存在 snapshot 中。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_arena_probe') -TestRegex '^dk\.memory\.arena' -Reason 'ScratchArena 定向验证'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_arena_probe -TestRegex '^dk\.memory\.arena_probe$' -Reason 'Scratch 用量采集探针'
& ./scripts/capture-profiling.ps1 -Mode arena
```

采集前准备匹配版本的独立工具，见 [Profiling 工具](tools/profiling/README.md)。
实现与验收见 [0026](spec/development/0026-scratch-arena.md)。Pool 用法见下节；
任务 token、worker 缓存和既有 Runtime/Jobs 自动装配仍待实现。

## Pool 与对象复用（M1.7.5）

`LocalPoolResource` 供同线程反复创建/销毁小对象，`SharedPoolResource` 支持并发申请与异线程释放。
前者由调用方保持存活，后者为可复制的拥有型句柄；两者都持有固定 heap 上游。
`ObjectPool<T>` 提供局部 unique 对象，`SharedObjectPool<T>` 提供拥有型 unique/shared 对象：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/ObjectPool.hpp>
#include <thread>

struct Particle { int value; explicit Particle(int n) : value(n) {} };

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto heap = memory->create_heap({"particles", mem::DomainCategory::scene});
    if (!heap) return 2;
    std::shared_ptr<Particle> result;
    std::weak_ptr<Particle> weak;
    {
        mem::LocalPoolResource local{*heap, {32, 256}};
        mem::ObjectPool<Particle> localObjects{local};
        { auto particle = localObjects.make(7); if (particle->value != 7) return 3; }
        if (!local.try_trim()) return 4; // 活对象已经销毁，归还缓存；后续还能重用

        auto pool = mem::SharedPoolResource::create(*heap, {32, 256});
        if (!pool) return 5;
        mem::SharedObjectPool<Particle> sharedObjects{*pool};
        result = sharedObjects.make_shared(42);
        weak = result;
        if (pool->try_trim()) return 6; // 仍有活对象，trim 应拒绝
    } // 工厂/外观已退出，result 和 weak 控制块继续保有池
    std::jthread consumer{[value = std::move(result)]() mutable { value.reset(); }};
    consumer.join();
    if (!weak.expired()) return 7;
    if (memory->try_close().closed()) return 8; // weak 控制块尚未释放
    weak.reset();
    return memory->try_close().closed() ? 0 : 9;
}
```

局部对象必须在池之前、同一线程销毁；共享 unique 的 deleter 和 shared/weak 控制块拥有池。
`make` 构造失败归还槽位，普通成员不自动改路由；allocator-aware 类型按标准 uses_allocator 传播。
共享容器使用 `std::vector<T, mem::PoolAllocator<T>>` 并显式传入 `PoolAllocator<T>{pool}`；
其复制/移动/swap 传播 pool owner，默认空 allocator 不能申请，也不查询 TLS。
借用 PMR 使用 `pool.pmr_resource()`，调用方需保持 owner；不同池不能相互释放指针。

`try_trim()` 在有活块（含 weak 控制块）或在途操作时返回 busy，不影响已有数据。
成功时清空标准后端，下次申请延迟重建；`try_close()` 先禁止新申请，待对象释放后重试关闭。
Shared 的 try_close 返回 CloseResult；Local 返回 expected&lt;CloseResult, AllocationError&gt;，可报告错线程。
关闭上游同样禁止缓存分配，但已有对象仍可释放。Pool 不会隐式接管现有 dk 容器或持久工厂。

PoolOptions 沿用标准 `{max_blocks_per_chunk, largest_required_pool_block}` 提示，实际值通过 options() 查询；
上游 heap budget 才是申请预算。统计中的 logical_live 是用户请求量，backing 是实际上游占用；
idle_backing 仅在完全空闲时报告保留量，不能当作活跃池的可用槽容量，也不等于 RSS。
`try_sample()` 在安全点发布 local/shared 各四条 Tracy 曲线；忙时返回 busy，不逐对象采集全局曲线。
当前 MSVC Debug 构造期的 iterator proxy 使用 control 内固定保留区，属于 bootstrap 元数据；
标准池 chunk 和运行期元数据继续经过上游，见 [设计与工具链限制](spec/design/foundation-memory.md#m175-实施细化)。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_pool_probe') -TestRegex '^dk\.memory\.pool' -Reason 'Pool 定向验证'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_pool_probe -TestRegex '^dk\.memory\.pool_probe$' -Reason 'Pool 采集探针'
& ./scripts/capture-profiling.ps1 -Mode pool
```

独立采集工具准备见 [Profiling 工具](tools/profiling/README.md)，验收见 [0027](spec/development/0027-memory-pools.md)。
ThreadContext 的 local pool 装配、拥有型任务路由与关闭集成见下一节。

## 线程上下文与拥有型任务路由（M1.7.6）

`RoutingToken::capture()` 保存当前持久资源和系统所有权；`from_resource(handle)` 显式选择路由。
token 可复制到另一个线程，不携带提交线程的 arena、pool、context 或 TLS 帧；没有隐式绑定时 capture 抛 ContextError。
worker 显式拥有 `ThreadContextCache`，按系统复用稳定 context，并在入口配置本线程局部资源：

```cpp
// 提交端已有 ExecutionScope；local_heap 是同系统的持久 heap handle。
auto token = dk::memory::RoutingToken::capture();
// 在 worker 自身线程创建 cache，并在每次回调建立以下作用域：
dk::memory::ThreadContextCache cache;
dk::memory::ThreadContextOptions options{
    .scratch_upstream = local_heap,
    .local_pool_upstream = local_heap,
};
{
    auto& context = cache.acquire(token, options);
    dk::memory::ExecutionScope execution{context, token};
    dk::memory::ScratchScope scratch;
    auto temporary = dk::memory::scratch_vector<int>();
    temporary.resize(32, 7);
    dk::memory::ObjectPool<int> objects{dk::memory::current_local_pool()};
    auto local = objects.make(temporary.front());
    // 持久结果使用拥有型容器/Buffer/智能指针，捕获 token 的持久域。
} // 回调结束、异常或协作取消均先析构局部对象，再回退 scratch/路由。
auto retired = cache.try_retire_closed(); // owner 线程安全点，返回 retired/busy；保留 Open 项。
```

完整可执行的双系统示例为 [ContextProbe.cpp](tests/integration/ContextProbe.cpp)。
同系统缓存复用要求相同的 `ThreadContextOptions`，配置冲突明确报错；`try_clear()` 可先清理闲置项再换配置。
两种清理都保留有 execution/domain scope、scratch checkpoint 或 pool 活块的 busy 项。
借用 context/resource 引用在清理后失效，所有局部对象必须同线程且早于 cache 析构；cache 不可复制/移动。

token 不阻止系统关闭，Closing 后禁止新的绑定/分配。已移交的 owning 结果及 weak 控制块可在 worker 退出后释放；
释放后重试 `MemorySystem::try_close()`。未执行的取消直接丢弃 token，不创建 context。
框架必须在所属线程安全点退休关闭系统的缓存；Memory 不创建/唤醒/join worker，也不实现 Jobs 调度。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_context_probe') -TestRegex '^dk\.memory\.context' -Reason '线程路由、缓存退休与多系统关闭'
& ./scripts/capture-profiling.ps1 -Mode context -Port 18093
& ./scripts/capture-profiling.ps1 -Mode context-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18094
```

采集前在对应目录构建 `dk_context_probe` 的 RelWithDebInfo，以及 [独立 inspector](tools/profiling/README.md)。
实现/验证记录见 [0029](spec/development/0029-memory-context-routing.md)；重复工作负载和性能基线见下一节。

## Memory 重复工作负载与性能基线（M1.7.7）

`dk_memory_benchmark` 使用确定性数据校验 heap、heap PMR、arena、local/shared pool 的成批申请/回收，
并运行 scratch/local pool 临时对象转 owning 容器的跨线程管线。它是独立 CPU 合成负载，不要求 M4 Jobs 或资产导入器。
默认直接运行是快速 smoke；参数为 `--rounds`、`--warmup`、`--batch`、`--max-threads`，
仅用于故障验证的 `--allocation-budget` 限制矩阵 heap，失败必须清理后退出。`--capture` 等待本地 Tracy 连接。

先配置三个同优化级别的目录；已有正确配置可直接构建：

```powershell
cmake --preset windows-dev -DDK_ENABLE_PROFILING=OFF -DDK_PROFILE_MEMORY=ON -DDK_WARNINGS_AS_ERRORS=ON
cmake --preset windows-profiling -B out/build/windows-profiling-cpu-only -DDK_PROFILE_MEMORY=OFF -DDK_WARNINGS_AS_ERRORS=ON
cmake --preset windows-profiling -DDK_PROFILE_MEMORY=ON -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason '优化 OFF 基线及正确性'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'CPU-only 基线及正确性'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'CPU+Memory 基线及正确性'
```

按 [工具说明](tools/profiling/README.md) 准备匹配的 capture/csvexport/inspector 后运行：

```powershell
pwsh -NoProfile -File scripts/benchmark-memory.ps1 -Rounds 512 -Warmup 32 -Batch 16 -MaxThreads 32 -Repetitions 3
```

`MaxThreads` 替换为本机 N；省略时默认为逻辑处理器数（最多 128）。脚本检查编译器、配置、profiling 开关和输入，
依次执行 OFF/CPU/Memory，并按重复轮次轮换顺序；不自动配置/构建，也不并发运行测量。
每个进程覆盖去重后的 1/2/4/N 线程、32/8、256/64、4096/256 字节/对齐；arena/local pool 仅同线程回收。
三配置逐场景 checksum 必须一致，全部资源最终归零；采集逐次检查 backing 计数、跨线程 free、CPU 区间和曲线。

结果写入 `out/benchmarks/<run>/`：原始 JSONL、trace/CPU CSV、环境与二进制 SHA256、`baseline.csv/json` 和 `report.md`。
吞吐以校验过的逻辑请求计，管线以结果数计；latency 是各 worker 的 batch 分布。
跨线程计时包含 barrier，arena 按批回收；峰值/保留量不等于 RSS。CPU/Memory 实际连接采集，开销含本地采集器竞争。
正式结果与限制见 [基线报告](spec/benchmarks/2026-09-28-memory.md) 和 [0030](spec/development/0030-memory-baseline.md)。

## Tracy CPU 性能分析（M1.7.1）

默认 `DK_ENABLE_PROFILING=OFF`，宏不求值参数，不链接或自动安装 Tracy。
专用预设继承 windows-dev 的模块集合，以 RelWithDebInfo 编译并保留符号：

```powershell
cmake --preset windows-profiling -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_profiling_probe', 'dk_profiling_disabled_test', 'dk_run') -TestRegex '^dk\.(profiling\.|runtime\.|bootstrap\.version$)' -Reason 'CPU profiling 与 runner 协议'
```

启用 on-demand 后，连接前的事件不会保留。普通 runner 和测试无需启动 viewer 即可退出；
已有埋点覆盖 Runner.Entry、Runtime.Create/Dispatch、IO.Read/Write/AtomicSave。
手动运行被测程序时先设置下面的进程环境；preset 的环境不会自动传给从其他终端或 VS 启动的进程：

```powershell
$env:TRACY_ONLY_LOCALHOST = '1'
$env:TRACY_ONLY_IPV4 = '1'
$env:TRACY_NO_EXIT = '0'
```

客户端使用 Tracy 0.14.1。独立安装同版本命令行工具，不引入 GUI 依赖到引擎构建：

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install --x-manifest-root=tools/profiling --x-install-root=out/profiling-tools/vcpkg_installed --overlay-ports=cmake/vcpkg-ports --triplet=x64-windows --host-triplet=x64-windows
pwsh -NoProfile -File scripts/capture-profiling.ps1
```

[采集脚本](scripts/capture-profiling.ps1) 需要 PowerShell 7，使用本机 IPv4 端口 18086（可用 `-Port` 改写），
在探针连接后运行有界工作负载，断开后验证正常退出；通过 tracy-csvexport 读回 `.tracy`，
检查两个线程、嵌套区间、调用位置、异常退出和动态文本。日志、CSV、capture 与 summary.json
保存在 `out/profiling/<本次运行>/`。这是采集正确性验证，探针有受控等待，不作为性能基准。
交互查看可自行使用同版本 Tracy viewer 打开文件；本次验收使用命令行工具。

模块链接 `dk::profiling` 后使用包装头，公开模板含埋点时需要 PUBLIC 传递依赖：

```cpp
#include <dk/profiling/Profiler.hpp>
#include <string_view>

void import_mesh(std::string_view asset_name)
{
    DK_PROFILE_ZONE("Assets.ImportMesh");
    DK_PROFILE_ZONE_TEXT(asset_name); // 支持临时 string；关闭时连参数表达式也不执行
    // 实际工作；同一词法作用域只放一个 zone，子块可继续嵌套。
}
```

ZONE/FRAME 名称使用静态期字符串；TEXT 立即复制文本，空文本忽略，最多 65534 字节，截断按字节。
线程命名使用 `DK_PROFILE_THREAD_NAME("worker")`；普通 `set_thread_name` 函数的实参仍按 C++ 规则求值。
`DK_PROFILE_CALLSTACK_DEPTH` 默认 0，允许 0–64；按诊断需要增加深度会增加开销，尚无性能基准。
`DK_PROFILE_MEMORY` 默认 ON，仅在 `DK_ENABLE_PROFILING=ON` 时启用 heap backing 事件。
设为 OFF 可保留 CPU 区间而关闭内存事件；GPU 和 Jobs 埋点尚未接入。
依赖使用[最小 Tracy overlay](cmake/vcpkg-ports/README.md) 显式启用客户端，配置时核验导出的宏，
避免只编译消费方埋点却链接禁用的 client。详见 [Profiling 设计](spec/design/foundation-profiling.md)。

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
cmake -S . -B out/build/local-stduuid -DDK_USE_VCPKG=ON -DDK_VCPKG_FEATURES= -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_UNIT_TESTS=OFF
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

枚举与同名字符串使用 `magic_enum`，当前已用于错误码、资产种类（含严格解析和命令 schema）、
命令 effect 与 Tracy 分类标签。现有 `error_code_name`、`asset_kind_name`、`effect_name` 接口及非法值行为保持不变；
反射只在实现文件中使用，新消费者通过 `find_package(magic_enum CONFIG REQUIRED)` 和
`target_link_libraries(... PRIVATE magic_enum::magic_enum)` 声明依赖。
未来新增枚举超过默认 [-128,127] 范围、含同值别名或外部名称不同，须明确配置与兼容策略，不能直接套用默认反射。

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
cmake --preset windows-dev -B out/build/windows-math-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES=
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
cmake --preset windows-dev -B out/build/windows-io-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
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
cmake --preset windows-dev -B out/build/windows-foundation -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-foundation --config Debug
ctest --test-dir out/build/windows-foundation -C Debug --output-on-failure
cmake --build out/build/windows-foundation --config Release
ctest --test-dir out/build/windows-foundation -C Release --output-on-failure
```

M1.6 当时该配置安装 stduuid、Eigen 及 vcpkg 构建辅助包，Debug/Release 各 **15/15** 通过；
当前基础依赖还包含 magic-enum，上述数字保留为当时验收结果。
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
cmake --preset windows-dev -B out/build/windows-scene-only -DDK_BUILD_MATH=ON -DDK_BUILD_IO=ON -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
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
cmake --preset windows-dev -B out/build/windows-scene-cpu -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-cpu --config Debug
ctest --test-dir out/build/windows-scene-cpu -C Debug --output-on-failure
```

## 开发留档流程

M1.6 与 M2.1–M2.4 已完成并分节本地提交。最终默认 Debug/Release 各 128 通过、
1 项既有符号链接权限跳过；独立 Scene 配置各 104 通过、1 跳过，纯 CPU 示例各 16/16。
上述计数为当时验收记录；M3 交付 A、M1.7.1–7 和 M4.1 也已完成，当前下一项为 M4.2.1。

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
