---
module: assets-runtime
created_at: "2026-09-22T18:20:46+08:00"
updated_at: "2026-09-28T14:45:27+08:00"
status: accepted
---

# M4 资产身份、缓存与 CPU 加载设计

## 范围与当前基线

M4.1 已实现 meta v1、身份目录、登记提交、受控改名和恢复，见 [0031](../development/0031-asset-metadata-catalog.md)
及 [0032](../development/0032-asset-commit-recovery.md)。M4.2 的同步 CPU 编译/产物读回已完成，
见 [0034](../development/0034-textures-assetc.md)。M4.3.1 内容键、同步命中与 current 发布已实现，见
[0035](../development/0035-asset-cache-publication.md)。M4.3.2 已完成失效/损坏/删除重建与有界清理，
见 [0036](../development/0036-asset-cache-invalidation.md)；M4.4 尚未实施。原有
[AssetReference](../../engine/assets/types/include/dk/assets/AssetReference.hpp) 和
[Project](../../engine/scene/include/dk/scene/Project.hpp) 的注册、类型与文件存在性校验保持兼容。
分步计划见 [0020](../development/0020-m4-development-plan.md)。

M4 交付源文件 → 导入 → 可重建缓存 → CPU Ready。Scene 仍只保存 AssetId/AssetKind，
GPU 上传、渲染资源和 GPU Ready 留到 M7。资产准备不修改实体、Scene revision、dirty 或撤销历史。

## 目录、所有者与依赖

types、data、importers、runtime 的身份/同步编译部分及 asset_services 已实现；Jobs 在对应小节才添加：

| 模块 | 计划 target | 职责与依赖 |
| --- | --- | --- |
| assets/types（已有） | dk::asset_types | 持久 ID/种类/引用，继续仅依赖 Core |
| assets/data | dk::asset_data | 不可变 CPU 网格/材质/纹理值；依赖 types、math、memory，数据拥有其分配资源 |
| assets/importers | dk::asset_importers | 源文件到 CPU 数据；依赖 data、IO，私有 fastgltf/stb_image |
| assets/runtime | dk::asset_runtime | 元数据/目录/持久化/恢复，PUBLIC types、IO、Memory，PRIVATE JSON、xxHash、profiling；启用 importers 时增加 PUBLIC importers、CPU 编译/产物及缓存/清理，Jobs 待实现 |
| framework/services、operations | dk::asset_services、dk::asset_operations | services 已提供 Project 适配及 AssetService 提交/改名；命令及 operations 尚未实现 |

资产底层不依赖 Scene、Commands、Vulkan 或 Editor。AssetService 从 ProjectDescription 构造
独立目录快照；不能令 assets/runtime 反向包含 Project.hpp，或暴露 SceneService 的可变 Project。
后台工作只持有输入快照和工作结果；运行时目录及 Ready 发布归属调用者线程。
M1.7 [Memory](foundation-memory.md) 与 [Tracy](foundation-profiling.md) 是实施前置。
Ready 数据和导入结果使用 Assets 域的 owning Buffer/容器，已有消费者可在 unload 后继续持有；
解析临时空间使用当前执行线程的 ScratchScope，不将 scratch PMR 容器直接 move 到 Ready。
AssetService 在入口绑定当前 Runtime 的 Assets 域，内部函数自动路由；异步任务捕获拥有型路由 token，
在 worker 重新绑定其线程 context。已有 Ready 容器的扩容/释放保持创建时的资源，不因主线程域改变而重路由。
内容哈希/导入/缓存/发布分别埋 CPU zone；heap 记录 backing，临时空间记录用量和高水位，避免重复统计。

## M4.1：身份与元数据

### M4.1.1 实施契约

