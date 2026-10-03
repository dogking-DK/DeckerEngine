---
module: render-command-reference
created_at: "2026-10-03T21:44:26+08:00"
updated_at: "2026-10-03T21:44:26+08:00"
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
| validation | if_available | if_available 或 required；required 缺少验证层时 Job 失败 |

camera 可选 up=[0,1,0]、fov_y=65（度，1&lt;值&lt;179）、near=0.05、far=100。
必须有限、near&gt;0、far&gt;near，eye 与 target 不同，up 不能平行于观察方向。
默认 eye=[8,1.8,0]、target=[-4,2,0]。坐标为世界空间，使用右手透视、顶部第一行。

返回 `{job_id,document_id,scene_id,revision,frame,width,height,output}`；
frame 在当前 Runtime 内从 1 递增，拒绝请求不消耗帧号。输出路径规范化为工程相对路径。
提交时复制 Scene 与 Project 映射，所以随后的编辑、撤销或场景替换不会影响该次截图。
资产文件在 worker 执行时读取；不保证外部资产文件被同时修改时的内容快照。

使用 [jobs.get / jobs.wait / jobs.cancel](jobs.md) 查询、等待和取消。
succeeded 的 result 为 `{kind:"capture",document_id,scene_id,revision,frame,width,height,output,draw_count}`。
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
命令尚不支持 PNG、PBR、透明混合、HDR 环境采样或天空盒。
