---
module: assets-importers
created_at: "2026-09-22T18:20:46+08:00"
updated_at: "2026-10-03T15:28:09+08:00"
status: accepted
---

# M4 静态 glTF 导入与 assetc 设计

## 范围和依赖

M4.2 已实现 CPU 网格/材质/纹理导入与 dk-assetc，见 [0033](../development/0033-cpu-mesh-import.md) 和
[0034](../development/0034-textures-assetc.md)。目标是得到可查询的 CPU 数据，
不是 glTF 场景编辑器。身份/缓存遵循 [资产运行时](assets-runtime.md)，小节安排见
[0020](../development/0020-m4-development-plan.md)。

`dk::asset_importers` 依赖 asset_data、IO；解析器、图像解码和 JSON 为 PRIVATE 依赖。
`dk::asset_data` 仅依赖 asset_types、Math 和 Memory，可由 Render Resources 单独启用而不引入导入器或 IO；
IO 前置检查位于 importers target。该边界调整见 [0061](../development/0061-render-data-resources.md)。
按用户选型，glTF/GLB 使用 fastgltf，PNG/JPEG 使用 stb_image，内容摘要使用 xxHash 的 XXH3-128。
这些库已接入并完成定向功能验证；M4.3 同步缓存/失效清理已实现，异步加载留在 M4.4。
导入器使用引擎提供的字节/依赖读取入口，不能绕过路径、大小限制自行访问网络或任意文件。
实施前完成 M1.7 [Memory](foundation-memory.md) / [Tracy](foundation-profiling.md)。服务/任务入口绑定
当前 Runtime 的 Assets 持久路由；普通导入函数通过隐式工厂/容器和 scratch 工厂取用资源，不层层传 thread/heap。
底层仍保留显式资源入口；最终 CPU 数据拥有其资源，失败/取消先析构解析对象再重置临时空间。
为解析、accessor 转换、解码和摘要建立稳定 zone。三方库内部暂未适配的分配明确属于观测范围之外；
不得仅因外层使用 PMR 就声称 fastgltf/stb 的全部分配已被接管。

## 已确定的三方库与基线

全工程版本政策与接入状态见 [三方库说明](../third-party-libraries.md)；
对应模块开工时重新核验 vcpkg 最新版本，按该规则升级，以下版本保留当前基线的可追溯性。

初次选型读取项目当时 builtin-baseline
`67b9e21f86e3034657a04da429a8bf274de67925` 的 baseline/port 文件。
M1.7.1 已升级到 `33d78c1ed898a06938f31312167c7abefd229455`，重新比较后下表版本均未变；
当时未安装 M4 依赖，见 [0023](../development/0023-tracy-cpu-profiling.md)；随后接入证据见 0032–0034。

| 用途 | 库 / vcpkg 包 | 基线记录版本 | port 声明许可证 | 接入点 |
| --- | --- | --- | --- | --- |
| glTF/GLB 解析 | fastgltf / fastgltf | 0.9.0 | MIT | assets/importers，PRIVATE 链接 fastgltf::fastgltf |
| PNG/JPEG 解码 | stb_image / stb | 2024-07-29#1；stb_image 2.30 | MIT OR CC-PDDC | 导入器内部的单一 StbImageDecoder.cpp |
| XXH3-128 内容摘要 | xxHash / xxhash | 0.8.4 | BSD-2-Clause | 资产管线内部 ContentDigest 封装，见运行时设计 |
| fastgltf 的传递依赖 | simdjson / simdjson | 4.6.11 | (Apache-2.0 OR MIT) AND BSL-1.0 AND BSD-3-Clause | 由 fastgltf port 引入 |

