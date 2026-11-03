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
- **Assets / Console**：查看项目资产映射和最近 64 条操作/失败信息；Console 可输入命名参数文本或 JSON 命令，见下文。

## 路径追踪预览（M16.9）

`windows-editor`预设默认启用`DK_BUILD_RENDER_PATHTRACING`。视口选择 **Path trace**，
右侧 **Path tracing** 面板调整目标spp、seed、反弹、常量环境、场景太阳和曝光EV，支持Pause/Resume和Reset accumulation。
默认64spp；修改曝光/目标保留已完成线性样本，相机、尺寸、积分设置、资产刷新或场景版本变化重新累积。
后台编译/资产准备/逐tile渲染期间主界面和IPC保持可用；旧图会标为STALE PREVIEW，当前输入的完整批次完成才替换。
切回Raster、隐藏/最小化视口或进入Simulation停止后续路径工作。Refresh重新加载资产并重试错误。

路径模式默认 **Surface only**；联合渲染场景空气/云时，在面板选择 **Scene media**，设置见下文。
可点击视口的 **Settings** 打开Path tracing面板。不支持的GPU/构建会显示禁用原因。
路径预览按设备可用显存、AS/纹理/缓冲能力准入；不再固定百万三角形、64纹理、128MiB源像素或1GiB额度。路径视口使用实际面板尺寸。
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
光栅环境要求 far≤1e6；透视另要求 far/near≤10000 和投影尺度范围，正交要求可见宽高各≤100000m；窗口变得过宽或外部启用环境后不兼容时，保留旧图并显示错误，可用 Camera 修改 FOV/裁剪或恢复默认值。
导航只能从实际图像内开始；文本输入、弹层、失焦、最小化、隐藏、切视图或关闭均释放鼠标，恢复后需重新按右键。
F 显式框选并更新 Walk 高度；Home 保留当前模式。

**Orthographic** 切换正交并保持当前焦平面尺度；从 Fly/Walk 切入时同时进入 Orbit，
回到 **Perspective** 恢复之前的导航模式。正交滚轮改变可见高度，不前后移动眼点；中键平移、右键自由环绕。
Camera 中可输入 **Orthographic height (m)**，范围 0.01–100000m。返回透视需要焦距落在 0.1–3000m，
越界会保留当前相机并报错，可先缩放或重新 Frame。Home/Reset 保留投影和高度，Restore defaults 恢复透视。

**Front / Back / Left / Right / Top / Bottom** 分别从世界 +Z/-Z/-X/+X/+Y/-Y 看向焦点；
前后左右上方为 +Y，Top 上方为 -Z，Bottom 上方为 +Z。右键环绕退出固定视向。
正交拾取、框选、Move/Rotate/Scale Gizmo 使用同一投影；模拟 **Frame cloth** 保留所选正交视向，两个视口互不影响。

