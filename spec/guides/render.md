---
created_at: "2026-10-03T15:28:09+08:00"
updated_at: "2026-10-03T15:28:09+08:00"
---

# 场景提取与 GPU 资源

[返回项目入口](../../README.md)。本页对应 M7.1；业务绘制管线与磁盘场景集成尚未实现。

## 配置与验证

`DK_BUILD_RENDER_DATA` 构建 `dk::render_data`（Scene/Memory，无 Vulkan 依赖）；
`DK_BUILD_RENDER_RESOURCES` 构建 `dk::render_resources`（asset_data/Device/Graph）。
两者默认 OFF，windows-graphics 启用；可独立开启，CPU Runtime 不链接它们。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_data_tests','dk_gpu_asset_tests') `
    -TestRegex '^dk\.render\.(data|assets)\.' -Reason 'Render CPU contracts'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target dk_render_resources_probe -TestRegex '^dk\.render\.gpu_validation$' `
    -Reason 'Graph asset upload and GPU lifetime'
```

GPU 探针要求 Vulkan 设备和 Khronos validation layer，缺失返回 77（跳过）。
本机旧 AMD 隐式层兼容处理与 [Graphics 指南](graphics.md) 相同，不能关闭 Khronos 层冒充验证通过。
探针通过时核对 vertex/index/RGBA8 全字节、上传最终状态、重载和 pending 卸载，
以及提交/预算失败后旧缓存不变，最终 GPU 与 Memory 分配回零。

## 数据提取

公开入口：[RenderScene.hpp](../../engine/render/data/include/dk/render/RenderScene.hpp)。
调用者先处理每一步 Result：SceneDocument::snapshot() → RenderScene::extract(heap, snapshot)。
entities() 按稳定 ID 排序并持有完整双精度世界矩阵；assets(entity_index) 返回原资产引用列表。
输入快照和编辑文档可以随后释放或继续编辑，输出 id()/revision() 仍指向提取时的版本。

RenderView::create(render_scene, ViewDescription) 固定 scene/revision、frame 和像素尺寸，
提供 world_to_view()/world_to_clip()。ViewDescription 需要非零 width/height，
可逆 camera_world 与有限可逆 projection；调用者负责 Vulkan clip-space 约定。
当前无 Camera 编辑组件、自动相机、可见性裁剪或材质 override 推断。

## 上传、获取和卸载

公开入口：[GpuAssets.hpp](../../engine/render/resources/include/dk/render/GpuAssets.hpp)。
调用顺序为 GpuAssets::create(heap) → cache.upload(queue, cpu_asset) → GpuAsset；
每一步返回 Result，业务须先判断成功再取得值。CPU 输入可来自导入器、CPU artifact 或程序化数据。
本模块不读取磁盘，不隐式调用 AsyncAssets。CPU 数据的容器仍遵循 Memory 上下文约定。

- upload 为同步 CPU 录制、异步 GPU 提交；成功后缓存可通过 find(mesh_id) 查到新快照。
- gpu.mesh() 返回借用型 GpuMesh，包含 ID 和 primitive span；gpu.textures()/materials() 同样借用快照。
  GpuVertex 是 32 字节 position/normal/UV，索引为 uint32。缺属性有默认字节及存在标志。
- GPU 完成须通过 gpu.wait(queue, timeout) 确认，false 是超时，Error 是失败。
  同队列后续 Graph 可以直接导入这些 GPU Buffer/Image，使用已提交状态，无需 CPU 阻塞。
- unload(mesh_id)/clear 只撤销当前缓存引用；旧 GpuAsset 保留旧版本，重载不会改写它。
  最后一个快照销毁后，在途提交仍保留实际 GPU 资源直到完成。
- 不同队列票据不能混用；CPU/GPU 持久资源域应在缓存、快照和队列完成释放后关闭。
  queue 析构会 drain，已有包装器可以保留设备并晚于队列释放。

纹理保留 sRGB RGBA8 和原行方向，只创建 mip 0；sampler 的 LOD 固定 0。
材质/纹理引用仅在每个 CpuAsset 包内部解析；generation 是缓存实例内的运行时版本，不能持久化为资产身份。

设计与完整边界见 [Render 数据](../design/render-data.md)、[GPU 资源](../design/render-resources.md)，
验收记录见 [0061](../development/0061-render-data-resources.md)。
