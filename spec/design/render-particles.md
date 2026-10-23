---
module: render-particles
created_at: "2026-10-23T18:16:00+08:00"
updated_at: "2026-10-23T18:16:00+08:00"
status: accepted
---

# 原生点绘制与粒子物理量可视化

## 目标与本次范围

给图形底座增加原生 point-list 支持，再用独立 PointCloudRenderer 消费通用 CPU/GPU 粒子输出。
首期显示点大小、方形/圆形、深度和速度/密度等物理量，满足 WCSPH/PBF 调试及实验观察。
这是 [M21](../roadmap.md#m21原生点绘制与粒子流体) 的设计，当前点绘制和流体能力尚未实现。

一个粒子对应一个 `vk::PrimitiveTopology::ePointList` primitive；不以三角形 billboard 或
`polygonMode=point` 代替原生点拓扑。首期不做透明排序、球面深度重建、流体表面重建和真实水材质。
场数据的 ray marching 后置；现有云 Texture3D 不能当作已实现的物理场输入。

## 模块边界和依赖

图形改动归 [Device](graphics-device.md)、[Vulkan 使用层](graphics-vulkan.md) 和 [Slang](graphics-shaders.md)。
拟新增 `engine/render/particles`，target `dk_render_particles` / `dk::render_particles`；
公开头文件 `include/dk/render/`，实现 `src/`，shader `shaders/`。
PUBLIC 依赖粒子公共输出/GPU 视图及 Graph/Device，PRIVATE Shaders/Profiling，不依赖 WCSPH/PBF/XPBD 算法或 Scene/Editor。
CPU 数据头文件仍可由无图形程序使用；CPU 渲染上传发生在渲染模块中。

模拟服务按已完成输出选择 CPU 上传或 GPU buffer 视图；GPU solver 不依赖 renderer。
原有 [ClothRenderer](render-simulation.md) 保持布片拓扑与接口，不把通用点云入口藏入布料分支。
图像通过现有拥有型完成帧与 ImageTransfer 交给宿主，复用队列/设备寿命规则。

## 原生点能力

| 层次 | 设计要求 |
| --- | --- |
| 设备查询 | 暴露 `largePoints` supported/enabled，以及 `pointSizeRange`、`pointSizeGranularity` |
| 设备创建 | 沿用功能请求策略区分可选/必需；Editor 宿主共享设备与独立 runner 均在创建设备时显式请求 |
| 管线 | 接受 point-list；继续支持 triangle list/strip，校验 shader 阶段与相关设置 |
| shader | vertex 输出 PointSize，fragment 读取 PointCoord；核验固定 Slang 编译器的 SPIR-V 内建映射及产物加载 |
| draw | 复用 draw/draw_indexed 的索引/范围校验；空点云由上层跳过 draw，不修改零数量拒绝规则 |

Vulkan 1.4 基线要求支持 largePoints，但功能仍须显式启用；不能仅凭物理设备支持便假定逻辑设备可用。
已创建设备不能补开 feature，宿主若未启用只允许 1 px，超过 1 px 的请求明确拒绝；禁止暗中改成三角形。
请求大小必须有限、为正且在合法范围内，提交前拒绝 NaN/Inf/越界；UI 显示设备实际范围。
硬件 granularity 可能带来栅格差异，测试按其限制取样，不要求不同 GPU 像素完全相同。

Slang 计划使用 `SV_PointSize` / `SV_PointCoord`，预期映射 `BuiltIn PointSize` / `BuiltIn PointCoord`。
现有固定版本是否完整支持以 M21.2 的编译、产物检查和真实绘制为准；若失败先记录实际差距并修订方案，
不通过换成三角形绕过用户要求，也不无记录升级依赖。

## 输入、显示和深度

PointCloudRenderer 接收拥有型粒子输出、显示配置、视图和输出尺寸；输入必须是同一 run/完成拍/generation。
GPU view 的 buffer 布局、数量、容量和状态可验证，禁止拿不匹配布局的 solver 工作数组直接当 position。
CPU 路径上传本次完成数组；GPU 路径直接从常驻完成 buffer 按 vertex ID 读取，不全量读回或逐帧重传。
静态/合成数据也能显示，渲染验收不依赖先完成流体求解器。

| 设置 | 首期语义 |
| --- | --- |
| pointSize | framebuffer 像素；UI 如用逻辑像素在入口按 DPI 转换，与粒子间距/物理半径无关 |
| shape | square 或 circle；圆形由 fragment PointCoord 裁剪，圆外 fragment 不写颜色/深度 |
| color | 常量色或同代标量通道映射；速度模可从 velocity 派生，密度/压力按通道有效性开放 |
| range / legend | 固定且显式的显示范围、单位与色标；范围外饱和，常量场规则确定；不靠每帧全量 CPU 读回找 min/max |
| view | 透视/正交及独立模拟相机；记录实际视图序号，暂停导航不推进物理 |

采用不透明点、深度测试和深度写入，遮挡语义与同目标中的几何一致；首期不是透明水渲染。
原生点按点中心的裁剪规则和点的深度绘制；圆形外观不代表球体，不声称有球面遮挡或近裁剪面球形截面。
CPU/GPU 的浮点布局与 padding 显式固定，避免直接假定 Eigen/宿主结构体内存可被 shader 正确解释。
数值健康失败不得被色标截断掩盖；显示入口验证数据身份、通道及有限性策略，错误保留旧帧并给出诊断。

## Graph、发布和资源寿命

Graph 声明上传/求解写入至 VertexShader/ShaderStorageRead 的依赖，以及颜色/深度附件读写。
若颜色由 compute 生成，同样声明其写后读；不假定位置写入与绘制必须紧邻，也不能遗漏中间访问状态。
跨图/跨提交导入携带真实状态与完成依赖；渲染器不得仅凭指针可用读取仍在写入的数据。

| 边界 | 对外约定 |
| --- | --- |
| 参数/能力校验 | 建图和提交前拒绝非法数量、尺寸、矩阵、点大小、通道、设备/代次；不改变求解器 |
| 候选帧 | 完成后才发布图像与实际 run/step/generation/view；旧图不会被候选覆盖 |
| GPU 共享输入 | 保活输入缓冲至读提交退休；下一求解步不得覆盖仍被读取的代次，可通过有界完成输出轮换或依赖解决 |
| 预览失败 | 返回显示错误并保留上一有效帧，不把图像失败改写为求解故障或假称图像属于新步数 |
| 销毁/交接 | 在途帧保活 shader/pipeline/buffer/image；ImageTransfer 持有至消费者 GPU 完成，最终按设备所属寿命释放 |

视图请求仅保留最新请求和最近完成图；持续相机输入不能使所有已完成帧失效而饿死显示。
结果携带实际相机与完成拍，UI 可显示落后图并标注更新中，不能改标为当前请求。
无预览订阅者不产生点渲染；运行时预览频率有界，暂停且视图/显示配置不变不重复绘制。
显式导出使用固定实验相机，同拍粒子/图像共享 provenance；普通预览不启用粒子或图像 readback。

## 验证计划

| 层次 | 必须覆盖 |
| --- | --- |
| Device/管线 | supported/enabled 差别、未启用时 1 px、非法大小拒绝、point-list 真实绘制、原 triangle 回归 |
| shader | 固定编译器的 PointSize/PointCoord 产物与实际方点/圆点，索引绘制和非索引绘制 |
| 显示 | 多种合法大小、遮挡/深度写入、圆外 discard、原生裁剪、透视/正交、DPI、空点集和缺失通道 |
| CPU/GPU 输入 | 同一静态数据两条入口图像对照；GPU compute 写入后 vertex 读取的真实同步 validation |
| 寿命/状态 | 暂停改视图不推进、旧图身份、完成代次不可覆盖、渲染器/求解器在途销毁、宿主和独立设备 |
| 交付 | 四种流体组合真实预览、显式图像导出、关闭/取消、现有布料预览与三角形链路回归 |

缺少 GPU 的环境单独记录 skip，不当作原生绘制通过。首期测量 draw 与上传/颜色派生成本及在途显存，
不把 GPU duration 等同窗口 FPS。CPU-only 只运行数据/数值验证，不要求图像生成。
实施阶段新增真实行为测试及选择表入口，本轮不创建空测试 target。

## 依据与相关记录

- [Vulkan 1.4 feature 要求](https://docs.vulkan.org/features/latest/features/proposals/VK_VERSION_1_4.html)、[功能启用](https://docs.vulkan.org/spec/latest/chapters/features.html)、[点栅格规则](https://docs.vulkan.org/spec/latest/chapters/primsrast.html)。
- [Slang SPIR-V 内建映射](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html)。
- [粒子流体设计](physics-fluids.md)、[原始计划](../research/2026-10-23-sph-pbf-plan.md)、[0147](../development/0147-particle-fluid-plan.md)。