带大气/云的 Edit 场景可直接选择 **Orthographic**，支持 Raster 与 Path trace（Surface only / Scene media）。
**Unlit inspection** 仅跳过当前视口的环境光照，不改 Scene 或 Undo；正交下也能切换该选项或渲染器。
投影或高度变化重置累积/云历史。正交介质从每个像素的近裁剪面开始；光栅眼点须在地面以上，
近面跨入行星内部的像素不透光。路径 Scene media 仍要求整个近面区域在其支持域内。
外部环境或窗口尺寸变化导致镜头超出支持范围时，保留最后图像、停止拾取并提示恢复方法。
固定 `render.capture` / `render.sequence` 可用 camera.projection=orthographic 和正数 ortho_height；省略投影仍为透视，
正交不能同时提供 fov_y。完整参数见[捕获命令](../commands/render.md)，相机约定见[相机设计](../design/editor-interaction.md#m18-相机扩展计划)。

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

命名参数文本、上下键命令历史和粘贴保护见下节，
实施状态见 [Roadmap M22](../roadmap.md#m22console-命令文本输入与-editor-交互)。

`dk.editor.media_pathtracing_gpu_validation` 使用独立夹具、--background 和 --media-pathtrace-smoke：真实点击模式/环境/时间/曝光、Console 输入，
与 IPC capture 比较冻结线性结果，连续错误恢复和换会话检查。该验收将路径画面固定为至多 96×96 以控制同步验证成本；
正常 Editor 分辨率不变，原路径验收继续覆盖正常视口、resize/minimize 与相机/实体编辑。不得把验收尺寸作为 Sponza 性能证据。

## Console 命令文本（M22）

在 Console 输入框逐行输入，按 Enter 或点击 Run 提交：

```text
scene.query
scene.query limit=20 offset=0
commands.describe name=entity.create
commands.list
```

格式为 `命令名 参数名=值`，允许空格/Tab 以及等号两侧空白；不支持位置参数或 `guard.revision=...`。
字符串可用裸词、JSON 双引号或原样单引号：`name="Hello World"`、`manifest='folder with spaces/project.json'`。
裸词 `true/false/null` 与合法 JSON 数字转换为对应类型，其他裸词为字符串；`1.0` 是浮点，不能用于整数字段。
数字或布尔文本作为字符串时必须加引号。整数保留完整 int64/uint64 精度，溢出直接拒绝。
数组和对象使用严格 JSON，例如 `assets=[]`、`transform={"translation":[1,2,3],"rotation":[0,0,0,1],"scale":[1,1,1]}`。

写命令仍显式提供 guard，UUID 与 revision 必须替换成当前查询结果：

```text
entity.set_name id="<entity_id>" name="Hello 中文" guard={"document_id":"<document_id>","revision":12}
```

单次只接受一行，不支持分号串联、管道、变量展开或注释。解析器拒绝超过 8192 UTF-8 字节、
实际换行/NUL、重复参数/JSON 键、未闭合字符串/括号或非法 UTF-8；完整解析通过后才分派，错误尾部不会执行合法前缀。
错误包含字符列号与 `byte=` 字节位置；非法 UTF-8 只报告字节位置。原合法单行 JSON `{method,params}` 仍可使用。

输入交互：

- 成功显示 `Succeeded` 并清空输入；异步返回 job_id 显示 `Accepted`，再用 `jobs.get id=<job_id>` 查询完成状态。
  失败显示错误并保留输入供修改；空白输入忽略。
- 候选弹窗关闭时，输入框按 Up 查看更早提交，Down 查看更新提交，越过最近一条恢复原草稿。边界不循环。
  回填只修改输入，再按 Enter/Run 才执行；编辑回填内容后退出历史浏览。输入法组合期间方向键/Enter 留给候选操作。
- 本窗口保留最近 64 条非空提交尝试（包括失败），裁掉首尾空格/Tab，连续相同文本只留一条。
  历史不写磁盘，重开窗口后清空；切换文档不重写历史中的 guard，旧 guard 会冲突。
- 输入上限为 8192 **UTF-8 字节**，中文通常占 3 字节。粘贴超限、多行或非法 UTF-8 会整段拒绝并保留现有输入，
  不执行被截断的前缀；选中文字后粘贴按替换后的总长度计算。输入达到上限后继续键入会提示拒绝。
- 日志保留最近 64 条，每条最多 4096 字节并保持完整 UTF-8 字符；长结果标记截断，完整结果通过 IPC 查询。

未应用草稿、缺失或过期 guard 继续由原契约拒绝；Console 的 jobs.wait 省略 timeout_ms 或显式整数 0 均为零等待。
看到 `Command executed, but view refresh failed` 表示业务已经成功、随后刷新失败，应先查询实际状态，不能直接假定未执行而重试。
参数 schema/单位/可用命令仍以 commands.list/describe 为准；本阶段没有新增 `restir`、`raytracing` 或 `cam.orbit` 控制命令。
设计及验证记录见 [Console](../design/editor-console.md)、[解析与执行](../development/0149-console-command-parser.md)、
[历史与 GUI](../development/0150-console-history-ui.md)、[基础补全](../development/0153-console-completion.md)、
[嵌套辅助与诊断](../development/0154-console-context-assistance.md)。

M22 后续扩展已纳入命令/参数补全、输入诊断、实体/资产候选、视口/相机命令以及 Editor 悬停帮助和设置搜索，
见 [扩展设计](../design/editor-console.md#m224-扩展设计帮助补全与设置命令) 与 [Roadmap](../roadmap.md#m22console-命令文本输入与-editor-交互)。
M22.5 已提供共享帮助信息，可以在 Console 输入 `commands.describe name=scene.query` 查看可选 help，
其中包含分页参数的说明、单位、默认值与示例；完整输出可通过 IPC 查询。

M22.6 支持命令名、顶层参数名和布尔/null/枚举值的基础补全，参数说明自动随光标显示：

| 输入与操作 | 结果 |
| --- | --- |
| 输入 `scene.qu`，Tab 打开，再按 Tab 或 Enter | 回填 `scene.query`，继续按 Enter 才执行 |
| 输入 `scene.query li`，Tab 打开并接受 | 回填 `limit=`；输入数值时显示类型、范围、默认值与示例 |
| 输入 `environment.preset name=`，Tab 打开 | Up/Down 选择预设，Tab/Enter 或鼠标点击回填带引号的字符串 |
| 输入 `render.capture renderer=`，Tab 打开 | 按本机实际注册 schema 列出 renderer 枚举；不会切换当前视口 |

第一按 Tab 打开候选，再按 Tab/Enter 接受；无候选时不修改输入。最多返回 32 项，列表最多显示 8 行，可滚动；
输入更长前缀可缩小范围。Esc 关闭候选/提示并保留原文；输入或光标改变后重新显示说明。
Run 直接提交当前原文，不代替用户接受候选。候选接受只改输入、退出历史浏览，不执行命令、不进入提交历史。
替换保留其他参数；整 token 选区可替换，部分 token 或跨字段选区保守不补全。超出 8192 字节时整次拒绝。
Tab 键用于补全，粘贴内容中的 Tab 仍保留为空白；输入法组合期间暂停辅助，不抢候选操作键。
M22.7 扩展到嵌套对象/数组与兼容 JSON envelope，以下输入在光标末尾按 Tab 打开并接受：

| 当前输入（故意尚未写完） | 候选或提示 |
| --- | --- |
| `entity.set_transform transform={"tr` | 补全 `"translation":`，其余必填项仍需填写 |
| `{"method":"scene.query","params":{"li` | 补全 `"limit":`；也支持 method 命令名补全 |
| `entity.get id=` | 按名称或 UUID 前缀筛选场景实体，接受后插入带引号 UUID |
| `entity.set_parent parent=` | 现有实体或 null；最终执行仍校验层级关系 |
| `entity.set_assets assets=[{"kind":"mesh","id":` | 当前工程的 mesh 身份；kind 未填时列出所有合法种类 |
| `assets.status id=` | 活动异步资产目录身份，需先显式 assets.open |

实体/资产候选显示名称或路径、种类和完整 UUID，重名仍可区分；不会为 entity.create 的新 ID 或 guard 提供已有身份。
当前工程引用与活动异步目录各取实际来源，补全不会打开目录、扫描磁盘或加载资产。无目录时提示不可用。
输入/光标/选区稳定 100 ms 后更新诊断与动态候选，每页最多 128 条；加载更多页期间显示提示，结果最多保留 32 项。
显式 Tab 可以立即显示静态候选，动态候选等待去抖；缩小前缀可筛选大型目录。场景/工程快照、目录或输入变化使旧候选失效。

诊断区分 `Incomplete`（缺值、未闭合、必填参数尚未输入）和 `Error`（已确定的类型/范围、重复/未知字段错误），
最多 16 条，每条给出字符列号和 UTF-8 字节位置。例如 `scene.query limit=` 为待补充，`limit=1.0` 为整数类型错误。
诊断只显示在辅助窗口，不进入命令日志/历史；业务前置条件、ID 存在性、guard 仍在最终 Enter/Run 时校验。
M22.8/9 已支持下述视口和相机命令；M22.10 增加下述字段帮助与设置搜索。

### 字段帮助与设置搜索（M22.10）

点击顶部工具栏的 **Settings** 打开搜索窗口，输入显示名、规范参数名或中文别名，例如
`相机 视场角`、`云 覆盖率`、`path.target_spp`、`simulation.gravity_y`。
空白分隔的多个词需同时匹配；英文不区分大小写。查询最多 256 UTF-8 字节，结果最多 64 项。
选择一项可查看面板路径、含义、实际 schema 类型/范围、已声明的单位/默认值，以及草稿、保存、Undo 和采样影响。
没有声明默认值时不猜测；`environment.set` 要求完整对象，字段帮助不自动补值。

**Locate** 打开对应面板、栏目或 Camera 弹层，滚动并高亮控件约 3 秒。
悬停控件也显示同一帮助；禁用控件仍可查看原因。定位不选实体、不切换渲染/导航模式、不 Apply 草稿。
例如 Classic MIS 下定位 `path.temporal_reuse`，会看到需要 ReSTIR DI 的禁用提示；仍由用户决定是否启用。
Edit 相机帮助不控制 Simulation 的独立相机。

有实际命令接口的字段提供 **Copy command** / **Insert into Console**：模板包含必填参数、完整必填对象和目标字段，
guard、UUID、revision、数值都以占位符保留。插入仅允许 Console 为空且没有输入法组合输入；已有文本保留，可改用复制。
插入不执行、不加入历史；先查询当前身份/guard，手动填齐占位符，再明确 Enter/Run。
Environment 的本地云预览质量、时间和阴影设置仍仅提供解释与定位，没有对应命令模板。
参考 [实现与验收记录](../development/0157-editor-settings-help.md)。

### Edit 视口设置命令（M22.8）

Console 先输入 `editor.viewport.query`，取得包含 view_id/document_id/revision 的视口 guard。
复制到下一条命令，可同时切换路径模式、ReSTIR 和绝对采样目标：

```text
editor.viewport.set guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} renderer=pathtrace direct_lighting=restir_di target_spp=12
```

占位符和 revision 必须替换成实际响应值。`editor.viewport.set direct_lighting=` 可按 Tab 查看枚举；
参数提示沿用共享 schema/help。GUI 改动、Console 和 Editor IPC 使用同一个会话，后续写入需使用最新 guard。
曝光用 exposure_ev，介质用 medium_mode=scene_media；暂停、继续、清空累积分别为 editor.viewport.pause/resume/reset，均只传 guard。
target_spp=12 表示总目标 12 spp，已有样本不会作为额外 12 次来累加。

命令成功表示设置已接受，查询 presentation.current、spp/target_spp、phase/error 确认实际画面。
设置不会改变 Scene dirty/Undo；有草稿或正在操作 Edit Gizmo/导航时先完成操作。关闭/重开文档会使旧 guard 失效。
这些命令仅在 Editor 宿主注册；完整字段、错误和 Python 用法见 [视口命令参考](../commands/editor-viewport.md)。

### Edit 相机命令（M22.9）

先输入 `editor.camera.query` 取得当前 guard，再提交明确单位的操作：

```text
editor.camera.orbit guard={"view_id":"<view_id>","document_id":"<document_id>","revision":0} yaw_deg=15 pitch_deg=20
```

占位符和 revision 替换为查询结果；角度单位为度。`editor.camera.set` 可修改模式、透视/正交、镜头、移速/灵敏度和姿态；
`pan right=0.2 up=0.1` 使用米，`zoom steps=1` 使用滚轮步数，均要求 Orbit 和最新 guard。
`frame` 接受实体或世界包围盒，`reset` 恢复姿态，`reset defaults=true` 同时恢复偏好。
完整命令都使用 `editor.camera.` 前缀；目前没有 `cam.orbit 15 20` 位置参数别名。

`editor.camera.set projection=` 按 Tab 可补全枚举；`editor.camera.frame entity=` 可按名称或 UUID 查找场景实体。
相机与视口设置共享 guard；GUI、Console、IPC 任一入口修改都会使旧 guard 失效。
外部写入遇到 Scene 草稿、Camera 弹层或活动 Gizmo/导航会拒绝。GUI 可保留 Scene 草稿继续导航；Simulation 相机独立。
移速/灵敏度等偏好不重置路径采样；姿态/镜头变化按现有渲染规则更新。
通过 `render_revision` 和 `presentation` 确认真实完成画面；完整范围、冲突及 Python 示例见[相机参考](../commands/editor-camera.md)。

## ReSTIR 直接光照

Path tracing 面板默认 **Classic MIS**。切到 **ReSTIR DI** 后，可以设置 **Initial candidates**（1–64），
并分别勾选 **Temporal reuse** 和 **Spatial reuse**。两个都关闭是初始 RIS；此选择与 Surface only / Scene media 独立。
设置只影响当前预览会话，不修改 Scene 或撤销历史；也不代表启用了 DLSS。

**Shown lighting** 与候选/复用信息来自实际已完成图像。面板同时显示采样 epoch 起点、历史年龄、有效 M 和复用/拒绝计数，
**Latest reset** 标出 worker 最近清空图像历史的原因。spp 只统计全图完成的样本，不把候选或内部 pass 当作 spp。
曝光、Pause/Resume 和目标 spp 不改变已有线性样本；Reset accumulation、算法/物理配置、尺寸/投影、Refresh 和文档变化会重置。
时间复用开启时，相机姿态变化可保留可重投影的采样历史；Scene media 的交互分辨率切换仍重置历史。
错误时保留旧图像及其来源，修正参数后可恢复。

使用 [pathtrace v3](../commands/render.md#restir-直接光照v3) 获取独立、可复现的固定 spp 捕获。
若要逐像素对照 GUI，先手动 Reset accumulation，再匹配尺寸/相机/seed/反弹/环境/介质/候选/复用和曝光。
捕获永远从空历史开始，不继承正在浏览的 GUI 历史。质量/性能与默认策略结论留待 M19.8。
