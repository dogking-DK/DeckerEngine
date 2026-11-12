---
module: command-reference-editor-viewport
created_at: "2026-11-03T14:56:20+08:00"
updated_at: "2026-11-12T16:55:54+08:00"
status: accepted
---

# Edit 视口会话命令

[返回命令目录](README.md)。这些命令由附加视口的 Editor 宿主提供，可从 Console、Editor IPC 和 Python SDK 调用；
普通 dk-run 不注册它们。GUI 工具栏、Path tracing 面板和光栅曝光调用同一个会话服务，修改后互相同步。
需要活动场景；目标始终是 guard 标识的 Edit 视口，Simulation 有独立相机和画面。
场景大气/太阳/云的持久参数仍使用 [environment.query/set](environment.md)，独立截图使用 [render.capture](render.md)。
规范命令与原 `restir on` / `raytracing iterate 12` 意图的对应、补全/面板检查和可运行 Python 示例见[跨入口指南](../guides/console-workflow.md)。
目标 spp 为绝对目标；图像完成需核对 presentation，不能以设置回执替代。

## 身份、状态和副作用

M23.6/12 的实体体素双后端 GUI/Console/IPC 和独立截图示例见[体素工作流](../guides/voxels.md)。

视口 guard 为 `{view_id,document_id,revision}`，从下述命令的直接返回值取得。
view_id 是本次附加视口的 UUID，document_id 是当前场景文档会话，revision 是视口设置与 Edit 相机共享的 uint64 修订号，初始为 0；GUI 或命令修改相机也会使旧 guard 失效。
它与 Scene guard 不同，不自动补齐。换文档保留设置但使旧 document_id 失效，关闭或替换视口使旧 view_id 失效。
修改成功后使用响应中的新 guard；重复提交旧 guard 返回 conflict。

query 的 effect=query；set/pause/resume/reset 的 effect=control；全部 undoable=false。
不改变 Scene revision/dirty、Undo/Redo、工程文件或 scene.save 内容，不允许嵌入 scene.transaction。
未应用 Inspector/Environment 草稿、打开的 Camera 弹层或活动 Edit Gizmo/导航手势拒绝写入，query 仍可调用。
任何参数/guard/能力检查失败均保留原设置和已发布画面；修改多个字段时一次校验、整体接受。
同值 set 和已处于目标状态的 pause/resume 不增加修订号或重置采样。

体素光栅设置也由 `editor.viewport.set` 接收并在 settings 返回：

| 字段 | 默认与约束 |
| --- | --- |
| voxel_shading | `unlit` 或 `directional`，默认 unlit；方向光要求非金属 palette |
| voxel_light_direction | `[0,0,1]`；指向光源的世界单位向量，长度误差最多 1e-6 |
| voxel_irradiance | `[1,1,1]`；每通道有限且位于 [0,1000000] |

这些字段不要求 RT 能力；只影响体素光栅显示，仍遵循同值/no-op、guard 和失败整体拒绝。
光栅 Scene 大气/云与体素组合尚不支持，可显式 `unlit_inspection=true` 忽略该视口的环境。
原生路径支持同资产、Surface only / Scene media 与 Classic MIS / 四种 ReSTIR DI，使用下述既有设置；
设备不支持路径时 `capabilities.pathtracing=false` 并给出原因，写入拒绝且 guard/已发布画面不变，光栅设置仍可使用。
路径拾取/Frame 与显示使用同一完成帧的 Scene、相机和 CPU 资产；刷新失败或迟到结果不覆盖该帧。
invalid_argument 的零向量/非法范围不会部分修改曝光或其他设置。GPU 材质/组合错误由 presentation.error 报告。

所有命令返回相同结构：

| 字段 | 含义 |
| --- | --- |
| guard | 当前视口写入身份，含 view_id/document_id/revision |
| settings | 下表所有设置，以及只读 paused、reset_sequence |
| capabilities | pathtracing 布尔能力与 unavailable 原因；能力通过也可能遇到具体场景/资源失败 |
| blocked_reason | 当前草稿或手势阻止写入的原因；空字符串表示没有该类阻止 |
| presentation | 最近一次 owner 绘制观察，区分已接受设置与已完成画面 |

presentation 包含 observed_guard、shown_guard（尚无图像时可为 null）、document_id、scene_revision、camera_revision、
renderer、width/height、has_image、current、phase、error、generation、spp 和 target_spp。尚无观察/图像时来源身份为零 UUID，不能拿它作写入 guard。
observed_guard 是观察时的设置身份；shown_guard 和场景/相机/尺寸来自实际已显示图像，失败时保留旧来源。
generation/target_spp 是路径 worker 进度，spp 是已显示样本数；光栅观察下这三个进度字段为 0。
设置接受后 current=false，匹配的完成图像发布后才为 true。渐进渲染可在 current=true 时继续向目标采样；
确认达到目标还需比较 spp/target_spp 和 phase。降低目标不会丢弃已完成样本，spp 可以大于 target_spp。
暂停后 reset 保留旧纹理但清空累积，新样本出现前 current=false。
后台渲染失败用 error/Failed 表示，保留上一完整图像；命令成功回执不保证 GPU 渲染成功。
隐藏视口不继续采样，phase 可为 Hidden；观察信息允许落后一帧，query 会立即识别新设置/新文档/新相机使旧观察过期。

## editor.viewport.query