本节实现 `dk::asset_runtime`（PUBLIC types、IO、Memory，PRIVATE nlohmann-json）及
`dk::asset_services` 中的只读 Project 适配（PUBLIC scene、asset_runtime）。底层不包含 Project/Scene。
`DK_BUILD_ASSET_RUNTIME` 默认 OFF，完整开发预设启用；单独开启要求 IO/Memory，不要求 Scene/Framework。
复用已固定 JSON/Memory 依赖，无新库或版本变化。登记入口不注册业务命令。

- `AssetMetadata`/`AssetOutput`、`parse_asset_meta`/`serialize_asset_meta` 提供纯值编解码；
  importer 固定 gltf-static/1，settings 对象允许省略 unit_scale（规范化为 1），其他字段必须完整。
  输入/输出上限 2 MiB、嵌套 16 层、1–10000 outputs；key 限 mesh/0、material/N、texture/N，
  N 为 0–9999 无前导零十进制，kind 必须对应，key/ID 均唯一且 root_id 必须等于 mesh/0。
  未知版本/导入器返回 not_supported；结构、UTF-8、键/ID、数值错误返回 invalid_argument。
  输出按 key 排序且显式写 unit_scale；不解释 glTF 内容，调用者提供所需 output selectors。
- `AssetCatalog::create` 从规范的 ID/kind/path 值建立独立只读目录；已有普通文件不在 create 阶段强制存在。
  会话使用新 StableId，初始 revision=0；`CatalogGuard` 同时检查会话与 revision。
  `inspect_source` 要求源为普通 glTF/GLB 文件并读取合法 meta；缺失 meta 返回 not_found。
  源路径沿用 Project 1–4096 字节规范相对 UTF-8，另保证加 .meta 后不超限。
  Windows 使用 ordinal ignore-case 拒绝同一目录中的大小写别名，其他平台按路径字节区分；
  不作 Unicode 归一化，子路径符号链接沿用 IO 词法边界，不提供文件系统隔离。
- `prepare_registration(guard, request)` 只构造拥有型候选，不改目录或文件。
  默认缺失 meta 即拒绝；调用者首次登记/迁移必须显式选择 create_or_adopt。
  有 meta 时始终校验/复用，即使选择 create_or_adopt 也不覆盖损坏文件。
  无 meta 时仅可采用同路径唯一 mesh 记录作为 root；多条或非 mesh 记录为歧义 conflict。
  无记录则新生成 root；新 selector 分配新 ID，旧 key/ID/未请求的旧 outputs 均保留。
  已有 meta 的每个同源目录记录必须与映射 ID/kind 一致，其他源不得占用这些 ID。
  request 未指定 settings 时保留旧值；显式更改只进入候选。总资产数不能超过 10000。
- `RegistrationCandidate` 私有构造、只读访问；持有 base/next guard、源路径、完整新记录、meta、
  原 sidecar 字节或不存在标记、changed。语义无变化时 next revision 不变，变化时 +1，不回绕。
  同一未提交候选须由调用者保留；未落盘的新候选不会因重复 prepare 自动复用随机 ID。
  `validate_registration` 检查 guard 和实际 sidecar 字节仍匹配，源仍为普通文件；sidecar 字节或存在性变化返回 conflict，
  缺失源返回 not_found，非普通文件/IO 故障保留对应诊断。
  这些是 M4.1.2 提交输入/前置契约；commit/rename 按下节协议持久化后发布，不能把 validate 的成功当作提交。
- `make_asset_catalog(Project)` 和 `prepare_asset_registration(Project, catalog, guard, request)` 位于服务适配层。
  要求 root 和旧资产记录与目录一致，构造候选 ProjectDescription 后再次 `Project::create`；
  返回拥有型 candidate + 新只读 Project，原 Project/Scene/revision/history 不变。
- 新目录/meta/候选的持久 string/vector 使用 dk::String/Vector，构造入口要求当前持久域绑定（宿主使用 Assets 域）。
  对象持有资源，可在作用域退出后读取/释放；沿用 Memory 的 ContextError/bad_alloc 异常约定，异常不修改输入或文件。
  IO/JSON DOM/旧 Project 边界暂用已有标准存储，不将 scratch 或临时 JSON 引用放进结果。

