---
created_at: "2026-10-03T15:28:09+08:00"
updated_at: "2026-10-03T21:44:26+08:00"
---

# 场景数据、GPU 资源与离屏渲染

[返回项目入口](../../README.md)。本页对应 M7 场景渲染和 Runtime 截图自动化。

## 配置与验证

`DK_BUILD_RENDER_DATA` 构建 `dk::render_data`（Scene/Memory，无 Vulkan 依赖）；
`DK_BUILD_RENDER_RESOURCES` 构建 `dk::render_resources`（asset_data/Device/Graph）。
`DK_BUILD_RENDER_PIPELINE` 构建 `dk::render_pipeline`，要求前两者、Graph 和 Shaders。
`DK_BUILD_RENDER_DISK` 构建 `dk::render_disk`，要求 Render Data/Resources、Asset Importers/Runtime。
`DK_BUILD_RENDER_CAPTURE` 构建可选的截图服务/操作并接入 Runtime，要求 Framework、Scene、Jobs、Render Disk/Pipeline。
五个选项默认 OFF，windows-graphics 启用；windows-dev 的 CPU-only Runtime 不链接渲染模块。

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

## 最小场景管线

公开入口：[ScenePipeline.hpp](../../engine/render/pipeline/include/dk/render/ScenePipeline.hpp)。
在上述上传完成提交后，用同一队列调用 ScenePipeline::create(heap, queue, shader_directory)，
shader_directory 指向仓库 shaders/render 或部署时复制的同名目录。创建时编译固定 Slang 源；
每帧调用 render(queue, view, cache, settings)，不必先在 CPU 等待资产上传完成。
每一步 Result 均须检查，渲染成功表示提交成功。

管线按 mesh 引用绘制全部 primitives：深度预处理 → 无光照 opaque HDR → Reinhard/sRGB → RGBA8 读回。
支持父级世界矩阵、显式相机/投影、实例、基础色纹理和 emissive；支持 alpha mask；当前无 alpha blend、灯光/PBR、阴影或可见性裁剪。
缺少 mesh、alpha blend 或世界到裁剪矩阵溢出会拒绝整帧。实体上的 material/texture 引用不自动覆盖 mesh 材质。

RenderFrame::wait(queue, timeout) 确认完成后，read_rgba8(span) 写入 width*height*4 字节。
像素为 sRGB 编码 RGB、alpha=255，保持正高度 Vulkan viewport 的行方向。
color() 返回同一帧的借用 Image（RGBA8 UNORM，内容已编码为 sRGB，最终 shader-read layout）；
后续消费者应按这一色彩约定使用。info() 给出 scene/revision/frame、尺寸与 draw_count。
settings.capture_plan=true 时 plan_text() 返回五个 Pass 的图计划。
每帧独立拥有输出，失败保留旧帧；释放 Frame/缓存/管线不会提前销毁在途资源，需通过队列回收完成工作。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target dk_render_pipeline_tests -TestRegex '^dk\.render\.pipeline\.' `
    -Reason 'Render settings and matrix conversion'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target dk_render_pipeline_probe -TestRegex '^dk\.render\.pipeline_gpu_validation$' `
    -Reason 'Depth, HDR tone mapping, image regression and frame lifetime'
