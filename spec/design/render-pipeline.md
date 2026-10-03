---
module: render-pipeline
created_at: "2026-10-03T16:28:50+08:00"
updated_at: "2026-10-03T17:31:34+08:00"
status: accepted
---

# 最小场景渲染管线

## 范围与依赖

M7.2 连接 RenderView、GpuAssets 和 Graph，渲染程序化场景，输出可等待的离屏 RGBA8。
engine/render/pipeline 提供 dk_render_pipeline / dk::render_pipeline；PUBLIC 依赖 render_data、
render_resources，PRIVATE 依赖 graphics_graph、graphics_shaders、profiling。
DK_BUILD_RENDER_PIPELINE 默认 OFF，windows-graphics 启用。无新增依赖/feature。
本阶段不接磁盘工程、Runtime/capture 命令、窗口、灯光/PBR、alpha blend、阴影或多队列。

## 公开入口

ScenePipeline::create(heap, queue, shader_directory) 编译随工程提供的固定 Slang 源并创建
深度/alpha-mask-depth/不透明/tone 管线；shader_directory 显式传入，不硬编码部署路径。
render(queue, view, gpu_assets, settings) 同步构建/录制一帧并异步提交，返回 RenderFrame。
Settings 为有限非负线性 HDR clear_rgb、有限非负 exposure 和可选 Graph 诊断。
队列/管线/缓存均由调用者串行使用；管线无裸队列指针，只用于创建它的队列。

按稳定实体 ID 和原资产引用顺序遍历 mesh 引用，每个 primitive 形成一次 draw。
重复 mesh 引用产生实例，GPU 资源以物理对象去重导入；material/texture 实体引用不推断 override。
缺少 mesh、alpha blend 材质、不可表示为有限 float 的世界到裁剪矩阵均拒绝整帧。
使用完整 world_to_clip * world，保持父级剪切；GPU 顶点着色使用显式四行 dot，避免矩阵内存布局歧义。
首版双面无背面剔除，颜色为 base_color.rgb * sRGB 纹理采样的线性 RGB + emissive；
无纹理使用清为白色的 1x1 图资源。metallic/roughness 和 normals 保留供后续光照使用，本阶段为 unlit。

RenderFrame 拥有 Submission、颜色 Image、紧密 RGBA8 readback、SceneId/revision/frame/extent/draw_count。
wait(queue, timeout) 返回完成/超时/错误；read_rgba8(span) 要求匹配字节数，未完成拒绝而不改写输出。
plan_text() 在启用诊断时提供拥有型计划文本；输出 alpha 恒为 1，正高度 Vulkan viewport 行顺序。
color() 的 Image 借用 Frame；跨帧使用者可显式 share。每帧独立资源，不覆盖先前 Frame。

## Graph 与数值约定

1. clear targets：完整清除 HDR RGBA32F、D32 depth=1、LDR RGBA8 及 1x1 white。
2. depth prepass：仅深度附件，Less/test/write，绘制 opaque 和 alpha mask primitives，mask 使用片段采样与丢弃。
3. opaque：HDR 颜色及同一 depth，Equal/test、不写 depth；复用同一顶点 ShaderModule、输入布局和 push 字节，保持深度计算一致。
4. tone mapping：全屏三角形，Texture.Load 读 HDR，不要求 float32 线性过滤；
   对每通道执行 Reinhard `1 - 1 / (1 + max(c,0)*exposure)`，再显式线性转 sRGB。
5. readback：LDR → host buffer，final host-read；颜色和 buffer 同时作为 Graph 输出。

图声明精确覆盖 vertex/index/texture，深度附件访问沿现有 encoder 保守声明 read/write。
每个 Pass 使用 device typed API，Render 不调用裸 Vulkan；Graph 管理初始内容、顺序和同步。
RGBA32F、D32、目标尺寸须满足实际设备能力；无替代格式或隐式降级。
投影继续由 RenderView 调用方提供（Vulkan z=[0,1]），不新增 Camera 持久组件。

## 提交与失败

输入解析、必要分配、Graph 编译、诊断及 Frame 容器准备均在提交前完成。
成功 Graph execute 是提交点，之后只移动拥有者。参数/分配/录制/submit 失败不返回半帧，
不修改 Scene、GpuAssets 或先前 Frame；device_lost 仍遵循队列失效契约。
不在 render 内等待；timeout 不丢弃 Frame。Frame/管线/缓存可先于 GPU 完成释放，pending owners
保活资源；最后通过 queue wait/poll/析构 drain 回收。Memory closing 后旧帧查询和读回仍有效，
拒绝新帧。Error 和 Slang/Device 已有内部 noexcept 分配边界不扩大为任意进程 OOM 保证。

## 验证与取舍

程序化重叠几何验证深度与遮挡、实例/相机变换、纹理 sRGB 解码、HDR emissive、曝光/tone，
检查背景/前景数值、完整图像重复性、不同尺寸、空场景及场景版本绑定，并输出 PPM/图计划。
验证缺失资产/alpha blend/溢出拒绝、提交/对象创建失败、等待超时及在途卸载/结果/管线销毁。
底层 HDR footprint/对齐/溢出、深度 clear/仅深度管线增加回归，保留旧颜色+深度路径。
固定几何选择避开像素中心落边界的输入，完整图像与 CPU 参考比较时 RGB 容差为 1 byte、alpha 精确；
同设备重复帧逐字节比较。不做 GPU 性能或跨厂商 bit-exact 承诺。

关联 [数据](render-data.md)、[资产](render-resources.md)、[Graph](graphics-graph.md)、
[Vulkan 使用层](graphics-vulkan.md)、[0062](../development/0062-render-pipeline.md)。

## M7.3 alpha mask

支持 opaque 和 alpha mask，拒绝 blend。mask 的 depth fragment 与颜色 fragment 使用一致的
采样 alpha * baseColor.a < cutoff 丢弃，顶点模块一致；深度 Pass 额外声明纹理采样。
opaque 保留无 fragment 的 depth-only 管线。磁盘装配独立在 [render-disk](render-disk.md)。