定向测试覆盖严格 codec/预算、首次与显式采用旧 ID、重复/增量候选、缺失/损坏 meta、
ID/kind/path/大小写冲突、过期 guard/sidecar、候选失败不修改原目录/Project/文件，以及资源持有与预算失败。
旧无 meta Project 的打开、解析、引用校验继续有效。开发记录见 [0031](../development/0031-asset-metadata-catalog.md)。

### M4.1.2 提交与恢复契约

`AssetCatalog` 增加登记提交、改名候选/提交及 needs_recovery 闸门；服务层 `AssetService`
从已保存的 Project manifest 打开，拥有独立 Project/目录，准备完整只读 Project 后调用底层提交，
最终以不分配的 swap 发布 Project。底层只接收 manifest 路径及预期/新 UTF-8 字节，不依赖 Scene。
入口仍由宿主绑定 Assets 内存域。成功改变目录 revision +1，no-op 不写文件、不增加 revision；
旧 guard 拒绝。Scene 的内容、revision、dirty、历史完全不参与。服务返回的借用引用在成功变更后失效。

采用单写者、同步 Windows 本地盘协议。每个工程最多一个 `.decker/asset-operations/pending.json`：
严格 JSON v1（format=`DeckerAssetOperation`、version=1、algorithm=`xxh3-128-v1`、
kind=`registration`/`rename`、source、target、source_digest、meta、manifest）。
meta/manifest 包含 path、before、after；每个 image 为 null（文件缺失）或 `{text,digest}`。
manifest 的两份 image 必须存在，meta after 必须存在，rename 的两份 meta image 完全相同。
source/target 是源相对路径，登记 target=source；meta.path=source+`.meta`。
路径必须互不冲突，不允许进入 `.decker`；manifest 不得与任一源/sidecar 重叠。
目录层拒绝 sidecar 与其他已登记资产路径重叠，服务层拒绝任一相关路径与 Scene 路径重叠（含当前未创建文件）。
记录限 128 MiB、嵌套 16 层，meta image 限 2 MiB，manifest image 限 16 MiB；
拒绝重复/未知字段、非法 UTF-8、未知版本/算法和摘要不匹配，不执行部分解析的记录。
摘要使用固定参数 XXH3-128 的 canonical 大端 16 字节/32 位小写 hex，源文件以 64 KiB 分块读取。

提交顺序：全部候选/序列化/内存分配与文件前置校验 → 原子写记录 →
登记替换 meta，或改名 source 再改名 meta → 原子替换 manifest → 校验最终文件 →
移除操作记录（持久提交点）→ 不分配地发布目录与 Project。现存 meta 的 no-op 保留原始字节。
同目录且同扩展名改名保留全部 ID/settings；拒绝大小写等价名称、跨目录、覆盖目标文件和目标目录记录。
持久入口拒绝 Windows reparse point、非常规文件名及父目录别名，避免跨根写入和路径别名覆盖。
不修改 glTF 内容及其外部 URI，不提供跨目录移动。

文件错误发生在持久提交点之前时，按 manifest → meta → source 逆序补偿；
补偿前先检查所有文件均处于记录的 before/after 状态，外部变化一律 conflict，保留未知内容。
新建 sidecar 只在摘要等于本次 after 时删除；移动不允许覆盖。补偿失败同时返回原错误、
恢复错误和记录路径，保留记录并阻止该实例后续写入。异常（如 OOM）若发生在记录写入后，
保留记录并关闭写闸门，不承诺异常时在线补偿。任何残留/未知操作目录文件都阻止打开和新提交。
`recover_asset_operations(root)` 显式检查并回滚未完成操作，包含 manifest 已替换但记录尚存的情况；
全部状态明确才执行，恢复本身可重试，成功移除记录后须重新打开服务（新 session/revision=0）。
不凭记录中的阶段猜测是否成功，也不删除未知临时文件。记录被外部破坏时保留并诊断。
记录不是安全边界，不承诺恶意文件抵抗、跨文件原子性、任意断电一致性或多进程并发隔离。