```

探针在 build/tests/integration 中输出 render-pipeline-validation.ppm 和同名 .txt 图计划，
覆盖重叠几何、相机/实例、曝光、不同尺寸与空场景，以及对象/提交/预算失败和在途释放。
设计见 [render-pipeline](../design/render-pipeline.md)，阶段记录见 [0062](../development/0062-render-pipeline.md)。

## 默认磁盘测试素材

默认输入集中在 [projects/demo/assets](../../projects/demo/assets/README.md)，
[defaults.json](../../projects/demo/assets/defaults.json) 指定 Sponza glTF、
Citrus Orchard Road HDR 和 sky_clouds_12 六面天空盒，路径均相对于 assets 目录。
原始素材为本地副本，Git 保存清单与来源说明；新 checkout 需按说明准备。
该清单是素材索引；真正的 M2 工程入口为 projects/demo/project.json，场景为
scenes/sponza.scene.json。默认示例读取这个工程。HDR 环境采样和天空盒绘制仍未接入。

## 磁盘场景与默认 Sponza

公开入口：[DiskScene.hpp](../../engine/render/disk/include/dk/render/DiskScene.hpp)。
调用方装配带 scratch 的 Memory ThreadContext，打开 Project 后依次检查：
DiskScene::load(heap, project, options) → disk.upload(queue) → RenderView::create(disk.scene(), view) →
ScenePipeline::render(queue, view, gpu_assets)。load 只读，不写 source.meta 或任何缓存索引；
upload 逐包等待完毕才返回新的完整缓存。重新加载失败时，旧 DiskScene、缓存、Frame 仍可使用。

Project 的 mesh 路径可以指向 .gltf/.glb 或 M4 产物的 manifest.json；已有 source.meta 的 mesh/0 ID
必须与 Project 对应，没有 meta 时用 Project ID。M4 产物读取并校验其自带身份和摘要。
多个实体引用相同 mesh 只加载一次；节点摆放来自 M2 Scene，不自动使用 glTF node transforms。
最多 64 个包、累计 512 MiB CPU 数值/纹理载荷，DiskSceneOptions 可降低限额。

默认 strict 保持 M4 导入范围；Sponza 示例显式使用 GltfImportProfile::unlit_preview，
校验但省略切线，忽略 normal/occlusion/metallicRoughness 纹理并输出诊断。base color 和 alpha mask 保留。
不省略 emissiveTexture、未知属性或扩展。已有资产编译器/缓存仍然 strict，预览不生成其持久产物。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_disk_tests','dk_render_disk_probe','dk_render_demo') `
    -TestRegex '^dk\.render\.(disk\.|disk_gpu_validation$|sponza_validation$)' `
    -Reason 'Disk scene loading, alpha mask and default Sponza'
```

需要 Vulkan/Khronos validation；本机旧 AMD 隐式层兼容设置见 [Graphics 指南](graphics.md)。
Sponza 测试带 local-assets 标签，缺少本机素材时明确跳过（77）；小型磁盘夹具不依赖 Sponza。
程序 dk-render-demo 无参数时读取仓库默认工程，输出当前目录 render-sponza.ppm 和 .txt 图计划。
也接受三个位置参数 `PROJECT_ROOT MANIFEST OUTPUT.ppm`（路径支持 Unicode）；示例使用固定 Sponza 相机，
通用库调用者通过 ViewDescription 自行配置相机。输出父目录须存在，PPM 文件在完成读回后原子写入。

上述库之外，Runtime 截图入口见下节；HDR/天空盒着色仍未接入。
设计与验收见 [render-disk](../design/render-disk.md)、[0063](../development/0063-render-disk.md)。

## Runtime 截图自动化

构建 windows-graphics 的 dk_run 后运行（PowerShell 7）：

```powershell
cmake --build out/build/windows-graphics --config Debug --target dk_run
./examples/render/capture.ps1 -RequireValidation
```

脚本通过 stdio 执行 scene.load → render.capture → jobs.wait → runtime.shutdown，
自动使用实际 guard/JobId，输出默认工程下 captures/sponza.ppm（生成目录被 Git 忽略）。
可指定 -Runner、-ProjectRoot、-Manifest、-Output、-Width、-Height；其他相机用直接命令配置。
源素材准备见 [默认素材](../../projects/demo/assets/README.md)，本机 AMD 层处理见 [Graphics 指南](graphics.md)。

render.capture 固定活动内存 Scene 版本，支持未保存编辑，成功结果携带 scene_id/revision/frame。
示例只是单次捕获；重复编辑/截图及取消可通过相同 stdio 连接完成。
EOF 会取消未发布任务，因此自动化客户端应先等待 succeeded 再关闭输入。
输出目前为 PPM；PNG 转换不属于 Runtime 命令能力。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_run `
    -TestRegex '^dk\.runtime\.capture_gpu_validation$' -Reason 'Capture automation delivery B'
```

进程测试使用仓库内小夹具，校验原图/冻结快照完全相同、新变换图像不同、空闲发布、
导入与原子替换失败保护、取消、EOF/shutdown；不需要本地 Sponza 文件。
完整字段见 [截图命令](../commands/render.md)，状态和限制见 [设计](../design/render-capture.md)。
