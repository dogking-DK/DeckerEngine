# 编辑器工作台

默认使用 Adobe Spectrum 浅色主题（白色/浅灰背景），包含其控件绘制、停靠与表格配色，保留中文系统字体和 DPI 缩放。
依赖仍由 vcpkg 管理；补丁来源和升级规则见 [Spectrum overlay](../../cmake/vcpkg-ports/imgui/SPECTRUM.md)。

Windows 可加 `--pipe NAME` 开放本机命令端点，用 `dk-ctl --pipe NAME --method scene.query` 查询。
构建、guard 编辑和重试见 [IPC 指南](ipc.md)。默认不开放 IPC；外部 `runtime.shutdown` 不自动保存。

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

Windows后台运行可加 `--background`：启动及程序恢复窗口时不抢焦点，并放在其他窗口后方；
仍正常渲染，用户可主动从任务栏切入。普通运行不加该参数，窗口照常自动弹出。

Release/RelWithDebInfo普通启动默认关闭Vulkan验证层；Debug默认尝试启用。`--validation`显式要求验证层，
`--no-validation`显式关闭。性能比较用优化配置且关闭验证层，正确性回归单独开启。
所有仓库编辑器自动化测试及GUI响应基准都使用后台模式；手动限帧截图也应加上：

```powershell
.\out\build\windows-editor\bin\Debug\dk-editor.exe --background --validation --frames 8 --screenshot out/editor-preview.ppm
```

- **Hierarchy**：显示实体层级，点击选择，与视口共用选择。存在未应用草稿时先 Apply 或 Revert。
- **Inspector**：修改名称、本地 translation、四元数 rotation（X/Y/Z/W）和 scale。
  Apply 以一次事务提交，Undo/Redo 使用引擎现有历史；无效变换或过期版本会报错并保留草稿。
- **Viewport**：预览当前内存版本，包括未保存编辑；显示 revision（光栅另显示draw count）。
  默认从 Sponza 相机位置开始，使用 unlit preview，HDR/天空盒仍只是项目测试资产。
  左键点击网格使用 CPU 三角形拾取，选择最近实体；点击空白清空选择。黄色框标示选择范围。
  拾取为双面几何查询，不检查纹理 alpha；不可逆零缩放实例不能拾取。
  工具栏选择 **Move / Rotate / Scale**，拖动红/绿/蓝本地轴或旋转环。
  拖动期间显示临时画面，松开生成一条撤销记录；**Esc**、窗口失焦、最小化或视口尺寸变化取消拖动。
  有 Inspector 草稿时先 Apply/Revert；拖动期间其他编辑入口禁用。
  **右键拖动**环绕、**中键拖动**平移、**滚轮**缩放；鼠标在视口中按 **F** 聚焦选择、**Home** 恢复默认相机。
  工具栏 **Frame / Reset** 提供相同操作。相机只影响会话视图，不改变场景文件和撤销历史。
  加载或渲染失败保留旧图，标记 STALE PREVIEW，并可 Refresh 重试。
- **Save / Open / Reload**：Save 先应用草稿，再原子保存场景文件；Open/Reload 或关闭前
  有未保存修改时提供 Save / Discard / Cancel。保存失败不会继续切换/关闭。
  本阶段不修改 manifest 和资产映射，也不提供新建项目向导。
- **Assets / Console**：查看项目资产映射和最近 64 条操作/失败信息；Console 可输入 JSON 命令，见下文。

## 路径追踪预览（M16.9）

`windows-editor`预设默认启用`DK_BUILD_RENDER_PATHTRACING`。视口选择 **Path trace**，
右侧 **Path tracing** 面板调整目标spp、seed、反弹、常量环境、场景太阳和曝光EV，支持Pause/Resume和Reset accumulation。
默认64spp；修改曝光/目标保留已完成线性样本，相机、尺寸、积分设置、资产刷新或场景版本变化重新累积。
后台编译/资产准备/逐tile渲染期间主界面和IPC保持可用；旧图会标为STALE PREVIEW，当前输入的完整批次完成才替换。
切回Raster、隐藏/最小化视口或进入Simulation停止后续路径工作。Refresh重新加载资产并重试错误。

