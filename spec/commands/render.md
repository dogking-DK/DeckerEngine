---
module: render-command-reference
created_at: "2026-10-03T21:44:26+08:00"
updated_at: "2026-10-13T14:37:00+08:00"
status: accepted
---

# 场景截图

[返回命令目录](README.md)。需要 DK_BUILD_RENDER_CAPTURE（windows-graphics 默认开启）。
无需窗口；设备延迟到后台 capture 执行时创建。

## render.capture

对活动内存 Scene 提交截图，effect=external、undoable=false，不允许进入 scene.transaction。
必须提供当前 guard；成功提交不改变 Scene revision、dirty 或撤销历史，也不隐式保存。

| 参数 | 必填/默认 | 含义 |
| --- | --- | --- |
| guard | 必填 | document_id + revision，必须匹配当前活动文档 |
| output | 必填 | 工程根内相对 .ppm 路径，1–1024 UTF-8 字节；父目录须存在 |
| width / height | 960 / 540 | 各为 1–2048 的整数 |
| camera | 默认 Sponza 相机 | 对象，提供时 eye[3] 和 target[3] 必填；其余可省略 |
| profile | unlit_preview | strict 或 unlit_preview；后者保留基础色/alpha mask，省略切线和光照纹理 |
| exposure | 1 | 有限线性曝光，0–1000000；HDR合成后tone时使用 |
| validation | if_available | if_available 或 required；required 缺少验证层时 Job 失败 |

camera 可选 up=[0,1,0]、fov_y=65（度，1&lt;值&lt;179）、near=0.05、far=100。
必须有限、near&gt;0、far&gt;near，eye 与 target 不同，up 不能平行于观察方向。
默认 eye=[8,1.8,0]、target=[-4,2,0]。坐标为世界空间，使用右手透视、顶部第一行。

关闭环境时与编辑器共用线性背景色 (0.04,0.08,0.16)；启用时按冻结环境执行天空、Lambert日光/天空漫射和空气透视。
需要 DK_BUILD_RENDER_ATMOSPHERE，否则启用环境的 Job 返回 not_supported。
非零云使用无历史混合介质参考，另需 DK_BUILD_RENDER_CLOUDS，当前总像素≤4096；
超过上限的 Job 失败且保留原输出。可使用64×64或更小截图，云大分辨率优化尚未接入。
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
其余状态 result=null；failed 的 error 记录导入、GPU 或 IO 错误；提交响应提供失败任务的原始版本关联。
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
命令尚不支持 PNG、PBR、透明混合、HDR 环境贴图采样或天空盒。

`provenance`包含完整环境（与environment.query同结构）、camera（含缺省up/fov_y/near/far）、
exposure、profile、time_seconds=0、quality（atmosphere_v1、unlit_v1或cloud_reference_v1）及shader_fingerprint。
时间固定0，质量不可覆盖；atmosphere_v1固定LUT/采样和64×64×128空气透视网格，
cloud_reference_v1固定逐区间256视线步、64云太阳步、64大气太阳步，无时域历史。
提交时shader_fingerprint=null；成功结果为`fnv1a64:`加16位小写十六进制，覆盖本次shader源、共享传输文件和算法版本。
它用于追踪源码变化，不是加密校验或跨驱动像素一致承诺；shader在执行中变化则Job失败，不发布。
元数据留在提交/Job响应中，调用者应保存；PPM不嵌元数据，也不隐式创建sidecar。

可运行的环境示例：[Python](../../examples/automation/environment_capture.py)、
[Luau](../../examples/scripting/environment-capture.luau)，操作说明见[大气指南](../guides/atmosphere.md)。
