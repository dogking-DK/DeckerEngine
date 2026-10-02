---
id: "0055"
created_at: "2026-10-02T22:00:00+08:00"
updated_at: "2026-10-02T22:20:00+08:00"
status: completed
design_refs:
  - ../design/graphics-presentation.md
  - ../design/graphics-resources.md
  - ../design/graphics-vulkan.md
---

# 0055 M5.6.2 交换链与帧同步

依据 [呈现设计](../design/graphics-presentation.md)，新增外部 Image 所有权、受保护帧 batch、
acquire/render/present 同步、交换链代际与 Frame 放弃清理。复用 M5.5 的 factory 和 encoder。

设计细化：每次获取默认丢弃此前像素内容，要求本帧 clear/full write；初始 Undefined 布局合法丢弃旧内容。
放弃未提交帧在 acquire fence 完成后释放图像、销毁已信号的 acquire semaphore，下次使用时新建；
没有排队 wait，不额外提交 GPU 空任务。present OOM 未入队与 out-of-date 已入队按 Vulkan 规范分别处理。

## 实际变更

- Presenter/Frame 封装 acquire、FIFO swapchain、typed render 和 present；每个获取操作独占访问外部 Image。
- private PresentationBridge 执行外部图像导入/导出与带 binary wait/signal 的 timeline 提交；普通 submit 拒绝 WSI batch。
- acquire fence、渲染 timeline、present fence 分别确认各自操作完成，再复用信号量和槽；Frame 放弃不留下已信号 semaphore。
- 外部 Image/View 保留交换链代际与 Device；不进入 VMA 销毁路径。扩展 BGRA8 sRGB 供常见 surface 格式。
- surface 支持 TransferSrc 时使用既有 readback 检查真实待呈现图像；常规绘制只使用 factory/encoder。

## 验证

Windows x64 Debug，RTX 4070 Laptop / NVIDIA 596.49 / Vulkan 1.4.329，Slang SPIR-V 1.5，
复用 common/triangle.slang 的 vertexMain/fragmentMain；Khronos 与同步验证开启。
沿用 0048 AMD 隐式层环境规避，测试进程结束恢复变量。

- `out/verify/20261002-221214-fdadef62`：3/3 交换链 CPU 策略与错误分类通过。
- `out/verify/20261002-221605-f0ce5a6e`：12/12 通过、零跳过。
  target 为 dk_presentation_tests、dk_presentation_probe、dk_graphics_resource_tests、dk_graphics_resource_probe；
  regex 为 `^dk\.(presentation\.(unit\.|frames_validation$)|graphics\.(unit\.|gpu_resources_validation$))`。
- 3 次窗口设备生命周期共 144 帧；首尾图像读回检查背景/三角形像素；丢弃获取、过期 view、跨 batch、
  移出 batch 绕过提交、重复 acquire/close 与 pending 提前释放 pipeline 均验证。
- Presentation 与既有资源 probe：零验证警告/错误，VMA allocation 与 Memory 活分配为零。
- 文档检查与 diff 检查提交前执行。未运行 Release/其他平台。

下一项 M5.6.3：系统化 resize/minimize/重建和失败注入、窗口示例及 CPU/离屏隔离验收。
