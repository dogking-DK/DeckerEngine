---
module: editor
created_at: "2026-10-08T09:36:00+08:00"
updated_at: "2026-10-08T10:13:00+08:00"
status: accepted
---

# M8.1 编辑器工作台

## 边界

`dk-editor` 装配 SDL3、ImGui docking、Vulkan Presenter、ScenePipeline 和 Runtime。
`dk_editor_model` 只依赖 Runtime，保存选择、Inspector 草稿和只读快照；不访问 ECS。
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

## 窗口与 GPU 接入

Platform 提供显式 SDL 借用句柄、同步事件 sink 和清除关闭请求；事件借用仅在回调期间有效，
SDL 的初始化、窗口销毁和主线程契约仍由 Platform 管理。Presenter Frame 暴露本代 image_count。
ImGui backend 是唯一原生 Vulkan 接入点，经 CommandBatch::unsafe_record 显式声明 framebuffer
写与 viewport 采样，并保留 ImageView；封装层继续管理提交、同步、交换链和资源寿命。
Windows 可用时读取系统微软雅黑字体（不打包字体），支持中文名称；无该字体时使用 ImGui 默认字体。
sRGB 交换链上的 UI 样式先转为线性颜色，Viewport 纹理单独按 sRGB 解码。
每帧等待提交完成后再更新/释放 backend 纹理；关闭先排空 GPU，再关闭 ImGui，最后释放 Presenter/Window/Memory。
交换链格式变化时重建 ImGui pipeline，图像数变化同步 backend 配置；最小化暂停并保留模型。

Viewport 基于当前内存快照，固定 Sponza 相机；按文档版本/面板尺寸失效后绘制，非持续重导入。
每个文档会话只加载一次 CPU/GPU 资产，编辑变换时重新提取 RenderScene；显式 Reload 可重读资产。
渲染结果的 RGBA8 已编码 sRGB，初版通过现有读回和上传桥发布 sRGB 采样纹理，避免再次编码。
候选图像及描述符成功后替换旧预览；失败保留旧图并明确显示实际 revision/过期状态，可重试。
此同步预览在大场景加载时会阻塞 UI；后台流式加载和取消不属于 M8.1。

## 验收与后续

CPU 定向测试覆盖选择/草稿、事务与撤销、过期 guard、保存重载与失败保护。
真实窗口 smoke 用 ImGui 输入事件走选择/Inspector/保存/重载/撤销按钮，检查版本与持久化，
验证 TRS 改动使预览像素变化、Undo 逐字节哈希恢复，再读回工作台画面并启用 Vulkan validation；
默认 Sponza 另做实际预览。
M8.2 才加入 CPU rayquery、Gizmo 和相机交互；M8.3 IPC 与 M8.4 跨入口一致性不提前标为完成。
