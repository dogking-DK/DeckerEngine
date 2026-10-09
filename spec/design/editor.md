---
module: editor
created_at: "2026-10-08T09:36:00+08:00"
updated_at: "2026-10-09T19:38:24+08:00"
status: accepted
---

# M8.1 编辑器工作台

## 边界

`dk-editor` 装配 SDL3、ImGui docking、Vulkan Presenter、ScenePipeline 和 Runtime。
`dk_editor_model` 依赖 Runtime/Geometry，保存选择、Inspector 草稿、只读快照并提供相机/手势计算；不访问 ECS。
`dk_editor` 实现窗口 UI 与渲染桥，私有依赖 ImGui/SDL3。新增 `DK_BUILD_EDITOR` 默认 OFF，
`windows-editor` 启用已有呈现/渲染模块。CPU-only Runtime 不新增窗口或 GPU 依赖。
ImGui 使用固定 baseline 中的 1.92.9 docking/SDL3/Vulkan backend；本次核验官方 port 仍为该版本。
SDL3 复用已接入版本，不升级 baseline。
ImGui overlay 保留官方 port/tag/hash，仅对该库启用 VK_NO_PROTOTYPES，
由 Device 借用 resolver 初始化 backend 私有函数表，避免其直接调用与 volk 同名的全局符号。

## 工作流与命令

启动接受项目根和相对 manifest，默认 `projects/demo/project.json`。
Hierarchy 显示父子结构并按稳定 EntityId 选择；Inspector 显示身份/资产引用，编辑名称与本地 TRS。
Apply 把两个编辑放入已有 `scene.transaction`，用草稿读取时的 document_id/revision，产生一个撤销单元。
Undo/Redo、Save、Open/Reload 复用 Runtime 命令，错误显示在 Console；不增加命令或另建历史。
Save 使用 `scene.save` 原子保存当前场景，manifest/资产映射在本阶段只读，不需要双文件事务。
Open 仅改变当前 root 内的 manifest；切换项目根通过重新启动。只读资产面板显示项目映射。

## 状态、失败与关闭

Model 持有当前拥有型 SceneReadSnapshot；Runtime 增加 guard 校验的只读快照转发。
编辑提交点仍为 SceneService 发布，成功后刷新；保存失败保留 dirty、编辑和历史。
换场景先由服务构造候选，失败保留旧会话/选择/视图，成功清空选择及草稿，历史按已有服务重置。
选择变化不修改文档；未应用草稿有单独 dirty 标记。切换选择、撤销/重做前必须处理草稿；
关闭/打开/重载遇到场景 dirty 或草稿时显示 Save / Discard / Cancel，Save 先 Apply 再保存，任何失败不继续。
当前草稿过期时拒绝 Apply，显示冲突，允许 Revert 从当前快照重新读取。

## IPC 接入

Windows `dk-editor --pipe NAME` 显式开启与 runner 相同的服务端。Workspace 持有服务端，
窗口主线程在每次循环（包括最小化时）pump Runtime 和 IPC，外部请求经同一命令注册表执行。
处理后刷新只读快照和历史；同一文档的未应用草稿保留旧 guard，Apply 明确冲突，不能悄悄覆盖。
换文档清空选择/草稿，旧 Gizmo 由现有 document/revision 校验取消。
外部 runtime.shutdown 是显式退出指令，不保存草稿/场景；回复后排空管道再销毁 GPU/窗口。
基本接入在 M8.3 验证，M8.4 按下述跨入口契约验收。

## M8.4 跨入口一致性

Runtime 是唯一已提交状态所有者。GUI Apply、外部 dk-ctl 以及 Undo/Redo 都走同一命令/历史；
scene_id/revision 标识内容版本，document_id 标识当前打开会话。Reload 或新 runner 会改变 document_id，
但保存后的 SceneId/revision/实体内容和指定视图图像应保持一致。未应用 Inspector 草稿与 Gizmo preview
不属于已提交场景，不能把它们的像素当成同 revision 的正式截图。

