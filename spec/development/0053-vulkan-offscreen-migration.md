---
id: "0053"
created_at: "2026-10-02T21:10:00+08:00"
updated_at: "2026-10-02T21:26:00+08:00"
status: completed
design_refs:
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
  - ../design/graphics-offscreen.md
  - ../design/graphics-device.md
  - ../design/architecture.md
---

# 0053 M5.5.5 离屏迁移与集成验收

## 范围

Offscreen 使用 ResourceFactory、pipeline/bindings、encoder 和 upload/readback；
pending 只保留结果请求与票据，GPU 对象闭包全部交给提交层。保持既有输出、超时与 drain 约定。
迁移指南及弃用原始 command_buffer，定向复验 GPU、相关 CPU 策略、CPU runner 和独立离线 shader 配置。

## 验证

## 实际变更

- Offscreen.cpp 移除全部 vk::raii/CreateInfo、descriptor 更新和原生命令录制；draw 使用 ResourceFactory 与 RenderEncoder，dispatch 使用不可变绑定、upload/prepare/dispatch/readback。
- pending Work 仅包含 ReadbackRequest 和票据；对象、VMA 资源、staging 全由提交域闭包保留。保持原返回值顺序、非整组线程边界、超时、drain、submit/等待失败与析构行为。
- CommandBatch 增加单次 dispatch 便利方法；Buffer::state() 提供已提交快照；command_buffer 标记 deprecated，常规消费者不再调用。
- 最终核对补齐兼容 copy 与类型化命令混用的保守读写状态/WAR；native vk::SystemError 映射 Result 并使批次 invalid；顶点字节地址检查乘法溢出。
- Resource GPU probe 增加 fill/clear 输出与二次写缺失 barrier 拒绝、legacy upload→typed overwrite 的 WAR 验证。
- 同步模块设计、索引、Graphics/Offscreen 指南、README 与 Roadmap；M5.5.1–5 完成，下一项 M5.6。

## 验证结果

所有构建为 Windows x64 Debug。GPU 为 RTX 4070 Laptop、NVIDIA 596.49、Vulkan 1.4.329；
Khronos 同步验证开启，沿用进程内 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 并在结束恢复。
没有关闭验证层，也没有将环境跳过记为通过。

| 证据（out/verify） | 范围 | 结果 |
| --- | --- | --- |
| 20261002-211246-2a84813f | windows-graphics：Device/资源/Offscreen CPU 策略与 3 个验证层 GPU probe | 28/28，零跳过 |
| 20261002-211349-3cbbb2fe | windows-dev：dk-run 版本与 batch_roundtrip/batch_errors | 3/3，零跳过 |
| 20261002-211406-075d5ed5 | windows-shaders：8 项编译/反射 CPU 测试、shaderc process/deployment | 10/10，零跳过 |
| 20261002-212105-94e87133 | 最后同步/范围修正：资源 CPU + 资源/Offscreen GPU | 9/9，覆盖前述重复项 |

合计 41 个不同用例通过；最后 9 项为受影响复验，不重复计数。
GPU probe 验证原图像/计算结果、重复执行/创建/销毁、超时/失败恢复、新增同步/对象寿命，零警告/错误、Memory/VMA 无残留。
实际命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_device_tests','dk_graphics_resource_tests','dk_offscreen_tests','dk_shader_tests','dk_device_probe','dk_offscreen_probe','dk_graphics_resource_probe','dk_shaderc') `
  -TestRegex '^dk\.(device\.unit\.|graphics\.unit\.|offscreen\.unit\.|shader\.|device\.gpu_validation$|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)' `
  -Reason 'M5.5.5 typed Offscreen migration and final affected graphics chain'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run `
  -TestRegex '^dk\.(bootstrap\.version$|runtime\.batch_(roundtrip|errors)$)' `
  -Reason 'M5.5 final CPU-only runner boundary regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-shaders -Target @('dk_shader_tests','dk_shaderc') `
  -TestRegex '^dk\.(shaders\.|shaderc\.)' -Reason 'M5.5 final offline shader compiler boundary and deployment'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_graphics_resource_tests','dk_offscreen_probe','dk_graphics_resource_probe') `
  -TestRegex '^dk\.(graphics\.unit\.|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)' `
  -Reason 'M5.5 final compatibility/typed synchronization WAR and vertex bounds checks'
```

第一条 regex 的 shader 单数不匹配实际 shaders/shaderc 测试；它们由第三条独立配置完整覆盖，
第一条实际选择以 summary.json 的 28 项为准，不声称其中执行了 Shader 测试。

附加核对：CPU 配置 DEVICE/SHADERS/OFFSCREEN 均 OFF；离线配置 DEVICE/OFFSCREEN OFF、SHADERS ON。
MSVC dumpbin /dependents 确认 CPU runner 无 Vulkan/Slang 直接 DLL 依赖，shaderc 有 Slang 而无 Vulkan，
资源 probe 无 Slang 依赖；结合 CMake 模块开关和 target 依赖核验隔离边界。
rg 扫描 Offscreen.cpp 的 vk::raii/CreateInfo/updateDescriptorSets/command_buffer 无命中。
文档示例的接口和流程对应已执行的资源、计算和绘制 probe；check-spec.ps1 与 git diff --check 通过。

未运行全量、Release、其他 GPU/平台或 Tracy GPU capture。功能范围仍为单队列、sample=1、五种 color 格式与 D32 深度；
image 高层读回输出紧密排列，索引值决定的顶点范围由调用者保证，Graph/WSI/多队列/MSAA/bindless 暂缓。
没有新增依赖或改变固定版本。
