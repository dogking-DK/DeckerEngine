---
id: "0049"
created_at: "2026-09-30T11:34:08+08:00"
updated_at: "2026-09-30T11:46:20+08:00"
status: completed
design_refs:
  - ../design/graphics-vulkan.md
  - ../design/graphics-device.md
  - ../design/graphics-resources.md
  - ../design/graphics-shaders.md
---

# 0049 M5.5.1 对象工厂与寿命基础

## 目标与设计依据

按 [Vulkan 使用层设计](../design/graphics-vulkan.md) 实现对象工厂、view/sampler/ShaderModule、
独立设备寿命和 shader 产物类型提取。后续 M5.5.2–5 分节实施，不由此节替代验收。

## 实际变更

- 提取 `graphics/shader-types` 的 ShaderArtifact.hpp/构造/名称与 SPIR-V 基础校验；
  compiler 和 device PUBLIC 消费该 target，保持旧编译头类型可见和产物 schema。
- 新增 GpuObjects.hpp/ResourceFactory，创建 view/sampler/shader；使用 weak queue 校验工厂存活，
  普通对象控制块持有独立 DeviceLifetime，view 同时持有 VMA image。
- 将资源/队列内部结构移到私有 ResourceInternal.hpp，供后续对象保留和录制共同使用。
- 扩展现有资源 probe：跨设备拒绝、view 保活 image、shader 元数据独立、移动赋值、
  原生对象创建后注入失败清理、关闭/失效工厂、非 VMA 对象晚于 queue 和另一 device 销毁。

## 验证记录

Windows x64、MSVC、Debug /W4 /WX、RTX 4070 Laptop、NVIDIA 596.49、Vulkan 1.4.329。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_graphics_resource_tests','dk_graphics_resource_probe','dk_shader_tests','dk_offscreen_tests') -TestRegex '^dk\.(graphics\.|shaders\.|offscreen\.unit\.)' -Reason 'M5.5.1 对象工厂、设备寿命和 shader 产物提取'
```

`out/verify/20260930-114254-019e2890` 构建成功，18 项中 17 通过；验证 probe 因已知旧 AMD 隐式层
版本警告失败（errors=0、warnings=10、liveAllocations=0）。沿用 [0048](0048-vulkan-14-baseline.md)
的 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1` 进程开关，仅重跑该失败用例：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graphics_resource_probe -TestRegex '^dk\.graphics\.gpu_resources_validation$' -Reason 'M5.5.1 隔离已知旧 AMD 隐式层，保留 Khronos 与同步验证'
```

`out/verify/20260930-114503-a304e30d` 1/1 通过，0 跳过，errors/warnings/liveAllocations 均为 0；
跨有效运行共 18 个不同测试通过。开关在 try/finally 中恢复；未关闭 Khronos/同步验证。
MSBuild/vcpkg 需要沙箱外执行；首次受限配置 `114234-dca1c1ec` 和受限 FileTracker `114442-f40d1fa6`
均在构建阶段失败，未当作测试证据。构建后检查 device-only probe 的实际 vcxproj 链接依赖无 Slang。
文档检查和 git diff --check 通过。未运行全量、Release、其他 GPU/平台或本阶段无影响的 CPU Runtime。

## 下一步

M5.5.1 完成；继续 M5.5.2 的管线/布局/绑定。子资源扩展和通用提交保留留给 M5.5.3，
尚不声称普通对象已可通过类型化 encoder 提交使用。
