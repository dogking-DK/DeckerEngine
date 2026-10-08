# 编辑器工作台（M8.1）

在仓库根配置和启动：

```powershell
cmake --preset windows-editor
cmake --build out/build/windows-editor --config Debug --target dk_editor_app
.\out\build\windows-editor\bin\Debug\dk-editor.exe --validation
```

默认打开 [demo 项目](../../projects/demo/project.json)，使用本地 Sponza 测试资产。
资产准备说明见 [默认测试资产](../../projects/demo/assets/README.md)。缺少 glTF/纹理时，
工作台仍显示文档和面板，Viewport 的失败原因显示在 Console；补齐后点击 Refresh。

其他项目使用 `--root PROJECT_ROOT --manifest project.json`。Open 输入项目根内的相对 manifest；
切换项目根请重新启动。窗口面板可停靠、移动和调整尺寸，布局仅在当前会话保留。

- **Hierarchy**：显示实体层级，点击选择。存在未应用草稿时先 Apply 或 Revert。
- **Inspector**：修改名称、本地 translation、四元数 rotation（X/Y/Z/W）和 scale。
  Apply 以一次事务提交，Undo/Redo 使用引擎现有历史；无效变换或过期版本会报错并保留草稿。
- **Viewport**：预览当前内存版本，包括未保存编辑；显示 revision 和 draw count。
  默认使用 Sponza 固定相机与 unlit preview，HDR/天空盒仍只是项目测试资产。
  加载或渲染失败保留旧图，标记 STALE PREVIEW，并可 Refresh 重试。
- **Save / Open / Reload**：Save 先应用草稿，再原子保存场景文件；Open/Reload 或关闭前
  有未保存修改时提供 Save / Discard / Cancel。保存失败不会继续切换/关闭。
  本阶段不修改 manifest 和资产映射，也不提供新建项目向导。
- **Assets / Console**：只读查看项目资产映射和最近 64 条操作/失败信息。

预览在版本或面板尺寸变化后同步渲染；每个文档会话复用 GPU 资产。
初版通过 RGBA8 读回/上传桥接 ImGui 纹理，大场景导入会短暂阻塞窗口。
CPU 拾取、Gizmo 和相机导航属于 M8.2；IPC 属于 M8.3。

定向验收：

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_tests','dk_editor_app') -TestRegex '^dk\.editor\.' -Reason '编辑器工作台验收'
```

GPU smoke 在独立复制的夹具中输入、点击真实 ImGui 控件，覆盖选择、编辑、事务、
Undo/Redo、Save、Reload、丢弃前取消、关闭取消、窗口 resize 和失败 Open；
检查 TRS 变更/撤销对应的预览像素、保存文件，并读回整个工作台为 PPM。证据在构建目录的 `test-artifacts/Debug/editor-*/`。
`--smoke` 要求项目内有 `.dk-editor-smoke` 标记，避免误写真实项目。
validation 必须启用；环境不支持时 exit 77，不能计为通过。
本机存在此前已定位的 AMD Switchable Graphics 隐式层冲突，验收仅在进程环境设置
DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，保留 Khronos 与同步验证。若使用相同本机环境，启动可采用：

```powershell
$previousAmdLayer = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./out/build/windows-editor/bin/Debug/dk-editor.exe --validation
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previousAmdLayer }
```

只读、限帧预览可用：

```powershell
.\out\build\windows-editor\bin\Debug\dk-editor.exe --validation --frames 8 --screenshot out/editor-sponza.ppm
```

`--fixture-camera` 使用面向原点的小场景相机，仅供测试夹具。没有该参数时采用 Sponza 相机。
详细边界见 [设计](../design/editor.md)，验收证据见 [0065](../development/0065-editor-workbench.md)。