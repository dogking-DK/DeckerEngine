---
module: render-capture
created_at: "2026-10-03T21:21:47+08:00"
updated_at: "2026-10-19T12:40:31+08:00"
status: accepted
---

# M7.4 截图作业与自动化验收

## M17 体积路径截图

[M17设计](render-pathtracing.md#m17-大气云与表面路径的统一传输)的统一积分器通过 pathtrace v2 接入，
冻结显式时间、环境内容、场 hash、模型/采样版本，与 Editor 共用渐进 renderer；继续在 owner 原子替换时发布文件。
具体字段见[命令参考](../commands/render.md#统一介质路径v2)，旧 v1 表面行为保留。

## 边界与依赖

新增可选 `DK_BUILD_RENDER_CAPTURE`，默认 OFF，windows-graphics 启用。
`dk_render_services` 依赖 SceneServices、Jobs、RenderDisk、RenderPipeline；
`dk_render_operations` 负责注册和 JSON 转换；Runtime 条件装配。
CPU-only Runtime 保持不依赖 Vulkan/Slang。复用 [Jobs](foundation-jobs.md) 和
[命令协议](automation-protocol.md)，不改同步 TaskId envelope，不新增传输分支。
具体参数见 [截图命令](../commands/render.md)。Jobs 的 JSON 注册拆到独立 JobOperations，
AssetOperations 默认仍注册资产 jobs；启用截图的 Runtime 改为一次注册统一路由。
当前 schema 子集无 oneOf，统一结果采用两种对象字段的可选并集，服务保证完整变体；CPU-only 保留资产严格 schema。

## 输入、快照与身份

`SceneService::read_snapshot(guard)` 在 owner 线程复制 Project 映射和 SceneSnapshot，
保留 document_id、scene_id、revision；worker 不访问 ECS 或 SceneService。
`DiskScene::load_snapshot` 使用该快照解析资产，不重新加载磁盘 Scene，因此未保存编辑可截图。
文件资产在 worker 执行时读取，不承诺外部文件热修改的时间点快照。
每次接受 capture 分配递增非零 frame（Runtime 会话内唯一，拒绝不消耗）；返回 JobId 和固定
文档/场景/revision/frame/尺寸/相对输出路径。后续编辑、撤销、换场景不会改变该输入。
终态 succeeded 的 result 返回同一身份及 draw_count；失败/取消 result 为 null，提交响应保留关联。

## 执行、提交点与关闭

CaptureService 独立拥有 MemorySystem、render/jobs heap、owner ThreadContext 与单 worker JobQueue。
queued=2、active=3、terminal=64、输入预约上限 192 MiB；每份 SceneSnapshot 上限 32 MiB，
宽高各 1..2048。预约包含快照、Project 路径映射、请求和最大读回/PPM字节；
CPU 资产仍受 DiskScene 的 64 个/512 MiB 载荷限额，只有一个 worker 同时导入/使用 GPU。

worker 依次创建候选 CPU 资产、独占 Device/SubmissionQueue、上传并等待、建管线、绘制、
等待 GPU、读取 RGBA8、编码 P6 PPM；GPU 对象在 worker 内回收，completion 只含拥有型字节与摘要。
不跨 worker 共享队列。首次捕获才初始化设备；创建 Runtime/查询能力不要求 GPU 存在。
取消在阶段间及 glTF importer 检查；已提交 GPU 工作必须等待并回收，不能抢占驱动/三方 IO。

owner drain 在非重入安全点通过 `write_file_bytes_atomic` 发布单个文件，原子替换是提交点。
取消先被 owner 接受则 consumer 不运行，旧文件保持；发布后取消 accepted=false。
IO/导入/GPU 可恢复错误进入 failed，旧文件保持。summary 在文件提交前完成必要分配，
基础设施异常仍沿 Jobs fatal 路径退出，不承诺进程中断/OOM 的跨资源事务。
stdout 只输出 JSON-RPC；诊断由 Device stderr sink 处理。验证开启时检查 validation errors。
默认设备 validation=if_available，集成验收要求 required。

Runtime idle pump 同时消费资产与截图；`jobs.*` 通过 JobId 路由到所属队列，
查询未知/被淘汰 ID 为 not_found。jobs.wait 最多1000ms、泵两种完成但不重入命令。
关闭取消并 join 后释放 completion/context/heap，不隐式保存场景、也不发布未消费截图。

## 限制与验证

只输出 PPM（顶部第一行、sRGB RGB8），父目录必须存在；路径在工程根内，输出扩展名 .ppm。
相机使用 eye/target/up、projection 与对应透视 fov_y 或正交 ortho_height、near/far，默认采用透视 Sponza 示例相机；
M18.1 的投影生成复用 [Math 透视计算](foundation-math.md#m181-透视计算)，FOV 校验、默认值和命令 schema 不变。
默认 unlit_preview，支持 opaque/mask，不新增 PBR/透明/天空盒/HDR 照明。
M8.4 与编辑器共用 Render 的 unlit_preview_settings。比较视口和截图必须显式匹配尺寸与相机，
包括 FOV/near/far（编辑器 far=10000，截图默认 far=100），并固定资产文件与设备；
比较对象是已提交场景，草稿/手势候选不在 capture 快照内。完整验收见 [编辑器设计](editor.md)。
重复输出按实际 owner 发布顺序替换，无多文件事务；成功只说明该次写入，后续 capture 可覆盖。

CPU 命令验收覆盖 discovery/schema、guard、无场景、路径/相机/尺寸、事务拒绝；
GPU 进程验收覆盖版本绑定、编辑后图像变化、无输入自动发布、失败保留、取消和退出。
原资产 jobs 定向回归与 CPU-only runner 构建确认原有能力和依赖边界。

## M14.6 环境绑定与可追溯截图

M15.3补充：非零云由共用环境入口选择混合介质参考，默认quality为`cloud_reference_v1`、time=0与256/64/64步；M15.6允许显式时间及temporal，
最多4096像素，超限在worker失败且保留旧输出。云与空气独立开关；零云仍用原atmosphere_v1/unlit_v1。
指纹覆盖云/空气共享传输与合成shader，ticket/result schema增加该quality值，命令参数/effect/guard/发布流程不变。

render.capture按接受时SceneSnapshot的空气/云开关和非零云参数选择环境合成或原unlit，profile仍仅为资产导入策略。
新增有限非负exposure（默认1）；相机仍完整显式输入，环境启用时遵循管线的有限透视/正交支持域，不自动改near/far。
worker创建ScenePipeline后调用共用render_environment：冷准备与GPU等待在worker，原取消/原子单文件发布契约保持。
未构建大气时enabled请求明确失败；disabled保持旧unlit；CPU-only不装配CaptureService，环境命令仍不初始化GPU。

提交ticket和成功result新增provenance：完整冻结environment、camera、exposure、profile、quality（unlit_v1/atmosphere_v1/cloud_reference_v1/cloud_temporal_v1）、
time_seconds（默认0，没有隐式时钟推进）、shader_fingerprint（ticket为null，成功时记录源码集FNV-1a64诊断指纹）。
指纹不是安全校验；执行期间源码集必须稳定，编译前后摘要变化则失败，保留旧文件。输出仍只有PPM，不增加多文件原子事务。
后续编辑/保存/撤销不改变冻结版本；GPU/IO/取消均不改旧文件。result保持<=4096字节Jobs元数据上限。
命令发现/schema及文档同步；Python与Luau例子实际修改环境并使用同一capture命令，GUI/IPC验收保存重载，验收固定参数图像与元数据。

## M15.6 固定时间与独立时域序列

capture保留单PPM原子输出并增加环境渲染会话设置；默认reference、time=0，非零云且空气启用时准备固定世界云影。render.sequence复用相同Service/worker/Jobs路由，冻结完整样本列表，从新Pipeline空历史开始，按序等待每帧；每次作业只发布一个有版本JSON报告。报告内含RGBA8和曝光前线性RGBA，Python工具再提取图像与比较。
所有相机、时间、模式、预热与像素预算在入队前预检；成功接受只占用一个作业身份，不修改Scene。worker循环检查取消，GPU在途必须等待；失败/取消不发布候选报告。owner drain单次write_file_bytes_atomic是唯一外部提交点，既有PPM/JSON在失败时保持；返回摘要先于提交完成分配。
每帧最多65536像素、总262144像素、1–128样本、temporal预热0–64且reference禁预热。输入预约保守计入每像素256字节、相机样本和完整Scene；最终JSON≤64MiB、Job摘要≤4096字节。报告保存全部原始线性值/图像、身份、完整参数、source指纹、历史/云影状态和设备；采集时间为端到端wall，gpu_ms为null表示未采集，不伪装成0。
新命令参数/schema/返回值见[截图命令](../commands/render.md)，完整上层契约见[云设计](render-clouds.md#m156-上层配置预设与用例清单)。

## M16.10 固定路径追踪 capture

render.capture 新增 renderer=raster/pathtrace（默认 raster）、format=ppm/linear_json（默认 ppm）及 pathtrace v1 对象。
pathtrace 必须显式提供 version=1、target_spp；默认 seed=1、max_bounces=8、rr_start=3、environment_rgb=[0,0,0]、sun=false、surface_only=false、exposure_ev=0。
固定 metallic_roughness/model=3/sampler=1/MIS、batch=1/tile=64；与 Editor 同一渐进 renderer 和 GPU tone。
不接受路径请求混用 raster 的 exposure/time/render_options/warmup；profile 固定 unlit_preview；sequence 不扩展后端。
路径 PPM 支持 1..2048，linear_json 最多 524288 像素/128MiB文件，预约256字节/像素，沿用192MiB总队列额度。
每个任务新建设备/renderer/视图，从样本0精确到目标1..65536；没有设备/模块/RT能力明确失败，不降级。
支持能力由 runtime.capabilities.pathtrace_capture 表示编译装配，不创建或探测设备；实际 RT 能力在执行时确认。

Job 进度由通用有界 metadata 返回 {phase,unit,completed,submitted,total}，unit=spp，phase 为 queued/preparing/building/rendering/readback/publishing。
completed 仅计全图完成样本；submitted 不代表完成。查询/取消/等待均包含 progress；非进度任务为null，终态保留最后进度。
worker 每 tile 轮询取消，关闭/取消必须收完在途 GPU；owner drain 唯一原子输出提交点不变。

路径 provenance 冻结完整场景环境/相机/参数；worker 加入导入内容 fingerprint、shader 源闭包摘要及模型/采样版本。
PPM 的 Job 摘要含上述来源和实际 spp；linear_json 是单独任务的唯一输出，包含完整 provenance、线性RGB、sRGB RGBA8、
尺寸/身份、device/driver/build、显存额度/峰值及worker开始到报告组装的 wall_ms（不含序列化/owner发布，GPU分项未测为null）。不做多文件事务。
源 shader 在编译前后和发布准备前比较，变化失败；资产摘要来自不可变 PathTraceScene，执行期不再重读资产。
路径支持域沿4096三角形/64纹理/源纹理64MiB/1GiB GPU额度，空气云只允许显式 surface_only 忽略体积。

验收新增真实RT capture任务、单文件保护、输入冻结、进度和Python/Luau；GUI验收显式打开测试专用线性读回，
通过真实呈现的完成帧与同相机/seed/spp capture报告逐像素比较，正常GUI不启用CPU读回。

## M17.5 统一介质捕获

保留 pathtrace version=1 的表面语义和 surface_only 确认；version=2 必须显式 medium_mode=surface_only/scene_media，禁止混用旧 surface_only。
scene_media 使用 model 4 / sampler 2 / MIS；surface_only 保留 model 3 / sampler 1。两种模式共用原固定 spp、取消、owner 原子单文件发布及旧文件保护。
v2 增加固定 time_seconds、cloud_albedo、cloud_anisotropy、每 tile max_tracking_steps 和每路径 max_steps_per_path；参数格式与物理模型分别版本化。
v1 禁止介质字段；v2 surface_only 禁止介质专用字段，避免静默忽略。公共反弹范围继续 1..16，RR 不大于反弹数。
冻结完整 Scene 环境和相机，worker 从不可变字段构建 PathMediumSnapshot；仅非零云生成字段。失败无隐式光栅或晴空降级。
提交来源保留 null shader/input/field hash，完成来源包含字段 hash、几何 hash 和包含空气/云的 shader 源闭包；媒体参数与实际 spp 同源。
GUI 与命令使用相同默认批次/tile；线性 JSON 做逐像素对照，非兼容 GPU 域和工作额度失败保护旧文件。
新增命令字段仍由 Operations 解码、Service 校验；Console/IPC/Python/Luau 共享注册表，无新协议分支。

M17.6 将 input_fingerprint_version 升为3：几何内容hash与源身份/revision、完整环境组合。资产刷新仍重新生成几何；模型4/采样2及capture参数版本不变。

## M18.4 投影与有效相机来源

render.capture 和 render.sequence 每个 sample 的 camera 接受 projection（缺省 perspective）及 ortho_height。
旧 eye/target/up、fov_y=65、near=.05、far=100 不变。正交必须显式给正有限 ortho_height 且禁止显式 fov_y；
透视禁止 ortho_height。Operations 保留字段是否提供，由 Service 校验完整相机后构建 Math 投影；schema 只描述字段和枚举。
来源 camera 总含 projection、eye/target/up/near/far，仅写实际生效的 fov_y 或 ortho_height，
正交额外写 primary_ray_origin=near_plane（近面前几何和介质不计入）；此只读字段不作为请求参数。
基础光栅、光栅环境、表面路径和 Scene media 支持正交；序列任何非法镜头 sample 都在入队前失败。
光栅环境支持域按管线设计检查，GPU/域错误保留旧输出；正交背景积分到物理边界，far 仅裁剪几何。
不改变 external/不可撤销、guard、Jobs 取消/关闭、owner 单文件原子发布或旧文件保护。
