---
module: render-pipeline
created_at: "2026-10-03T16:28:50+08:00"
updated_at: "2026-10-13T09:28:33+08:00"
status: accepted
---

# 最小场景渲染管线

## 范围与依赖

M7.2 连接 RenderView、GpuAssets 和 Graph，渲染程序化场景，输出可等待的离屏 RGBA8。
engine/render/pipeline 提供 dk_render_pipeline / dk::render_pipeline；PUBLIC 依赖 render_data、
render_resources，PRIVATE 依赖 graphics_graph、graphics_shaders、profiling；构建大气模块时额外 PRIVATE 链接 render_atmosphere。
DK_BUILD_RENDER_PIPELINE 默认 OFF，windows-graphics 启用。无新增依赖/feature。
最小核心不负责磁盘/Runtime/窗口装配；M14.4 增加显式 Lambert 入口。不支持完整 PBR、alpha blend、几何阴影或多队列。

## 公开入口

ScenePipeline::create(heap, queue, shader_directory) 编译随工程提供的固定 Slang 源并创建
深度/alpha-mask-depth/不透明/tone 管线；shader_directory 显式传入，不硬编码部署路径。
render(queue, view, gpu_assets, settings) 同步构建/录制一帧并异步提交，返回 RenderFrame。
Settings 为有限非负线性 HDR clear_rgb、有限非负 exposure、可选 Graph 诊断及仅日光入口消费的 sky_diffuse 开关。
M8.4 提供 `unlit_preview_settings()` 作为编辑器视口和 Runtime capture 的共用预览设置
（线性 clear_rgb=(0.04,0.08,0.16)、exposure=1），通用 RenderSettings 的默认黑色不变。
队列/管线/缓存均由调用者串行使用；管线无裸队列指针，只用于创建它的队列。

按稳定实体 ID 和原资产引用顺序遍历 mesh 引用，每个 primitive 形成一次 draw。
重复 mesh 引用产生实例，GPU 资源以物理对象去重导入；material/texture 实体引用不推断 override。
缺少 mesh、alpha blend 材质、不可表示为有限 float 的世界到裁剪矩阵均拒绝整帧。
使用完整 world_to_clip * world，保持父级剪切；GPU 顶点着色使用显式四行 dot，避免矩阵内存布局歧义。
首版双面无背面剔除，颜色为 base_color.rgb * sRGB 纹理采样的线性 RGB + emissive；
无纹理使用清为白色的 1x1 图资源。render 保持 unlit；render_sunlight 使用 normals，metallic/roughness 仍不参与 Lambert。

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

## 环境集成与后续边界

[大气](render-atmosphere.md)与[体积云](render-clouds.md)草案拟扩展环境快照、ViewId、显式时间和质量输入，
3D资源和可采样深度底座已接入；M14.4 显式 render_sunlight 提供太阳漫反射，尚不自动接入视口/capture；tone 前的天空/介质合成留到 M14.5。
旧场景默认关闭环境并保留unlit输出；云接入时统一组合空气/云区间，避免重复应用空气透视。
物理缓存和每视图历史按完成状态发布，保持旧RenderFrame的独立寿命；不复用跨帧Graph句柄。
实施每阶段时同步本设计与真实接口、诊断和测试；阶段顺序见[Roadmap](../roadmap.md)。

## 深度采样底座

场景D32目标增加Sampled usage，创建时核对附件/clear/sampled组合能力。现有unlit图顺序和输出保持原契约；
深度写后采样屏障由Graphics Graph体积探针独立验证，世界射线重建和大气合成属于M14.5。

## M14.4 最小日光接收实施契约

新增显式 `render_sunlight(queue, view, assets, sky, settings)`；原 render/unlit_preview_settings 保持旧路径。
启用且环境完全匹配的 AtmosphereSky 提供太阳、T 与不含太阳盘的 Sky View；disabled 环境回到原 unlit。
未构建大气时此入口返回 not_supported；pipeline 仅在该 target 存在时 PRIVATE 链接大气。
不自动准备或刷新 LUT，不增加 render 内部等待；调用方先 prepare，再绑定同版本 Scene 环境。
天空背景和空气透视留到 M14.5；曝光仍只在 tone 一次执行。

几何输出为 base_color * linear_texture / pi * (E_sun * T_sun(position) * max(N dot sun_direction,0)
+ E_sky(N)) + emissive。直射在每片段真实世界位置查 T 并检查行星遮挡；GPU 使用相对快照锚点的 km
和独立高度计算，使用变换后的世界轴对齐包围盒保守拒绝地下接收体，最远角点不超过 2H，位置分量不超过 ±1e9m。太阳采用中心方向的方向光，不对盘面积重复积分。
天空漫射在快照锚点的 Sky View 上用固定 16×16 等立体角方向积分 L * max(N dot omega,0)；
这是局部环境漫射近似（所有接收点共享锚点天空），不宣称逐点 GI；可通过 settings.sky_diffuse 关闭以诊断直射。
Sky View 不含太阳盘，避免把太阳重复计入漫射。无镜面/PBR、几何阴影和地表间接反弹。

完整仿射矩阵的逆转置变换法线，片段重新归一化；奇异/数值不可逆变换拒绝 lit 整帧，unlit 保持原约定。
缺失、零或插值抵消法线使用屏幕导数计算几何面法线；退化三角形无有效面积，安全返回零漫射。
负 determinant 修正正反面判定；lit 的单面材质在 depth 和 color 同时丢弃背面，双面翻转背面法线。
约定依据 [glTF 2.0](https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#double-sided)，
unlit 历史双面可见行为不变。alpha-mask 两 pass 使用相同裁剪规则，emissive 不乘光照或太阳透射。

AtmosphereSky 增加只读锚点和借用 LUT Image 查询。Graph 显式导入并声明 fragment sampled read，
逐 draw uniform 声明 vertex/fragment uniform read。首次显式调用按需创建 lit 深度/颜色管线，完整候选成功后发布为静态缓存；后续绘制失败不撤销已创建的静态管线。提交保留资源，旧快照/帧不因 prepare 或释放调用者而变化。
环境不匹配、非法矩阵/范围、分配/创建/记录/提交失败均不改变旧帧、场景或大气代次。

验收：真空 Lambert 解析 RGB/方向/斜法线、剪切/负缩放、单/双面、缺失/零法线、alpha-mask 和 emissive；
真实大气直射对 CPU 高采样透射，天空漫射对高密度角积分，量化前固定 RGB tone 后容差 2 byte；
太阳能量线性与曝光一致、disabled/显式 unlit 完整图一致；版本错配/对象/提交/预算/跨队列失败和在途寿命。
局部 CPU 矩阵测试与原 pipeline GPU 图像回归，required GPU 探针输出可复现 PPM。
