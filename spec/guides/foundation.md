---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-09-28T16:15:12+08:00"
---

# Foundation 与 Scene 使用

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
构建前提见[构建指南](build.md)，验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## Core 能力

| 使用入口 | 头文件 | 内容 |
| --- | --- | --- |
| dk::core | [Error.hpp](../../engine/foundation/core/include/dk/core/Error.hpp)、[Result.hpp](../../engine/foundation/core/include/dk/core/Result.hpp) | ErrorCode、错误上下文、std::expected 别名 |
| dk::core | [StableId.hpp](../../engine/foundation/core/include/dk/core/StableId.hpp) | EntityId/AssetId/SceneId；生成、解析、比较、哈希 |
| dk::logging | [Log.hpp](../../engine/foundation/core/include/dk/core/Log.hpp) | 显式 Logger 生命周期；stderr/追加文件；模块、级别和 fmt 格式化 |

稳定 ID 由 stduuid 1.2.3 实现生成、解析、格式化与哈希，保留 dk 强类型接口。
ID 为 128 位值，解析要求 36 字符的 8-4-4-4-12 格式，文本输出统一小写；
默认 nil 由上层业务决定是否允许。日志使用方须链接 dk::logging，
其 PUBLIC 依赖会传递 fmt。实例销毁前结束写入线程，需确认写入结果时检查 flush。
完整接口、错误约定及限制见 [Core 设计](../design/foundation-core.md)。

枚举与同名字符串使用 `magic_enum`，当前已用于错误码、资产种类（含严格解析和命令 schema）、
命令 effect 与 Tracy 分类标签。现有 `error_code_name`、`asset_kind_name`、`effect_name` 接口及非法值行为保持不变；
反射只在实现文件中使用，新消费者通过 `find_package(magic_enum CONFIG REQUIRED)` 和
`target_link_libraries(... PRIVATE magic_enum::magic_enum)` 声明依赖。
未来新增枚举超过默认 [-128,127] 范围、含同值别名或外部名称不同，须明确配置与兼容策略，不能直接套用默认反射。

## Eigen 基础数学（M1.2）

消费者链接 `dk::math`，包含
[Math.hpp](../../engine/foundation/math/include/dk/math/Math.hpp)；
[Types.hpp](../../engine/foundation/math/include/dk/math/Types.hpp)
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
具体参数和边界见 [数学设计](../design/foundation-math.md)。

## Transform（M1.3）

继续链接 `dk::math`，包含
[Transform.hpp](../../engine/foundation/math/include/dk/math/Transform.hpp)。
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
cmake --preset windows-dev -B out/build/windows-math-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-math-only --config Debug
ctest --test-dir out/build/windows-math-only -C Debug --output-on-failure
```

M1.3 验证记录见 [0005](../development/0005-transform.md)。

## 工程路径与文件 IO（M1.4）

消费者链接 `dk::io`，包含
[Path.hpp](../../engine/foundation/io/include/dk/io/Path.hpp) 和
[File.hpp](../../engine/foundation/io/include/dk/io/File.hpp)，无需额外三方库。

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
cmake --preset windows-dev -B out/build/windows-io-only -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-io-only --config Debug
ctest --test-dir out/build/windows-io-only -C Debug --output-on-failure
```

