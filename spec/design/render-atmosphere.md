---
module: render-atmosphere
created_at: "2026-10-12T14:38:00+08:00"
updated_at: "2026-10-12T14:48:32+08:00"
status: draft
---

# 大气渲染设计草案

## 目标与当前基线

本次交付是M14/M15的开发任务计划，不是功能实现。阶段、顺序与验收状态统一维护在
[Roadmap](../roadmap.md)；本设计确定大气模块的边界和必须满足的约定，各小阶段开工时再细化具体接口。

默认面向Windows x64、Vulkan/Slang、地球尺度的地表至高空实时观察；支持昼间、日出/日落、
行星遮光造成的暗侧和空气透视，供编辑器与离屏截图使用。采用单太阳、球对称大气、线性RGB近似。
首轮验收相机高度为地表上方1m至100km；大气顶外的稳定处理需要测试，但轨道级行星地形不是交付要求。
不含月亮/星空、天文历法、天气流体模拟、完整光谱渲染、地形系统或完整PBR/IBL。
接入普通ScenePipeline场景视口与render.capture；独立RenderSimulation布料视口不在本轮范围。

代码基线为744e737：

| 已核实的现状 | 本轮需要补齐的能力 |
| --- | --- |
| [ScenePipeline](../../engine/render/pipeline/include/dk/render/ScenePipeline.hpp)只提供clear/exposure，depth→unlit opaque→tone→readback | 环境输入、太阳漫反射、天空/空气透视合成，保持原unlit路径 |
| [资源描述](../../engine/graphics/device/include/dk/graphics/Resources.hpp)只有width/height；创建/视图限定2D及2D array | 原生3D image/view、体积采样及传输范围校验，不能把array_layers当连续z坐标 |
| [深度目标](../../engine/render/pipeline/src/ScenePipeline.cpp)是D32附件，没有sampled usage | 深度只读采样、线性距离/世界射线重建及Graph屏障 |
| [RenderView](render-data.md)包含双精度相机/投影，但没有持久环境或ViewId | 场景环境快照；稳定视图身份与显式渲染时间约定 |
| [截图](render-capture.md)每个作业独立worker/device、输出PPM | 环境版本快照、无历史参考模式及线性HDR测试读回 |
| [M13实验格式](../../sdk/python/decker/experiments.py)绑定XPBD/cloth | 复用SDK调用及追溯原则，另设有版本的渲染用例清单；不直接塞入布料计划 |

## 技术路线与依据

