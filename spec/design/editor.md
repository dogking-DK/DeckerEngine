---
module: editor
created_at: "2026-10-08T09:36:00+08:00"
updated_at: "2026-10-16T11:29:51+08:00"
status: accepted
---

# M8.1 编辑器工作台

## M17 环境与路径的后续集成

[M17设计](render-pathtracing.md#m17-大气云与表面路径的统一传输)规划Scene media模式，显式使用场景空气/云，
与原Surface only分开；设置、输入版本与实际完成spp必须对应。环境编辑需细分几何与介质失效，避免无条件重建Sponza AS。
当前仅推进CPU介质参考，GUI仍沿M16要求surface-only确认。M17.5才接入真实GUI/Console/IPC/capture一致性，
M17.6再验收交互响应和固定质量性能；不能提前声称联合渲染可用。

## Sponza 交互性能与云预览

Release/RelWithDebInfo普通启动默认关闭Vulkan验证层，Debug保持if_available；显式--validation仍要求验证层，--no-validation明确关闭。
自动化正确性测试继续显式required validation，性能采集显式关闭，不将验证层的CPU开销计作普通使用成本。

环境 worker 的时域云先发布首个完整帧，再在输入不变时逐步发布至32次历史更新；每个结果携带实际更新次数。
仍只有一个活动请求、一个可替换请求与结果槽，序列失效立即停止后续提交；GUI一致性/截图验收等待最终32次更新。
相机变化清空旧历史，不能混入旧视角。失败保留最后图像，但仍允许相机导航恢复；拾取/变换继续要求有效且当前的画面。
4096像素参考模式仅供小视图，正常视口在面板禁用该选项并提供可用的Temporal模式，不自动修改物理云参数。
路径worker以在途GPU票据的有界等待推进，活动时不再逐块固定sleep；暂停/取消/请求检查保持在工作块之间，GUI线程不等待路径工作。
Sponza worker探针分别记录首次与稳态完整帧、云首帧/收敛帧、路径逐spp耗时；性能关闭validation，正确性另跑required validation。
这些计时包含worker提交、等待和环境读回，不等同GPU耗时或GUI帧率。具体证据见[0122](../development/0122-editor-render-performance.md)。

## M16.9 编辑器路径追踪接入契约

编辑场景视口提供Raster/Path trace显式切换；Path trace显示实际completed/target spp、阶段、暂停/继续、
Reset、seed、反弹、环境RGB、太阳和曝光EV。设置为会话状态，不写Scene/Undo；空气/云启用时要求显式surface-only确认。
构建未启用或设备RT未启用时显示原因，不退回光栅冒充路径图。默认目标64spp，batch=1，tile≤64。
窗口设备在启用模块的构建请求RT和纹理索引if_available，不切模式重建设备；windows-editor预设启用路径模块。

正常启动的默认Sponza也属于预览验收范围。路径视口显式使用与光栅相同的unlit_preview导入配置，
保留base color、mask、双面、金属度/粗糙度常量，面板说明不导入法线/AO/金属粗糙度纹理和切线。
路径场景允许100万三角形、4096 primitive、64张纹理和128MiB RGBA8源数据，GPU仍受1GiB实际分配额度约束；
CPU参考默认额度不变。失败不替换已显示图像；切换到大气时默认模型穿过地面的包围盒不应拒绝整帧。
场景重建先校验CPU候选，再在旧提交排空后退休旧路径history/GPU场景，避免两套大场景同时占据预算；
若新GPU准备失败，GUI仍保留旧显示图并标过期，Retry重新构建，不承诺恢复已退休的累积。
验收使用正常相机、真实窗口尺寸和默认工程副本，分别实际显示路径完成样本、大气及sparse带云图像，再返回路径模式。

PathPreview worker独占自己的SubmissionQueue、renderer、DiskScene和资产版本；GUI只提交拥有型快照、
相机/尺寸/手势与设置、领取已完成图像。最多一个待处理输入、一个进行中输入和一个待领取结果；输入合并，
长导入/编译使用stop_token，tile间收集新输入，过期generation/sequence不能覆盖新场景。
重配前以poll收完旧producer提交，避免连续拖动耗尽命令槽；reset计数在配置成功后才记为已应用。
场景会话或显式Refresh重载磁盘资产，revision/手势重建冻结RT场景，相机/尺寸只重置历史；
曝光/目标/暂停保留已完成线性历史；新请求取消未完成批次，不硬抢占已提交tile。隐藏/最小化/切Simulation停止后续批次；恢复可从已完成历史继续。
错误停止重试并报告原因，Retry/配置变化才重新尝试；旧图保留但标记来源与过期，不能继续旧图拾取。

worker与Presenter共享同一逻辑设备的原生队列0；Device为每个原生队列提供共享gate，
SubmissionQueue和Presenter的native submit/present/queue-idle入口持锁，独立队列ledger仍由各自线程串行访问。
ImGui动态字体更新含自己的queue submit/idle，在GuiRenderer中提前处理并持同一gate；常规draw录制不持锁。
这也是单队列设备回退，无需CPU图像回传或UI执行路径编译/AS build/完成等待。已有Simulation队列1保持独立。
同队列GPU执行按提交顺序串行，tile额度限制单次工作；不宣称GPU抢占或毫秒硬上限。

renderer增加GPU tone输出：已完成线性buffer经Reinhard与曝光写RGBA8 UNORM，sRGB交换链前保留线性，
非sRGB交换链前显式编码一次。显示image和参数继承同一allocation预算，GPU完成后export/import ImageTransfer，
GUI在前次GUI提交完成后替换texture descriptor，正常预览不读回全帧。外部图像释放可能跨线程，
AllocationBudget的计费和统计同步保护；queue资源ledger仍不跨线程共享。
worker退出先停止请求、取消并排空自己提交，GUI完成后释放显示图，最后销毁设备；异常关闭同样join。

验收通过真实--background窗口和ImGui输入切模式/暂停/重置/曝光/导航/手势，并以IPC修改和Reload同一场景，
检查输入版本与已显示图像一致、暂停计数稳定、曝光不重采样、旧结果隔离、resize/最小化/活动关闭、
共享单队列同步validation及零存活资源。只在验收模式显式读回已显示图像作签名与CPU/GPU参考核验。
M16.10固定capture与GUI共享积分/显示，验收模式显式读回完成线性帧进行比较；正常GUI不读回。M16.11性能基线单独推进。

## 窗口启动方式

WorkbenchOptions.background及CLI `--background`为Windows可选后台显示：创建和最小化恢复时不抢焦点，放在其他窗口后方。
普通启动默认false，仍自动显示；参数只作用本次进程，不改项目或持久设置。窗口保持可聚焦，用户可主动切换进去。
窗口政策由[Platform](platform.md)负责；所有编辑器自动化入口、IPC夹具和GUI响应基准显式传入该参数。
测试仍走真实交换链、ImGui注入、截图和生命周期，不能通过最小化整个测试过程跳过渲染。

## M12.1 模拟面板与运行控制

Simulation面板经Workspace调用已有Runtime命令，CPU/GPU能力依构建显示；无GPU时不提供GPU选项。
模型保存会话级SimulationDraft（布片全部参数、固定纳秒步长、目标步数、批次上限），不写场景或历史。
草稿仅在新实验启动时应用，运行中可修改供下一次启动；活动配置和当前run_id单列，外部IPC不覆盖草稿。
启动要求已打开场景且Inspector已Apply/Revert，guard取当前快照；普通失败保留草稿、Edit和活动运行。
Play用simulation.run异步启动，Prepare paused以paused=true从零步准备；稳定paused后Step只推进1拍。
Pause/Resume/Cancel/Stop携带本帧观察到的run_id，过期请求由服务拒绝，不自动重定向新运行。
每帧从Runtime读取已发布状态，显示初始化/暂停中/取消中/停止中、终态与fault；受理不冒充完成。
终态仅允许Stop和只读查看，停止回收期间禁止重启；外部legacy同步运行也按其实际状态提供控制。
GPU冷初始化和有限任务单步在worker执行，不经旧同步start绕开M11响应性路径。
M12.1提供控制，Play视口、指标与导出接入见下节。
验证覆盖参数拒绝/旧run_id、草稿与Edit/history隔离、真实IPC外部控制，以及ImGui按钮输入的CPU/GPU零步准备、单步和停止。

## M12.2 实时模拟视口

Viewport在活动布片实验时默认显示Simulation，可切换回Edit；Stop立即停止请求并恢复最新编辑视图。
只读预览经Workspace/Runtime/SimulationService请求，不执行step，也不改Scene/历史。
有限任务worker在已完成批次边界响应合并后的视图请求，最多一个待处理请求、一个正在绘制的帧和一个待领取结果；
运行期间最多约30Hz生成预览，暂停时仅步数/相机/尺寸改变才重绘；请求具有250ms租期，视口不再请求后停止采样。
结果绑定run_id、完成步数和实际绘制的矩阵/尺寸/视图序号；旧run结果不发布。
连续导航时允许发布落后于最新请求的已完成图像，序号和步数不得倒退；不能因新请求而丢弃所有在途结果。
仍只合并保留最新请求和最新完成结果，不积累请求队列。图像沿用自身尺寸和视图身份，落后时标记更新中，静止后收敛到最新视图。
GUI使用最近已完成图像，不等待后台编译或求解；显示图像对应步数及准备/过期/失败状态，不假称图像等于最新任务进度。
CPU布片上传当前位置后绘制；GPU布片通过零步Graph直接读取求解输出，不回读粒子或图像。
GPU图像通过[已完成图像交接](graphics-resources.md)转入呈现队列，ImGui直接采样。
图像/描述符仅在前一GUI提交完成后替换；窗口缩放保留旧图直至新尺寸完成，最小化不新增预览请求；
Stop/关闭不丢弃在途GPU所有权，worker排空，图像可独立保活至GUI完成。
模拟相机与编辑相机独立；首次运行自动取景，Frame/F/Home重置取景，RMB环绕/MMB平移/滚轮缩放。
模拟图像禁用实体拾取/Gizmo，Inspector仍可编辑EditWorld。相机取景按布片输入包围范围，不隐式回读GPU粒子。
无共享双队列设备时明确显示实时预览不可用；不偷偷使用CPU图像回传。CPU-only模型不增加Vulkan依赖。
验收覆盖CPU/GPU连续变化、精确暂停图像稳定、运行及暂停中持续环绕/平移期间实际视图序号前进、停止输入后收敛、缩放/最小化恢复、Stop新run隔离、带活动预览关闭及同步/寿命。
默认Simulation放在左下，Inspector保留完整高度；控制分行、参数用可滚动双列表，避免默认窗口宽度裁切按钮。
一次性smoke与response probe统一单列已知AMD Loader Message原文，其他warning/error及存活分配仍失败；普通工作台退出策略不变。

## M12.3 指标与实验导出

Simulation 面板显示已完成步数、精确模拟时间、有限任务进度、最近完成批次墙钟耗时和每步均值。
指标区标注 Current / Stale / Unavailable 及对应步数；性能说明其统计口径，未采样值不显示为零。
数值通过 Sample metrics 按钮显式调用 simulation.sample；仅在健康且已初始化的稳定暂停/完成/取消边界可用。
显示约束误差、相对误差、速度、能量、地面穿透与固定点漂移；运行中保留并标注旧采样。

Export 按钮复用 simulation.export，传入按钮当帧观察到的 run_id/steps 和项目相对新目录/尺寸；
界面说明导出使用固定实验相机、五个文件及显式同步操作。成功回执保留路径、run_id、步数、时间；
失败显示实际错误并保留最近成功回执，供用户找到有效产物。Stop/新运行不把旧回执冒充新实验。
模型只通过 Runtime 命令采样/导出；没有场景/history 写入或隐式 Play 烘焙。
验证覆盖无样本/当前/旧样本、旧身份/步数拒绝、CPU/GPU真实按钮采样与导出、产物身份和失败保留。

## M12.4 跨入口验收

增加仅用于一次性工程的 `--simulation-consistency-smoke`，要求 pipe、fixture-camera、screenshot 和 .dk-editor-smoke 标记；
与其他 smoke/frames/response 模式互斥。固定检查点文件只同步测试步骤，不开放通用命令或修改生产态。
驱动通过实际 ImGui 鼠标输入操作 CPU/GPU 配置、Prepare、Step、Sample、Export、Resume、Stop，
外部 Python SDK/dk-ctl 在相邻检查点接管同一 run_id，严格推进到300拍、编辑Edit、暂停并校验当前GUI/视口。
检查点须在状态/显示/视口都到达预期边界且一帧已呈现后发布；带运行身份、完成步数、样本、图像签名和编辑版本/历史。
暂停边界比较跨帧像素与时钟，运行边界允许预览落后，不能把旧图像标成最新步数。

GUI导出后Stop保留最近外部编辑和撤销历史；外部脚本在同一窗口宿主重放导出配置，
比较config/metrics/particles/image逐字节内容，provenance核对新的run_id。重放后的GUI回到最新Edit。
CPU和GPU串行执行，禁止另建GUI模拟状态或在夹具里直接修改Workspace/Service。
同步validation、存活分配和退出资源检查沿用工作台；单列既有精确AMD Loader警告。
定向复用实时视口/活动关闭、图像交接与GUI响应smoke；响应子集仍标partial，不冒充M11完整统计矩阵。

## 边界

`dk-editor` 装配 SDL3、ImGui docking、Vulkan Presenter、ScenePipeline 和 Runtime。
`dk_editor_model` 依赖 Runtime/Geometry，保存选择、Inspector 草稿、只读快照并提供相机/手势计算；不访问 ECS。
`dk_editor` 实现窗口 UI 与渲染桥，私有依赖 ImGui/SDL3。新增 `DK_BUILD_EDITOR` 默认 OFF，
`windows-editor` 启用已有呈现/渲染模块。CPU-only Runtime 不新增窗口或 GPU 依赖。
ImGui 使用固定 baseline 中的 1.92.9 docking/SDL3/Vulkan backend；本次核验官方 port 仍为该版本。
SDL3 复用已接入版本，不升级 baseline。
ImGui overlay 保留官方源码 tag/hash，在 1.92.9b-docking 上应用 Adobe Spectrum 的定版外观补丁，并对该库启用 VK_NO_PROTOTYPES，
由 Device 借用 resolver 初始化 backend 私有函数表，避免其直接调用与 volk 同名的全局符号。

### Spectrum 主题

通过项目内 overlay 的显式 spectrum feature 保存补丁与来源提交，继续由 vcpkg 编译和安装 imgui::imgui；不修改安装缓存。
移植 Adobe 的色板、控件、文字及箭头绘制，保留当前 docking 和 SDL3/Vulkan backend。
默认采用 Spectrum 浅色（白色/浅灰背景），补齐停靠、表格及新版 ImGui 颜色；沿用现有中文系统字体和 DPI 支持，
不引入 Adobe 内嵌 Source Sans Pro 字体或新增主题切换设置。
标准 style 颜色和控件直接使用的 Spectrum 色板均按交换链格式执行一次 sRGB 到线性转换，
重建时从原始色值恢复；透明度沿用 ImGui Alpha/DisabledAlpha，避免禁用控件仍以不透明色绘制。
补丁适配当前 PathStroke 参数和 mixed checkbox 几何；保持输入命中、导航和场景操作语义。
验证集中于主题颜色/禁用状态、真实工作台、停靠和交互；自动窗口始终使用 --background。

## 工作流与命令

启动接受项目根和相对 manifest，默认 `projects/demo/project.json`。
Hierarchy 显示父子结构并按稳定 EntityId 选择；Inspector 显示身份/资产引用，编辑名称与本地 TRS。
Apply 把两个编辑放入已有 `scene.transaction`，用草稿读取时的 document_id/revision，产生一个撤销单元。
Undo/Redo、Save、Open/Reload 复用 Runtime 命令，错误显示在 Console；不增加命令或另建历史。
Save 使用 `scene.save` 原子保存当前场景，manifest/资产映射在本阶段只读，不需要双文件事务。
Open 仅改变当前 root 内的 manifest；切换项目根通过重新启动。只读资产面板显示项目映射。

## 状态、失败与关闭

Model 持有当前拥有型 SceneReadSnapshot；Runtime 增加 guard 校验的只读快照转发。
编辑提交点仍为 SceneService 发布，成功后刷新；保存失败保留 dirty、编辑和历史。
换场景先由服务构造候选，失败保留旧会话/选择/视图，成功清空选择及草稿，历史按已有服务重置。
选择变化不修改文档；未应用草稿有单独 dirty 标记。切换选择、撤销/重做前必须处理草稿；
关闭/打开/重载遇到场景 dirty 或草稿时显示 Save / Discard / Cancel，Save 先 Apply 再保存，任何失败不继续。
当前草稿过期时拒绝 Apply，显示冲突，允许 Revert 从当前快照重新读取。

## IPC 接入

Windows `dk-editor --pipe NAME` 显式开启与 runner 相同的服务端。Workspace 持有服务端，
窗口主线程在每次循环（包括最小化时）pump Runtime 和 IPC，外部请求经同一命令注册表执行。
处理后刷新只读快照和历史；同一文档的未应用草稿保留旧 guard，Apply 明确冲突，不能悄悄覆盖。
换文档清空选择/草稿，旧 Gizmo 由现有 document/revision 校验取消。
外部 runtime.shutdown 是显式退出指令，不保存草稿/场景；回复后排空管道再销毁 GPU/窗口。
基本接入在 M8.3 验证，M8.4 按下述跨入口契约验收。

## M8.4 跨入口一致性

Runtime 是唯一已提交状态所有者。GUI Apply、外部 dk-ctl 以及 Undo/Redo 都走同一命令/历史；
scene_id/revision 标识内容版本，document_id 标识当前打开会话。Reload 或新 runner 会改变 document_id，
但保存后的 SceneId/revision/实体内容和指定视图图像应保持一致。未应用 Inspector 草稿与 Gizmo preview
不属于已提交场景，不能把它们的像素当成同 revision 的正式截图。

Viewport 与 CaptureService 共用 `render::unlit_preview_settings()`：线性背景 (0.04,0.08,0.16)、
exposure=1、unlit_preview 导入。比较时显式传递已发布视口的尺寸、相机 eye/target/up、65 度 FOV、
near=0.05/far=10000；不把“同 revision”误当成“不同相机也应同图”。资产文件在验收期间不变。
同设备、相同资产/视图/设置逐字节比较 RGB8 PPM；跨设备/驱动的像素完全一致不作承诺。

增加仅用于一次性夹具的 `--consistency-smoke`（要求 `.dk-editor-smoke`、`--pipe`、
`--fixture-camera` 和 `--screenshot`，与其他 smoke/frames 互斥）。私有 ConsistencyDriver 注入真实
ImGui 鼠标/键盘输入；外部脚本通过真实 dk-ctl 子进程调用公开命令。固定 checkpoint/continue 文件
仅同步测试步骤，不增加业务命令或绕过 guard，180 秒内未完成则失败。

checkpoint 在窗口 GPU 提交完成后发布：先写实际视口已上传的 RGB8 像素，再原子写 JSON 摘要。
摘要保存当前 document/scene/revision/dirty、选择/Inspector/历史以及已发布视口的身份、相机和尺寸；
必须没有活动 preview、无过期视口、相机匹配。测试模式才保留该帧 CPU 像素，常规编辑器不增加常驻副本。
窗口最终截图额外证明真实工作台呈现；PPM 比较使用不含 UI overlay 的原始视口，避免窗口布局影响结果。

验收顺序：初始图 → GUI 名称/TRS Apply → 外部事务（含重复 ticket 与旧 guard 拒绝）→
未提交草稿期间外部编辑 → GUI 旧草稿 Apply 冲突/Revert → GUI Undo/Redo → Save/Reload →
关闭编辑器 → runner 重载、核对全部实体及原视图截图。旧版本 capture 先提交再编辑，完成结果和图像
仍必须对应原版本；任何 capture 不隐式保存。拒绝过期请求、草稿冲突不改 revision/history/文件。
M8.3 的超时/断连去重用例按传输回归复用，不额外改变 IPC 协议。

## 窗口与 GPU 接入

Platform 提供显式 SDL 借用句柄、同步事件 sink 和清除关闭请求；事件借用仅在回调期间有效，
SDL 的初始化、窗口销毁和主线程契约仍由 Platform 管理。Presenter Frame 暴露本代 image_count。
ImGui backend 是唯一原生 Vulkan 接入点，经 CommandBatch::unsafe_record 显式声明 framebuffer
写与 viewport 采样，并保留 ImageView；封装层继续管理提交、同步、交换链和资源寿命。
Windows 可用时读取系统微软雅黑字体（不打包字体），支持中文名称；无该字体时使用 ImGui 默认字体。
sRGB 交换链上的 UI 样式先转为线性颜色，Viewport 纹理单独按 sRGB 解码。
每帧等待提交完成后再更新/释放 backend 纹理；关闭先排空 GPU，再关闭 ImGui，最后释放 Presenter/Window/Memory。
交换链格式变化时重建 ImGui pipeline，图像数变化同步 backend 配置；最小化暂停并保留模型。

Viewport 基于当前内存快照，默认 Sponza 相机；M8.2 增加会话相机、CPU 拾取和独立 TRS 手势预览，详见 [交互设计](editor-interaction.md)。按文档版本/面板尺寸/相机与手势序号失效后绘制，非持续重导入。
每个文档会话只加载一次 CPU/GPU 资产，编辑变换时重新提取 RenderScene；显式 Reload 可重读资产。
渲染结果的 RGBA8 已编码 sRGB，初版通过现有读回和上传桥发布 sRGB 采样纹理，避免再次编码。
候选图像及描述符成功后替换旧预览；失败保留旧图并明确显示实际 revision/过期状态，可重试。
此同步预览在大场景加载时会阻塞 UI；后台流式加载和取消不属于 M8.1。

## 验收与后续

CPU 定向测试覆盖选择/草稿、事务与撤销、过期 guard、保存重载与失败保护。
真实窗口 smoke 用 ImGui 输入事件走选择/Inspector/保存/重载/撤销按钮，检查版本与持久化，
验证 TRS 改动使预览像素变化、Undo 逐字节哈希恢复，再读回工作台画面并启用 Vulkan validation；
默认 Sponza 另做实际预览。
M8.2 的 CPU rayquery、Gizmo 和相机交互见 [交互设计](editor-interaction.md)；M8.3 IPC 见 [传输设计](automation-transport.md)。M8.4 证据见 [0068](../development/0068-editor-consistency.md)。

## M11.4 模拟期间的事件循环响应

现有editor IPC与Runtime owner pump承载simulation.run/query/pause/cancel/stop；正常编辑视口仍使用EditWorld。
增加仅验收使用的--simulation-response-probe，要求--pipe及一次性工程.dk-editor-smoke标记，与其他smoke/frames互斥。
真实窗口完成初始视口呈现后发布ready；外部夹具才开始进程首次模拟初始化。
在事件循环每次poll前保存单调时钟、owner发布的模拟状态和提交/完成数，最多120000项，超限失败。
退出后统一保存JSON，不逐帧写盘；首次窗口/视口初始化不混入模拟期间的心跳，暂停/空闲与活动区间分开。
依然实际运行SDL事件、ImGui布局、Vulkan呈现和IPC，不用后台定时器代替GUI心跳。
M12的Play视口/参数工作台不属于本阶段；这里只验收现有宿主对有限任务的响应和关闭。

验收单列已知AMD隐式层API版本警告，保留原始stderr与knownLoaderWarnings计数；
仅该精确消息不阻止响应性探针通过，其他warning/error仍失败，不关闭validation。常规编辑器策略不变。

性能采样与正确性验收分开：`--validation` 要求验证层，`--no-validation` 显式关闭GUI设备验证层，
两者互斥；未指定时仍if_available。响应夹具默认required，disabled只用于独立性能诊断；
两种配置的数据单列，不能互相替代。response_editor_smoke固定required，并验证真实呈现/资源寿命。独立仿真设备保持if_available，共享设备时继承GUI验证模式，
所以不能把GUI与runner延迟之差解释为纯呈现开销。退出探针另记录各所有者销毁阶段，包含到进程退出的完整成本。

IPC/Runtime owner pump在每帧开头、成功获取交换链图像后和渲染提交等待完成后执行。
GPU等待期间收到的控制请求在下一次UI绘制前处理，避免无谓追加一帧呈现后才开始关闭。
获取后收到shutdown时直接退出循环，由Frame原有放弃路径等待获取完成并归还图像；
仍先排空IPC回复。正常退出先close渲染队列证明所有绘制完成，再释放Viewport/ImGui资源，
随后Presenter.close等待present fence并释放交换链；worker同时回收自己的资源。
呈现清理后销毁Runtime以join模拟worker，最后销毁GUI设备和窗口。
不把渲染完成当作呈现完成；异常路径仍先完整排空Presenter，再展开UI资源栈。
两个设备的最终销毁不竞争驱动生命周期锁，且所有对象仍在原拥有线程回收。
验证只覆盖真实窗口控制/关闭和受影响的退出延迟，复用未改动的求解与数值结果。

M11.4复用已存在的GUI Vulkan设备：请求同族第二队列，支持时将设备寿命提供给Runtime，
模拟worker独占index 1，Presenter独占index 0；不支持时保留原独立模拟设备路径。
GUI设备required validation同时覆盖模拟队列；实例/设备创建属于编辑器启动，首次模拟仍包含
首次求解器/shader/Graph初始化，报告明确该成本变化，不伪称重新创建第二设备。
作用域守卫在所有正常/异常退出路径上先销毁所有UI使用者、关闭IPC并销毁Runtime（join worker），
最后释放Presenter/Window，保证共享设备和窗口的最终释放仍在主线程。
保持原FIFO绘制节奏，不限制工作台到60Hz。

若一次IPC pump只处理握手而未分派命令，Workspace使用先读取的RuntimeEvents sequence
最多等待1ms以接收同连接的执行帧，再pump一次。普通命令和空闲循环不增加等待；
客户端未继续发送时仍有界返回，不让每次两段握手都依赖下一帧的绘制/呈现。

## M14.6 环境面板与异步大气视口

Environment面板拥有EnvironmentDraft（完整CPU参数、读取时guard、modified），所有Apply使用environment.set，
与IPC/Python/Luau共用同一Scene/历史；外部编辑保留有修改草稿和旧guard，Apply冲突，Revert取最新。
Undo/Redo/切换文档前处理任一草稿；Save先提交草稿再保存；两个面板不同时编辑；API若出现两份已修改草稿，在提交任何一份前拒绝Save/Apply，要求先Revert其中一份。
面板可编辑启用、太阳方向/线性RGB/角半径，以及球面、大气散射/消光/密度与地面反照率；曝光为视口会话参数，不写环境。
未应用草稿不渲染。面板使用原子完整Apply，不每次拖动重建LUT。

启用环境的Edit视口采用独立后台worker/Device/Queue，复用ScenePipeline的环境绑定入口和最近LUT。
主线程只复制拥有型SceneReadSnapshot/相机/尺寸/曝光/preview身份；最多一个正在执行、一个合并待处理请求和一个完成结果。
worker只发布最新已提交请求的序号；GUI在首帧完成前合并相机/尺寸输入，完成后提交最新视角，避免持续输入饿死画面。
完成图必须匹配当前document/revision/exposure/preview和渲染设置；相机/尺寸允许落后但保留真实来源，不标为当前帧，拾取/Gizmo仍要求完全匹配。
错误保留旧画面并标过期，相机导航仍可用。
关闭/取消清掉待处理，已提交GPU必须等待回收；worker join后释放队列。GUI不等待LUT冷编译/积分；完成RGBA8沿既有上传桥呈现。
关闭环境保持旧同步unlit路径，并使旧大气请求失效；Simulation独立视口不接入。资产文件仍在执行时读取。
同一Scene会话资产缓存复用；32MiB快照上限，现有1600×1200视口上限；无无界任务/结果积累。

环境模式相机near=1m、far=10000m，以满足M14.5深度精度域；关闭时保持near=.05m/far=10000m。
绘制、拾取、投影和检查点使用同一相机状态；UI明确显示裁剪面。不能通过静默改动capture参数匹配画面。
真实GUI自动化使用--background，验证环境Apply、IPC/脚本交替修改、旧草稿冲突、Undo/Redo、Save/Reload与同设置截图逐字节一致。
独立后台worker测试验证连续改参、resize、取消、失败恢复及在途关闭不发布旧结果；
相机导航允许在旧帧上继续发送新请求，拾取/Gizmo启动仅在完整匹配当前帧时启用；已捕获手势按原guard继续接收输入并提交，
不因自身异步预览尚未返回而取消，外部revision或尺寸变化仍取消。单独CPU模型验收草稿/历史保护；CPU-only不引入GPU依赖。

## M15.6 云面板与固定时间预览

Environment面板提供clear/sparse/overcast/sunset版本化预设和完整云物理参数。预设查询只填草稿，Apply仍是一次environment.set，Revert/guard冲突/Undo/Redo/保存重载共用原契约。
会话区提供reference/temporal、固定时间与单步、云影开关/固定覆盖区，不保存到Scene。后台请求携带全部设置；结果包含实际生效时间、mode、history和shadow generation，旧请求的迟到结果不能覆盖新请求。
temporal每个请求独立重置视图、固定时间；先发布首帧，再每4次更新发布一次，至32次结束。实际warmup随结果变化，一致性测试等待32次后与序列截图比较。
空气或云启用时均走后台环境路径。UI显示当前scene revision与结果设置，常规视口禁用超过4096像素的reference选择；底层接口仍明确拒绝越界，temporal供常规视口。外部环境修改、取消、退出延续单active/单replaceable队列与完整资源释放规则。
