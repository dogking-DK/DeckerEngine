---
module: graphics-device
created_at: "2026-09-28T16:38:00+08:00"
updated_at: "2026-09-28T16:55:37+08:00"
status: accepted
---

# Vulkan 设备与诊断

## 目标与边界

M5.1 提供无窗口 instance、physical device、logical device 和单队列底座。
不创建 surface/swapchain、Buffer/Image、提交系统或 shader；这些属于 M5.2–5。
源码位于 [device](../../engine/graphics/device)，target 为 dk_graphics_device / dk::graphics_device。
PUBLIC 依赖 Core、Memory、Vulkan::Headers，PRIVATE 使用 Profiling 和平台动态库 API；
不依赖 Scene、Assets、Framework、SDL 或 Slang。CPU runner 不链接该模块。
DK_BUILD_GRAPHICS_DEVICE 默认 OFF，windows-graphics 预设显式开启；vulkan-device feature
仅准备 vulkan。原 graphics feature 保留为全部规划桌面依赖的安装入口。

## 能力和接口

公开 [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp) 使用 Vulkan 类型，
不引入跨 API RHI。Device::create 接受调用者 Memory resource 和 DeviceOptions，返回拥有型 Device。
最低 API 固定 Vulkan 1.3；必须具有 timelineSemaphore、synchronization2、dynamicRendering，
一个 queueCount > 0 且同时支持 graphics/compute 的 family（按规范也支持 transfer）。
只启用这三项 feature，创建该 family 的第 0 个队列，不要求 present 或任何 device extension。
这些条件为下一阶段资源/同步与离屏动态渲染提供基线，不代表已实现相应业务。

AdapterInfo 是能力快照，包含属性、驱动名/信息、feature 与队列。
select_adapter 是无 GPU 副作用的确定性选择函数：显式索引必须适用，不静默换卡；
自动选择按 discrete、integrated、virtual、CPU、other 排序，同级保持枚举顺序。
拒绝时 Error.context 列出每张候选的名字和缺少能力；空列表与不适用设备分别报告。
枚举索引仅在当次创建中有效，不是持久 GPU 身份。

ValidationMode 为 disabled / if_available / required，默认 if_available。
只有 Khronos layer 与 EXT_debug_utils 均存在才启用；required 缺失就失败，
if_available 明确记录未启用状态并通过诊断 sink 报告原因。
callback 覆盖 instance 创建/销毁及 device 生命周期，转发 severity、type、message id、正文；
线程安全原子计数 warning/error，不缓存无界消息。sink 为 noexcept 函数指针和借用 user data，
调用者须保证线程安全且活到 create 失败或 Device 析构返回；禁止回调重入设备操作。
未配置 sink 时 warning/error 写 stderr。默认不启用 GPU assisted 或同步扩展验证设置。

## 所有权、发布与失败

每个 Device 独立持有 loader、dispatch、instance、messenger、device、queue，无全局 dispatch。
动态加载系统 Vulkan loader（Windows 默认 System32/vulkan-1.dll），缺少 loader 可返回 Error，
避免进程装载前失败；显式绝对 loader 路径用于部署和可复现诊断，不改变全局搜索路径。
所有 owning CPU 对象和枚举容器使用调用者 resource；错误字符串为 Core 的标准分配边界。
驱动/验证层内部 host allocation 使用 Vulkan 默认 allocator，不声称归入 Memory 域；GPU VMA 留待 M5.2。

create 在 loader→instance→messenger→选择→device→queue 全部成功后才发布。
任意失败或 C++ 分配异常沿 RAII 逆序销毁已创建对象；bad_alloc 保持 Memory 标准异常约定。
Device 不可复制，可移动拥有型包装；Impl 不移动，保证 callback 地址稳定。
移后源只能销毁或重新赋值，不能访问原生句柄与属性。
销毁顺序为 device→messenger→instance→loader。M5.1 不提交 GPU 工作，析构不做隐式 wait；
原生句柄仅借用，后续消费者须在销毁前完成工作和销毁所有子资源，外部同步 queue/device 访问。
getInstanceProcAddr/getDeviceProcAddr 入口供后续模块装载函数，句柄/函数不能活过 Device。
VkResult 错误保留操作名、符号名和原始数值；Device lost 不自动重建。

## 验证计划

CPU 单元验证能力缺失、显式索引、优先级、queueCount/queue flags、validation 策略；
内部 fake loader dispatch 确定性验证无设备、instance/device 创建失败和逆序清理，
不增加生产公开的故障注入开关。缺失 loader 路径走真实动态加载失败。
独立 GPU probe 验证 repeated create/destroy、句柄/队列、驱动报告、验证消息投递及销毁后零错误。
验证 probe 缺少环境时 CTest 返回 77（跳过），非环境错误必须失败；另设不要求验证层的设备 probe。
CPU-only 预设构建 runner 并执行版本/CPU 进程用例，检查无 Vulkan 运行时链接。

## 依据与记录

依赖核验：官方 [vulkan port](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan/vcpkg.json)、
[headers](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-headers/vcpkg.json)、
[loader](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-loader/vcpkg.json) 与固定 baseline 一致，
分别为 2023-12-17、1.4.357.0、1.4.357.0，不扩大升级。
API 依据为 Khronos [初始化](https://docs.vulkan.org/spec/latest/chapters/initialization.html)、
[队列](https://docs.vulkan.org/guide/latest/queues.html) 和
[验证层](https://docs.vulkan.org/guide/latest/validation_overview.html)。
关联 [架构](architecture.md)、[Memory](foundation-memory.md)、[0042](../development/0042-vulkan-device.md)。