大气采用Hillaire 2020路线：透射、多次散射近似、视点天空和空气透视分层计算，支持参数更新与不同质量档。
作者提供了论文配套实现及参考比较入口；这里只选择算法方向，不引入其DX11平台或整套代码。
依据：[作者项目](https://github.com/sebh/UnrealEngineSkyAtmosphere)、
[作者论文索引](https://sebh.github.io/publications/)。
Sky View只用于天空，Aerial Perspective保存视锥中的散射和透射；分开处理可避免对天空重复加空气散射。
相关说明见[Epic官方Sky Atmosphere文档](https://dev.epicgames.com/documentation/en-us/unreal-engine/sky-atmosphere-component-in-unreal-engine)。

保留不依赖低分辨率LUT插值的高采样参考路径，用于量化近似误差；CPU双精度积分验证单位、透射和单次散射。
Bruneton实现的量纲检查、CPU参考及自动测试作为验证方法参考，不把两种多次散射近似强行要求逐像素相等。
依据：[Bruneton作者文档](https://ebruneton.github.io/precomputed_atmospheric_scattering/)。

## 模块边界与拟议接口

以下名称为计划，尚无对应新target或能力开关。

| 位置 | 职责和依赖 |
| --- | --- |
| 现有Scene/Services/Operations | CPU环境值类型、校验、持久化、guard/事务/撤销；不依赖Render或Vulkan |
| 现有render/data | 从SceneSnapshot提取只读环境；相机、尺寸、ViewId和显式时间为逐帧输入 |
| 拟新增engine/render/atmosphere，dk_render_atmosphere / dk::render_atmosphere | 大气参数到LUT/天空/区间散射，依赖render_data、graphics_graph、graphics_shaders、Memory/Profiling；不依赖Framework/Editor |
| 现有render/pipeline | 编排大气、几何与tone；同图传递资源，Render模块不调用裸Vulkan |
| 现有render services / editor | 装配、截图任务及界面；通过统一命令改环境，不能持有可变GPU缓存作为场景状态 |

拟用DK_BUILD_RENDER_ATMOSPHERE隔离GPU模块，默认OFF；启用时明确检查target依赖。
CPU-only仍能读取、保存和修改环境描述，渲染能力未编译/设备不支持时明确报告不可用。
不新增三方依赖；若实际移植外部代码/素材，另行记录版本、许可和来源，不能将参考链接当作已引入依赖。

## 数据、坐标与持久状态

- Scene保存一个场景级EnvironmentDescription，缺省disabled，旧工程视觉保持原unlit背景。
  拟包含AtmosphereParameters、SunParameters及后续CloudParameters；编辑完整候选后一次提交。
  云字段到M15.1才定义版本和校验，不预先保存无解释占位数据。
- 所有公开距离/高度用米、时间用显式纳秒或秒并标单位；散射/消光系数用m^-1，光学厚度无量纲。
  Rayleigh/Mie密度与吸收层、地面半径/大气顶、地表反照率和相函数参数有有限值与范围校验。
  RGB太阳能量与已有线性HDR材质采用显式共同标度，曝光只在tone执行一次；M14.1冻结转换及测试向量。
- 使用现有Y-up局部场景坐标，CPU双精度保存行星中心/地表锚点与相机。
  GPU接收相机相对坐标及已分离的高度/半径；内部改用千米或归一化时同时转换系数。
  禁止直接用两个地球尺度float位置相减求米级高度，亦不把曲率模型等同于现有平面场景地形。
- sun_direction统一为“采样点指向太阳”的归一化方向；积分中入射/出射方向与相函数cos符号须明确。
  太阳角半径有限且非零；行星遮挡参与光程，地平线/切线求交不使用任意距离钳制掩盖NaN。
- 显式升级场景格式承载环境，旧版本读入补disabled，未知版本/字段拒绝而不静默丢失。
  持久化、SceneSnapshot::same_content/logical_bytes、restore、事务、undo/redo和源身份必须一并覆盖。
  参数无变化不推进revision；无效参数/guard冲突/分配失败保持Scene、dirty和历史不变。
- 拟议environment.query / environment.set复用Commands→Operations→Services，名称/schema在M14.1定案并同步命令参考。
  ViewId、quality、exposure、time、frame序号属于渲染请求，动画时钟不逐帧修改场景revision；
  自动化固定时间和seed，不能读取不受控wall clock作为内容输入。

## LUT、管线与最小光照

初始资源预算候选如下，尺寸和精度必须由参考误差/硬件能力共同验证，不作为既有格式支持声明：

| 资源 | 初始候选 | 失效输入 |
| --- | --- | --- |
| Transmittance | 256×64，RGB透射，候选RGBA16F | 大气物理参数、行星半径/高度、算法/精度版本 |
| Multiple Scattering | 32×32，候选RGBA16F | 前项、地面反照率；选择太阳能量是否归一化后完整纳入key |
| Sky View | 192×108，候选RGBA16F | 物理代次、观测高度、太阳方向/能量、该实现使用的视图映射 |
| Aerial Perspective | 32×32×32，两张候选RGBA16F保存RGB L与RGB T | 前项、相机/投影、积分距离、质量档 |

不能把RGB透射无说明压成alpha单值。设备不支持所需storage/sampling/filter格式时明确拒绝，
或选择提前声明且独立验证的RGBA32F/手动插值路径；不偷偷降级。3D资源的mip、copy footprint、
descriptor反射、usage/范围和Graph导入导出先在M14.2验证；初版不扩成通用RHI或异步compute。

组合路径：更新需要的LUT → 深度预通道 → 不透明/alpha-mask几何 → 天空与空气透视 → 原tone → 呈现/按需读回。
depth=1背景按天空射线处理，几何按重建的实际距离应用`L_out = L_air + T_air * L_surface`。
云接入后由[统一区间合成](render-clouds.md)负责介质顺序，不能在同一路径重复应用整段空气透视。

为了让大气日光和后续云影真正作用到物体，M14.4新增可选最小Lambert太阳直射及天空漫反射项，
验证法线逆转置、剪切/负缩放、双面约定和无效法线。保持unlit材质/旧预览可选且回归图像不变。
这不是完整PBR：不含镜面IBL、多灯、几何阴影映射、GI或透明物体体积接收。

## 缓存、提交点与失败

renderer实例拥有大气缓存；物理LUT可在同设备/队列内共享，视图LUT按ViewId隔离。
key必须覆盖实际被shader读取的参数、格式/尺寸和shader版本；曝光不改变物理LUT，
太阳移动不应无条件重建与其无关的透射表。不能凭scene revision一次性清空所有共享表。

参数解析、预算预约、资源/图候选和返回元数据先准备；Graph execute成功只表示提交，
对应Submission完成后才发布可用代次。异步新代失败或过期不覆盖旧代；已有Frame继续拥有自身资源。
新请求若不能满足其版本，返回明确未就绪/失败，允许界面继续显示带旧版本标识的上一帧，不伪称已更新。
不原地改写仍被在途帧读取的LUT；占用达到约定上限时采用有界排队/拒绝，不能无限积累旧代。
resize、Scene切换、设备重建和关闭有明确失效/回收路径；device_lost遵循现有队列失效契约。

截图冻结环境/Scene revision、相机、时间、质量和shader摘要，默认无跨请求历史；
GPU失败、取消、IO发布失败沿用现有capture保留旧文件的约定。CPU-only保存场景不创建GPU资源。

## 验证和实施入口

按Roadmap M14.1–M14.7推进，默认下一项M14.1；计划本身不注册虚构测试入口。
验证分为：CPU量纲/积分/持久化，GPU格式与同步，大气参考图，GUI/命令/保存重载一致性，最后性能采样。
真实用例包含海平面/高空、正午/低太阳/日落后、切线/背太阳/朝地面、不同near/far和曝光；
透射率应在[0,1]，零消光返回T=1/L=0，单段拆分合成与整段参考一致，所有输出有限。
后续自动化启动dk-editor使用`--background`，普通交互启动保持常规窗口行为。

建议首轮数值门槛：透射绝对误差≤2e-3；固定测试集的线性RGB归一化RMSE≤3%、p99归一化绝对误差≤8%。
归一化尺度使用参考图RGB亮度p95与固定1e-3下限的较大者，不用测试图自身数值；
太阳盘/地平线单列最大误差与边界连续性，不从总图平均中隐藏。多次散射近似偏差与离散/插值误差分别记录。
这些为待参考路径建立后冻结的验收候选值；不得以“看起来正确”替代量化或事后悄悄放宽。

性能候选目标为1080p质量档下大气增量GPU p95≤1.5ms，含每帧Sky/AP更新及合成，
不把仅物理参数变化时的LUT重建藏入稳态；冷编译/重建、CPU建图/提交、GPU区间、端到端和显存分别报告。
以实际选定设备、驱动、构建、validation/profiling开关记录基线后冻结，当前没有性能实测或达标承诺。

## 相关文档

[架构](architecture.md)、[管线](render-pipeline.md)、[Graph](graphics-graph.md)、
[资源](graphics-resources.md)、[Scene](scene.md)、[GPU观测](foundation-profiling.md)、
[体积云](render-clouds.md)、[0093规划记录](../development/0093-atmosphere-cloud-plan.md)。
