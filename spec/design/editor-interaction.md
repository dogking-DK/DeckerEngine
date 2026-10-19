---
module: editor-interaction
created_at: "2026-10-08T11:35:00+08:00"
updated_at: "2026-10-19T09:48:00+08:00"
status: accepted
---

# 编辑器交互：选择、Gizmo 与会话相机

## 模拟取景（M12.2）

模拟视图拥有独立Camera；首次按规则布片宽深、初始高度和地面范围自动取景，不读取GPU粒子。
Frame、F、Home重新取景；默认 Orbit 的右键环绕、中键平移、滚轮缩放沿用Camera限制，Fly/Walk 与编辑视口共用输入规则。
输入仅响应模拟Image或已经捕获的导航，文本输入/模态框/失焦/最小化取消导航。
图像视图序号用于识别相机矩阵和尺寸；持续拖动仍发布最近完成的视图，不能要求结果追上每帧新输入才显示。
序号/步数单调前进，落后于最新请求时明确标记更新中，停止输入后收敛。模拟图像不做实体选择或Gizmo，编辑拾取相机只用于Edit。
切回Edit保留原相机、选择和历史，Stop后自然恢复编辑操作。
跨入口验收用真实ImGui输入驱动模拟按钮，由外部IPC交替控制同一运行；仅在实际呈现后发布检查点，
验证暂停图像不漂移、外部编辑不会写入Play、Stop后Edit/撤销内容仍可访问，见[编辑器验收](editor.md)。

## 边界

CPU Camera 与变换手势进入 `dk_editor_model`；Geometry 提供独立射线/BVH。
UI 私有适配负责 ImGui 图像矩形、轴/环绘制、输入路由和 DiskScene 网格到 BVH 的转换。
沿用已有 ImGui/Eigen，不新增三方库。Gizmo 首版提供本地轴平移、旋转、缩放，
使用原始 TRS 及父级仿射变换计算，不对含 shear 的世界矩阵强行 TRS 分解。

## 选择与相机

鼠标坐标相对实际 Image 矩形换算为 [0,1]；投影复用 Vulkan Y 翻转、Z [0,1] 约定。
左键最近三角形命中选择 EntityId，空白清空；Hierarchy 与 Inspector 共用同一选择。
拾取使用成功发布图像对应的 Scene/CPU 网格/相机，过期或失败预览禁止交互。
实例世界变换包含父级和资产导入后的网格坐标；资产 BVH 按会话复用。
选中实体用包围盒和轴标记；没有网格的实体仍可从 Hierarchy 选择。

编辑器相机只属于 UI 会话，不写 Scene Camera、revision、dirty 或历史。
相机提供 Orbit、Fly、固定高度 Walk 与可调镜头/导航偏好，实施契约见下文。
右键拖动环绕、滚轮缩放、中键平移；F 聚焦选择、Home/Reset 恢复默认相机。
距离和俯仰限制避免奇点；尺寸/相机改变使视口失效，绘制与拾取共享相机矩阵。
输入仅在图像区域或已捕获的拖动中处理；文本/模态框不触发快捷键。

## 手势状态与提交

Workspace::begin_transform 返回由 UI 拥有的独立 TransformEdit 候选：捕获 EntityId、document_id/revision、原始本地 TRS 和父世界变换。
Inspector 有未应用草稿时不能开始手势；手势期间禁止 Inspector/选择/历史/保存/换场景。
UI 在手势期间禁用其他编辑入口；Workspace 提交仍复查草稿/选择/guard，其他调用者不依赖 UI 禁用保证。
移动只更新候选和预览序号；RenderScene 提取支持单实体只读 local override，后代也继承预览变换。
这不修改 Runtime 文档、revision、dirty 或历史；UI 明示 Transform preview。
松开一次提交已有 entity.set_transform（guard），产生一个撤销单元；原值无变化不发命令。
Esc、焦点丢失、最小化、图像矩形变化、关闭请求、图像不可用取消候选；原生事件在暂停 NewFrame 前取消，避免恢复时误提交。
失败提交清理候选、保留原文档并显示错误。
过期 guard 不覆盖后来编辑。父级不可逆、非有限/非法 TRS 在开始或更新时拒绝。
旋转在本地轴后乘四元数，缩放只改变该轴数值；平移通过父级逆变换转换世界位移。
平移/缩放使用投影轴屏幕位移，旋转使用射线/局部环平面交点；端视不足 15px 的平移/缩放轴隐藏，环绕后可操作。

