---
module: render-command-reference
created_at: "2026-10-03T21:44:26+08:00"
updated_at: "2026-10-19T17:07:00+08:00"
status: accepted
---

# 场景截图与固定时间序列

[返回命令目录](README.md)。需要 DK_BUILD_RENDER_CAPTURE（windows-graphics 默认开启）。
无需窗口；设备延迟到后台 capture 执行时创建。

## render.capture

对活动内存 Scene 提交截图，effect=external、undoable=false，不允许进入 scene.transaction。
必须提供当前 guard；成功提交不改变 Scene revision、dirty 或撤销历史，也不隐式保存。

| 参数 | 必填/默认 | 含义 |
| --- | --- | --- |
| guard | 必填 | document_id + revision，必须匹配当前活动文档 |
| output | 必填 | 工程根内相对路径：ppm为.ppm，linear_json为.json；1–1024 UTF-8字节，父目录须存在 |
| renderer | raster | raster或pathtrace，显式选择后端 |
| format | ppm | ppm或linear_json；linear_json仅用于pathtrace |
| pathtrace | pathtrace后端必填 | 版本化固定spp设置，见下节 |
| width / height | 960 / 540 | 各为 1–2048 的整数 |
| camera | 默认 Sponza 相机 | 对象，提供时 eye[3] 和 target[3] 必填；其余可省略 |
| profile | unlit_preview | strict 或 unlit_preview；后者保留基础色/alpha mask，省略切线和光照纹理 |
| exposure | 1 | 有限线性曝光，0–1000000；HDR合成后tone时使用 |
| time_seconds | 0 | 有限秒数，[-1e9,1e9]；不推进场景或模拟时钟 |
| render_options | 见下表 | reference/temporal模式与固定世界云影区域 |
| warmup_frames | 0 | 0–64整数，仅temporal允许非零；首样本固定时间预热，从空历史开始 |
| validation | if_available | if_available 或 required；required 缺少验证层时 Job 失败 |

camera 可选 projection=perspective、up=[0,1,0]、near=0.05、far=100。
透视可选 fov_y=65（度，1&lt;值&lt;179），禁止 ortho_height；orthographic 必须给正有限 ortho_height（世界米），禁止显式 fov_y。
正交支持基础光栅、空气/云光栅和 Surface only / Scene media 路径。光栅环境要求 far≤1000000 m、可见宽高各≤100000 m，眼点位于地面以上；不支持的域返回 not_supported/invalid_argument，保留旧输出。
正交主射线从各像素的近裁剪面开始（路径追踪含像素抖动），近面前的几何和介质不计入；far 只裁剪主网格查询，介质仍到物理边界。
路径 Scene media 的整个近面图像区域须处于既有介质支持域（不可在地下）；光栅环境允许近面跨地面，行星内起点像素不透光。无隐式改相机或关闭环境。
必须有限、near&gt;0、far&gt;near，eye 与 target 不同，up 不能平行于观察方向。
默认 eye=[8,1.8,0]、target=[-4,2,0]。坐标为世界空间，使用右手坐标、顶部第一行。

关闭环境时与编辑器共用线性背景色 (0.04,0.08,0.16)；启用时按冻结环境执行天空、Lambert日光/天空漫射和空气透视。
需要 DK_BUILD_RENDER_ATMOSPHERE，否则启用环境的 Job 返回 not_supported。
非零云另需 DK_BUILD_RENDER_CLOUDS。reference使用无历史混合介质参考，总像素≤4096；
temporal使用半尺寸重建和每作业独立历史，输出最多1920×1080。超过渲染支持域的Job失败且保留旧输出。
云可在空气关闭时单独渲染；零coverage/density/extinction或关闭云退化为原大气/无环境路径。
要重现 GUI 当前已提交画面，须显式匹配视口的尺寸、相机和 `profile=unlit_preview`；
编辑器使用 far=10000，启用环境时 near=1，关闭时 near=.05；截图默认 near=.05/far=100。
启用环境要求 far/near<=10000，刚体相机与其他支持域见[大气指南](../guides/atmosphere.md)。guard 只关联场景版本，不传递相机或未提交草稿。
完整比较流程与边界见 [编辑器指南](../guides/editor.md)。

