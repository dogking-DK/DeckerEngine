---
module: guide-voxels
created_at: "2026-11-12T09:48:00+08:00"
updated_at: "2026-11-12T10:31:11+08:00"
status: accepted
---

# 实体体素光栅工作流

M23.6 使用 OpenVDB/NanoVDB 实体材质索引，不是密度云或 SDF。文件格式、有限域、palette 与单位变换见
[体素设计](../design/render-voxels.md#m235-资产实施契约)。需要 `windows-voxel-editor` 预设；无 CUDA 依赖。

## 加载、编辑与刷新

工程的 voxel AssetRecord 指向 `.dkvoxel` 或编译产物 `manifest.json`，Scene 的 `dk.AssetReferences` 使用同一 ID 与 `kind=voxel`。
已有 [assets.import / assets.register / assets.load](../commands/assets.md) 支持该种类；
首次创建 meta 时显式提供 `outputs=[{key:"voxel/0",kind:"voxel"}]`，具体 schema 以 `commands.describe` 为准。
可通过 [entity.set_assets](../commands/entity.md) 绑定或替换资产，通过原有 Inspector 编辑整实体 TRS。
打开工程后 Editor 自动装配 mesh 与 voxel；视口点击按最近表面选择，Frame 使用整个有限域，Gizmo 提交沿用 Scene Undo/Redo。
文件内容在外部修改后点 Refresh；引用或路径改变会自动创建候选。错误保留旧画面并标记非 current，不允许用错误画面继续拾取。
不自动监视文件，不提供逐体素编辑。

## 显示设置

Viewport 的 Voxel settings 弹层支持 Unlit/Directional、单位世界光方向及线性 RGB irradiance。
编辑完各分量后点击 Apply 整体校验，Cancel 丢弃草稿；弹层打开后其他入口更新视口会使旧草稿 Apply 返回 conflict，重新打开即可读取新值。
GUI、Console、IPC 使用同一 [editor.viewport.set](../commands/editor-viewport.md) 服务：

```text
editor.viewport.query
editor.viewport.set guard={"view_id":"<查询值>","document_id":"<查询值>","revision":0} voxel_shading=directional voxel_light_direction=[0,0,-1] voxel_irradiance=[2,2,2] raster_exposure=1
```

使用实际返回的 guard。显示设置不改变 Scene dirty/revision/history；设置接受后还需等待 `presentation.current=true`。
混合场景方向光要求 mesh 材质与 voxel palette 均非金属；大气/云组合暂不支持，需主动启用 Unlit inspection，Scene 的环境数据仍保留。
路径追踪暂不支持体素，相关后端在 M23.8/9 接入。

## 独立截图

`render.capture` 明确接收相机、尺寸、曝光与相同体素参数；不会读取 Editor 当前相机、显示参数或未提交 Gizmo。
含体素任务在接受前冻结最多 64 MiB 的 CPU 资产；`provenance.asset_snapshot=accepted`，后续源文件替换不影响该任务。
任务 succeeded 表示 GPU 完成、读回和原子文件替换均完成。详见[截图命令](../commands/render.md#实体体素光栅截图)。

已打开/加载工程的 Editor 或 dk-run（例如 `--pipe voxel-demo`）可运行：

```powershell
$env:PYTHONPATH = "$PWD/sdk/python"
python examples/render/voxel_capture.py --endpoint voxel-demo --client out/build/windows-voxel-editor/bin/Debug/dk-ctl.exe --output voxel.ppm --directional
```

示例使用 eye=(0,0,-2)、target=(0,0,0)；实际工程按物体位置传 `--eye x y z --target x y z`。
输出父目录必须存在，覆盖同名 PPM；示例只读取已加载 Scene，不修改工程。