Viewport 与 CaptureService 共用 `render::unlit_preview_settings()`：线性背景 (0.04,0.08,0.16)、
exposure=1、unlit_preview 导入。比较时显式传递已发布视口的尺寸、相机 eye/target/up、65 度 FOV、
near=0.05/far=10000；不把“同 revision”误当成“不同相机也应同图”。资产文件在验收期间不变。
同设备、相同资产/视图/设置逐字节比较 RGB8 PPM；跨设备/驱动的像素完全一致不作承诺。

增加仅用于一次性夹具的 `--consistency-smoke`（要求 `.dk-editor-smoke`、`--pipe`、
`--fixture-camera` 和 `--screenshot`，与其他 smoke/frames 互斥）。私有 ConsistencyDriver 注入真实
ImGui 鼠标/键盘输入；外部脚本通过真实 dk-ctl 子进程调用公开命令。固定 checkpoint/continue 文件
仅同步测试步骤，不增加业务命令或绕过 guard，180 秒内未完成则失败。

checkpoint 在窗口 GPU 提交完成后发布：先写实际视口已上传的 RGB8 像素，再原子写 JSON 摘要。
摘要保存当前 document/scene/revision/dirty、选择/Inspector/历史以及已发布视口的身份、相机和尺寸；
必须没有活动 preview、无过期视口、相机匹配。测试模式才保留该帧 CPU 像素，常规编辑器不增加常驻副本。
窗口最终截图额外证明真实工作台呈现；PPM 比较使用不含 UI overlay 的原始视口，避免窗口布局影响结果。

验收顺序：初始图 → GUI 名称/TRS Apply → 外部事务（含重复 ticket 与旧 guard 拒绝）→
未提交草稿期间外部编辑 → GUI 旧草稿 Apply 冲突/Revert → GUI Undo/Redo → Save/Reload →
关闭编辑器 → runner 重载、核对全部实体及原视图截图。旧版本 capture 先提交再编辑，完成结果和图像
仍必须对应原版本；任何 capture 不隐式保存。拒绝过期请求、草稿冲突不改 revision/history/文件。
M8.3 的超时/断连去重用例按传输回归复用，不额外改变 IPC 协议。

## 窗口与 GPU 接入

Platform 提供显式 SDL 借用句柄、同步事件 sink 和清除关闭请求；事件借用仅在回调期间有效，
SDL 的初始化、窗口销毁和主线程契约仍由 Platform 管理。Presenter Frame 暴露本代 image_count。
ImGui backend 是唯一原生 Vulkan 接入点，经 CommandBatch::unsafe_record 显式声明 framebuffer
写与 viewport 采样，并保留 ImageView；封装层继续管理提交、同步、交换链和资源寿命。
Windows 可用时读取系统微软雅黑字体（不打包字体），支持中文名称；无该字体时使用 ImGui 默认字体。
sRGB 交换链上的 UI 样式先转为线性颜色，Viewport 纹理单独按 sRGB 解码。
每帧等待提交完成后再更新/释放 backend 纹理；关闭先排空 GPU，再关闭 ImGui，最后释放 Presenter/Window/Memory。
交换链格式变化时重建 ImGui pipeline，图像数变化同步 backend 配置；最小化暂停并保留模型。

Viewport 基于当前内存快照，默认 Sponza 相机；M8.2 增加会话相机、CPU 拾取和独立 TRS 手势预览，详见 [交互设计](editor-interaction.md)。按文档版本/面板尺寸/相机与手势序号失效后绘制，非持续重导入。
每个文档会话只加载一次 CPU/GPU 资产，编辑变换时重新提取 RenderScene；显式 Reload 可重读资产。
渲染结果的 RGBA8 已编码 sRGB，初版通过现有读回和上传桥发布 sRGB 采样纹理，避免再次编码。
候选图像及描述符成功后替换旧预览；失败保留旧图并明确显示实际 revision/过期状态，可重试。
此同步预览在大场景加载时会阻塞 UI；后台流式加载和取消不属于 M8.1。

## 验收与后续

