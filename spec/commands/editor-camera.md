---
module: command-reference-editor-camera
created_at: "2026-11-03T16:35:00+08:00"
updated_at: "2026-11-03T17:13:21+08:00"
status: accepted
---

# Edit 相机命令

[返回命令目录](README.md)。Editor 的 Console、IPC 和 Python SDK 通过以下命令控制 Edit 相机；普通 dk-run 不注册它们。
GUI 相机弹层、模式/投影按钮、鼠标导航和这些命令使用同一个会话状态。Simulation 相机独立，不是这些命令的目标。
场景/工程文件、实体 Transform、Scene revision/dirty 和 Undo/Redo 均不改变；相机偏好不随 scene.save 保存。

## Guard、状态和冲突

相机与 [Edit 视口设置](editor-viewport.md)共享 `{view_id,document_id,revision}` guard。
从 editor.camera.query、editor.viewport.query 或成功写入响应取得；GUI 相机改动、视口设置改动均使旧 guard 失效。
换文档保留相机偏好但使旧 document_id 失效，关闭/替换宿主使旧 view_id 失效。Scene guard 不能替代此 guard，也不自动补齐。
同值设置和零增量不增加修订号。命令先构造候选并完整验证，再一次发布；拒绝时保留原相机、设置、草稿和图像。

未应用 Inspector/Environment 草稿、打开的 Camera 弹层或活动 Edit Gizmo/导航会拒绝外部写入；query 仍允许。
显式 GUI 操作保留 M18 的行为：用户可以保留 Scene 草稿继续移动相机或完成自己的 Camera 弹层，草稿内容不会被覆盖；Gizmo 仍阻止相机修改。
query 的 effect=query，其他命令为 control；全部 undoable=false，不允许进入 scene.transaction。

所有命令返回以下字段：

| 字段 | 含义 |
| --- | --- |
| guard | 当前共享视口/相机写入身份 |
| settings | 下表中的当前设置；standard_view 还可能为 none |
| render_revision | 实际姿态/投影的渲染修订号；只改速度、灵敏度、导航模式或 focus_distance 不改变实际视图时保持 |
| effective_clip | 当前实际 near_plane/far_plane，单位米；可能不同于 settings 中保留的手动值 |
| aspect | 最近一次 Edit 视口的宽高比，初始为 1；隐藏视口保留最近比例 |
| blocked_reason | 当前阻止外部写入的草稿/手势原因；空串表示没有这类阻止 |
| presentation | 实际显示画面的视口 guard、文档/场景/相机修订号、尺寸、采样和错误；字段见[视口参考](editor-viewport.md) |

修改成功表示相机已接受，实际画面异步更新。用 presentation.current 和 camera_revision 对照 render_revision；
路径还可比较 spp/target_spp 和 phase。失败保留上一完整画面及其来源，不把新姿态回执视为新画面完成。
自动裁剪按当前文档环境、renderer 和 unlit_inspection 推导；查询即时刷新派生上下文，不等待下一帧 UI，不修改偏好或 guard。
导航偏好改变后，下一次观察可确认旧图仍适用，不会因此丢弃已有路径样本。

## editor.camera.query

参数为空对象，可以省略；需要活动场景及已附加的 Edit 宿主，无需 guard。

```text
editor.camera.query
commands.describe name=editor.camera.set
```

## editor.camera.set

guard 必填，至少提供一个设置；省略字段保持当前值。所有数值必须有限，未知字段拒绝。
命令名、参数和枚举支持既有 Console 补全/提示；提示中的范围不能替代投影/后端联合校验。

| 参数 | 类型/范围 | 新建视口初始值与语义 |
| --- | --- | --- |
| navigation_mode | orbit / fly / walk | orbit；Walk 为固定世界 Y，无碰撞/重力/跳跃 |
| projection | perspective / orthographic | perspective；切换按焦平面换算尺度，正交只允许 Orbit |
| standard_view | front / back / left / right / top / bottom | 无固定视向；仅正交，眼点分别位于 pivot 的 +Z/-Z/-X/+X/+Y/-Y |
| fov_y_deg | 数字，严格 1 < 值 < 179 | 65；垂直 FOV，单位度，正交时保留 |
| ortho_height | 数字 0.01–100000 | 2；整个正交可见高度，单位米；切入正交时先由焦平面换算 |
| automatic_clip | boolean | true；自动裁剪 |
| near_plane / far_plane | 有限 0 < near < far | 0.05 / 10000 米；手动值，automatic_clip=true 时保留但不生效 |
| move_speed | 数字 0.01–1000 | 3 m/s；Fly/Walk 基础速度 |
| fast_multiplier / slow_multiplier | 1–100 / 0.01–1 | 4 / 0.25；Shift/Ctrl 倍率，Ctrl 优先 |
| look_sensitivity / orbit_sensitivity | 各 0.0001–0.05 | 0.003 / 0.006 rad/逻辑像素；鼠标灵敏度 |
| pan_multiplier / wheel_sensitivity | 0.01–10 / 0.01–1 | 1 / 0.15；鼠标平移倍率与滚轮指数系数 |
| invert_y | boolean | false；鼠标 Y 反转，不反转命令的角度/米数符号 |
| eye | 三元组，各 -1e9–1e9 | 默认 [8,1.8,0] 米；世界眼点，验收夹具默认 [0,0,-2] |
| yaw_deg / pitch_deg | -180–180 / -90–90 | 相机后轴的方位角/仰角，角度单位度；默认姿态朝向 [-4,2,0]，无 roll |
| focus_distance | 数字 0.1–3000 | 默认眼点与目标距离；pivot=eye+forward×focus_distance |