内部每次操作的故障注入点覆盖写记录、源改名、meta 写入/改名、manifest 替换、提交清理及对应回滚。
测试同时覆盖真实文件故障、补偿失败/重试、外部冲突、进程终止后的恢复、哈希已知向量/分块一致性，
以及只读 Project 替换、所有 ID 和 Scene 状态保持。新增 PRIVATE xxHash::xxhash，固定 vcpkg baseline。

### 文件职责

- 源文件及依赖：工程内的 glTF/GLB、buffer、图片，属于用户内容。
- `<source>.meta`：UTF-8 JSON，保存持久身份、导入设置及子资产映射，进入版本管理。
- `<project>/.cache/assets/`：可删除、可重建的导入产物，不保存唯一身份；不能据此恢复丢失的 ID。
- Project v1 的 `assets[].path` 继续指向源文件，不能悄悄改成缓存文件或带 fragment 的路径。
  同一源文件的 mesh/material/texture 子资产各有记录，以 ID 区分，path 可以相同。

已实现元数据 v1 字段如下；JSON 编解码拒绝重复键、未知字段、非法 UTF-8、nil/重复 ID 和未知版本。

| 字段 | 约定 |
| --- | --- |
| format / version | `DeckerAssetMeta` / 整数 1 |
| root_id | 主网格 AssetId；等于 outputs 中 mesh/0 的 ID |
| importer | `{name:"gltf-static",version:1}`，声明支持的导入契约版本 |
| settings | v1 仅 `unit_scale`，默认 1，有限且大于 0；规范化后参与缓存键 |
| outputs | `{key,id,kind}` 数组：mesh/0、material/N、texture/N 等源内选择器到 UUID 的映射 |

元数据提供限长输入和数量限制，复用 Project 的路径/ID 约定；输出数量不能突破工程
10000 条资产记录的上限。源路径由 sidecar 位置决定，不在 meta 再存第二份路径。
导入器实现版本另写入产物并参与缓存键；升级实现不重分配 AssetId。

首次显式登记时生成 stduuid-backed ID；重复登记复用合法 meta。新增子资产先在候选 meta 中
分配 ID，完整验证后发布，不能每次导入重新编号。首版以源内索引定位子资产，名称只作诊断；
不承诺导出器重排数组后仍能识别同一个语义对象，重排需显式检查/重映射，不能靠名称猜测。

已实现 `inspect_source`、`prepare_registration`、`validate_registration`、`commit_registration`、
`prepare_rename`/`commit_rename`；服务入口为 `AssetService::register_source`/`rename_source`。输入/输出均为 dk 值类型与 Result，
不暴露 JSON 或导入器内部对象。目录快照携带会话 ID 和单调 catalog revision，更新需要匹配两者。

