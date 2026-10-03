---
module: render-disk
created_at: "2026-10-03T17:17:15+08:00"
updated_at: "2026-10-03T17:17:15+08:00"
status: accepted
---

# 磁盘场景与资产集成

## 边界与依赖

M7.3 新增独立可选 engine/render/disk、dk_render_disk / dk::render_disk。
PUBLIC 依赖 render_data、render_resources、asset_importers（公开 profile）、Scene；
PRIVATE 依赖 asset_runtime、IO、Memory、profiling。DK_BUILD_RENDER_DISK 默认 OFF，
windows-graphics 启用；不接入 CPU Runtime、Jobs/capture 命令或窗口，不升级依赖。

DiskScene::load(heap, project, options) 只读加载 M2 场景并提取 RenderScene，按实体/引用顺序
导入每个唯一 mesh。Project 的 mesh path 支持 .gltf/.glb 或 M4 CPU artifact 的 manifest.json。
源 glTF 使用 M4 import_gltf；已有 source.meta 必须合法且 mesh/0 ID 与 Project 一致，保留
output identities/unit_scale；无 meta 时由 Project 指定 mesh/0 ID，子 ID 仅在候选内生成。
artifact 使用 load_cpu_artifact，校验产物 mesh ID 与 Project 对应；不要求源文件仍在。
不自动生成/写回 meta、cache/current、Project 或 Scene；未恢复的资产操作先拒绝。
非 mesh 实体引用仍由 M2 验证登记，保持 M7.2 不推断 material override 的约定。

DiskScene 共享拥有不可变 RenderScene 和 CpuAsset 候选，asset span 随所有者存活。
每次 load 独立，不替换先前候选。最多 64 个 mesh 包、累计 CPU 数值/纹理载荷 512 MiB；
options 只能降低限额，单次导入继续使用 M4 硬上限。跨文件外部并发改写不承诺原子快照。
调用方绑定带 scratch 的 ThreadContext；load 临时切入传入 heap，恢复原路由。

upload(queue) 创建全新的 GpuAssets，逐包上传并等待完成后返回。发布点为全部成功返回；
不操作调用者旧缓存。上传后半段失败不能撤销之前的 GPU 提交，候选包装器正常释放，
仍在途资源按 queue pending owners 回收；错误不返回部分缓存。调用方可保持旧 DiskScene/缓存/帧。
渲染仍通过 RenderView::create(scene, desc) 和 ScenePipeline::render；相机由调用者显式提供。

## Sponza 与导入子集

新增 GltfImportProfile::unlit_preview，默认 strict 保持 M4 严格拒绝行为。
预览仅允许明确省略 TANGENT 和 normal/occlusion/metallicRoughness 纹理，输出诊断；
切线仍校验 Vec4/FLOAT、数量、范围和有限值，未用光照纹理不解码，不持久化预览产物。
emissiveTexture、其他未知属性、多 mesh/动画/skin/扩展仍拒绝。已有编译/cache 工具只用 strict，
不会把预览结果混入持久缓存键。预览不承诺 PBR，base color/alpha/emissive 数值保持原语义。

为保留 Sponza 镂空，ScenePipeline 支持 alpha mask（仍拒绝 alpha blend）。
mask depth fragment 和 opaque fragment 使用同一 baseColorTexture/factor/cutoff discard；
复用同一 vertex 模块和顶点/push 输入，以 Equal depth 绘制，避免不透明物体遮住镂空。
opaque 原仅深度管线保留；Graph 深度 Pass 声明所需纹理采样。

projects/demo/project.json 与 scenes/sponza.scene.json 使用既有 M2 v1 格式，固定 mesh ID、
实体缩放 0.008，显式对应源 glTF 唯一节点。加载器仍不推断/导入 glTF node placement。
dk-render-demo 默认加载该工程，以显式 unlit_preview 渲染默认 Sponza 并保存 PPM；
可传工程根、manifest、输出路径。示例相机固定供 Sponza 使用，通用库不猜测相机。
HDR/六面天空盒继续作为预备素材，不在本阶段加载或采样。

## 验证

CPU 夹具使用临时 M2 Project/Scene 与小型 M4 glTF/CPU artifact，核对身份、世界矩阵、
重复 mesh 去重、meta 冲突、缺失/损坏/不支持输入、预算/closing 和旧候选保留。
真实 GPU 读盘→导入→上传→绘制→读回，像素断言验证 alpha mask 的孔洞和深度；
修改/保存/重载 Scene 后检查 revision 和图像变化，失败保持旧图像，最后资源归零。
本地 Sponza 单独验收 103 primitives 和非空图像，素材缺失明确跳过该可选样例，不让
版本化小夹具依赖本机大素材。Khronos/sync validation 零错误警告。

关联 [导入](assets-importers.md)、[场景](scene.md)、[管线](render-pipeline.md)、
[0063](../development/0063-render-disk.md)。