xxHash 0.8.4 已在 M4.1.2 核验官方最新 port 并接入 runtime 私有依赖，见 [0032](../development/0032-asset-commit-recovery.md)。
fastgltf 在 M4.2.1、stb 在 M4.2.2 已核验官方最新 port，版本与当前固定 baseline 一致，无需升级基线。
JSON 清单继续使用 nlohmann-json，ID 继续使用 stduuid，数学继续使用 Eigen；
工作队列使用标准库。simdjson 属于导入器依赖，不替换引擎的 JSON 接口。

实际 CMake 用法：fastgltf 使用 `find_package(fastgltf CONFIG REQUIRED)`；
stb 使用 `find_package(Stb REQUIRED)` 与 `Stb_INCLUDE_DIR`，以 SYSTEM PRIVATE 添加头文件目录；
xxHash 使用 `find_package(xxHash CONFIG REQUIRED)`，PRIVATE 链接 `xxHash::xxhash`；
不启用命令行工具 `xxhsum` feature，不在公共头文件暴露 xxHash 类型。
对应模块落地时才追加其所需 vcpkg feature/自动选择逻辑，不把导入器依赖加到最小 Core 必需项。

核验依据：[固定基线](https://github.com/microsoft/vcpkg/blob/67b9e21f86e3034657a04da429a8bf274de67925/versions/baseline.json)、
[fastgltf port](https://github.com/microsoft/vcpkg/blob/67b9e21f86e3034657a04da429a8bf274de67925/ports/fastgltf/vcpkg.json)、
[fastgltf 0.9.0 CMake](https://github.com/spnda/fastgltf/blob/v0.9.0/CMakeLists.txt)、
[Stb 查找模块](https://github.com/microsoft/vcpkg/blob/67b9e21f86e3034657a04da429a8bf274de67925/ports/stb/FindStb.cmake)、
[xxHash port](https://github.com/microsoft/vcpkg/blob/67b9e21f86e3034657a04da429a8bf274de67925/ports/xxhash/portfile.cmake)、
[xxHash 0.8.4 CMake](https://github.com/Cyan4973/xxHash/blob/v0.8.4/build/cmake/CMakeLists.txt)。
本次基线升级见 [0021](../development/0021-vcpkg-baseline-update.md)，旧选型版本留在历史记录中。

## 实现封装约定

fastgltf 只负责解析与 accessor 提取，输出转换为引擎拥有的 CPU 值，不将 fastgltf::Asset 放进公共接口。
外部 buffer/图片经 IO 读取并登记依赖，再由有所有权的字节存储/BufferDataAdapter 供 accessor 使用；
不能将尚未读取的 URI 当作已加载内存，也不让自动外部加载绕过引擎检查。Parser 按工作线程独占，
调用工具前完成形状/范围及首版子集检查。向量首版逐分量转换到 Eigen，不假设两者内存布局相同。
依据：[解析与线程约定](https://fastgltf.readthedocs.io/latest/overview.html)、
[accessor/BufferDataAdapter](https://fastgltf.readthedocs.io/latest/tools.html)。

stb_image 适配器仅一个翻译单元定义 STB_IMAGE_IMPLEMENTATION，同时限定 STBI_ONLY_PNG、
STBI_ONLY_JPEG、STBI_NO_STDIO。所有输入通过 stbi_load_from_memory 解码为 RGBA8，
先检查长度能表示为 int、图像尺寸与解码预算；16 位源图明确返回 not_supported，不静默降低精度。
返回数据使用 RAII 释放，不把 stb 指针交给公共接口；解码层保持像素字节，颜色空间由材质绑定解释。
不修改全局翻转设置；图片编码字节的加载与像素解码分成两个步骤。
依据：[锁定源码的内存解码、格式宏与释放接口](https://github.com/nothings/stb/blob/f75e8d1cad7d90d72ef7a4661f1b994ef78b4e31/stb_image.h)。

库能解析的特性不自动成为引擎支持项；本次选型保持下列已定义的静态 mesh 局部数据范围。

## 首版格式覆盖

以下是 DeckerEngine 主动限定的子集，依据
[Khronos glTF 2.0 规范](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html)；
拒绝不支持的数据时报告定位和原因，不以“完整支持 glTF”对外描述。

| 项目 | M4 计划范围 |
| --- | --- |
| 容器 | `.gltf` + 本地外部 buffer/图片，及 `.glb` 的嵌入 buffer/图片；拒绝网络 URI/data URI |
| 网格 | 源中恰好一个 mesh，可有多个 primitive，仅 TRIANGLES；不把 nodes/scenes 导入 Scene |
| 顶点 | POSITION 必需；NORMAL、TEXCOORD_0 可选，首版仅 FLOAT；支持合法 offset/stride |
| 索引 | 无索引或 unsigned byte/short/int，转换为 uint32；三角形数量及顶点索引合法 |
| 材质 | metallic-roughness 的数值参数和 baseColorTexture，保留 alphaMode/cutoff、doubleSided；无材质使用默认值 |
| 纹理 | PNG/JPEG 到 RGBA8，支持来源 bufferView 或外部文件；保留 sampler 和纹理使用语义 |
| 暂不支持 | 多 mesh、skin/animation/morph、sparse accessor、压缩几何/纹理、扩展、额外 UV/顶点颜色/切线 |

首版不支持 normal/occlusion/emissive/metallicRoughness 纹理通道；检测到会影响所选网格结果的
未支持通道/属性明确返回 not_supported，不静默丢失。required extension 必须拒绝；
首版对非空 extensionsUsed 也采取明确拒绝策略，后续按测试覆盖扩展。
nodes 的摆放不烘焙到 mesh，也不创建实体；包含节点时输出“仅导入 mesh 局部数据”的诊断。
NORMAL 缺失时保留缺失标记，首版不生成平滑法线；baseColorTexture 存在时要求 TEXCOORD_0。

## CPU 数据与数值约定

### M4.2.1 实施契约

增加可选 `DK_BUILD_ASSET_IMPORTERS`（要求 Math/Memory/IO；独立于 Scene/Framework/Runtime）。
`asset_data` PUBLIC types/math/memory；`asset_importers` PUBLIC data/IO，PRIVATE fastgltf/JSON/profiling。
完整开发预设启用，vcpkg `asset-importers` feature 选择 fastgltf；固定 baseline 不变。
返回纯候选 `ImportResult`，拥有 mesh、materials、output identities、输入文件快照和诊断；
调用方可作为 const 值/共享拥有值发布。不写 meta、Project 或磁盘产物，不改变 Scene。
输入为 ProjectPaths、源相对路径、旧 output 映射和 unit_scale；缺失映射生成新 UUID，已有映射保留。
只输出被该 mesh 使用的材质/纹理（mesh/0、material/N、texture/N）；旧映射已无对应输出时返回 not_found，
不会把旧 ID 指向其他 selector。默认材质使用内置数值，无 nil 资产引用。
M4.2.1 最初拒绝 baseColorTexture；M4.2.2 已完成关联和解码。

默认硬上限：源 16 MiB、单依赖 64 MiB、全部输入 128 MiB、CPU 数值载荷输出 256 MiB（不含容器/JSON 管理开销），
累计顶点 1000000、索引 3000000、primitive 4096、材质/纹理及 output 总数 10000；
图片每边 8192、单图 64 MiB RGBA、累计 RGBA 128 MiB。可通过 ImportLimits 降低，不允许提高硬上限。
读取/解析前限制源与依赖，算术先除法检查再相乘；顶点/索引/解码预算在分配前扣除。
UTF-8 JSON 先做结构/重复键/深度（64）检查，拒绝所有扩展、稀疏、skin/animation/morph；
手动解码和约束 URI 后由 IO 读取，fastgltf 不启用自动外部 IO，不解析 data URI。
经过 buffer/view/accessor 范围、步长/对齐、类型、索引和数值校验后才调用 fastgltf accessor 工具。
结果为 dk 拥有型容器，临时解析/索引使用局部对象和 ScratchScope；宿主须装配带 scratch 的 ThreadContext。
三方内部标准分配不宣称被接管。
任何 Result 失败或异常都销毁候选并保持输入/文件不变；ContextError/bad_alloc 沿用 Memory 约定。

计划公开头文件位于 `engine/assets/data/include/dk/assets/`，使用 dk 类型，不暴露解析器对象。

- MeshData：primitive 列表，位置/可选法线/可选 UV、uint32 索引、计算得到的局部包围范围、可选材质 ID。
- MaterialData：上述数值参数、纹理 AssetId；sampler 由对应 TextureData 持有。默认材质作为明确的内置值，避免伪造 nil 引用。
- TextureData：宽/高、RGBA8 字节、原始来源诊断；base color 使用 sRGB 语义，数值因子保持线性。
- ImportResult：独立只读数据、输出 ID/kind 列表、依赖摘要和诊断；不保存源解析树的悬空 view。

沿用 [Math](foundation-math.md) 的右手、Y-up、米和列向量；单位缩放显式作用于位置。
保持源 mesh 局部坐标、绕序和 UV，不根据引擎观察方向暗中旋转模型或翻转图片。
纹理字节行顺序与 UV 采样契约一同保存，在 M7 上传时统一适配；不调用全局图像翻转开关。
所有数值有限；乘法/offset/count/stride 检查在分配及读取前完成，拒绝越界和整数溢出。

外部 URI 以源文件所在目录为基准，解码后转换为规范工程相对路径，允许工程内部兄弟目录，
拒绝越过工程根、绝对路径、网络 scheme、NUL 和无效编码。沿用 IO 的词法边界，
不把它宣称为防止子路径符号链接逃逸的文件系统沙箱。

M4.2 开工时固定并测试源字节、依赖总字节、顶点/索引数、图像尺寸和解码总内存预算；
不能只在解码完成后检查大小。损坏输入为 invalid_argument，不支持特性为 not_supported，
缺失依赖为 not_found，IO 错误沿用 io_error；上下文含源路径、mesh/primitive/accessor 或图片位置。

## dk-assetc 离线入口

### M4.2.2 实施契约

PNG/JPEG 使用单一 StbImageDecoder.cpp（STBI_ONLY_PNG/JPEG、STBI_NO_STDIO），先检查签名、
位深、尺寸和预算再解码 RGBA8；保留原始顶行顺序，不翻转。baseColorTexture 要求 UV0，
使用 texture/N 身份，保存 glTF sampler 的未指定 min/mag 与 wrap 语义，颜色空间固定 sRGB。
相同 image 被不同 texture 使用时分别保存输出（sampler/身份可不同），消耗累计解码预算。

`dk-assetc import --project-root ROOT --source REL --output REL [--unit-scale NUMBER]`：
output 是工程内新的独立目录，父目录必须已存在，拒绝覆盖、根外或源/输入/sidecar 的祖先路径。
默认 unit_scale 沿用旧合法 meta，没有旧 meta 时为 1；非法旧 meta 始终拒绝。成功保存/复用 meta，
完全不读取或改写任意 Project/Scene 清单；只有可识别且受支持的导入产物才提交新身份。
构建入口有 runtime+importers 时增加 assetc，工具只链接资产管线/Memory/IO，不依赖 Scene 或 Runtime。

CPU 产物 v1 是 `manifest.json` + `data.bin`：manifest format=`DeckerCpuAsset`、version=1、
hash_algorithm=`xxh3-128-v1`、meta（现有 meta v1 对象）、source、inputs（path/bytes/digest）、
primitives、materials、textures、diagnostics、data（file 固定 data.bin、bytes/digest）。
primitive 的 positions/normals/texcoords/indices 各记录 offset/count，依次存放 float32 x3/x3/x2、uint32，
所有数值显式 little-endian，不 dump Eigen/容器内存；包含 bounds_min/max 和可选 material ID。
textures 记录 ID、width/height、RGBA8 offset/count、origin、sampler、color_space=srgb、row_order=top-to-bottom。
材料包含各数值、alpha 语义和可选 base_color_texture ID。所有块严格连续、无洞/重叠/尾部；
manifest 限 16 MiB、data 限 256 MiB，reader 检查摘要、形状、有限数、边界、数量和全部 ID 引用后返回拥有型 CPU 数据。
输入摘要来自本次实际读取的快照，M4.3 再增加内容 key/current；本阶段没有缓存命中语义或 CPU Ready 状态机。

管线先只读导入并准备 meta/产物/JSON 摘要，在独占临时目录写入产物并完整读回验证，
提交前再次比对全部输入字节摘要和旧 meta 字节/缺失状态。以不覆盖的目录改名发布新输出，
最后原子替换 meta（最终提交点）；之后只返回已准备结果。已存在 meta 字节/语义未变则保留原文件。
普通失败删除本次拥有且摘要未变的临时/新输出文件，保留旧 meta 与全部其他产物；清理失败附路径和原错误，不删除未知内容。
调用者必须串行化同一工程的写入；摘要/路径复查不能替代跨进程文件锁，不承诺抵抗检查后的并发外部改写。
不使用 M4.1 的 Project 操作记录，不承诺跨文件断电原子性；进程中断最多留下完整或临时孤立产物，
meta 仍是持久身份来源，失败/中断不宣称输出已注册。已有 `.decker/asset-operations` 未恢复时拒绝编译。
新增内部故障点验证输出发布后/meta 提交前失败与输入变化，真实进程验证 UTF-8 JSON/stdout、Unicode 参数和退出码。

工具位于 tools/assetc，target `dk_assetc`，程序 `dk-assetc`。复用同一资产管线，
不链接 framework Runtime、渲染器或窗口。M4.3 已增加 cache/cache-clean 子命令，
缓存的 key/提交/清理边界见[资产运行时](assets-runtime.md#m431-实施契约)，import 仍用于独立目录导出。

调用形式：

```text
dk-assetc import --project-root ROOT --source assets/model.glb --output imported
```

命令显式创建/复用 meta，输出 JSON 摘要（root_id、outputs、依赖摘要、unit_scale、输出位置及诊断），
日志到 stderr；不会自动修改当前场景或任意 Project 清单。调用者可使用输出登记工程引用，
M4.4 的 AssetService 提供有版本检查的清单适配。M4.2.2 先固定并验证 CPU 产物 v1，
无缓存时写入独立输出目录；M4.3 复用该格式并增加内容键/current 索引，不能暂用对象内存 dump。
根目录与输入使用 IO 路径约定，Windows 原生参数兼容 Unicode；退出 0 成功、1 导入失败、
2 参数错误、3 致命基础设施错误。flags/help 与可执行夹具示例见[根 README](../guides/assets.md#cpu-导入与离线工具m42)。

## 验证与演进

使用仓库自制的小型有来源夹具：单三角形、两个 primitive、带 base color 纹理、GLB 与外部依赖。
验证数据/ID/材质关联，错误索引、截断 buffer、越界 stride、坏图、超预算、Unicode 路径，
以及明确不支持的特性。mesh 子节不要求整个进程/任务套件；assetc 子节增加真实进程输入输出验证。
版本化测试夹具应可再生成，不依赖临时下载的模型或大型外部数据集。

发布前检查图片失败是否保留旧 meta 映射与已发布产物；导入器只产生候选，由资产管线控制提交。
子资产删除留下明确缺失诊断，已有引用不自动指向新资产。多 mesh/更多材质通道按后续需求单独扩展。

M4.4.2 在 GltfImportRequest 中加入可选 stop_token，读取、解析、primitive 和纹理解码之间协作检查；取消不抢占三方调用。
