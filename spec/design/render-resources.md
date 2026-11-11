---
module: render-resources
created_at: "2026-10-03T15:06:44+08:00"
updated_at: "2026-11-11T20:28:55+08:00"
status: accepted
---

# GPU 资产缓存与上传

## 目标与依赖

M7.1 将已有 CpuAsset 数据转换为 GpuMesh、纹理 image/view/sampler 和只读材质元数据。
engine/render/resources 提供 dk_render_resources / dk::render_resources，PUBLIC 使用
asset_data、graphics_device，PRIVATE 使用 graphics_graph；不依赖导入器、磁盘缓存、Scene 或 Runtime。
DK_BUILD_RENDER_RESOURCES 默认 OFF，要求 Memory、Math、Device、Graph；asset_data 可独立于
导入器启用。windows-graphics 同时开启本模块和 render_data。无新增三方库或 feature。

## 接口和数据

GpuAssets::create(heap) 创建可移动、不可复制缓存；upload(queue, CpuAsset) 以 mesh AssetId
为包键，新上传始终创建新资源，生成缓存实例内单调 generation。find(id) 返回当前 GpuAsset
共享只读快照；unload(id) 删除缓存引用并返回是否存在，重复卸载为无操作；clear 释放全部缓存引用。
GpuAsset 保存 mesh ID/generation、Submission、GpuPrimitive 列表、GpuTexture 列表和 MaterialData。
mesh() 返回借用型 GpuMesh（ID 和 primitive span）；借用视图必须由 GpuAsset 保活。
空快照查询返回空集合/零值；wait(queue, timeout) 委托提交票据验证所属队列并等待完成。

每个 primitive 使用独立 interleaved vertex buffer 和 uint32 index buffer，三角列表；
GpuVertex 固定 32 字节 position[3]/normal[3]/uv[2]，不依赖 Eigen 的内部布局。
缺 normals 填 (0,0,1)，缺 UV 填零，同时保留 has_normals/has_texcoords；bounds 从 positions 重算。
TextureData 的紧密 RGBA8 sRGB 字节原样上传 R8G8B8A8Srgb，单 mip/layer，保留行和 UV 方向；
sampler 映射 glTF min/mag/wrap，mip 过滤方式保留但 LOD 限为 0，无 mip 生成。
材质/纹理按输入顺序拥有副本，引用仅在本 CpuAsset 包内解析；不提供跨包子资产去重或全局子 ID 查找。缓存线性查找，元数据唯一性校验首版为二次扫描。
校验非 nil 且全包唯一 ID、索引范围、三角计数、属性长度/有限性、材质数值范围/枚举/引用、
纹理尺寸/字节数和 sampler 枚举。不读取 outputs/diagnostics/origin，也不再次应用导入时 unit_scale。

## 提交、失败与生命周期

所有缓存/队列操作由调用者串行；queue 只在 upload/wait 时借用，不存放裸队列指针。
一个缓存可以接收不同队列的包，使用快照的 GPU 对象必须在其上传队列；wait 拒绝异队列票据。
生成号仅在当前缓存实例内用于诊断，不能替代持久 AssetId 或当成跨缓存句柄。

upload 先完成输入校验、缓存槽预留、候选资源、staging 和 Graph 编译。CPU staging.write 之后，
Graph copy Pass 声明完整写入 GPU 资源，final access 导出 vertex/index read 和 fragment sampled read。
只有 Graph execute 成功提交后，通过无分配的 shared_ptr 交换发布缓存并递增 generation。
失败保留旧 find 结果/代际；候选 RAII 释放，未提交状态不发布。驱动 device_lost 仍使队列失效，
不承诺 GPU 设备失败后旧资源继续可用。发布成功只表示 submitted，不能宣称 GPU 已完成；
wait=true 才确认完成，同队列后续 Graph 可以依靠队列顺序和资源状态直接接续使用。

unload/clear/缓存销毁不取消已提交工作。已取得快照继续持有旧 GPU 数据，重载后旧快照不变。
所有快照释放后，在途提交仍由 Device 的 pending owners 保留资源；完成并回收后才销毁。
CPU 输入只在 upload 调用中借用，可在返回后立即释放。Memory closing 后允许查询、卸载及
已有快照等待，拒绝新上传；queue 销毁使用既有 drain 规则，快照中的 GPU 包装器可晚于队列释放。

## 验证与限制

真实 GPU 逐字节读回 vertex/index/RGBA8，核对状态导出、sampler、多个 primitive/texture。
验证 pending 卸载、重载旧快照、缓存销毁、队列关闭/异队列等待、失败提交、候选分配失败和
Memory 预算；同步验证层无告警，最终 pending/VMA/Memory 回零。
CPU 验证输入拒绝无需初始化 GPU。无后台任务、自动缓存淘汰、渲染帧/descriptor 管理，
这些不能作为本模块已实现能力。业务管线见 [render-pipeline](render-pipeline.md)，
磁盘装配见 [render-disk](render-disk.md)；后者同步读取候选并准备 GPU 缓存，未接入 AsyncAssets/Jobs。

关联 [CPU 资产](assets-importers.md)、[Graph](graphics-graph.md)、[资源寿命](graphics-resources.md)、
[0061](../development/0061-render-data-resources.md)。

## M23.5 体素 GPU 资产

启用 render_voxels 时，resources PRIVATE 消费该模块并公开 `VoxelAssets.hpp`。`VoxelAssets::create(heap, queue)` 绑定单队列，
缓存以 voxel AssetId 索引 `GpuVoxelAsset`（generation、VoxelGpuGrid 共享所有权、不可变 palette）。旧 GpuAssets 明确拒绝 voxel 包。
上传校验独占 CPU payload、唯一 voxel/0 输出、全域 palette 后预留槽与候选 palette；底层上传成功后，以无分配操作发布 current 和 generation。
原网格快照再次上传可以复用 buffer；不同 CPU 快照整份上传。失败保持旧版本，提交成功仍需 wait 判断 GPU 完成。

默认最多 64 当前资产；每个底层 grid cache 512 MiB / 两个存活网格代，计入在途 staging，复用 M23.3 额度与回收规则。
这些是每缓存实例的 payload 限额，不是跨资产或卸载重建后的进程显存总上限；palette 快照与帧载荷另受计数/帧预算约束。
unload/clear 删除查找引用，不取消已提交帧。旧句柄保留 grid/palette，Frame 保留 grid 和上传后的 palette；等待/读回允许发生于缓存释放和 Memory closing 后。
新 upload 拒绝 closing；device_lost 不承诺旧 GPU 资源仍有效。

ScenePipeline::render_voxel_assets 从 RenderScene 的 voxel 引用解析资产、EntityId 和世界变换，复用既有 HDDA/深度/HDR/tone。
每实体最多一个 voxel 引用，最多 64 实例；缺失资源或使用 mesh-only 入口明确失败，不能静默漏画。
原生 PathTraceScene 和 DiskScene/capture 当前同样拒绝 voxel，留待 M23.8/9、M23.6 接入。
验证：真实 GPU 文件→缓存→Scene 引用、palette 替换、失败提交、代际预算、卸载、在途帧及 closing，见 [0163](../development/0163-voxel-assets.md)。