三方库和固定基线核验集中见 [选型说明](assets-importers.md#已确定的三方库与基线)。
xxHash 的 XXH3-128 在 M4.1.2 首次用于恢复记录的文件摘要，M4.3 复用同一个内部 ContentDigest 封装；
先放在 assets/runtime 实现侧，不为尚无调用者的通用 hash 模块创建空 target。

### 兼容与提交点

已有无 meta 的 M2 工程仍可做文件校验和场景保存/加载。进入 M4 托管链路时必须显式登记：
可无歧义采用已有单个 mesh AssetId；已有 ID/kind/path 与 meta 冲突时返回 conflict，不能静默覆盖。
新的 ProjectDescription 先经 Project::create 校验，提交时整体替换只读 Project；不引入可变索引后门。
持久清单由服务层 serialize_project/Project::create 验证，底层复用带读回验证的 IO atomic writer；
目录 revision 与 Scene revision 分离，加载/重命名不伪造场景编辑。

`.meta` 和 Project 清单是两个持久文件。登记/重命名先准备候选、校验版本和目标冲突，
再保存带前后路径及摘要的操作记录；文件变更成功且清单替换完成后，才发布内存目录。
失败优先回滚；回滚失败返回原错误和恢复信息，并将受影响记录标记需恢复，禁止继续写入。
重启通过操作记录检查实际文件状态，能唯一判断时恢复，否则报告 conflict；不自动删除冲突文件。
操作记录位于工程 `.decker/asset-operations/`，属于恢复数据，不能随缓存一起清理。
该机制不承诺跨文件原子性、任意断电恢复或多个进程同时写同一工程；首版串行单写者。

重命名首版只允许**同目录、保持扩展名的文件改名**，同时处理源、meta 和所有相关 Project 路径，
保留全部 AssetId。大小写等价改名按平台语义拒绝，跨目录移动与 URI 重写延后。
目标已存在、meta 丢失/损坏、重复 ID 或未恢复操作均拒绝；外部单独移动源文件须诊断，不补造身份。

## M4.2：同步编译与 CPU 产物

`compile_asset` 和 `load_cpu_artifact` 仅在 runtime+importers 同时启用时构建；
`dk-assetc` 创建自身 MemorySystem、Assets 域和带 scratch 的 ThreadContext，调用相同底层入口。
源/依赖快照和拥有型 CPU 结果来自 import_gltf，编译器复用合法旧 meta 的 ID 与默认 unit_scale，
写入新目录后完整读回验证，重新核对输入/旧 meta，再独占改名发布、最后原子提交 meta。
没有 Project 或 Scene 依赖，不会写清单或发布内存目录/Ready；不与 M4.1 的 Project 操作恢复混用。
有未恢复操作时拒绝写入。同工程外部写入需串行化，进程中断可留下孤立目录，不宣称跨文件原子。
CPU manifest v1、little-endian 数据布局、摘要/读取预算和失败清理约定见
[导入器实施契约](assets-importers.md#m422-实施契约)。未知版本/算法只读拒绝并保留原文件。
M4.3.1 已复用此格式/验证器实现内容键、缓存命中和 current 索引，未直接序列化对象内存。

## M4.3：缓存与依赖

### M4.3.1 实施契约

增加 `compile_cached_asset(paths, {source, optional unit_scale})`，返回 key、产物相对目录、
cache_hit、miss_reason、JSON 摘要和已验证的拥有型 CpuArtifact；使用既有导入硬预算。
`dk-assetc cache --project-root ROOT --source REL [--unit-scale NUMBER]` 复用此入口。
原 `import --output` 保留独立导出语义。当前没有 Jobs/Ready，调用者绑定 Assets 域与 scratch 并串行化同工程写入。

key 编码 v1 为 XXH3-128 的带类型字段流：每字段 `tag:u8, length:u64le, payload`；
整数 u64le、unit_scale 为 IEEE754 binary64 位模式 little-endian，字符串为严格 UTF-8，ID 为规范小写 UUID 文本。
固定顺序为 domain、algorithm=`xxh3-128-v1`、cache/artifact/importer implementation/contract 版本（均 1）、
importer=`gltf-static`、source 工程相对路径、unit_scale、按 selector 排序的 output（selector/kind/ID），
按路径排序的 input（规范路径/字节数/规范摘要）；数组先编码长度。源也在 input 中。
键生成验证完整 meta、输入唯一且含 source，并对排序后字段编码，禁止依赖 JSON 对象遍历次序或 mtime。
实现版本只由程序常量指定，不接受 CLI 自称版本。新增版本会失效，不更换 AssetId。

磁盘范围固定 `.decker/cache/assets/v1/`，包括 `entries/<key>/<build-uuid>/`、`tmp/<build-uuid>/` 和
`current/<root-id>.json`。key 是内容版本；build UUID 只用于物理发布，不参与 key。
每次构建在独占 tmp 目录生成 M4.2 的 manifest.json/data.bin，再加 entry.json，完整读回后以不覆盖改名发布。
采用 key 下的不可变 build 目录，是为同 key 损坏或未知格式时仍能重建而不覆盖旧内容；正常命中不生成新 build。
entry format=`DeckerAssetCacheEntry`/version=1，保存 algorithm、key、build、descriptor、manifest_digest。
current format=`DeckerAssetCacheCurrent`/version=1，保存 algorithm、root_id、source、key、build；
路径只从校验过的 key/UUID 构造，不信任持久文件中的任意目录字符串。
entry/manifest 各 16 MiB，current 16 KiB；JSON 禁重复键、深度超 64，沿用产物 v1 和源/输入硬预算。

命中要求 current/entry/产物全部兼容且摘要、ID 映射、设置和源路径匹配，全部源/依赖当前内容摘要匹配。
首版通过 current 查询，不搜索旧版本或孤立条目；current 丢失时会重新导入到新的 build。
显式 cache 请求可替换该源不兼容/损坏的 current，旧未知条目仍保留；清理不能自行猜测坏索引。
命中读回 CPU 数据但不调用 glTF 解码器、不写 meta/current；mtime/大小一致仍读内容。
失效只触发一次显式同步重建：源/依赖读取与 key 来自 importer 同一快照，发布前再次检查输入和旧 meta/current，
变化返回 conflict，不无限重试。未知版本或损坏条目保留，并在 miss_reason 中说明。

提交有两个明确步骤：先发布完整产物、原子提交合法 meta 身份/设置，再原子替换 current（缓存提交点）。
current 写入前失败始终保留旧索引；meta 提交前失败清理本次拥有且未改动的文件。
meta 已提交而 current 失败时保留合法 meta 与完整孤立产物，错误附 `identity committed; current unchanged`，
重试继续复用 ID。不会报告缓存成功或 Ready；这沿用 M4.2 身份提交，明确不承诺多文件事务/断电原子性。
所有成功返回值在提交前构造；已有 M4.1 未恢复操作时禁止缓存写入。

### M4.3.2 实施契约

补齐仅图片/源/参数变化、删除/损坏/未知版本重建的验收，并实现显式未引用缓存清理。
清理前读取所有 current；任何不可识别索引都拒绝删除。只删除完整可识别且未被任何 current 引用的 build，
保留未知条目、外部改动、源/meta/Project 与 `.decker/asset-operations`；不做递归任意目录删除或 LRU。
入口 `clean_asset_cache(paths, scan_limit=10000)` / `dk-assetc cache-clean --project-root ROOT`。
只扫描本格式的 entries（key/build 两层），不扫描 tmp 或其他版本目录；预算允许降低，不能提高。
先有界枚举全部 current 和 entries，确认全部索引可解析且文件名匹配 root_id，再形成候选；超预算/坏索引时尚未删除。
每个候选要求恰好 entry.json/manifest.json/data.bin 三个普通文件、当前版本、摘要/产物/身份完整有效；
未知、损坏、额外文件、链接、改写后的条目均保留并给诊断。删除前再次确认 current 集合/字节及候选文件摘要。
只 remove 三个验证过的文件和随后空目录，不递归删除、不删除 key 桶/源/meta/Project/恢复日志。
结果包含 removed/retained/skipped/failed 计数及诊断；逐条删除不是事务，中途 IO 失败可留下部分旧缓存，
返回 failed 并记录已删除项，源与当前引用始终不动。OOM 同样不承诺撤销已经完成的缓存回收。
测试注入清理前冲突与逐文件 IO 失败；同工程单写者约定继续适用，不引入 LRU、目录监视或新依赖。

缓存按输入内容寻址，key 为以下规范化输入的 XXH3-128：摘要算法标识、缓存格式版本、导入器实现版本、
支持的契约版本、settings、源字节摘要、按稳定顺序排列的依赖 URI/字节摘要，以及输出 ID 映射。
不使用 mtime/大小作为唯一正确性依据。散列实现确定为 xxHash，采用 `XXH3_128bits` 默认参数，
不使用随机 seed 或自定义 secret。摘要固定 16 字节，对外文本为 32 个小写十六进制字符。
持久字节使用 `XXH128_canonicalFromHash` 生成的规范大端编码，禁止直接保存 `XXH128_hash_t` 内存；
该编码独立于产物数值块的小端格式。CacheKey 表示输入版本，不能替代或重新生成 AssetId。

摘要包装支持分块输入，使用 `XXH3_128bits_reset/update/digest` 默认参数接口，与一次性计算保持一致；
状态通过 `XXH3_createState/freeState` 配对并由 RAII 管理，检查分配/接口错误，块间检查 IO 错误和取消。
对已读取输入快照分块消费，避免为散列再复制整份数据，也不能重读另一版本后给旧产物盖上新摘要。
构建输入描述采用带类型/长度边界的确定性编码，固定字段与依赖顺序，数值表示在 M4.3.1 固定；
禁止无分隔地拼接字符串，不将时间戳、线程完成顺序或进程地址混入 key。
依据：[xxHash 0.8.4 的 XXH3-128、流式与规范编码接口](https://github.com/Cyan4973/xxHash/blob/v0.8.4/xxhash.h)。

缓存 entry/current 使用 `algorithm: "xxh3-128-v1"`，CPU manifest 与 M4.1.2 恢复记录也使用该版本化算法标识，
均有各自格式版本，禁止把其他算法的摘要
当作当前摘要解释。缓存算法/版本不兼容时视为未命中并重建，保留未知条目；恢复记录算法未知时返回
not_supported 并保留记录，不猜测文件状态或自动删除。首版不维护双摘要，也不自动切换算法。
XXH3-128 用于本地内容变化与缓存意外损坏检测，不提供密码学抗碰撞或真实性认证；
真实性要求若在未来出现，应另行设计。性能收益在实施时按实际资源与 IO 路径测量，当前没有工程基准结果。

M4 的源依赖为 buffer/图片文件，不递归解释任意其他资产工程。按需请求时重新核验依赖；
缺失或损坏、设置/源内容/导入器变化、产物版本不兼容均不能命中有效缓存。首版不做目录监视。
解析、散列和导入消费同一份已读取字节快照，避免 key 与产物来自不同版本；发布前重新检查输入。
检测到变化返回 conflict 供显式重试，不无限自动重建；外部并发写文件不承诺文件系统级一致快照。

产物使用自描述 manifest 和定长小端数值块，记录格式版本、类型、长度、摘要、依赖与输出 ID；
不能序列化 Eigen/容器的内存布局。先在本次独占临时目录写完并读回验证，再发布不可变 key 目录，
最后原子替换小型 current 索引。每步失败保留旧索引；碰到已存在的 key 先校验，禁止覆盖未知内容。
源与 meta 从不由缓存清理删除；失败或取消只清理本次拥有的临时文件。

删除缓存后的下次导入必须重建相同 ID；损坏条目视为无效并报告重建原因。
首版已支持显式清理可验证的未使用磁盘条目，不做复杂 LRU。拥有型 CPU 返回值在清理磁盘后仍可读取；
异步目录/只读句柄和 unload 语义由 M4.4 实现，禁止把本阶段同步结果声称为 CPU Ready。

## M4.4：加载状态与发布

### M4.4.2 实施接口与提交边界

缓存新增 `prepare_cached_asset(paths, request, stop_token)`，后台只读源/meta/current，
生成并读回完整不可变产物，不写 meta/current；`PreparedCachedAsset::publish()` 仅 owner 调用一次，
复核输入和原 meta/current，才执行身份与 current 提交。未发布候选析构只清理本次拥有且未改动的文件。
同步 `compile_cached_asset` 复用 prepare/publish；既有 meta 已提交/current 失败的部分保证保持。
importer 在文件读取、解析、primitive 与图片解码之间检查 stop_token；三方调用内部不抢占。

可选 Jobs 构建下提供 `AsyncAssets`（无 Scene/Framework 依赖），借用生命周期更长的 JobQueue，
所有方法在 owner 线程。每个 source 最多一份当前 slot，最多 1024 个 slot；输入作业按硬上限
128 MiB 预留，单个 completion CPU 产物最多 256 MiB。slot 持有 generation、JobId、只读 CachedAsset，
pending 持有 session/generation/source/id；pump 先拒绝旧代/旧会话，再提交磁盘、发布 Ready，最后确认 Job 成功。
load 同源 Loading 复用作业、Ready 复用句柄；import 是显式重新构建，递增代次并取消旧请求。
unload 清除当前拥有值并递增代次；外部 `shared_ptr<const CachedAsset>` 保持可读。
reset_session 取消旧请求并清空 slot，generation/session 均检查溢出。未知 ID 不推断源路径，
由上层已登记目录提供 (id, source)，查询未加载登记项返回 Unloaded。
后台不捕获 AsyncAssets 或任何服务的裸引用。普通失败在 pump 后变 Failed；取消变 Unloaded。

查询描述 `AssetId/kind/state/request_generation/ready_generation/diagnostic`，
结果句柄包含 AssetId、产物 key 和只读数据所有权；CPU Ready 不等于 GPU 可用。

| 操作 | 状态及保证 |
| --- | --- |
| 首次 load / 显式 retry | Unloaded 或 Failed → Loading；参数拒绝不改变状态 |
| 同一 ID/同一输入正在加载 | 复用现有作业；首版不建立多个独立取消订阅者 |
| 当前代成功且输入仍有效 | 候选整体发布为 Ready；mesh 所需材质/纹理一并有效 |
| 当前代失败 | Loading → Failed，保留定位错误；旧句柄仍有效，不把旧数据宣称为最新 Ready |
| 当前代取消 | Loading → Unloaded；作业终态 cancelled，与失败分开 |
| 新请求覆盖、unload 或会话替换 | 递增 generation 并取消旧作业；旧结果不能回写当前目录 |

generation 不回绕；新结果只在所属目录会话和 generation 均匹配、输入仍有效、未接受取消时发布。
提交点前接受取消则不发布；已发布后取消返回“已终止”，不撤销成功。发布后再更新作业成功终态，
查询不得观察到 succeeded 却取不到对应资产。首版由主线程轮询 completion 完成该原子可观察步骤。
磁盘已有完整不可变产物可在取消后保留为未引用缓存，但 current/Ready 不得随后偷偷切换。

## 定向验证与实施前检查

- M4.1：身份往返、冲突、合法重命名和每个文件提交点失败；Project v1 与旧无 meta 工程兼容。
- M4.3：逐项改变 key 输入、仅修改图片、损坏/删除缓存、写入中断；失败保留旧索引且 ID 不变。
  摘要封装首次落地时验证官方已知向量、空输入、分块/一次性结果一致及规范字节/十六进制编码；
  所属小节验证算法标识不匹配时的缓存重建/恢复拒绝，缓存编码测试区分不同字段边界。
- M4.4：复用请求、取消/完成竞争、旧代晚到、卸载后旧句柄、会话切换、CPU-only 生命周期。

CPU 产物 manifest 在 M4.2.2 固定，缓存索引/预算和恢复记录格式在所属小节先补充再实现；这些细节不阻塞
M4.1.1 的 meta/身份设计。当前 `dk_asset_tests` / `^dk\.assets\.` 验证本阶段；完整 Project 兼容结果见 0031。

关联：[导入器](assets-importers.md)、[任务队列](foundation-jobs.md)、
[已有资产类型](assets-types.md)、[工程格式](project-format.md)、[IO](foundation-io.md)。
