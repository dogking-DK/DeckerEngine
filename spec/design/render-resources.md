---
module: render-resources
created_at: "2026-10-03T15:06:44+08:00"
updated_at: "2026-10-03T15:24:26+08:00"
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
这些不能作为已实现能力。后续 [M7.2](../roadmap.md) 实现管线，M7.3 才连接磁盘/AsyncAssets。

关联 [CPU 资产](assets-importers.md)、[Graph](graphics-graph.md)、[资源寿命](graphics-resources.md)、
[0061](../development/0061-render-data-resources.md)。
