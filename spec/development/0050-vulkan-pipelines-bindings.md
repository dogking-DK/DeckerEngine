---
id: "0050"
created_at: "2026-09-30T11:48:00+08:00"
updated_at: "2026-09-30T12:06:23+08:00"
status: completed
design_refs:
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
---

# 0050 M5.5.2 管线与绑定

## 目标

实现 [使用层契约](../design/graphics-vulkan.md) 的反射布局合并、graphics/compute 管线、
不可变 BindingSet、多 set/固定数组和有界描述符池。类型化命令录制在 M5.5.3 实施。

## 实际变更

- Pipeline.hpp/Pipeline.cpp 合并 `(set,binding)`、stage flags、uniform 最小字节和 push ranges，
  检查数组冲突、空洞 set、设备限制及管线布局覆盖。布局兼容比较完整规范化定义。
- graphics 管线集中默认光栅/深度/混合/dynamic rendering 状态；compute 验证本地工作组。
  ShaderModule 可在管线创建后释放，Pipeline 控制块独立保活布局与设备。
- Bindings.hpp/Bindings.cpp 以类型化 BindingWrite 完整初始化不可变 BindingSet；验证 owner、
  type、数组索引、usage、buffer offset/range/alignment 与 image layout。寿命保留与忙引用分开。
- 描述符池按类型数量分页，最多 64 页、每页 32 set、每 set 4096 descriptor；
  BindingSet 持有 page，队列弱目录不阻止释放。池耗尽报 conflict，释放后可恢复。
- CPU 测试覆盖反射合并/冲突/limits，现有离屏 probe 新增真实多 set/纹理数组、sampler、
  uniform/storage buffer、storage image、管线复用、布局/绑定拒绝、失败清理、页边界与容量恢复。
  测例通过原生互操作录制验证对象；普通 encoder 与自动提交保留由下一节完成。

## 验证

沿用 0049 的 Windows Debug/GPU 环境，进程内使用已知 AMD 层隔离开关，保留 Khronos/同步验证。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_graphics_resource_tests','dk_offscreen_probe','dk_graphics_resource_probe') -TestRegex '^dk\.(graphics\.unit\.|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)' -Reason 'M5.5.2 反射布局、管线复用、多 set/数组、不可变绑定与 GPU 读回'
```

`out/verify/20260930-120304-544efb89`：8/8 通过，0 跳过。补充池容量耗尽/恢复后，
仅重跑 `dk_offscreen_probe` / `^dk\.offscreen\.gpu_validation$`，
`out/verify/20260930-120435-73995fd0`：1/1 通过。GPU 测试最终零相关 warning/error、零 Memory 残留。
测试验证两个重复 dispatch 的 float 数据与 RGBA8 像素，保留原 12 组 draw/compute 和失败寿命回归。
文档检查与 diff 检查通过。未运行全量、Release、其他 GPU/平台、Tracy GPU capture。

首次 `115509-98ff6f33` 因 Hpp 拒绝临时 span/花括号赋值未通过编译，
`120205-7a32736e` 因测试 take<void> 辅助函数编译失败；两次均未运行测试，修复后以上述结果验收。
一次自动审批超时后按工具允许重试成功，没有修改工具链或降低验证标准。

Slang storage image fixture 明确声明格式，依据官方
[format 属性](https://docs.shader-slang.org/en/stable/external/core-module-reference/attributes/format.html)，
避免隐式使用额外 storage image feature；设备特性集合不变。

## 下一步

M5.5.2 完成；继续 M5.5.3 的类型化 encoder、统一保留闭包与资源范围/同步状态。