返回 `{job_id,document_id,scene_id,revision,frame,width,height,output,provenance}`；
frame 在当前 Runtime 内从 1 递增，拒绝请求不消耗帧号。输出路径规范化为工程相对路径。
提交时复制 Scene 与 Project 映射，所以随后的编辑、撤销或场景替换不会影响该次截图。
资产文件在 worker 执行时读取；不保证外部资产文件被同时修改时的内容快照。

使用 [jobs.get / jobs.wait / jobs.cancel](jobs.md) 查询、等待和取消。
succeeded 的 result 为 `{kind:"capture",document_id,scene_id,revision,frame,width,height,output,draw_count,provenance}`。
路径结果draw_count=0（无光栅draw），采样数见provenance.actual_spp。其余状态 result=null；failed 的 error 记录导入、GPU 或 IO 错误；提交响应提供失败任务的原始版本关联。
任务成功表示 GPU 已完成、读回和原子文件替换已完成。PPM 为 P6、RGB8/sRGB，覆盖同名文件。
取消先被 owner 接受则不会发布；失败保留既有输出。之后的另一次成功截图仍可替换该文件。

截图独立队列：queued=2、active=3、terminal=64、input_bytes=201326592，
输入预算包括快照、映射和图像字节预约；场景快照不超过32 MiB，资产限额同 DiskScene。
超时不隐式取消；进程 EOF/shutdown 取消并 join 未完成工作，不能硬抢占驱动或解码 IO。

常见错误：无活动场景 invalid_state、过期 guard conflict、非法尺寸/相机/路径 invalid_argument、
父目录缺失 io_error、容量满 invalid_state；资产缺失、unsupported 材质、GPU 初始化或提交失败发生在后台 Job。
已接受任务的失败不会反向改变提交命令的 succeeded TaskId。

```json
{"jsonrpc":"2.0","id":1,"method":"scene.load","params":{"manifest":"project.json"}}
{"jsonrpc":"2.0","id":2,"method":"render.capture","params":{"guard":{"document_id":"<scene.load 返回的 document_id>","revision":1},"output":"captures/sponza.ppm"}}
{"jsonrpc":"2.0","id":3,"method":"jobs.wait","params":{"id":"<render.capture 返回的 job_id>","timeout_ms":1000}}
```

先创建 captures 目录；使用实际返回的 document_id/revision/JobId 替换占位值。
完整可运行示例：[capture.ps1](../../examples/render/capture.ps1)，从仓库根执行：

```powershell
./examples/render/capture.ps1 -RequireValidation
```

该示例使用默认 Sponza 工程，创建输出父目录，自动取得 guard、等待终态并关闭 runner。
光栅截图不支持PBR；路径追踪材质沿下节。两者均不支持PNG、透明混合、HDR环境贴图采样或天空盒。

`provenance`包含完整环境（与environment.query同结构）、camera（含 projection、up/near/far 及生效的 fov_y 或 ortho_height；正交还含 primary_ray_origin=near_plane）、
exposure、profile、time_seconds、render_options、warmup_frames、sample_count及shader_fingerprint。
quality反映实际路径：atmosphere_v1、unlit_v1、cloud_reference_v1或cloud_temporal_v1。
atmosphere_v1固定LUT和64×64×128空气透视网格；cloud_reference_v1为256/64/64步，无历史；
cloud_temporal_v1使用M15.4的96到256自适应主步、8云太阳步、保边重建及历史。零云仍返回原大气/unlit质量。
提交时shader_fingerprint=null；成功结果为`fnv1a64:`加16位小写十六进制，覆盖本次shader源、共享传输文件和算法版本。
它用于追踪源码变化，不是加密校验或跨驱动像素一致承诺；shader在执行中变化则Job失败，不发布。
元数据留在提交/Job响应中，调用者应保存；PPM不嵌元数据，也不隐式创建sidecar。

可运行的环境示例：[Python](../../examples/automation/environment_capture.py)、
[Luau](../../examples/scripting/environment-capture.luau)，操作说明见[大气指南](../guides/atmosphere.md)。

## render_options

所有字段可省略；未知字段拒绝。不会保存到Scene，调用方负责在GUI/capture/sequence使用相同设置。

| 字段 | 默认 | 含义 |
| --- | --- | --- |
| mode | reference | reference或temporal；每次capture/sequence都从空历史开始 |
| cloud_shadows | true | 非零云且空气开启时自动绑定太阳云透射图，仅乘直射；空气关闭不施加地表云影 |
| shadow_center_m | [0,0,0] | 固定世界覆盖中心，分量绝对值≤1e9m |
| shadow_extent_m | 8192 | 太阳正交平面宽度，16–65536m |
| shadow_resolution | 256 | 16–512的2次幂，固定256步/云柱 |