## 验证与范围

CPU 验证投影/射线对应、相机奇点/无文档副作用、父级缩放旋转、取消/无变化、
过期提交、单次历史、撤销重做、保存重载；Geometry 单独测试。
真实窗口 smoke 注入拾取、三种 Gizmo、取消与相机输入，检查 preview/commit 像素与 revision，
最后保存、重载和 runner 读取同一状态；保留 M8.1 smoke。
默认 Sponza 做实际窗口预览，GPU validation 必须零告警/错误；CPU-only 模型独立构建。
M8.3 IPC、纹理 alpha 精确拾取、多选、世界轴 Gizmo、吸附和飞行相机不在原 M8.2 阶段。

关联：[工作台](editor.md)、[几何查询](geometry-query.md)、[Render 数据](render-data.md)、[0066](../development/0066-editor-interaction.md)。

## M18 相机扩展计划

本节以 2026-10-16 的源码检查为起点；阶段状态与执行顺序由 [Roadmap M18](../roadmap.md#m18相机操作模式与投影扩展) 维护。
M18.1 已实现镜头与导航设置；M18.2 接入 Fly/Walk，正交仍为规划。历史检查结果见下表，实际变更和验证见 [0129](../development/0129-camera-settings.md)。
首期 Walk 按水平漫游设计；没有地面碰撞、重力、台阶或跳跃，不作为角色行走模拟。

### 扩展前能力与缺口（M18 检查基线）

| 范围 | 源码检查结果 | 扩展影响 |
| --- | --- | --- |
| 编辑相机 | [Camera](../../engine/editor/src/Camera.cpp) 仅有 target/yaw/pitch/distance；右键 Orbit、中键 Pan、滚轮 Dolly、F 聚焦与 Home 重置 | 没有导航模式、键盘连续移动、移速或可调镜头 |
| 投影与尺度 | FOV 固定 65°，投影、ray、pan、frame、units_per_pixel 共用硬编码 tan(32.5°)；far=10000，near 通常 0.05、环境标志启用时为 1 | FOV 必须同时进入所有投影/交互计算，不能只改矩阵 |
| 输入 | [ViewportInput](../../engine/editor/src/ViewportInput.cpp) 与 [SimulationViewUi](../../engine/editor/src/SimulationViewUi.cpp) 分别实现导航 | 抽出共用控制器；编辑和模拟仍持有独立相机状态 |
| 基础光栅/模拟 | [RenderView](../../engine/render/data/src/RenderScene.cpp) 接收有限可逆投影；[基础管线](../../engine/render/pipeline/src/PipelinePolicy.hpp) 和 [模拟视口](../../engine/editor/src/SimulationViewport.cpp) 消费矩阵 | 有接入正交的基础，但没有完整正交编辑器功能或专项验收 |
| 路径追踪 | [CPU 相机](../../engine/render/pathtracing/src/PathTraceReference.cpp) 明确验证透视形式；[GPU 参数](../../engine/render/pathtracing/src/PathTraceRenderer.cpp) 和 [主射线](../../engine/render/pathtracing/shaders/pathtrace.slang) 使用共用眼点 | 正交须同时改 CPU/GPU 主射线、验证和历史；Scene media 也受影响 |
| 光栅大气/云 | [PipelinePolicy](../../engine/render/pipeline/src/PipelinePolicy.hpp) 明确拒绝正交；[合成](../../engine/render/pipeline/src/ScenePipeline.cpp) 使用透视深度、眼点锚定的天空/AP | 不能只放开校验或替换矩阵；需要单独的正交射线/深度/介质方案 |
| 固定截图 | [CaptureCamera](../../engine/framework/services/include/dk/services/CaptureService.hpp) 和 [render.capture](../commands/render.md) 已能设置透视 FOV/near/far；不支持正交 | 截图的可调 FOV 不等于交互视口已有设置；新增正交需兼容旧请求 |
| 一致性驱动 | [ConsistencyDriver](../../engine/editor/src/ConsistencyDriver.cpp)、[PathSmoke](../../engine/editor/src/PathSmoke.cpp) 输出相机时写死 fov_y=65 | 接入真实镜头参数和已发布相机，避免比较了不同输入 |

检查结论：用户界面不支持正交；底层矩阵容器可承载它，路径追踪和环境合成仍明确限制为透视。
已有 [相机测试](../../tests/unit/EditorTests.cpp)、[路径相机拒绝测试](../../tests/unit/PathTraceReferenceTests.cpp)、
[大气投影拒绝测试](../../tests/unit/RenderPipelineTests.cpp) 是扩展回归入口；最初设计检查没有运行这些测试或启动窗口。

### M18.1 实施契约

CameraSettings 分为 CameraLensSettings 与 CameraNavigationSettings；姿态以 eye/yaw/pitch/focus_distance 表示，
pivot 从姿态推导，不存两套眼点。set_settings 先验证完整候选与当前 aspect/光栅环境支持域，再整体发布；
同值不改变 revision，纯导航设置不改变渲染 revision。Home/reset 仅恢复姿态，Restore defaults 恢复姿态与设置。
光栅空气或云启用时 Auto near=1，基础光栅、模拟与路径使用 near=0.05，far=10000；
手动参数不随渲染上下文改变，外部环境/后端变化或 resize 不适用时保留旧图并阻止新提交，面板仍可恢复。
Camera::validate 校验实际视图；渲染入口再次执行，不依赖 UI 唯一保护。现有路径主射线支持域仍由路径模块校验。
frame 计算所有包围盒角点的投影/近远裁剪约束，若无法在相机距离范围 0.1–3000 内完整框选，返回错误并保留姿态。
设置面板由 Edit/Simulation 共用，草稿 Apply 成功才发布，错误保留草稿以便修正，Cancel 丢弃；Gizmo 活动时不能打开。
移速/加速/观察灵敏度作为会话偏好可修改，由 M18.2 Fly/Walk 消费；Orbit/平移/滚轮沿用独立参数。
本节不实现 Fly/Walk、正交、相对鼠标或命令 schema 扩展；Capture 只替换内部共享投影计算，保持既有参数契约。

### M18.2 实施契约

Camera 保存 NavigationMode；CameraNavigation 为不依赖 SDL/ImGui 的连续输入模型，NavigationUi 为两个视口共用的适配器。
模式提示保持相同两行高度，切换不能间接 resize 或清空路径累积。
模式切换只改会话控制状态，不改姿态/镜头或渲染 revision；Walk 每次水平位移保持当前 eye.y，只有显式 Frame/Home 可以改变该高度。
输入模型整体计算候选后提交，拒绝非有限输入；组合移动归一化、Ctrl 优先、dt 上限 0.05 秒，开始捕获的首拍不移动。
UI 以 steady_clock 计时，相对鼠标仅消费同窗口 SDL motion.xrel/yrel；这些值已经是 SDL 窗口坐标单位，不再乘 framebuffer DPI。
一个主线程 RelativeMouse owner 借用 SDL 窗口，捕获成功才启动模型；结束时清空增量、恢复原鼠标位置并关闭相对模式。
只将相对 motion 从 ImGui 转发中滤除，按键/按钮/滚轮仍交给 ImGui；因此不会同时消费两种鼠标增量。
退出/隐藏/最小化/失焦/关闭及原生 resize/DPI 变化在事件或帧尾清理；每帧只有实际绘制 Image 的适配器能续用捕获。
相对导航不依赖旧 Image 矩形，允许介质路径高低分辨率图像切换带来的显示比例舍入变化；Orbit/Gizmo 的屏幕拖动仍按矩形变化取消。
真实后台窗口验收区分 SDL 相对模式开关/释放与注入键鼠，不主动获取 OS 前台焦点；持续移动检查实际完成图像版本。
Surface only 路径连续相机输入须等待当前图像发布再提交最新姿态；暂停或失败不能阻塞恢复，物理/尺寸变化立即失效。

### 功能与输入约定

导航枚举 `NavigationMode { orbit, fly, walk }` 与投影枚举 `ProjectionMode { perspective, orthographic }` 分开。
导航定义眼点如何移动，投影定义画面如何形成；渲染后端 Raster/Path trace 是第三个独立选择。
首期 Fly/Walk 只开放透视，正交采用 Orbit/Pan，避免把正交前移误称为缩放。

| 模式 | 旋转与平移 | 滚轮与快捷键 |
| --- | --- | --- |
| Orbit | 保留右键围绕 pivot 旋转、中键沿视平面平移 | 透视滚轮改距离；正交滚轮改可见高度；F 聚焦，Home 重置视角 |
| Fly | 按住右键进入鼠标观察；W/S 沿观察方向，A/D 沿相机右轴，Q/E 沿世界 -Y/+Y；保持无 roll | 捕获时滚轮按指数调基础移速；Shift 加速、Ctrl 精细移动；松开右键立即停止 |
| Walk | 按住右键观察；W/S 使用 yaw 对应的水平前向，A/D 水平侧移；保持进入模式时的世界 Y 高度；Q/E 不移动 | 同 Fly 调速；俯仰改变观察方向但不改变行进高度 |

Fly/Walk 未捕获时滚轮不移动相机，避免意外穿行；模式提示显示当前按键与速度。
F 在三种模式中均显式移动眼点以框住选择，并更新 pivot/focus_distance；Walk 同时更新固定高度。
Home 恢复当前视口的默认姿态并停止导航，保留用户设置；单独的 Restore camera defaults 恢复模式、镜头和导航设置。
模拟视口 F/Home 继续按规则布片和地面范围取景，保留其独立镜头/导航偏好；新 run 重新取景，Stop 不覆盖 Edit 相机。

### 参数、面板与范围

Viewport 工具栏提供模式、投影和 Camera 设置入口；Camera 弹层显示当前编辑/模拟视口的设置。
姿态与偏好首期均为本次应用会话状态，不写 Scene Camera、项目文件、revision、dirty 或 Undo。
跨启动偏好保存、相机书签和实体 Camera 组件另行设计，本阶段不新增配置文件格式。

| 设置 | 初始值/建议操作范围 | 语义 |
| --- | --- | --- |
| 垂直 FOV | 65°；滑条 10–120°，数值输入要求 1° < 值 < 179° 且通过当前后端校验 | 透视专用；保持姿态和 pivot，改变镜头；正交时禁用并保留该值 |
| 基础移速 | 3 m/s；0.01–1000 m/s | Fly/Walk 的世界空间速度，与帧率和 FOV 无关 |
| 加速/精细倍率 | 4 / 0.25；范围分别 1–100 / 0.01–1 | 同时按下时精细优先；释放修饰键恢复基础速度 |
| 观察灵敏度 | 0.003 rad/逻辑像素；0.0001–0.05 | Fly/Walk 鼠标观察，可切换反转 Y |
| Orbit 灵敏度 | 0.006 rad/逻辑像素；0.0001–0.05 | 默认保持既有拖动手感 |
| 平移倍率/滚轮系数 | 1 / 0.15；0.01–10 / 0.01–1 | 平移乘相机实际屏幕尺度；滚轮采用指数变化并限制结果 |
| 正交可见高度 | 首次由透视焦平面换算；建议 0.01–100000 m | 表示整个视口的世界空间高度，宽度=高度×aspect；后端支持域另校验 |
| 裁剪 | Auto 默认开启；普通 near=0.05、环境 near=1、far=10000 | 高级设置允许 Manual near/far；显示实际生效值及后端约束 |

所有输入须有限，速度/尺寸为正、far>near>0；手动非法输入显示错误并保留最后有效相机，不静默夹取数值输入。
鼠标连续缩放/调速允许在已公布范围内饱和，饱和无变化不递增视图版本。
Auto 应由实际渲染路径决定裁剪，覆盖 cloud-only 情况，而非只检查空气 enabled。
光栅环境现有限制仍保留：far≤1e6、far/near≤10000、投影 |p00|/|p11|≥0.1；
因此宽高比也参与 FOV 校验，不能宣称 1–179° 在任意环境视口都可用。resize 使设置不再适用时停止提交、
保留旧图并显示原因，允许调参/恢复 Auto；不能污染场景设置或悄悄更改 FOV。
后端/环境切换和外部 IPC 修改也触发能力复查；UI 禁用不是唯一校验层。

### 数据模型、共享计算与切换

`dk_editor_model` 维护 Camera 的姿态/镜头值与不依赖 ImGui/SDL 的导航控制器；UI 将每帧输入转为
`NavigationInput`（观察增量、移动轴、滚轮、修饰键、有效 dt），Edit/Simulation 共用同一更新规则。
模型建议以 eye + yaw/pitch 为唯一姿态，pivot/focus_distance 用于 Orbit 和取景；不维护两套可漂移的眼点。
纯投影/反投影计算适合提取到 [Foundation Math](foundation-math.md)，供 Editor 与 Capture/Render 复用，
无 Scene/SDL/Vulkan 类型、无新三方依赖；`geometry::Ray` 的包装留在 Geometry/Editor，避免 Math 反向依赖 Geometry。
实施 M18.1 时先更新 Math 的现有范围设计和显式 target 依赖；RenderView 继续是不可变矩阵输入，不拥有导航状态。

导航模式切换保持世界眼点、观察方向和镜头不变；Orbit→Fly/Walk 从当前姿态开始，进入 Walk 记录当前 Y。
Fly/Walk→Orbit 令 pivot=eye+forward×有效 focus_distance；使用最近聚焦距离，否则使用保留的有效距离，避免跳回旧 target。
自由导航俯仰限制保持约 ±1.55 rad、yaw 有界环绕；正交六视图采用固定姿态，允许顶/底视精确到 ±π/2，
使用预定义稳定基向量，不能依赖退化的 world-up 叉积。右键拖动退出固定视向并进入受限自由 Orbit，UI 同步取消视向高亮。
切换导航但姿态/镜头不变时只改控制状态，不清空路径累积。

透视→正交以 pivot 焦平面保持尺度：`ortho_height = 2 * focus_distance * tan(fov_y / 2)`。
正交→透视沿当前观察轴调整眼点距离为 `ortho_height / (2 * tan(fov_y / 2))`，保持 pivot 和该平面构图；
它可能改变眼点位置，应与保持姿态的导航切换区分。候选越界时拒绝切换，不能用截断破坏构图承诺。
正交首期只允许 Orbit；从 Fly/Walk 切入时 UI 明示“切换至正交 Orbit”，一次提交两个状态，并记住透视导航模式供返回。
增设 Front/Back/Left/Right/Top/Bottom 视向按钮，明确世界 +Y 向上与屏幕朝向，不另引入命名为“正交”的导航控制器。

正交 `ray` 每像素使用不同近裁剪平面起点、相同 forward，区间为 [0, far-near]；
project 与 ray 遵循 Vulkan Y 翻转/Z [0,1]，拾取限制仍与真实裁剪面一致。
正交 `units_per_pixel=ortho_height/image_height`，与物体深度无关；pan 和 Gizmo 使用该尺度。
frame 按相机基向量下的包围盒水平/垂直范围和 aspect 决定高度，同时确保深度裁剪覆盖；
透视 frame 使用实际 FOV。不能仅使包围盒中心位于视口而宣称已完整框选。

### 输入寿命与失败保护

相机与控制器由 GUI owner 串行修改；worker 只读取拥有型相机快照。输入更新先计算、校验候选，再一次提交。
非法参数、切换不支持组合或非有限计算失败保留旧姿态/设置；设置错误显示在面板，不能留下半个模式切换。
移动使用归一化后的组合方向×有效速度×dt，避免斜向加速；鼠标增量不再乘 dt。
dt 取单调时钟秒数，单帧最大 0.05 秒；切入、恢复与取消时重置时钟，不追补后台或卡顿时间；首期不引入惯性。
Walk 水平前向从 yaw 计算，仰视/俯视时不会因投影归一化而出现方向奇点。

右键只能在实际 Image 内、窗口可输入且没有文本/弹层/Gizmo 活动时开始捕获。键盘移动仅属于已捕获视口，
不能在 Console/Inspector 输入 WASD 时移动。Fly/Walk 使用 UI 私有的相对鼠标适配，防止到达屏幕边缘后停止转向；
复用 [Platform 借用窗口入口](platform.md)，鼠标相对模式/游标恢复由一个主线程 owner 管理。
启用相对模式失败时不进入导航；不能并行消费 SDL 原始增量与 ImGui 增量导致双倍旋转，DPI 换算只做一次。
松开右键、Esc、失焦、最小化、隐藏视口、切视图/模式、打开设置/文本输入/弹层、关闭请求均取消捕获并清空移动轴。
原生失焦/最小化在跳过 NewFrame 前释放鼠标；关闭失败或返回应用时不自动恢复上一段移动。
Gizmo 期间禁用相机/镜头修改，导航期间禁止拾取和 Gizmo；恢复交互要求画面与当前相机一致。

姿态、有效投影/FOV/裁剪/正交高度变化才递增渲染视图 revision；速度/灵敏度/模式等纯偏好变化不使画面失效。
同值设置和零输入不递增。路径累积/云历史按视图变化重置，几何与资产/AS 不因此重建。
保留 [0086](../development/0086-simulation-camera-progress.md)、[0122](../development/0122-editor-render-performance.md)、
[0127](../development/0127-editor-media-pathtracing.md) 的最近完整帧发布规则：连续移动不能饿死显示；
旧图保留真实相机、尺寸、序号与样本来源，并标记更新中；停下后收敛，旧图不能用新镜头拾取。
相机进入地下或超出现有介质支持域时仍可导航、Home 或改设置恢复，不把 Walk 当作阻止地下相机的碰撞系统。

### 正交支持分层与截图兼容

M18.3 首先验收基础光栅、模拟、拾取/Gizmo 与正交取景。编辑器增加显式的会话级 Unlit inspection 视图选项，
让带环境的场景也能检查正交几何；该选项选择基础光栅入口，不改 Scene.environment，面板明确显示未渲染环境。
切换正交前必须选择支持它的后端/显示组合；不自动关环境或悄悄切换渲染器。
若外部修改使当前组合不再支持，保留最后完成图并说明恢复方式。Path trace 和光栅环境正交在对应阶段完成前保持禁用。
模拟不使用场景空气/云合成，仍需独立验证矩阵、裁剪和发布序号，不能由 Edit 的成功推定。

M18.4 扩展路径 CPU/GPU（Surface only 和 Scene media）的正交主射线；平行方向与像素相关起点必须共同进入 GPU 参数。
正交主射线统一从近裁剪平面开始，近裁剪面之前的几何/介质不计入图像；此约定写入捕获来源并由 CPU/GPU 同时验证。
像素抖动移动正交起点，不能修改平行方向；介质从每条射线的起点计算行星坐标/云采样，不复用透视共用眼点的径向/相位量。
CPU/GPU 共用相机语义，保留主射线裁剪与反弹射线裁剪分离；比较解析几何、不同深度等尺寸物体、CPU/GPU及历史重置。

同阶段规划 `render.capture` / `render.capture_sequence` 的 camera 增加可选 `projection` 与 `ortho_height`：
缺省仍为 perspective，旧 eye/target/up/fov_y/near/far 请求及默认 far=100 保持；orthographic 必须给正高度。
正交分支拒绝显式 fov_y，透视分支拒绝 ortho_height，避免无效设置被当作生效；schema 支持字段发现，跨字段规则由服务校验。
provenance 表达实际生效的投影字段；同步 [RenderOperations](../../engine/framework/operations/src/RenderOperations.cpp) 的请求和结果 schema、
[CaptureService](../../engine/framework/services/src/CaptureService.cpp)、[捕获设计](render-capture.md)、命令参考、序列逐样本相机及 Python/Luau 相关示例。
保持 effect=external、guard、任务/取消和原子输出约定，失败保留旧文件；导航模式/速度不属于离屏截图参数。
M18.1 就消除一致性驱动中的固定 65°，无需等正交 schema 才保证可调透视 FOV 的 GUI/capture 一致。
首期不增加控制 UI 相机的 Runtime 命令；需要该能力时先设计 Editor owner 服务/能力发现，不能让无窗口 runner 持有虚构相机。

M18.5 为独立的光栅环境正交扩展：先更新 [管线设计](render-pipeline.md)、[大气](render-atmosphere.md) 与 [云](render-clouds.md)，
建立逐像素起点/平行方向和正交线性深度的 CPU 参考，明确从哪里开始介质积分、天空锚点近似适用域，
再修改天空查询、AP 体积/深度重建、云射线/保边上采样/时域重投影与云影接入。
仅放宽 `perspective_depth` 校验不构成支持；若暂不能保证误差，保持显式不支持，不以共用天空眼点伪装全正交环境。
M18.1–4 可先交付可用相机和有限正交；只有此阶段验收后才宣称光栅环境支持正交。

### 定向验收计划

以下覆盖整个 M18 的验收要求；M18.1 项目见 [0129](../development/0129-camera-settings.md)，M18.2 见 [0130](../development/0130-camera-fly-walk.md)；正交项尚未运行。不默认执行全量或双配置。

| 范围 | 关键可观察断言 | 现有验证入口 |
| --- | --- | --- |
| 镜头与控制器 CPU | FOV 改变仍 project/ray 一致；frame 完整框选；30/60/144Hz 相同总时间移动近似相同；斜向不加速；Walk 高度不变；模式切换不跳位；非法/同值状态保护 | [EditorTests](../../tests/unit/EditorTests.cpp)，dk_editor_tests；新增纯数学部分用 dk_math_tests |
| 输入与会话 | 真实键鼠模式/调参、松键停机、失焦/文本/模态/最小化/关闭释放鼠标；两视口独立；无 Scene/Undo 副作用；持续运动显示更新 | [交互验收](../../tests/integration/EditorInteractionTest.ps1)、[模拟驱动](../../engine/editor/src/SimulationSmoke.cpp)，dk_editor_app / dk_run |
| 正交编辑 | 同尺寸物体在不同深度像素尺寸一致；不同像素平行射线；近远裁剪、宽高比、六视图、平移/缩放、拾取/Gizmo 对齐 | dk_editor_tests、dk_render_data_tests、dk_render_pipeline_tests 与真实后台窗口 |
| 渲染和截图 | 透视 FOV 的 GUI/capture 匹配；正交 CPU/GPU 主射线与线性图像一致；切投影重置而改移速不重置；旧请求兼容、非法组合拒绝且输出不变 | [PathTraceReferenceTests](../../tests/unit/PathTraceReferenceTests.cpp)、[PathTraceProbe](../../tests/integration/PathTraceProbe.cpp)、[EditorPathTraceTest](../../tests/integration/EditorPathTraceTest.py)、capture 协议/集成测试 |
| 光栅环境正交 | 逐像素起点和真实米制光程；天空/地平线/几何遮挡/云内外、历史重投影、时域反遮挡、失败旧图保护 | 对应大气/云 CPU 参考与 required GPU 用例，在 M18.5 设计中细化 |

实现时按 [测试选择表](../../.agents/skills/decker-build-verify/references/test-selection.md) 选实际受影响用例并补足缺失断言；
CPU 控制器保持可在不启用 GUI/GPU 的配置中构建。窗口自动化一律 `--background`，不能为了捕获测试抢用户焦点；
相对鼠标的 OS 捕获与输入注入分开验证，不能只测试直接调用 Camera 方法。
GPU 正确性使用 required validation；持续导航分别记录 GUI 呈现与实际新视图发布，停机后核对最终镜头/序号，不能用 GUI FPS 代替画面更新。