M1.6 的 Windows 开发构建包含 101 项 CTest（26 项 IO/安全保存、42 项数学/Transform、16 项 Core、
15 项 Foundation 集成、日志流探针和版本探针），bootstrap 保留 1 项版本测试。
边界与验证见 [IO 设计](../design/foundation-io.md) 和 [0006](../development/0006-foundation-io.md)。

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
故障注入与真实共享冲突/只读目标等验证见 [0007](../development/0007-atomic-file-save.md)。

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
cmake --preset windows-dev -B out/build/windows-foundation -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_SCENE=OFF -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-foundation --config Debug
ctest --test-dir out/build/windows-foundation -C Debug --output-on-failure
cmake --build out/build/windows-foundation --config Release
ctest --test-dir out/build/windows-foundation -C Release --output-on-failure
```

M1.6 当时该配置安装 stduuid、Eigen 及 vcpkg 构建辅助包，Debug/Release 各 **15/15** 通过；
当前基础依赖还包含 magic-enum，上述数字保留为当时验收结果。
当前安全保存验收仍限 Windows 本地文件。
详见 [集成设计](../design/foundation-integration.md) 和 [0008](../development/0008-foundation-integration.md)。

## SceneDocument（M2.1–M2.4）

链接 `dk::scene`，包含 [SceneDocument.hpp](../../engine/scene/include/dk/scene/SceneDocument.hpp)。
`create()` 返回持有私有 flecs world 的文档；`create_entity()` 生成持久 EntityId，
也可传入已有 ID。重复/nil ID 和缺失删除会返回错误，成功修改递增 revision。
`entity_ids()` 返回排序副本，`validate()` 检查 ECS 与身份索引一致性。
新文档 dirty=true。`entity()` 返回名称、局部 Trsd 和父 ID 的副本；
`set_name` / `set_local_transform` / `set_parent` 验证后编辑，`world_transform` 返回派生仿射矩阵。
重挂保留局部 TRS，删除有子节点的实体被拒绝；循环、缺失父级、非有限变换均不改变旧状态。
`scene_component_descriptors()` 提供稳定组件名、版本及字段类型。Scene 要求 math/io 同时开启。
`set_asset_references()` 设置 AssetId/AssetKind 列表，验证 nil、重复 ID 和种类。
`dk::asset_types` 仅包含持久资产类型；没有资产解码或设备资源。
设计见 [scene.md](../design/scene.md)，验收见 [0009](../development/0009-scene-identity.md)、
[0010](../development/0010-scene-hierarchy.md)。

[Project.hpp](../../engine/scene/include/dk/scene/Project.hpp) 提供版本 1 工程清单的
`parse_project` / `serialize_project`、`Project::create` / `open`、资产路径解析和文件校验。
`check_asset_references(scene, project)` 给出包含实体/资产 ID 的引用诊断。
工程根必须存在，清单的 scene/assets 路径是使用 `/` 的规范相对路径。
JSON 限制 16 MiB、64 层嵌套；拒绝未知版本、字段、重复键/ID 和路径越界。
设计与协议见 [project-format.md](../design/project-format.md)，记录见 [0011](../development/0011-project-assets.md)。

独立 Scene 配置（关闭日志、示例与 runner）：

```powershell
cmake --preset windows-dev -B out/build/windows-scene-only -DDK_BUILD_MATH=ON -DDK_BUILD_IO=ON -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-only --config Debug
ctest --test-dir out/build/windows-scene-only -C Debug --output-on-failure
```

## 场景保存与 CPU 示例

场景持久化入口为 [SceneIO.hpp](../../engine/scene/include/dk/scene/SceneIO.hpp)：

- `snapshot()` 捕获不可变实体数据和 revision；`serialize_scene` / `parse_scene` 处理 v1 JSON。
- `save_scene(document, project)` 保存当前快照；也可传入此前快照，后续编辑仍保持 dirty。
- `load_scene(project)` 返回干净文档；内存 `parse_scene` 返回 dirty 的导入文档。
- `reload_scene(owner, project)` 先构建并验证候选，成功才交换所有者，失败保留旧对象。
- `save_project(project, relative_manifest)` 安全保存工程清单。

场景和组件均显式 version=1，最多 10000 实体；先登记实体再解析关系，不依赖文件顺序。
保存先读回并验证临时文件，然后原子替换；继承 Windows 本地文件边界。
Scene/Project 文件各自原子，不构成跨文件事务。协议细节与测试见
[Scene 设计](../design/scene.md)、[0012](../development/0012-scene-persistence.md)。

CPU 示例要求已有根目录和资产引用占位文件（本阶段不解码网格）。create 会覆盖其中的 scene.json/project.json：

```powershell
New-Item -ItemType Directory -Force out/demo-scene | Out-Null
Set-Content -LiteralPath out/demo-scene/mesh.bin -Value "M2 reference fixture"
.\out\build\windows-dev\bin\Debug\dk-scene-demo.exe create out/demo-scene
.\out\build\windows-dev\bin\Debug\dk-scene-demo.exe load out/demo-scene
```

无日志、runner 和 Catch2 的场景示例配置：

```powershell
cmake --preset windows-dev -B out/build/windows-scene-cpu -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-cpu --config Debug
ctest --test-dir out/build/windows-scene-cpu -C Debug --output-on-failure
```