云影接收几何须完全在云底以下、地表之上、图半像素安全边界之内；超界Job失败，不静默夹边。
可显式关闭云影来渲染云内/云上几何；camera位置本身不受接收体云底限制。
曝光前线性读回和性能目标边界见[云指南](../guides/clouds.md)。

## render.sequence

effect=external、undoable=false；需要活动场景和guard，不允许scene.transaction，不修改revision/dirty/历史、不隐式保存。
与capture共用有界Jobs队列、取消和原子单文件发布契约。output须为工程相对`.json`；父目录须存在。
参数沿用width/height/exposure/profile/validation/render_options/warmup_frames；不接受顶层camera或time_seconds。
另需`samples`数组（1–128），每项`{camera,time_seconds,camera_cut?}`；camera字段/缺省与capture相同，允许各样本不同投影/高度，任何非法样本在入队前拒绝；时间必填有限[-1e9,1e9]，camera_cut默认false。
连续样本保留稳定视图身份，true明确重置相机历史；回退/大时间跳跃沿M15.4重置。预热只重复首样本的相机/时间，不隐式推进风。

每样本≤65536像素、全部样本合计≤262144像素；非零云reference仍≤4096像素。宽高/模式/时间/相机/序列预算先验证，非法请求不消耗frame或创建Job。
每次调用使用独立Pipeline，先等待GPU，再推进下一帧。关闭/取消等待在途GPU回收。失败/取消result=null、保留既有JSON；成功表示完整报告已原子替换。

返回与capture相同ticket；provenance中的camera/time为首样本、sample_count为样本数。成功Job result的kind为`render_sequence`，其他身份字段与ticket一致。
provenance.renderer=raster、format=sequence_json、pathtrace=null。frame表示作业请求身份，报告内各样本以index从0计数；samples完整列表和图像在文件中，Job摘要保持≤4096字节。

输出JSON schema_version=1、kind=`decker.render_sequence`、status=`succeeded`，含document_id/scene_id/revision/frame/width/height、完整provenance、device(name/driver)、wall_ms/gpu_ms和frames：

- 每帧记录index、完整camera、time_seconds、camera_cut、shadow_generation、history.available/source_sequence/candidate_sequence/reset。
- rgba8为顶部第一行的RGBA/sRGB整数数组，长度width×height×4；linear_rgba为同顺序曝光前线性RGBA四元组数组，另有linear_summary.min/max/mean三个RGB向量。
- wall_ms是端到端测量（包含等待/读取），gpu_ms=null表示未采集GPU时间；不能用于M15.7的GPU pass预算验收。