参数为空对象，可以省略。返回上述状态，不需要 guard。

```text
editor.viewport.query
commands.describe name=editor.viewport.set
```

```json
{"jsonrpc":"2.0","id":1,"method":"editor.viewport.query"}
```

## editor.viewport.set

guard 必填，且至少提供一个设置字段。省略字段保持当前值；下表默认值指新建视口的初始值。
数字必须有限，整数参数不接受 `12.0`，未知字段拒绝。完整 schema/help 可由 commands.describe 查询并用于 Console 补全。

| 参数 | 类型、范围或枚举 | 初始值与含义 |
| --- | --- | --- |
| renderer | raster / pathtrace | raster；Edit 渲染器 |
| medium_mode | surface_only / scene_media | surface_only；路径是否追踪场景空气和云 |
| direct_lighting | classic / restir_di | classic；路径直接光照算法 |
| initial_candidates | 整数 1–64 | 8；ReSTIR 初始候选数 |
| temporal_reuse / spatial_reuse | boolean | false / false；ReSTIR 时空复用，Classic 下保留设置 |
| target_spp | 整数 1–4294967295 | 64；绝对采样目标，不是追加次数 |
| seed | 整数 0–4294967295 | 1；随机种子 |
| bounces | 整数 1–32 | 4；路径最大弹射数 |
| environment_rgb | 三个数字，各 0–1000 | [0.3,0.4,0.6]；路径常量环境辐射 |
| sky_enabled | boolean | true；启用上述环境辐射，关闭保留 RGB 值，不修改 Scene 大气 |
| sun | boolean | true；路径使用 Scene 太阳，Scene 未启用太阳时不凭空创建光源 |
| exposure_ev | 数字 -16–16 | 0；路径显示曝光，单位 EV |
| raster_exposure | 数字 0–1000000 | 1；光栅环境曝光倍率，与路径 EV 分开 |
| unlit_inspection | boolean | false；光栅无光照检查 |
| time_seconds | 数字 -1000000000–1000000000 | 0；Scene media 的固定时间，单位秒 |
| cloud_albedo | 数字 0–1 | 0.9；路径云单次散射反照率 |
| cloud_anisotropy | 数字 -0.95–0.95 | 0.6；路径云相函数参数 |

除 renderer/raster_exposure/unlit_inspection 外均为路径参数；没有路径模块或兼容 GPU 时，显式提交这些字段返回 not_supported，
即使值与默认值相同或当前仍是 raster。支持路径时可提前配置另一渲染模式的参数，当前模式不消费的设置保留。
曝光、目标与暂停保留已完成样本；算法、有效物理输入、介质、种子等变化沿已有路径 worker 规则使累积失效。

以下 `<view_id>`、`<document_id>` 和 revision 必须替换为 query 返回的实际值：

```text
editor.viewport.set guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} renderer=pathtrace direct_lighting=restir_di target_spp=12
```

相同设置的 IPC 请求：

```json
{"jsonrpc":"2.0","id":2,"method":"editor.viewport.set","params":{"guard":{"view_id":"<view_id>","document_id":"<document_id>","revision":0},"renderer":"pathtrace","direct_lighting":"restir_di","target_spp":12}}
```

## editor.viewport.pause / resume / reset

每条命令只接受必填 guard。要求当前 renderer=pathtrace 且路径能力可用。
pause 暂停后续工作，已提交的 GPU 工作可能完成；resume 继续到绝对 target_spp。
reset 每次递增 reset_sequence 和视口 revision；保持目标、算法、曝光与暂停状态，异步清空路径累积。
若保持暂停，将继续显示旧图像并标为过期，直到 resume 后产生新样本。

```text
editor.viewport.pause guard={"view_id":"<view_id>","document_id":"<document_id>","revision":1}
editor.viewport.resume guard={"view_id":"<view_id>","document_id":"<document_id>","revision":2}
editor.viewport.reset guard={"view_id":"<view_id>","document_id":"<document_id>","revision":3}
```

示例假定每步真实变更且没有其他入口修改；实际始终使用前一步响应 guard。

## 常见错误与 Python 示例

| 错误 | 原因与处理 |
| --- | --- |
| invalid_argument | 缺 guard、空 set、未知/错误类型/越界参数；按 describe 修正 |
| conflict | 视口、文档或设置修订号过期；重新 query 并核对目标 |
| invalid_state | 无活动场景、宿主已解除、未应用草稿/活动手势，或在 raster 模式调用路径控制 |
| not_supported | 构建或设备不支持显式要求的路径设置 |
| unknown method | 当前 Runtime 未附加 Editor 视口，例如普通 dk-run |

连接已启动并指定 `--pipe demo` 的 Editor；[SDK 配置](../guides/python.md)和 [IPC 启动](../guides/ipc.md)见相关指南：

```python
from decker import Client

client = Client("demo", "out/build/windows-editor/bin/Debug/dk-ctl.exe")
state = client.call("editor.viewport.query").value
state = client.call("editor.viewport.set", {
    "guard": state["guard"], "renderer": "pathtrace",
    "direct_lighting": "restir_di", "target_spp": 12,
}).value
state = client.call("editor.viewport.pause", {"guard": state["guard"]}).value
print(state["settings"], state["presentation"])
```

该示例只接受/暂停设置；不等待采样完成。没有 `restir on`、`iterate N` 位置参数别名；相机控制见 [M22.9 命令参考](editor-camera.md)。
