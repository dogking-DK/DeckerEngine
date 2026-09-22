---
module: assets-importers
created_at: "2026-09-22T18:20:46+08:00"
updated_at: "2026-09-22T18:51:42+08:00"
status: draft
---

# M4 静态 glTF 导入与 assetc 设计

## 范围和依赖

此为 M4.2 实施设计稿，尚无导入器或 dk-assetc 实现。目标是得到可查询的 CPU 数据，
不是 glTF 场景编辑器。身份/缓存遵循 [资产运行时](assets-runtime.md)，小节安排见
[0020](../development/0020-m4-development-plan.md)。

`dk::asset_importers` 依赖 asset_data、IO；解析器、图像解码和 JSON 为 PRIVATE 依赖。
按用户选型，glTF/GLB 使用 fastgltf，PNG/JPEG 使用 stb_image，缓存摘要使用 PicoSHA2。
库选型已确定；实际安装、链接与功能测试在使用它们的实施小节完成，本次只更新设计。
导入器使用引擎提供的字节/依赖读取入口，不能绕过路径、大小限制自行访问网络或任意文件。

## 已确定的三方库与基线

已读取本机 vcpkg 仓库在项目 builtin-baseline
`62159a45e18f3a9ac0548628dcaf74fcb60c6ff9` 的 baseline/port 文件，记录如下：

| 用途 | 库 / vcpkg 包 | 基线记录版本 | port 声明许可证 | 接入点 |
| --- | --- | --- | --- | --- |
| glTF/GLB 解析 | fastgltf / fastgltf | 0.9.0 | MIT | assets/importers，PRIVATE 链接 fastgltf::fastgltf |
| PNG/JPEG 解码 | stb_image / stb | 2024-07-29#1；stb_image 2.30 | MIT OR CC-PDDC | 导入器内部的单一 StbImageDecoder.cpp |
| SHA-256 | PicoSHA2 / picosha2 | 1.0.1 | MIT | 资产管线内部 Sha256 封装，见运行时设计 |
| fastgltf 的传递依赖 | simdjson / simdjson | 4.3.1 | Apache-2.0 OR MIT | 由 fastgltf port 引入 |

这些是当前基线数据，不是已安装/编译通过的声明；实施时记录实际解析结果与兼容性。
JSON 清单继续使用 nlohmann-json，ID 继续使用 stduuid，数学继续使用 Eigen；
工作队列使用标准库。simdjson 属于导入器依赖，不替换引擎的 JSON 接口。

计划 CMake 用法：fastgltf 使用 `find_package(fastgltf CONFIG REQUIRED)`；
stb 使用 `find_package(Stb REQUIRED)` 与 `Stb_INCLUDE_DIR`，以 SYSTEM PRIVATE 添加头文件目录；
PicoSHA2 使用 `find_path(DK_PICOSHA2_INCLUDE_DIR NAMES picosha2.h REQUIRED)`，同样仅内部包含。
此基线的 picosha2 port 只安装头文件，没有提供可假定使用的 CMake 导出 target。
对应模块落地时才追加其所需 vcpkg feature/自动选择逻辑，不把导入器依赖加到最小 Core 必需项。

核验依据：[固定基线](https://github.com/microsoft/vcpkg/blob/62159a45e18f3a9ac0548628dcaf74fcb60c6ff9/versions/baseline.json)、
[fastgltf port](https://github.com/microsoft/vcpkg/blob/62159a45e18f3a9ac0548628dcaf74fcb60c6ff9/ports/fastgltf/vcpkg.json)、
[fastgltf 0.9.0 CMake](https://github.com/spnda/fastgltf/blob/v0.9.0/CMakeLists.txt)、
[Stb 查找模块](https://github.com/microsoft/vcpkg/blob/62159a45e18f3a9ac0548628dcaf74fcb60c6ff9/ports/stb/FindStb.cmake)、
[PicoSHA2 port](https://github.com/microsoft/vcpkg/blob/62159a45e18f3a9ac0548628dcaf74fcb60c6ff9/ports/picosha2/portfile.cmake)。

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

计划公开头文件位于 `engine/assets/data/include/dk/assets/`，使用 dk 类型，不暴露解析器对象。

- MeshData：primitive 列表，位置/可选法线/可选 UV、uint32 索引、计算得到的局部包围范围、可选材质 ID。
- MaterialData：上述数值参数、纹理 AssetId 与 sampler 语义。默认材质作为明确的内置值，避免伪造 nil 引用。
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

工具计划位于 tools/assetc，target `dk_assetc`，程序 `dk-assetc`。复用同一资产管线，
不链接 framework Runtime、渲染器或窗口。先完成单源同步导入，再由 M4.3 接入可复用缓存。

拟定调用形式（实现前不可执行）：

```text
dk-assetc import --project-root ROOT --source assets/model.glb
```

命令显式创建/复用 meta，输出 JSON 摘要（root_id、outputs、依赖、产物 key/位置及诊断），
日志到 stderr；不会自动修改当前场景或任意 Project 清单。调用者可使用输出登记工程引用，
M4.4 的 AssetService 提供有版本检查的清单适配。M4.2.2 先固定并验证 CPU 产物 v1，
无缓存时写入独立输出目录；M4.3 复用该格式并增加内容键/current 索引，不能暂用对象内存 dump。
根目录与输入使用 IO 路径约定，Windows 原生参数兼容 Unicode；退出 0 成功、1 导入失败、
2 参数错误、3 致命基础设施错误。具体 flags/help 在 M4.2.2 固定并补入根 README。

## 验证与演进

使用仓库自制的小型有来源夹具：单三角形、两个 primitive、带 base color 纹理、GLB 与外部依赖。
验证数据/ID/材质关联，错误索引、截断 buffer、越界 stride、坏图、超预算、Unicode 路径，
以及明确不支持的特性。mesh 子节不要求整个进程/任务套件；assetc 子节增加真实进程输入输出验证。
版本化测试夹具应可再生成，不依赖临时下载的模型或大型外部数据集。

发布前检查图片失败是否保留旧 meta 映射与已发布产物；导入器只产生候选，由资产管线控制提交。
子资产删除留下明确缺失诊断，已有引用不自动指向新资产。多 mesh/更多材质通道按后续需求单独扩展。
