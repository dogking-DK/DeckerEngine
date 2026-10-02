---
module: graphics-presentation
created_at: "2026-10-02T21:40:00+08:00"
updated_at: "2026-10-02T21:40:00+08:00"
status: accepted
---

# 窗口与 Vulkan 呈现

## 范围与依赖

`engine/graphics/presentation` / `dk::graphics_presentation` PUBLIC 依赖 graphics_device、platform，
PRIVATE SDL3 适配。`DK_BUILD_GRAPHICS_PRESENTATION` 默认关闭，要求 DEVICE 和 PLATFORM；
windows-presentation 预设独立启用。Shader 编译仅为示例/探针的依赖。
Device、Offscreen、CPU runner 不链接 SDL；不引入 Graph、ImGui 或多队列调度。

## 设备与 Surface

create_present_device 取得 SDL 必需实例扩展，在实例创建后、选卡前创建 surface。
DeviceOptions 的可选 SurfaceSource 提供扩展、创建回调与寿命 token，不依赖 SDL。
Device 持有 surface 和窗口 token；销毁顺序为 GPU 子对象、device、surface、instance、window。
选卡必须同时满足 Vulkan 1.4、现有特性、graphics+compute+present 同一队列族与 swapchain。
本阶段要求 VK_EXT_swapchain_maintenance1 / swapchainMaintenance1，并启用实例依赖
VK_EXT_surface_maintenance1、VK_KHR_get_surface_capabilities2。
缺失时明确 not_supported；离屏设备不请求或查询 WSI 能力。

选择 maintenance1 是为了用 present fence 证明呈现信号量和交换链可回收，并能释放未呈现的已获取图像。
submit timeline 完成只证明渲染完成。依据：[present fence](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainPresentFenceInfoKHR.html)、
[semaphore reuse](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)、
[释放图像](https://docs.vulkan.org/refpages/latest/refpages/source/vkReleaseSwapchainImagesKHR.html)。

## 使用接口与所有权

Presenter 拥有 SubmissionQueue、窗口引用与交换链代际；调用者通过 queue/resources 复用 M5.5。
每次 acquire 返回状态（ready/suspended/retry）及独占 Frame；每个 Presenter 同时最多一个未提交帧。
Frame 提供绑定到该获取操作的 CommandBatch、color Image/View 与 extent/format。
使用标准 prepare/begin_rendering/RenderEncoder 录制，present 自动导出 PresentSrcKHR 并提交 acquire wait、
render-finished signal、timeline。Frame 不公开可移动的 batch 所有权，防止绕过 WSI 提交。
帧提交成功后即消费，即使随后的 present 失败也不回滚已发生的 GPU 工作。
pipeline 在交换链 format 变化后由调用者重建；extent 变化只更新动态 viewport/scissor。

交换链图像是外部 Image，保留独立代际 owner，不调用 VMA/vkDestroyImage。
外部资源只有在对应 frame 的 batch 内可访问；旧引用/未获取图像/另一 batch 被拒绝。
typed view/encoder/提交保活链保留图像及代际，兼容已完成 M5.5 的接口。
代际 owner 不持有 Image，避免循环；销毁交换链之前释放 view。

## 状态机、同步与恢复

- acquire 使用有限超时；TIMEOUT/NOT_READY 返回 retry，无帧、无状态发布。
- 零像素或最小化返回 suspended，避免创建零尺寸交换链；恢复后重建。
- OUT_OF_DATE 标记重建并返回 retry；SUBOPTIMAL 允许本帧呈现，下一 acquire 重建。
- 每个在途槽保存 acquire semaphore、获取 fence、render-finished semaphore、present fence 和 timeline ticket。
  重用前同时确认渲染与呈现 fence；不把渲染完成当作 present wait 消费。
- Frame 放弃或录制/submit 失败：先销毁未提交 batch，等待获取 fence，释放 acquired image；
  已信号 acquire semaphore 经单独受控 wait 消费后才重用。失败返回明确错误或进入终态。
- 重建先 drain 本 Presenter 的渲染及 present fence，再创建交换链及 view。
  Vulkan oldSwapchain 一经传入创建即退休，不能承诺失败回滚；本实现明确进入 needs-rebuild，
  中途失败清理候选/退休代际，下次从无活动交换链重试。成功才发布完整新代际。
- surface lost / device lost 进入终态，拒绝新帧；调用者销毁后重新创建窗口设备链。
  close 幂等，拒绝仍有活动 Frame；析构完成受控排空，无未知完成状态的强制销毁。

窗口、Presenter、Frame、queue 及最终对象释放全部在主线程串行；native 互操作需遵守原有外部同步约定。
第一版使用 FIFO、BGRA/RGBA 8-bit sRGB 或 UNORM、单 color attachment、sample=1；
无跨队列 ownership transfer、HDR、独占全屏、present 模式切换或多窗口共享设备。

## 验证与实施

M5.6.1 窗口与 surface-aware device；M5.6.2 交换链、external image、帧同步；
M5.6.3 resize/minimize/异常与最终集成。各阶段独立提交，进度只在 Roadmap 维护。
CPU 策略覆盖格式/extent/image count、present family/特性拒绝与状态分支；真实窗口验证持续三角形、
多次 resize/minimize/restore、帧放弃、失败恢复及重复创建关闭。同步验证零错误/警告，Memory/VMA 无残留。
同时定向复验现有离屏/资源链和独立 CPU 构建。其他平台/驱动的验证不由本次 Windows 结果推断。

相关：[Platform](platform.md)、[Device](graphics-device.md)、[资源](graphics-resources.md)、
[使用层](graphics-vulkan.md)、[Roadmap](../roadmap.md)。
