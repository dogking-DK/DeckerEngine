---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-09-28T16:15:12+08:00"
---

# CPU 资产使用与验证

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
构建前提见[构建指南](build.md)，验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## 资产元数据、登记与改名（M4.1）

`dk::asset_runtime` 提供 [Metadata.hpp](../../engine/assets/runtime/include/dk/assets/Metadata.hpp) 和
[Catalog.hpp](../../engine/assets/runtime/include/dk/assets/Catalog.hpp)；Project 适配位于
`dk::asset_services` 的 [AssetRegistration.hpp](../../engine/framework/services/include/dk/services/AssetRegistration.hpp)。
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

完整提交使用 [AssetService.hpp](../../engine/framework/services/include/dk/services/AssetService.hpp)。
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
cmake --preset windows-dev -B out/build/windows-assets-only -DDK_BUILD_ASSET_RUNTIME=ON -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_MEMORY=ON -DDK_BUILD_SCENE=OFF -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-assets-only -Target dk_asset_tests -TestRegex '^dk\.assets\.' -Reason '独立资产底层'
```

设计、错误边界和证据见 [assets-runtime](../design/assets-runtime.md)、[0031](../development/0031-asset-metadata-catalog.md)
及 [0032](../development/0032-asset-commit-recovery.md)。摘要采用 xxHash 0.8.4，作为资产底层私有依赖。

## CPU 导入与离线工具（M4.2）

CPU 网格/材质/纹理导入由 `DK_BUILD_ASSET_IMPORTERS` 启用，要求 Math/Memory/IO，
提供 [GltfImporter.hpp](../../engine/assets/importers/include/dk/assets/GltfImporter.hpp) 和拥有型 CPU 数据。
`import_gltf(paths, {source, old_output_identities, unit_scale})` 只产生候选，不写文件。
调用者装配 `ThreadContext{memory_system, scratch_heap}` 并绑定 Assets 持久域；返回值可跨 scope 存活。
支持的格式子集、预算和诊断见[导入设计](../design/assets-importers.md)，测试入口为
`scripts/verify.ps1 -Target dk_import_tests -TestRegex '^dk\.import\.' -Reason 'CPU glTF 导入'`。

同时启用 `DK_BUILD_ASSET_RUNTIME` 后提供 `dk_assetc` target（程序 `dk-assetc`），
以及 [compile_asset](../../engine/assets/runtime/include/dk/assets/AssetCompiler.hpp) / [load_cpu_artifact](../../engine/assets/runtime/include/dk/assets/CpuArtifact.hpp)。
导入单 mesh glTF/GLB、基础材质及 PNG/JPEG base color，产物为拥有型 CPU 数据；另有下述同步缓存入口，尚无异步 Ready。

```text
dk-assetc import --project-root ROOT --source REL --output REL [--unit-scale NUMBER]
```

ROOT 必须存在；source/output 是工程内规范相对路径，output 必须不存在且父目录已存在。
拒绝覆盖或 `.decker` 保留路径。unit_scale 为正有限数，默认沿用旧 meta，没有时取 1。
成功产生 `output/manifest.json` 和 `output/data.bin`，最后创建/更新 `source.meta`；重复导入到新目录复用输出 ID。
不会修改 Project/Scene 清单。失败保留旧 meta/产物；同工程写入由调用者串行执行。
进程中断可能留下孤立产物，尚不提供跨文件断电原子性。

stdout 只输出 UTF-8 JSON 摘要：format/version、source/output、root_id、unit_scale、
outputs（key/id/kind）、inputs（path/bytes/digest）及 diagnostics。错误和 `--help` 输出到 stderr。
退出码：0 成功（含 help），1 导入/文件失败，2 参数错误，3 基础设施异常。

以下例子复制自制夹具到新的演示目录，保留源码夹具：

```powershell
cmake --build out/build/windows-dev --config Debug --target dk_assetc
$assetDemo = Join-Path 'out' ('assetc-demo-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $assetDemo | Out-Null
Copy-Item tests/fixtures/assets/triangle.gltf,tests/fixtures/assets/triangle.bin,tests/fixtures/assets/rgba.png -Destination $assetDemo
.\out\build\windows-dev\bin\Debug\dk-assetc.exe import --project-root $assetDemo --source triangle.gltf --output cpu --unit-scale 1
```

独立工具配置（不依赖 Scene/Framework/日志/runner），以及完整 M4.2 定向验证：

```powershell
cmake --preset windows-dev -B out/build/windows-assetc-only -DDK_BUILD_ASSET_RUNTIME=ON -DDK_BUILD_ASSET_IMPORTERS=ON -DDK_BUILD_MEMORY=ON -DDK_BUILD_MATH=ON -DDK_BUILD_IO=ON -DDK_BUILD_SCENE=OFF -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-assetc-only -Target @('dk_import_tests','dk_asset_pipeline_tests','dk_assetc') -TestRegex '^dk\.(import|pipeline|assetc)\.' -Reason 'M4.2 独立离线资产管线'
```

验收见 [0033](../development/0033-cpu-mesh-import.md) / [0034](../development/0034-textures-assetc.md)。

## 内容缓存（M4.3）

[compile_cached_asset](../../engine/assets/runtime/include/dk/assets/AssetCache.hpp) 与离线工具共用缓存管线：

```text
dk-assetc cache --project-root ROOT --source REL [--unit-scale NUMBER]
```

source/设置/输出 ID/导入版本和全部依赖字节决定 key，重复请求核验内容并读回已有 CPU 数据，不重新解码。
结果为 `DeckerAssetCacheResult` v1 JSON，包含 key、directory、cache_hit、miss_reason、root_id、outputs、inputs、diagnostics。
目录固定在 `.decker/cache/assets/v1/`，`current/<root-id>.json` 选择不可变的 `entries/<key>/<build-id>/`。
未知或损坏条目不会被覆盖；显式 cache 请求重建后可以替换该源的坏 current 索引。
unit_scale 默认沿用合法 meta；重建保留 AssetId。无 meta 时成功导入后生成身份；不修改 Project/Scene。
同工程写入需串行化。先提交合法 meta，再原子提交 current；若第二步失败，错误明确报告
`identity committed; current unchanged`，保留合法 meta、完整孤立产物和旧索引以便重试。

可在前述 `$assetDemo` 演示目录上连续运行两次以下命令，第二次返回 `cache_hit: true`：

```powershell
.\out\build\windows-dev\bin\Debug\dk-assetc.exe cache --project-root $assetDemo --source triangle.gltf
& ./scripts/verify.ps1 -Target @('dk_asset_cache_tests','dk_assetc') -TestRegex '^dk\.(cache\.|assetc\.)' -Reason '内容缓存与离线进程'
```

设计/验收见 [资产缓存](../design/assets-runtime.md#m431-实施契约)、[0035](../development/0035-asset-cache-publication.md)。

显式回收未引用的旧缓存：

```powershell
.\out\build\windows-dev\bin\Debug\dk-assetc.exe cache-clean --project-root $assetDemo
```

清理先检查所有 current，再删除完整可识别且未引用的 build。坏索引会拒绝清理；未知版本、坏条目、
额外文件、链接与 tmp 目录保留。只删除验证过的缓存文件，不碰源/meta/Project/恢复记录。
`DeckerAssetCacheCleanResult` JSON 包含 removed/retained/skipped/failed 与 diagnostics。
清理是逐条进行的，中途文件 IO 失败可能只完成部分回收；此时仍输出 JSON 报告并返回 1，stderr 说明失败。
前置检查失败只输出 stderr。API 可把默认 10000 的扫描预算调低；超限在开始删除前拒绝。

M4.3 在独立 CPU 配置的验证命令（配置方式沿用上文）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-assetc-only -Target @('dk_asset_cache_tests','dk_assetc') -TestRegex '^dk\.(cache\.|assetc\.)' -Reason 'M4.3 内容缓存、失效重建及清理'
```

失效/清理证据见 [0036](../development/0036-asset-cache-invalidation.md)。缓存没有 LRU、文件监视或跨进程写锁，
异步作业和 CPU Ready 留在 M4.4；当前命中以 current 指向的版本为准，索引丢失会重新导入。

## 异步 CPU 资产（M4.4）

windows-dev 启用 DK_BUILD_JOBS=ON（依赖 Memory）及资产导入/运行时。dk-run --stdio 支持 assets.open/catalog/import/register/rename/load/status/unload 和 jobs.get/wait/cancel；TaskId 仍表示同步命令完成。stdin 空闲时继续发布后台结果；退出取消并 join，未完成作业须先 wait 再关闭输入。详见[资产命令](../commands/assets.md)与[作业命令](../commands/jobs.md)。

Memory 构建使用同版本 mimalloc overlay 显式关闭 Windows redirect，不替换全局分配器。

M4.4 验收记录：[0040](../development/0040-cpu-assets-delivery.md)。复现真实 CPU 进程链路：

```powershell
& ./scripts/verify.ps1 -Target @('dk_run') -TestRegex '^dk\.runtime\.assets_stdio$' -Reason 'CPU asset delivery end-to-end'
```
