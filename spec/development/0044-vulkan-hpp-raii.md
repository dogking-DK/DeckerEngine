---
id: "0044"
created_at: "2026-09-28T17:30:00+08:00"
updated_at: "2026-09-28T17:40:00+08:00"
status: completed
design_refs:
  - ../design/graphics-device.md
---

# 0044 Vulkan-Hpp RAII 所有权

## 目标与设计依据

按用户要求将 [设备模块](../design/graphics-device.md) 的 Vulkan 对象切换到
vulkan_raii.hpp / vk::raii，保留 volk、vk-bootstrap、VMA 的现有用途与 M5.1 边界。
公开只读 RAII 引用供后续模块创建子资源，C 句柄只用于库间互操作。

## 决策与实际变更

现有 vulkan-headers 1.4.357.0 已提供 Vulkan-Hpp；不增加已弃用的 vulkan-hpp port，
不变更固定 baseline 或其他依赖。Hpp 通过当前 loader 的 resolver 构造独立 Context，
禁用默认 dispatcher 和隐式 loader。VMA 资源保留配对销毁，不交给 Hpp 重复拥有。
核验官方 [headers port](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-headers/vcpkg.json)
仍为 1.4.357.0（port #0），[hpp port](https://github.com/microsoft/vcpkg/blob/master/ports/vulkan-hpp/vcpkg.json)
标记 deprecated 且仅传递依赖 headers。依据官方
[RAII 指南](https://github.com/KhronosGroup/Vulkan-Hpp/blob/main/docs/VkRaiiProgrammingGuide.md)
及本地固定版本生成头的接管构造函数检查所有权。

vk-bootstrap 返回原生对象后，Hpp 接管仍可能因 dispatcher 分配失败而抛出；
交接前保留短期 native guard，接管后正常寿命全部交给 vk::raii。
成员顺序保持 allocator→device→messenger→instance→context→loader，诊断状态晚于回调销毁。
工厂的 dk::Result、枚举 Memory 分配与有界重试契约保持。

[Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp) 公开只读 RAII instance、
physical_device、logical_device 和 queue；AdapterInfo 改用 vk::* 属性与队列快照，
native_device 保留 C 互操作。直接使用 Hpp 的操作遵循默认 vk::SystemError 异常约定。
[Device.cpp](../../engine/graphics/device/src/Device.cpp) 的 Impl 改为 RAII 成员，
无手工 vkDestroy* 析构序列；VMA allocator 独立 owner 最先释放。
[Bootstrap.cpp](../../engine/graphics/device/src/Bootstrap.cpp) 将临时 guard 的句柄交给 Hpp；
instance/device dispatcher 的标准分配异常仍能逆序回收。
[CMake](../../engine/graphics/device/CMakeLists.txt) PUBLIC 传递 Hpp 配置宏，保证消费者采用相同设置。
同步设备设计、Graphics 指南、依赖说明、索引及 Roadmap，M5.2 仍待实施。

## 验证记录

Windows x64、MSVC 19.51.36257、Debug /W4 /WX、动态 CRT，固定 baseline 不变。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_device_tests', 'dk_device_probe') -TestRegex '^dk\.device\.' -Reason '验证 Vulkan-Hpp RAII 所有权、交接异常、子资源及 VMA 共存'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_roundtrip)$' -Reason '确认 RAII 图形接口与宏未进入 CPU-only runner'
```

- 设备验证通过 16/16（14 个 CPU 单元 + 2 个 GPU 探针），无失败/跳过；证据目录
  `out/verify/20260928-173611-eaa8337d`。新增模拟 Hpp 接管构造异常清理与移动赋值测试，
  现有移动测试补充 instance 销毁期间诊断 callback，检查状态仍然有效。
- RTX 4070 Laptop、NVIDIA 596.49、API 1.4.329：每个 probe 3 轮双设备，
  RAII fence 创建/状态/重置、command pool 创建/移动/重置/析构、VMA buffer 映射/flush/invalidate/释放均通过。
  第一台 Device 销毁后第二台继续执行同样检查；验证模式 errors=0、warnings=0、routed=3、liveAllocations=0。
- CPU-only 两项通过 2/2，无失败/跳过；证据 `out/verify/20260928-173550-2d23f82b`。
- dumpbin /DEPENDENTS 检查 graphics probe 和 CPU runner，均无 Vulkan DLL 静态导入；
  graphics probe 继续通过当前系统 loader 动态装载。
- 开发中两次构建失败，均未执行测试：C/C++ create-info 指针转换需 reinterpret_cast；
  禁用默认 dispatcher 时 command pool reset 需显式传空 flags。已修复并由上述完整定向构建复验。
- `scripts/check-spec.ps1` 通过（93 个 Markdown、921 个本地链接及全局索引/测试入口）；
  `git diff --check` 通过。

未运行全量、Release、Linux/其他 GPU；未验证队列提交或 GPU 工作完成（仍属 M5.2）。
Hpp dispatcher 内部分配不计入 Memory heap；没有改变此前三方默认 allocator 边界。