CPU 定向测试覆盖选择/草稿、事务与撤销、过期 guard、保存重载与失败保护。
真实窗口 smoke 用 ImGui 输入事件走选择/Inspector/保存/重载/撤销按钮，检查版本与持久化，
验证 TRS 改动使预览像素变化、Undo 逐字节哈希恢复，再读回工作台画面并启用 Vulkan validation；
默认 Sponza 另做实际预览。
M8.2 的 CPU rayquery、Gizmo 和相机交互见 [交互设计](editor-interaction.md)；M8.3 IPC 见 [传输设计](automation-transport.md)。M8.4 证据见 [0068](../development/0068-editor-consistency.md)。

## M11.4 模拟期间的事件循环响应

现有editor IPC与Runtime owner pump承载simulation.run/query/pause/cancel/stop；正常编辑视口仍使用EditWorld。
增加仅验收使用的--simulation-response-probe，要求--pipe及一次性工程.dk-editor-smoke标记，与其他smoke/frames互斥。
真实窗口完成初始视口呈现后发布ready；外部夹具才开始进程首次模拟初始化。
在事件循环每次poll前保存单调时钟、owner发布的模拟状态和提交/完成数，最多120000项，超限失败。
退出后统一保存JSON，不逐帧写盘；首次窗口/视口初始化不混入模拟期间的心跳，暂停/空闲与活动区间分开。
依然实际运行SDL事件、ImGui布局、Vulkan呈现和IPC，不用后台定时器代替GUI心跳。
M12的Play视口/参数工作台不属于本阶段；这里只验收现有宿主对有限任务的响应和关闭。

验收单列已知AMD隐式层API版本警告，保留原始stderr与knownLoaderWarnings计数；
仅该精确消息不阻止响应性探针通过，其他warning/error仍失败，不关闭validation。常规编辑器策略不变。

性能采样与正确性验收分开：`--validation` 要求验证层，`--no-validation` 显式关闭GUI设备验证层，
两者互斥；未指定时仍if_available。响应夹具默认required，disabled只用于独立性能诊断；
两种配置的数据单列，不能互相替代。response_editor_smoke固定required，并验证真实呈现/资源寿命。独立仿真设备保持if_available，共享设备时继承GUI验证模式，
所以不能把GUI与runner延迟之差解释为纯呈现开销。退出探针另记录各所有者销毁阶段，包含到进程退出的完整成本。

IPC/Runtime owner pump在每帧开头、成功获取交换链图像后和渲染提交等待完成后执行。
GPU等待期间收到的控制请求在下一次UI绘制前处理，避免无谓追加一帧呈现后才开始关闭。
获取后收到shutdown时直接退出循环，由Frame原有放弃路径等待获取完成并归还图像；
仍先排空IPC回复。正常退出先close渲染队列证明所有绘制完成，再释放Viewport/ImGui资源，
随后Presenter.close等待present fence并释放交换链；worker同时回收自己的资源。
呈现清理后销毁Runtime以join模拟worker，最后销毁GUI设备和窗口。
不把渲染完成当作呈现完成；异常路径仍先完整排空Presenter，再展开UI资源栈。
两个设备的最终销毁不竞争驱动生命周期锁，且所有对象仍在原拥有线程回收。
验证只覆盖真实窗口控制/关闭和受影响的退出延迟，复用未改动的求解与数值结果。

M11.4复用已存在的GUI Vulkan设备：请求同族第二队列，支持时将设备寿命提供给Runtime，
模拟worker独占index 1，Presenter独占index 0；不支持时保留原独立模拟设备路径。
GUI设备required validation同时覆盖模拟队列；实例/设备创建属于编辑器启动，首次模拟仍包含
首次求解器/shader/Graph初始化，报告明确该成本变化，不伪称重新创建第二设备。
作用域守卫在所有正常/异常退出路径上先销毁所有UI使用者、关闭IPC并销毁Runtime（join worker），
最后释放Presenter/Window，保证共享设备和窗口的最终释放仍在主线程。
保持原FIFO绘制节奏，不限制工作台到60Hz。

若一次IPC pump只处理握手而未分派命令，Workspace使用先读取的RuntimeEvents sequence
最多等待1ms以接收同连接的执行帧，再pump一次。普通命令和空闲循环不增加等待；
客户端未继续发送时仍有界返回，不让每次两段握手都依赖下一帧的绘制/呈现。