先处理投影换算，再处理其他设置。切入正交会保存透视导航模式，返回透视恢复该模式；同一 set 可显式指定新的 navigation_mode。
若旧正交高度无法换算到合法透视距离，应先单独修正高度，再切换投影；不会截断距离破坏构图。
显式 eye/yaw_deg/pitch_deg/focus_distance 与 standard_view 互斥；普通姿态修改退出固定视向高亮，同值姿态往返保留当前视向。
姿态角沿用 Orbit 约定：后轴为 `[cos(pitch)sin(yaw), sin(pitch), cos(pitch)cos(yaw)]`，实际观察方向为其负方向。
yaw=pitch=0 看向 -Z，正 pitch 使眼点相对 pivot 升高并向下看；命令入参用度，三角函数按换算后的弧度解释。
镜头须通过当前 aspect 和后端支持域；光栅空气/云的跨度及裁剪限制见[相机设计](../design/editor-interaction.md)。

以下 UUID 和 revision 需要替换为 query 的实际返回值：

```text
editor.camera.set guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} projection=orthographic ortho_height=3 standard_view=front
```

## editor.camera.orbit / pan / zoom

三者都要求 guard 和当前 navigation_mode=orbit；支持透视和正交。

| 命令 | 参数 | 行为 |
| --- | --- | --- |
| orbit | yaw_deg（-360–360）、pitch_deg（-180–180），至少一个轴，另一个默认 0 | 围绕当前 pivot 的角度增量；正 yaw 增加方位角，正 pitch 抬高眼点；自由俯仰饱和到 ±1.55 rad |
| pan | right、up（各 -1e6–1e6 米），至少一个轴，另一个默认 0 | 沿相机右/上轴平移，保持朝向与焦距；不受鼠标像素或 pan_multiplier 影响，最终眼点须在 ±1e9 米内 |
| zoom | steps 必填，-1000–1000 | 滚轮步数，正值拉近；距离或正交高度乘 exp(-steps×wheel_sensitivity)，沿用已有指数/距离/高度饱和规则 |

```text
editor.camera.orbit guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} yaw_deg=15 pitch_deg=20
editor.camera.pan guard={"view_id":"<view_id>","document_id":"<document_id>","revision":1} right=0.2 up=0.1
editor.camera.zoom guard={"view_id":"<view_id>","document_id":"<document_id>","revision":2} steps=1
```

每步使用最新 guard；上述 revision 仅为示意。正交 zoom 只改可见高度，透视 zoom 沿观察轴改距离。

## editor.camera.frame

guard 必填；entity UUID 与 bounds 对象恰选一个。bounds 为 `{minimum:[x,y,z],maximum:[x,y,z]}`，
每轴 minimum<=maximum，所有分量有限且在 ±1e9 世界米内。按当前镜头、比例和裁剪面框住边界；无法完整容纳时拒绝，不改变相机。
Frame 支持三种导航模式；Walk 的显式 Frame 可以改变高度，之后的连续漫游保持新高度。

entity 使用当前已发布 Edit 几何（含实体子树）；必须是同一文档/Scene revision 的当前画面，且正在显示 Edit。
没有准备好或图像来源过期时返回 invalid_state，不加载磁盘或等待 GPU。无网格实体沿用 GUI 的世界位置小包围盒。
实体 ID 支持既有动态候选，删除/切换文档仍在执行时重新核对。

```text
editor.camera.frame guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} bounds={"minimum":[-1,-1,-1],"maximum":[1,1,1]}
```

## editor.camera.reset

guard 必填；可选 defaults 布尔，缺省 false。false 只恢复本视口默认姿态，保留模式、投影、镜头和导航设置；
true 同时恢复这些设置。与 GUI Home/Reset 和 Restore defaults 一致，不清除场景或选择。

## 错误与 IPC/Python 示例

invalid_argument 表示字段/类型/范围/组合不合法或镜头无法使用；conflict 表示 guard 过期；
invalid_state 表示无场景/宿主、草稿/活动手势、错误导航模式或尚无当前 Frame 几何；not_supported 表示正交 Fly/Walk 等不支持组合；
not_found 表示实体不存在；向普通 runner 发起这些未注册 method 返回未知命令（-32601）。失败后先修正问题或重新 query，不自动重试旧 guard。

IPC 格式沿用 JSON-RPC：

```json
{"jsonrpc":"2.0","id":1,"method":"editor.camera.query"}
```

连接已用 `--pipe demo` 启动的 Editor（[SDK 配置](../guides/python.md)）：

```python
from decker import Client

client = Client("demo", "out/build/windows-editor/bin/Debug/dk-ctl.exe")
state = client.call("editor.camera.query").value
state = client.call("editor.camera.orbit", {
    "guard": state["guard"], "yaw_deg": 15, "pitch_deg": 20,
}).value
print(state["settings"], state["presentation"])
```

示例要求 Orbit；回执不等待新画面。当前使用命名参数，尚无 `cam.orbit 15 20` 等位置参数别名。
