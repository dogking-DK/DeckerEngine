---
id: "0043"
created_at: "2026-09-28T17:05:00+08:00"
updated_at: "2026-09-28T17:15:26+08:00"
status: completed
design_refs:
  - ../design/graphics-device.md
---

# 0043 引入 volk、vk-bootstrap 和 VMA

## 目标与设计依据

按用户要求在 [设备模块](../design/graphics-device.md) 实际接入三个库。
volk 提供每实例函数表，vk-bootstrap 构建 instance，VMA allocator 随 Device 创建/销毁。
不将依赖接入记为 M5.2 资源/提交阶段完成。

## 决策与实际变更

核验官方 port：volk 1.4.357.0、vk-bootstrap 1.4.357、VMA 3.4.0（均无 port 修订），
与固定 baseline 33d78c1ed898a06938f31312167c7abefd229455 一致，保持基线。
核对 [vk-bootstrap 固定版本源码](https://github.com/charles-lunarg/vk-bootstrap/blob/v1.4.357/src/VkBootstrap.cpp)：
该版本缓存全局入口且 messenger 创建失败不返回已创建 instance，
内部转发适配器与即时接管句柄保留原有独立 loader 和失败清理契约；不修改第三方源码。

[Bootstrap.cpp](../../engine/graphics/device/src/Bootstrap.cpp) 限定使用 InstanceBuilder，
缓存稳定转发入口，创建时串行设置线程局部 resolver，捕获原生成功句柄后立刻交给 RAII。
不同 loader 及已销毁首实例后的新创建不会使用旧 resolver；不暴露 vkb 类型或调用其他 vkb builder。

[Device.cpp](../../engine/graphics/device/src/Device.cpp) 使用 volk 填充实例/设备函数表；
volk 全局初始化只存在于私有互斥区，后续调用/销毁使用各 Device 的 table。
PUBLIC 传递 VK_NO_PROTOTYPES，消费者使用 Device 的 proc 接口，避免误链接全局 vk*。
VMA 使用当前 table 显式提供的函数，关闭自动静态/动态装载；API 配置为 1.2，
不要求现阶段未启用的 maintenance4，底层 Device 要求仍为 1.3。

新增 [Vma.cpp](../../engine/graphics/device/src/Vma.cpp) 单一定义 implementation，
独立 dk_graphics_vma target 隔离三方警告；引擎/测试继续 /W4 /WX。
Device::allocator() 返回借用 VmaAllocator，退出顺序为 allocator→device→messenger→instance→loader。
vk-bootstrap/VMA 内部 CPU 元数据使用默认分配器，未计入引擎 Memory heap；GPU 分配由 VMA 统计。
外部持有的 allocation 必须先于 Device 释放；没有隐式等待或跨设备 allocation 转移。

vulkan-device 和 graphics 两个 feature 均包含这三个库；默认 CPU 配置不选择它们。
同步设备设计、构建/Graphics 指南、依赖说明、设计/开发索引和 Roadmap 接入边界。

## 验证记录

环境与 0042 一致：Windows x64、MSVC 19.51.36257、Debug、动态 CRT；固定 baseline 的依赖实际编译链接通过。
GPU 为 NVIDIA RTX 4070 Laptop，驱动 596.49 / API 1.4.329；系统 loader 与 SDK validation 1.4.321.1。

| 检查 | 结果 | 证据目录 |
| --- | --- | --- |
| windows-graphics，dk_device_tests + dk_device_probe，`^dk\.device\.` | 初次接入 13/13，通过 | out/verify/20260928-170716-04b4d8fd |
| 同配置/目标/筛选，增加 loader 隔离与真实 VMA/双设备用例 | 14/14，通过，无跳过 | out/verify/20260928-170924-b7fa17b3 |
| 同配置 /WX，最终 PUBLIC VK_NO_PROTOTYPES 与诊断改动 | 14/14（12 CPU + 2 GPU），零编译警告，无跳过 | out/verify/20260928-171251-c14e2eaa |
| windows-dev /WX，dk_run，版本与 batch_roundtrip | 2/2，通过；设备模块保持 OFF | out/verify/20260928-171203-d0907e65 |

执行入口为 [verify.ps1](../../scripts/verify.ps1)，确切参数和目标见每目录 summary.json。
设备/GPU 命令为 `-BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.'`；
CPU 筛选为 `^dk\.(bootstrap\.version|runtime\.batch_roundtrip)$`。
单元覆盖 allocator 创建失败后的 device/instance 清理、vk-bootstrap messenger 失败后回收、
首实例销毁后切换 resolver、原有选择/验证策略及 Memory 分配异常清理。
验证开关两种 GPU 路径各执行三轮双设备；每轮在两个 allocator 分配映射 4 KiB buffer，
写入/flush/invalidate/核对并释放，销毁首设备后再次使用第二个 allocator。
均为 errors=0、warnings=0、liveAllocations=0；VMA 活 allocation 为零，验证消息 routed=3。
这证明 host-visible 分配与生命周期，不是 GPU 提交/读回测试。

dumpbin /DEPENDENTS 检查 dk-device-probe 与 CPU dk-run 均无 Vulkan 导入 DLL；
设备探针仅动态加载系统 loader，CPU runner 保持原链接边界。
`scripts/check-spec.ps1` 通过：92 Markdown、911 条本地链接、元数据/表格/索引/测试入口/JSON；
`git diff --check` 通过。

## 遗留与下一步

M5.2 的拥有型 Buffer/Image、提交/完成/延迟销毁仍待实现。
本次接入范围无未解决问题。未运行全量、Release、其他平台/显卡或 Tracy GPU 采集；
未验证三方内部默认分配器的 OOM 行为，不宣称覆盖全部 CPU 内存统计。

## 修改记录

- 2026-09-28T17:05:00+08:00：建立依赖接入设计与记录。
- 2026-09-28T17:14:00+08:00：完成三库实际接入、设备/VMA/CPU 定向验收。