路径模式默认 **Surface only**；联合渲染场景空气/云时，在面板选择 **Scene media**，设置见下文。
可点击视口的 **Settings** 打开Path tracing面板。不支持的GPU/构建会显示禁用原因。
编辑器额度为100万三角形、64纹理、纹理源总计128MiB及1GiB GPU实际分配，可预览默认Sponza。
导入沿用光栅的预览配置：保留base color纹理和材质常量，省略法线/AO/金属粗糙度纹理及切线；不支持blend。
完整大气及带云预设允许默认模型跨地面的几何，地下部分由行星遮挡，不因一个包围盒拒绝天空。
固定spp路径截图和线性报告通过[render.capture](../commands/render.md#固定-spp-路径追踪)提供，仍使用严格导入和原小场景额度；
不把默认Sponza视口支持视为capture能力扩展。小场景对比需显式匹配GUI相机、尺寸和Path参数。

`--desktop-render-smoke --root DISPOSABLE_PROJECT --pipe NAME --screenshot OUTPUT.ppm`使用正常相机和窗口，
由EditorDesktopRenderTest.py配合真实GUI与IPC，完成路径样本、大气/云连续拖动、命令启用云、稀疏云预设、
云影失败恢复、地下相机Home恢复和路径往返，项目必须有`.dk-editor-smoke`标记。
默认工程回归为`dk.editor.desktop_render_gpu_validation`，自动复制demo并使用`--background`；产物含各阶段PPM和JSON。

定向验收使用真实后台窗口和外部IPC：

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_editor_app','dk_ctl','dk_pathtracing_probe') -TestRegex '^dk\.(editor\.pathtracing_gpu_validation|pathtracing\.progressive_images_gpu_required)$' -Reason 'M16.9 GUI/IPC和GPU显示'
```


## Simulation面板（M12.1）

默认布局左下显示Simulation，Inspector保留右侧完整高度；参数位于Next experiment，包含CPU/GPU求解器、目标拍数、每批拍数、
固定纳秒步长及布片尺寸/seed/间距/高度/质量/compliance/迭代/重力/地面/阻尼。未编译GPU时仅提供CPU。
参数多时滚动面板查看。Reset draft恢复默认值，不改变当前运行。草稿仅保存在本次编辑器会话中。

- **Play**：异步初始化并执行目标拍数；默认1000000拍、每批8拍，按工作线程进度推进，不按墙钟帧率限速。
- **Prepare paused**：异步准备求解器，完成后保持0拍，适合从初态单步。
- **Pause / Resume**：请求暂停或继续；pausing尚未稳定，paused才表示暂停完成。
- **Step**：稳定paused时推进恰好1拍，完成后再次暂停；达到目标时显示succeeded。
- **Cancel**：结束有限任务并保留已完成结果；取消后不可恢复。
- **Stop**：回收当前运行，回到edit后才允许新建实验。错误/终态可通过Stop清理。

活动配置、run_id、状态、完成拍数及故障与Next experiment草稿分开显示；运行时修改草稿只影响下一次启动。
启动前先Apply/Revert未提交的Inspector草稿，参数错误会显示原因并保留输入。
外部`simulation.*`通过`--pipe`进入同一服务，面板随实际状态更新，不覆盖本地参数草稿。
旧run_id请求拒绝，Stop保留最新编辑场景及撤销历史；运行结果不会自动写回场景。
启动布片后Viewport自动显示实时模拟图像；指标采样与导出UI属于M12.3。

## 实时布片与相机（M12.2）

点击**Prepare paused**观察0拍初态，再用**Step**逐拍查看；**Play/Resume**持续更新CPU或GPU布片。
视口标注图像对应的完成步数，可能略晚于面板中的任务进度。暂停后最终图像保持稳定，移动相机仍可重绘。

- 首次运行自动取景；**Frame cloth**或鼠标在视口内按**F/Home**重新取景。
- **右键拖动**环绕、**中键拖动**平移、**滚轮**缩放；模拟相机与编辑相机相互独立。
- **Edit view / Simulation view**切换显示。模拟图像没有实体拾取和Gizmo；可切回编辑视图或使用Inspector编辑场景。
- **Stop**恢复最新编辑场景，保留原相机、选择、修改和撤销历史；缩放/最小化恢复后自动更新图像尺寸。
- 预览失败显示原因并保留最近有效图像，**Retry preview**重新请求。求解故障仍需Stop后重启。

GPU预览直接使用GPU输出与图像，不经CPU读回再上传；CPU预览只上传当前位置。预览最多约30Hz，
不是模拟墙钟限速；默认有限实验仍以工作线程速度运行。显示使用共享设备的第二队列，设备不具备该能力时明确提示不可用。

定向真实窗口验收：`dk.editor.simulation_gpu_validation`以ImGui输入操作CPU/GPU按钮、相机及窗口生命周期，
检查连续图像变化与暂停稳定，并保存带运行中布片的工作台截图；
只使用带`.dk-editor-smoke`标记的一次性项目。该模式启用required+同步验证，仅单列已知AMD加载层版本警告。

## 相机镜头与导航设置

Edit 和 Simulation 视口工具栏均有 **Camera** 按钮，各自保存独立的会话设置：

- 垂直 FOV 默认 65°；可拖动滑条或输入角度。**Auto clipping** 默认开启，光栅大气/云使用 near=1m，其余视图 near=0.05m，far=10000m；关闭后可输入 near/far。
- Orbit 灵敏度、平移倍率、滚轮系数和反转 Y 立即影响应用后的鼠标导航。
- **Orbit / Fly / Walk** 按钮切换模式并保持当前姿态。Orbit 保留右键环绕、中键平移、滚轮缩放。
- **Fly**：按住图像内右键，鼠标观察，W/S 沿观察方向前后移动，A/D 侧移，Q/E 沿世界 Y 下降/上升。
- **Walk**：按住右键观察，WASD 水平移动，固定进入模式时的高度；Q/E 不移动，没有碰撞、重力或跳跃。
- Fly/Walk 捕获期间滚轮调整基础移速，Shift 加速、Ctrl 精细（同时按下时 Ctrl 优先）；Camera 中可修改移速、倍率和观察灵敏度。松右键或 Esc 停止导航。
- **Apply** 校验整组设置后应用，**Cancel** 丢弃草稿。无效值保留原相机和草稿并显示原因。
- **Home / Reset** 只恢复视角、保留设置；**Restore defaults** 恢复默认相机、Orbit 模式和设置。模拟 **Frame cloth / F / Home** 按当前镜头重新取景。

镜头改变同步影响绘制、拾取、Gizmo 尺寸与聚焦；调移速或切导航模式不重置路径累积。相机设置不写入 Scene、Undo 或项目文件，退出后不保留。
光栅环境还受 far/near≤10000、far≤1e6 和投影尺度限制；窗口变得过宽或外部启用环境后不兼容时，保留旧图并显示错误，可用 Camera 修改 FOV/裁剪或恢复默认值。
导航只能从实际图像内开始；文本输入、弹层、失焦、最小化、隐藏、切视图或关闭均释放鼠标，恢复后需重新按右键。
F 显式框选并更新 Walk 高度；Home 保留当前模式。
正交仍未提供；其分期范围见 [相机设计](../design/editor-interaction.md#m18-相机扩展计划)。

## 编辑预览与外部命令

预览在版本或面板尺寸变化后同步渲染；每个文档会话复用 GPU 资产。
初版通过 RGBA8 读回/上传桥接 ImGui 纹理，大场景导入会短暂阻塞窗口。
当前只支持单选与本地轴 Gizmo；世界轴、吸附、多选尚未实现。

外部命令与 GUI 共用当前 Runtime 的场景与撤销历史。外部编辑成功后，Hierarchy、无草稿的
Inspector 和 Viewport 刷新到新 revision；已输入的 Inspector 草稿保留，Apply 时用旧 guard
报冲突，可点击 Revert 读取当前值。未提交草稿和拖动预览不属于 `render.capture` 的已提交版本。

比较视口与 [render.capture](../commands/render.md) 时，要使用相同 scene/revision、资产文件、
相机和像素尺寸。两者共用背景色与 exposure，但相机是编辑器会话状态，不写入场景：
截图需显式提供实际 eye/target/up、Camera 中生效的 fov_y/near/far 和 `profile=unlit_preview`，不能固定假设 FOV 仍为 65°。
截图默认 far=100；只提供 guard 不会自动取得 GUI 当前视图。
Save 后另启 runner 加载同一 manifest，scene_id/revision/实体保持一致，document_id 会更换。

定向验收：

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_tests','dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.' -Reason '编辑器工作台、交互与一致性验收'
```

GPU smoke 在独立复制的夹具中输入、点击真实 ImGui 控件，覆盖选择、编辑、事务、
Undo/Redo、Save、Reload、丢弃前取消、关闭取消、窗口 resize 和失败 Open；
检查 TRS 变更/撤销对应的预览像素、保存文件，并读回整个工作台为 PPM。证据在构建目录的 `test-artifacts/Debug/editor-*/`。
`--smoke` 要求项目内有 `.dk-editor-smoke` 标记，避免误写真实项目。
`--interaction-smoke` 使用相同保护，覆盖真实视口拾取、移动/旋转/缩放、Esc/失焦取消、
相机导航与像素变化；对应 CTest 还会启动 runner 读取保存的全部实体 TRS。
上述两项 smoke 必须启用 validation；环境不支持时 exit 77，不能计为通过。

只验收 M8.4 的 GUI/IPC/截图/runner 一致性：

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.consistency_gpu_validation$' -Reason 'M8.4 跨入口一致性'
```

此测试自动复制小型固定资产夹具，通过真实 ImGui 输入和独立 dk-ctl 进程交替编辑，
核对草稿冲突、旧 guard、重复请求、撤销/重做、旧版本截图及 Save/Reload；
最后关闭编辑器，由独立 runner 重现全部实体和图像。9 个窗口检查点和 runner 的离屏图像
逐字节比较，`acceptance.json`、相机/尺寸/版本摘要、RPC 回复、PPM 和工作台截图
保存在 `test-artifacts/Debug/editor-consistency-*/`。要求 Vulkan validation，无法运行视为验收失败。
脚本管理 `--consistency-smoke`、一次性标记和同步文件，不要对真实工程手动启用此模式。
精确像素验收仅覆盖相同设备/驱动、固定资产和匹配视图，不承诺跨 GPU 完全一致。

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
.\out\build\windows-editor\bin\Debug\dk-editor.exe --background --validation --frames 8 --screenshot out/editor-sponza.ppm
```

`--fixture-camera` 使用面向原点的小场景初始相机，仅供测试夹具。没有该参数时采用 Sponza 初始相机。
VS2026 用户打开 `out/build/windows-editor/DeckerEngine.slnx`，选择 Debug/x64，
将 Apps/dk_editor_app 设为启动项目。项目属性“配置属性/调试”的环境填入上述 AMD 变量（仅相同本机问题时需要），
合并环境保持“是”，命令参数可填 `--validation`，按 F5 运行。
详细边界见 [工作台设计](../design/editor.md)、[交互设计](../design/editor-interaction.md)，
验收证据见 [0065](../development/0065-editor-workbench.md)、[0066](../development/0066-editor-interaction.md)、
[0067](../development/0067-ipc-client.md)、[0068](../development/0068-editor-consistency.md)。


环境面板、后台大气视口、near/far与曝光约定、capture冻结信息和跨入口验收见
[大气使用指南](atmosphere.md#编辑器和脚本m146)。它使用同一Scene环境和Undo/Redo历史；
未应用草稿不改变视口，外部编辑导致旧guard冲突时先Revert再编辑。

云面板预设、固定时间/质量/阴影区域与序列截图的一致性设置见[云指南](clouds.md)。

## Scene media与Console（M17.5）

在 Viewport 选择 **Path trace**，Path tracing 面板选择 **Scene media**，再在 Environment 面板开启/应用空气或云。
**Surface only** 是默认模式，明确忽略场景空气和云。介质时间使用 Path tracing 面板的 **Time (s)**，不随墙钟推进，
与 Raster 的时间设置独立；云反照率和各向异性控制散射。相机、介质、种子、模式与会话变化重置累积，曝光只改变显示。
面板的 Shown 来源来自最后完整帧，显示模式、模型/采样版本、固定时间和场 hash；失败时旧图保留并标 STALE PREVIEW。
修改合法参数或按 Refresh 可恢复；GPU 不支持的尺度/工作额度仍会明确报错。Scene media 操作相机时使用长边192像素的临时预览，面板显示 Motion preview/Shown size；停动200ms恢复原尺寸并重新累积。
这只改善操作反馈，完整尺寸收敛仍需要等待；固定质量和性能边界见[测量指南](path-media-benchmark.md)。

Console 输入 `{"method":"scene.query","params":{}}` 后按 Enter 或 Run，取得当前 document_id/revision。
用 `{"method":"render.capture","params":{...}}` 提交[完整 v2 参数](../commands/render.md#统一介质路径v2)，guard 必须显式填写。
返回 job_id 后通过 `{"method":"jobs.get","params":{"id":"<job_id>"}}` 观察进度/终态；不隐式等待或保存 Scene。
输入最多 8192 字节，每条结果显示最多 4096 字节，完整 schema/结果可用 IPC；保留最近 64 条日志。
有未应用草稿时仅允许 query 命令，先 Apply/Revert 再修改或捕获；Console 的 jobs.wait 仅允许 timeout_ms=0，避免占用 GUI 线程。

`dk.editor.media_pathtracing_gpu_validation` 使用独立夹具、--background 和 --media-pathtrace-smoke：真实点击模式/环境/时间/曝光、Console 输入，
与 IPC capture 比较冻结线性结果，连续错误恢复和换会话检查。该验收将路径画面固定为至多 96×96 以控制同步验证成本；
正常 Editor 分辨率不变，原路径验收继续覆盖正常视口、resize/minimize 与相机/实体编辑。不得把验收尺寸作为 Sponza 性能证据。
