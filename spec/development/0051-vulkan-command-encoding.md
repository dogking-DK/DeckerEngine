---
id: "0051"
created_at: "2026-09-30T12:08:00+08:00"
updated_at: "2026-10-02T20:56:00+08:00"
status: completed
design_refs:
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
---

# 0051 M5.5.3 命令录制与同步

## 范围

按[使用层设计](../design/graphics-vulkan.md)实现 encoder、统一对象/资源保留、
显式 prepare/barrier、多 mip/layer/depth、索引绘制、范围传输和失败状态。

## 实际变更与验证

- CommandEncoder/ResourceSync/CommandTransfer 提供 render/compute、绑定/常量/vertex/index、范围 copy/fill/clear 和受控 native recorder。
- 对象闭包进入 batch/pending；Buffer 与 Image 同时限制一个未提交预约，状态仅在 submit 成功发布。encoder 使用弱批次身份与代次，避免悬空访问。
- ImageDesc/ViewDesc 支持 2D arrays、多 mip、D32 attachment；Image::state(mip,layer) 替代单值 layout。
- prepare 和显式 before/after barrier 共用同步账本；依赖访问缺少声明、未初始化 image read、scope 内 barrier/compute 等提前拒绝。
- GPU probe 覆盖同 layout WAW、upload→compute、compute→vertex→indexed draw、depth Less、提前销毁对象包装；资源 probe 覆盖子资源独立、局部初始化拒绝、放弃/submit 失败回滚、Graph before 不匹配、过期 encoder 和 native 异常。
- CPU 策略用例补充 mip/layer、pitch/footprint、阶段集合边界。索引值决定的真实 vertex 地址由调用者保证，未实现 GPU 数据扫描。

验证使用 Debug 定向目标，无新增依赖/feature。GPU 仅在进程内设 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复；原因沿用 [0048](0048-vulkan-14-baseline.md) 的旧 AMD 隐式层问题，Khronos 同步验证保持开启。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_graphics_resource_tests','dk_offscreen_probe','dk_graphics_resource_probe') `
  -TestRegex '^dk\.(graphics\.unit\.|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)' `
  -Reason 'M5.5.3 shader fixture isolation and stricter access validation'
```

| 结果目录（out/verify） | 结果 |
| --- | --- |
| 20260930-122547-0cf9af58 | 构建失败：新增整数比较触发 /WX；无测试执行 |
| 20260930-122817-fb6fa062 | 修正后构建与既有 6 项 CPU 用例通过 |
| 20261002-204832-9c5f4347 | 8/9 通过；geometry fixture 的全局反射带入其他入口的 push 声明，typed 检查拒绝未设置常量 |
| 20261002-205142-96891fc0 | 使用编译宏隔离 fixture 阶段；7 CPU + 2 GPU 全部通过，零跳过，GPU 零警告/错误、Memory/VMA 无残留 |

文档检查与 git diff --check 通过；未运行全量、Release 或其他 GPU/平台。
同步规则核对 [Khronos synchronization](https://docs.vulkan.org/spec/latest/chapters/synchronization.html) 与
[dynamic rendering](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdBeginRendering.html)。
下一步按 M5.5.4 接入批量 staging 与完成记录，再做 M5.5.5 消费迁移。