JSON最大64MiB。报告是唯一提交文件；[Python用例工具](../guides/clouds.md#版本化用例与对比产物)在成功核对来源后提取PPM/小端RGBA32F和比较报告，失败/缺失不会伪造图像。

```json
{"jsonrpc":"2.0","id":2,"method":"render.sequence","params":{
  "guard":{"document_id":"<当前 document_id>","revision":1},
  "output":"cloud-sequence.json","width":32,"height":32,"validation":"required",
  "render_options":{"mode":"temporal","cloud_shadows":true},"warmup_frames":32,
  "samples":[
    {"camera":{"eye":[0,10,-2],"target":[0,10,0],"near":1,"far":10000},"time_seconds":0},
    {"camera":{"eye":[0,10,-2],"target":[0,10,0],"near":1,"far":10000},"time_seconds":0.0333333333}
  ]
}}
```

可运行[Python](../../examples/automation/cloud_sequence.py)和[Luau](../../examples/scripting/cloud-sequence.luau)示例修改夹具环境并输出序列，说明见[云指南](../guides/clouds.md)。


## 固定 spp 路径追踪

`renderer="pathtrace"`另需编译DK_BUILD_RENDER_PATHTRACING；runtime.capabilities.pathtrace_capture只表示编译能力，
不保证设备支持RT。未编译时提交返回not_supported；设备/RT不可用时Job明确failed，不降级光栅。
pathtrace对象禁止未知字段：

| 字段 | 必填/默认 | 范围与语义 |
| --- | --- | --- |
| version | 必填1（本节） | v2 见下节，参数版本与物理模型独立 |
| target_spp | 必填 | 1–65536整数，从样本0精确渲染到目标 |
| seed | 1 | 完整uint64整数；Luau使用其精确整数范围 |
| max_bounces | 8 | 1–16整数 |
| rr_start | 3 | 0关闭RR，否则≤max_bounces；减少反弹时须同时调整 |
| environment_rgb | [0,0,0] | 常量线性环境辐亮度，每分量0–1e6 |
| sun | false | true使用冻结Scene太阳，false不采样太阳 |
| surface_only | false | 场景空气或云开启时，须true确认只渲染表面光照 |
| exposure_ev | 0 | [-32,32]，显示曝光乘2^EV；不影响线性样本 |

固定metallic_roughness/model_version=3/sampler_version=1/MIS、1spp批次和64像素tile，与GUI共用渐进renderer。
支持上限4096三角形、64纹理/源纹理64MiB、renderer自有GPU分配1GiB；完整Sponza通常超出范围。
路径后端只接受profile=unlit_preview；显式传入顶层exposure/time_seconds/render_options/warmup_frames均拒绝，
这些字段属于光栅配置；render.sequence仍只支持原后端。宽高≤2048，全部样本的保守射线预算≤2^32。

每次任务拥有新视图和空历史。Job.progress的unit=spp，completed为全图已完成样本、submitted为提交到的样本上界、total为目标，
仅提交或部分tile完成不增加completed。阶段为queued/preparing/building/rendering/readback/publishing；终态由state判断，
completed==total仍不等同于文件发布成功。取消按tile间安全点处理，在途GPU须完成回收；旧文件保护同上。

provenance额外包含renderer/format/pathtrace完整参数、material_model、model_version、sampler_version、estimator、actual_spp、input_fingerprint和input_fingerprint_version=3。
提交时actual_spp=0、input_fingerprint和shader_fingerprint为null；成功记录实际目标spp、导入几何/材质/纹理的内容摘要及本次shader源闭包摘要。
provenance.sample_count保留相机/时间帧数语义，路径任务为1；Monte Carlo样本数使用actual_spp和报告sample_count。环境和相机始终是提交时完整值。fingerprint为非加密诊断，不是跨驱动逐位一致承诺。资产文件仍在worker开始时读取。

`format="linear_json"`每图≤524288像素、文件≤128MiB，预约256字节/像素，仍受队列192MiB总输入额度限制。
每次任务只发布这个JSON，不隐式写PPM/sidecar。报告schema_version=1、kind=decker.pathtrace_capture、status=succeeded，包含：

- document_id/scene_id/revision/frame/width/height及完整provenance。
- sample_first=0、sample_count=实际spp；linear_rgb为顶部第一行、每像素3个曝光前线性值；rgba8为同顺序展平的sRGB RGBA8数组。
- device.name/driver、build（配置/编译器）、validation、memory.limit_bytes/peak_bytes（renderer自有资源，额外RGBA8读回不含在此计数）。
- wall_ms为worker开始到报告组装的耗时，包含准备/编译/渲染/读回；不含后续JSON序列化和owner文件发布，gpu_ms=null表示GPU分项未测。

PPM仍是GPU Reinhard显示后sRGB RGB8，线性JSON与PPM应作为两次独立任务调用。若需与GUI比较，使用相同已提交场景、尺寸、相机裁剪、
seed/目标/反弹/RR/环境/太阳/surface_only；GUI默认环境[.3,.4,.6]、太阳开启、反弹4，与命令默认值不同。曝光只影响显示。

```json
{"jsonrpc":"2.0","id":2,"method":"render.capture","params":{
  "guard":{"document_id":"<当前 document_id>","revision":1},
  "renderer":"pathtrace","format":"linear_json","output":"path.json","width":48,"height":32,
  "camera":{"eye":[0,0,-2],"target":[0,0,0],"projection":"orthographic","ortho_height":3,"near":0.05,"far":10000},
  "pathtrace":{"version":1,"target_spp":3,"seed":7,"max_bounces":4,"environment_rgb":[0.3,0.4,0.6]},
  "validation":"required"
}}
```

可执行[Python](../../examples/automation/pathtrace_capture.py)通过IPC渲染活动场景；[Luau](../../examples/scripting/pathtrace-capture.luau)加载project.json。
先将tests/fixtures/render-disk复制到独立工程；Python正交示例：`python examples/automation/pathtrace_capture.py --pipe <endpoint> --ctl <dk-ctl路径> --orthographic`（省略该选项保留透视）。
两者输出path-python.json/path-luau.json；使用小型受支持场景，v1 若空气/云开启须明确 surface_only；联合介质使用下节 v2。

## 统一介质路径（v2）

仍用 render.capture、renderer=pathtrace、format=ppm/linear_json，guard/effect/Jobs/原子发布与上文相同。
pathtrace.version=2 时必须提供 medium_mode：surface_only 使用旧 model=3/sampler=1 并显式忽略 Scene 空气/云；
scene_media 使用 model=4/sampler=2，联合追踪冻结 Scene 空气、非均匀云和表面光照。v2 禁止旧 surface_only 字段。
公共字段 target_spp/seed/max_bounces/rr_start/environment_rgb/sun/exposure_ev 的范围和默认不变；以下字段仅 scene_media 可用：

| 字段 | 默认 | 范围与语义 |
| --- | --- | --- |
| time_seconds | 0 | 有限 [-1e9,1e9]，固定风时间，不使用墙钟 |
| cloud_albedo | 0.9 | [0,1]，云单次散射反照率 |
| cloud_anisotropy | 0.6 | [-0.95,0.95]，云 HG 各向异性 |
| max_tracking_steps | 67108864 | 整数 1..268435456，每 GPU tile 的追踪步额度 |
| max_steps_per_path | 65536 | 整数 1..1048576，每路径追踪步额度 |

v1 禁止 medium_mode 及上述字段；v2 surface_only 也禁止介质字段，错误在创建 Job 前拒绝。
GPU 数值域仍要求行星半径至少 1000m，云 weather/shape 尺度至少 100m、detail 至少 10m，地下相机不支持；
运行时不支持域/工作额度耗尽使 Job failed、result=null，旧文件保留，无静默回退。场景和捕获尺寸上限沿上文。

v2 provenance.quality=pathtrace_v2，增加 geometry_fingerprint；scene_media 另有 medium_model_version=1、cloud_field_hash，
pathtrace 对象返回全部实际参数，顶层 time_seconds 同步固定时间。提交时两个新增 hash 为 null；完成时为 fnv1a64 字符串，
零云的场 hash 为 fnv1a64:0000000000000000。shader_fingerprint 覆盖表面、空气、云与显示源文件；哈希用于诊断，不是加密或跨 GPU 一致性保证。

```json
{"jsonrpc":"2.0","id":3,"method":"render.capture","params":{
  "guard":{"document_id":"<当前 document_id>","revision":1},
  "renderer":"pathtrace","format":"linear_json","output":"media.json","width":48,"height":32,
  "camera":{"eye":[0,1,-2],"target":[0,1,0],"near":0.05,"far":10000},
  "pathtrace":{"version":2,"medium_mode":"scene_media","target_spp":3,"seed":7,"max_bounces":4,
    "environment_rgb":[0.3,0.4,0.6],"sun":true,"time_seconds":2,"cloud_albedo":0.9,"cloud_anisotropy":0.6},
  "validation":"required"
}}
```

Editor Console 使用同一 method/params 对象（不带 jsonrpc/id）；先 scene.query 获取 guard。
[Python](../../examples/automation/pathtrace_capture.py) 增加 --media，对活动 Scene 固定时间零捕获；
[Luau](../../examples/scripting/pathtrace-media-capture.luau) 从 project.json 加载已保存 Scene，使用 3 m 正交高度输出 path-luau.json；按场景地面高度设置相机，保证近面在地面以上。
两者不隐式启用空气/云；先通过环境面板或 environment.set 配置，需要跨进程一致时显式 scene.save。

## ReSTIR 直接光照（v3）

`pathtrace.version=3` 在 v2 基础上必填 `medium_mode` 和 `direct_lighting`。后者为 `classic_mis` 或 `restir_di`，
与 `surface_only` / `scene_media` 正交。classic_mis 禁止 `restir`；restir_di 必须提供 `restir` 对象（可为空使用默认初始 RIS）。
v1/v2 不接受 direct_lighting/restir，继续原经典 MIS；v3 禁止旧布尔 surface_only。
不提供 BSDF-only、外部 history、first_sample、RTXDI 原始对象或 DLSS 参数；未知字段拒绝。render.sequence 仍为光栅。

restir 中字段均可省略，成功来源返回全部有效值；缺少 temporal/spatial 表示关闭，对象 `{}` 表示按默认开启。

| 字段 | 默认 | 合法范围 / 含义 |
| --- | --- | --- |
| version | 1 | 仅 1 |
| initial_candidates | 8 | 整数 1..64，每像素/epoch 新候选数，不是 spp |
| max_candidate_evaluations | 16777216 | 整数 1..16777216，GPU 调度候选评估额度；不足时 Job 失败 |
| temporal | 无 | 可选对象，开启上一完整 epoch 重投影 |
| temporal.version | 1 | 仅 1 |
| temporal.max_history_candidates | 64 | 整数 1..4096，有效历史 M 上限 |
| temporal.max_history_age | 8 | 整数 1..32 |
| temporal.position_tolerance | 0.1 | 有限 (0,1000000] 米，转换到 float 后必须非零 |
| temporal.normal_cosine | 0.95 | 有限 [0,1] |
| spatial | 无 | 可选对象，开启冻结输入平面的邻域复用 |
| spatial.version | 1 | 仅 1 |
| spatial.neighbor_count | 4 | 整数 1..8 次尝试 |
| spatial.radius | 2 | 整数 1..32 像素 |
| spatial.max_source_candidates | 64 | 整数 1..4096 |
| spatial.position_tolerance | 1 | 有限 (0,1000000] 米，转换到 float 后必须非零 |
| spatial.normal_cosine | 0.9 | 有限 [-1,1] |

新捕获总是创建独立完整视图，从 sample/epoch 0 和空历史逐 epoch 渲染；GUI 当前历史不参与。
时间/空间阶段只增加内部工作，Job.progress.completed 和 actual_spp 仍只计完成全图的 spp。
若要逐像素对照 GUI，先 Reset accumulation，再匹配算法、候选/复用、介质和全部相机/物理设置。
相机移动后的 GUI 可携带历史，因此不能直接当成空历史捕获基准。

v3 的 linear JSON 为 schema_version=2（v1/v2 仍为 1），provenance.quality=pathtrace_v3，新增：

- provenance_version=1；pathtrace 含 direct_lighting 和完整有效 restir（仅 ReSTIR）。
- history={start:"empty",sample_first:0,epoch_first:0,temporal_history_epoch:null}。
- restir_sampler_version=1、rtxdi_version="3.1.0"；classic_mis 对应 0/null。物理 model/sampler 沿 v2。
- reuse 提交时为 null；ReSTIR 成功后含 temporal 的 reused/rejected/expired/support_corrected/max_age/max_candidates，spatial 的 reused/rejected/support_corrected/current_visibility_rays/max_age/max_candidates，以及 prepared_surfaces/applied_surfaces（介质完整 epoch 准备/实际到面数，表面模式为 0）。classic_mis 仍为 null。
- shader_fingerprint 对 ReSTIR 覆盖本地 Slang 入口/bridge/共享文件与固定 RTXDI Include 文件内容；不含机器绝对路径。

复用计数是完整 epoch 诊断，M/age 不是已完成 spp，也不是质量或性能结论。其余指纹、guard、尺寸、Job 额度、
错误/取消与原子发布契约不变，配置错误先于入队拒绝，执行额度耗尽保留旧文件。

```json
{"jsonrpc":"2.0","id":4,"method":"render.capture","params":{
  "guard":{"document_id":"<当前 document_id>","revision":1},
  "renderer":"pathtrace","format":"linear_json","output":"restir.json","width":48,"height":32,
  "camera":{"eye":[0,1,-2],"target":[0,1,0],"near":0.05,"far":10000},
  "pathtrace":{"version":3,"medium_mode":"scene_media","direct_lighting":"restir_di",
    "target_spp":3,"seed":7,"max_bounces":4,"environment_rgb":[0.3,0.4,0.6],
    "restir":{"initial_candidates":8,"temporal":{},"spatial":{}}},
  "validation":"required"
}}
```

[Python](../../examples/automation/pathtrace_capture.py) 支持 `--restir initial|temporal|spatial|joint`，
可组合 `--media --orthographic`；[Luau](../../examples/scripting/pathtrace-restir-capture.luau) 是固定正交 Scene media 联合复用示例。
两者从空历史渲染 3 spp，分别输出 path-python.json/path-luau.json；环境仍由已有场景决定。
