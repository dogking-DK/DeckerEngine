---
module: console-workflow-guide
created_at: "2026-11-03T18:29:00+08:00"
updated_at: "2026-11-03T18:40:35+08:00"
status: accepted
---

# Console、设置面板与 Python 的完整操作流程

这条流程使用 Editor 已注册的真实命令和共享帮助。输入格式、补全键位与历史详见
[Editor 指南](editor.md#console-命令文本m22)，字段契约见[视口命令](../commands/editor-viewport.md)和[相机命令](../commands/editor-camera.md)。
普通 `dk-run` 不提供 `editor.*`；需要打开有活动场景的 `dk-editor`。

## 在 Console 中发现、补全、修改并确认

1. 先输入下面两条查询，查看实际参数/单位/帮助与当前共享 guard：

   ```text
   commands.describe name=editor.camera.set
   editor.camera.query
   ```

2. 输入 `editor.camera.se`，按 Tab 打开候选，再按 Tab 或 Enter 接受 `editor.camera.set`。
   接受只改文本；继续输入 ` fov_y_deg=` 会显示角度单位和范围。把刚查询的 guard 完整复制到参数中：

   ```text
   editor.camera.set fov_y_deg=35 guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0}
   ```

   示例中的两个 UUID 和 revision 全部换成实际响应值，`0` 不是可复用默认值。关闭 Camera 弹层、处理未应用草稿后，明确点击 Run 或再次 Enter。

3. 顶部 **Settings** 搜索 `相机 视场角`，选中 **Vertical FOV** 后点击 **Locate**。
   Camera 弹层应显示 35 度；悬停可看同一字段帮助。定位没有执行修改；Cancel 只关闭弹层。
   如果改成 45 度再 Apply，Console/IPC 查询也会得到 45，之前的 guard 会失效。不要自动重试旧命令。

4. 旧 JSON 格式仍等价可用。若要恢复 35 度，先重新查询，把最新 guard 填入：

   ```json
   {"method":"editor.camera.set","params":{"guard":{"view_id":"<view_id>","document_id":"<document_id>","revision":0},"fov_y_deg":35}}
   ```

5. 若设备支持路径追踪，查询当前视口，再设置 ReSTIR 和**绝对**目标 12 spp：

   ```text
   commands.describe name=editor.viewport.set
   editor.viewport.query
   editor.viewport.set guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} renderer=pathtrace direct_lighting=restir_di target_spp=12
   ```

   也可在 `renderer=pa` 后 Tab 补全 `"pathtrace"`。用 Settings 搜索 `path.target_spp` 定位面板，确认显示 12。
   这不是“再迭代 12 次”：已有足够样本时可能立即满足；需要重新累积时显式执行 `editor.viewport.reset`，使用当时最新 guard。

6. 查询 `editor.camera.query` 或 `editor.viewport.query` 确认画面。
   设置回执成功仅表示状态接受；需检查 `presentation.current=true`、`has_image=true`、`error` 为空、文档身份相符，
   相机 `presentation.camera_revision` 与 `render_revision` 一致。路径还需 `renderer=pathtrace`、`spp >= target_spp`。
   隐藏视口或暂停时可能无法推进；失败时仍可能保留旧图，不要只凭屏幕上有图或命令成功作判断。

这些相机/视口修改都是会话状态，不进入 Scene/Undo，也不随 scene.save 保存。
Settings 的 Copy/Insert 会保留必填占位符；插入不执行，不替用户填 guard，不覆盖非空 Console。

## 用 Python 重复同一类设置

在仓库根启动 Editor，打开待观察的场景；启动参数不要求测试夹具：

```powershell
& ./out/build/windows-editor/bin/Debug/dk-editor.exe --root D:/your/project --pipe dk-demo
```

保持 Edit 视口可见、Orbit 模式、采样未暂停，并关闭 Camera 弹层及处理未应用草稿。在另一个终端运行：

```powershell
$env:PYTHONPATH = (Resolve-Path sdk/python).Path
python examples/automation/editor_settings.py --pipe dk-demo --ctl out/build/windows-editor/bin/Debug/dk-ctl.exe --target-spp 12 --yaw-deg 15 --pitch-deg 20 --timeout 30
```

[可运行示例](../../examples/automation/editor_settings.py)先发现命令、查询能力和阻止原因，再显式携带完整视口 guard，
设置路径/ReSTIR/绝对 spp，随后用前一条成功回执的 guard 执行角度 Orbit。
正 yaw 增加绕 pivot 方位角，正 pitch 抬高眼点，单位为度；它们不是像素或鼠标灵敏度。
最后在总时限内轮询只读状态，只有匹配相机的当前路径图像达到目标才返回 `status=passed`。

每条命令分别提交。后一步失败不会撤销已成功的前一步；失败 JSON 中 `accepted` 列出已收到成功回执的写入。
发生并发改动、渲染错误或超时即停止；没有写入重试、自动换 guard、保存场景或关闭 Editor。
若传输失败导致某次写入结果不确定，重新查询后由调用者决定下一步，不能把缺少回执当作没有执行。
默认 30 秒总期限可用 `--timeout` 调整到 120 秒以内；大场景不保证在该时间内收敛。

## 能力与限制

| 需求 | 当前接口与边界 |
| --- | --- |
| Console 命令 | `command key=value` 或单行 `{method,params}`；整数精度、完整 JSON 数组/对象、UTF-8 与输入预算保留 |
| 便捷位置语法 | `restir on`、`raytracing iterate 12`、`cam.orbit 15 20` 尚无别名；使用上述规范命令及命名参数 |
| 补全/诊断 | 命令、字段、枚举、嵌套 JSON、受限实体/资产候选；辅助不替代最终业务校验，不执行或填 guard |
| 共享帮助/搜索 | 主要面板类型、单位、约束、明确默认值、影响和禁用原因；稳定定位。不是所有 UI 的自动生成表单 |
| 场景光照物理参数 | `environment.query/set`，完整对象、Scene guard、Undo 与显式保存；区别于视口曝光/介质会话设置 |
| 本地云预览质量/时间/阴影 | 已有 GUI 设置可搜索定位；没有对应模板，不能假装存在命令接口 |
| 相机与视口身份 | 共享 `{view_id,document_id,revision}`；不能用 SDK 的 Scene `guard(state)` 代替。Simulation 相机独立 |
| 模拟参数 | `simulation.run`/控制命令及已有模拟面板；下一实验草稿不改变当前运行 |
| 脚本与传输 | Python/IPC 使用原 JSON-RPC 方法和参数；Console 文本未变成新的 Python/Luau 脚本语言，网络远程控制未新增 |

## 验收证据

M22 各阶段独立证据与本阶段实际通过、失败/修正、未运行项见 [0158 交付记录](../development/0158-console-workflow-delivery.md)。
新闭环在独立场景副本中执行：原生命令名/枚举补全、GUI 字段显示、GUI Apply、JSON 等价、旧 guard 拒绝、Python 示例与实际 PPM 来源核对。
它使用缩小的验收视口和 2 spp，不表示生产场景图像质量或实时性能通过。已有 Simulation GPU 发布频率问题仍单列在记录中。
