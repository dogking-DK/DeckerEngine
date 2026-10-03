---
id: "0062"
created_at: "2026-10-03T16:28:50+08:00"
updated_at: "2026-10-03T16:57:00+08:00"
status: completed
design_refs:
  - ../design/render-pipeline.md
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
  - ../design/graphics-graph.md
  - ../design/architecture.md
---

# 0062 M7.2 最小场景渲染管线

## 目标与设计依据

按 [render-pipeline](../design/render-pipeline.md) 实现 depth/opaque/tone Graph 管线和程序化图像回归。
底层补足 HDR RGBA32F 格式、深度清除和仅深度绘制，避免业务层裸 Vulkan 调用。

## 实际变更

- 新增 [render/pipeline](../../engine/render/pipeline)：ScenePipeline 创建固定 Slang 管线，
  将 RenderView 和 GpuAssets 解析为稳定 draw 列表，经五个 Graph Pass 异步输出 RenderFrame。
  Frame 绑定 scene/revision/frame/尺寸，提供 wait、RGBA8 读回、颜色图像和可选计划文本。
- [着色器](../../shaders/render) 使用显式矩阵行变换、sRGB 纹理采样、base color/emissive、
  Reinhard 和显式 sRGB 输出。depth/opaque 复用同一个顶点 ShaderModule，保证 Equal 深度计算一致。
- Device 支持 RGBA32F 的 16-byte footprint/传输/读回、D32 TransferDst clear_depth、无颜色附件的
  depth-only 管线与 encoder；Graph copy 校验实际格式字节数，PassContext 增加 clear_depth。
- GPU 资源/帧所有者在提交前准备；失败不发布半帧，不改写旧帧或缓存。
  输入拒绝、对象/submit/预算失败、关闭和 pending 销毁由定向测试覆盖。
- 新增 DK_BUILD_RENDER_PIPELINE（默认 OFF，windows-graphics 开启）及两个测试 target，
  同步架构、构建/Render 指南和测试选择表。无新依赖、命令或持久化格式变更。

## 验证记录

环境：Windows、VS 2026/MSVC、windows-graphics Debug；NVIDIA GeForce RTX 4070 Laptop GPU，
驱动 596.49，Vulkan 1.4.329，Slang 2026.18。GPU 测试要求 Khronos validation 和同步验证；
仅在进程内设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 兼容本机旧 AMD 隐式层，运行后恢复。

实际执行：

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_pipeline_tests','dk_graphics_resource_tests','dk_render_pipeline_probe') `
    -TestRegex '^dk\.(render\.pipeline\.|graphics\.unit\.)' `
    -Reason 'M7.2 CPU policy, HDR transfer bounds and compile scene pipeline GPU regression'
```

GPU 两次 verify 的参数如下（均在上述进程环境兼容设置内运行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target dk_render_pipeline_probe -TestRegex '^dk\.render\.pipeline_gpu_validation$' `
    -Reason 'M7.2 real GPU golden image, HDR/depth and failure lifetime validation'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_graph_probe','dk_offscreen_probe','dk_render_resources_probe') `
    -TestRegex '^dk\.(graph|offscreen|render)\.gpu_validation$' `
    -Reason 'M7.2 device optional color/HDR and Graph footprint changes affect existing execution, offscreen and asset upload paths'
```

- 配置通过，日志 out/m72-configure.log。
- 初次构建 out/verify/20261003-164118-f314e432 失败：Pipeline.cpp 缺少格式 helper 的头文件；
  该次未运行测试。补入 ResourcePolicy.hpp 后重建成功。
- out/verify/20261003-165055-ef94d475：12/12 CPU 测试通过，0 跳过；含 3 个管线策略用例与
  9 个 Device 策略用例，RGBA32F 对齐/范围/溢出、D32 usage/copy 拒绝覆盖。
- out/verify/20261003-165336-b07b3aac：新 GPU 探针 1/1 通过，0 跳过。
  完整图像按解析几何和 CPU 色彩公式比较，RGB 容差 1 byte、alpha 精确；重复帧全字节一致。
  覆盖先画近物体再画远物体的遮挡、同 mesh 实例、父级/相机、sRGB 纹理、HDR emissive、曝光、
  128x96/129x97/17x11、空场景、旧视图版本，以及上传后无需 CPU 等待即可绘制。
  检查缺资产、alpha mask、float 溢出、外队列、对象创建/提交错误、等待超时、分配预算逐步恢复、
  提交时关闭 heap、在途清缓存/释放管线与结果。RGBA32F 双向传输逐 float 精确，Graph 拒绝欠声明 copy 范围。
  输出 pending=0、VMA=0、Memory=0、errors=0、warnings=0。
- out/verify/20261003-165431-5c01e0bd：直接受影响的 Graph/Offscreen/GPU 资产探针 3/3 通过，
  0 跳过，三者均零验证警告/错误及零残留分配；旧 color+depth/compute/upload 路径保留。
- 输出 out/build/windows-graphics/tests/integration/render-pipeline-validation.ppm 与同名 .txt 图计划，
  并转换同名 PNG 做可视检查；五个 Pass 均保留，图中 10 个资源、两个帧输出。
- scripts/check-spec.ps1 通过：127 Markdown、1270 本地链接、元数据、表格、索引、测试入口和 JSON。
  git diff --check 通过。

未运行：Release、全量回归、独立 CPU-only 构建、窗口/跨 GPU/性能验收；本次新增选项默认关闭，
Runtime 无新增链接依赖。上述验证不包含 M7.3 磁盘资产和场景集成。

## 偏差与决策

采用 unlit base color/纹理/emissive 和 Reinhard+sRGB；无 PBR/透明或磁盘集成。

## 遗留问题与下一步

本阶段完成；下一项 M7.3 磁盘场景与资产集成。
限制：固定 RGBA32F/D32、单队列、每帧独立瞬态分配；不承诺性能或任意 OOM 可恢复。

## 修改记录

- 2026-10-03T16:28:50+08:00：创建设计与记录。
- 2026-10-03T16:57:00+08:00：完成管线、底层补充、图像/失败寿命验收和直接受影响回归。
