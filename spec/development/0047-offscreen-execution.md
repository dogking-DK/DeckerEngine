---
id: "0047"
created_at: "2026-09-30T09:00:00+08:00"
updated_at: "2026-09-30T09:28:00+08:00"
status: completed
design_refs:
  - ../design/graphics-offscreen.md
  - ../design/graphics-resources.md
  - ../design/graphics-shaders.md
---

# 0047 M5.4 离屏绘制与计算

## 目标与实际实现

按 [离屏设计](../design/graphics-offscreen.md) 完成真实 Vulkan draw/dispatch/readback，
复用 M5.2 提交/VMA 和 M5.3 Slang，普通 Vulkan 对象使用 vk::raii。
固定依赖 baseline `33d78c1ed898a06938f31312167c7abefd229455` 保持不变，无新增/升级依赖、Runtime 命令或窗口。

- [OffscreenExecutor](../../engine/graphics/offscreen/include/dk/graphics/Offscreen.hpp) 消费 Device、持有单槽 SubmissionQueue。
  新 `dk::graphics_offscreen` / `DK_BUILD_GRAPHICS_OFFSCREEN` 可选模块；windows-graphics 开启，CPU/离线 Shader 默认不链接。
- [实现](../../engine/graphics/offscreen/src/Offscreen.cpp) 使用动态 rendering 绘制无顶点缓冲/descriptor 的 triangle list，
  返回 RGBA8_UNORM；compute 根据反射创建 set 0 storage buffer 描述符和 push constant 布局，上传、dispatch、逐 buffer 读回。
  ShaderModule、Pipeline/Layout、DescriptorSetLayout/Pool/Set、ImageView 全部 vk::raii；VMA 资源使用既有配对所有者。
- [策略检查](../../engine/graphics/offscreen/src/OffscreenPolicy.cpp) 验证基础 SPIR-V 流/入口/阶段/capability，
  图像/viewport、工作组数量/本地大小/invocations、descriptor 数量/range 和 push constant 限制。
  只消费未经修改的受信任 compile_shader 产物，不声称实现完整 SPIR-V 或跨阶段接口验证。
- 返回容器和 GPU 所有者在提交前准备。提交前异常回收预留 work；提交后异常/超时保持 pending，
  拒绝新操作，drain 确认完成后才释放。析构先排空队列，再释放非 VMA Vulkan 对象，最后释放保持设备寿命的 VMA owners。
  不修改输入，不发布部分输出，Memory 关闭后仍可 drain/析构。
- [triangle.slang](../../shaders/common/triangle.slang) 使用 `SV_VulkanVertexID`。
  原 `SV_VertexID` 经 Slang 生成 BaseVertex/DrawParameters，要求未启用的 shaderDrawParameters；
  按 [Slang 映射说明](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html)
  改用直接 VertexIndex 语义，保持 Device 特性基线。
- 增加 [CPU 策略测试](../../tests/unit/OffscreenTests.cpp)、[GPU 探针](../../tests/integration/OffscreenProbe.cpp)、
  [离屏指南](../guides/offscreen.md)；同步设计/依赖边界、使用入口、测试选择表和 Roadmap。

## 验证与环境

Windows x64、VS 2026 / MSVC 19.51.36257、Debug /W4 /WX、动态 CRT、profiling OFF。
GPU 为 NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49，Vulkan API 1.4.329；
系统 loader、Vulkan SDK 1.4.321.1 的 Khronos 验证层，headers 1.4.357.0。
Slang 2026.18、SPIR-V 1.5、row-major、默认优化、无 debug，入口 vertexMain / fragmentMain / computeMain。

```powershell
cmake --preset windows-graphics -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_offscreen_tests', 'dk_offscreen_probe') -TestRegex '^dk\.offscreen\.' -Reason '验证 M5.4 CPU 策略、GPU 绘制计算、提交异常及超时对象寿命'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_offscreen_probe', 'dk_graphics_resource_tests', 'dk_graphics_resource_probe', 'dk_shader_tests') -TestRegex '^dk\.(offscreen\.gpu_|graphics\.|shaders\.graphics entry points emit named SPIR-V$)' -Reason '复验离屏图像产物、资源同步寿命及 SV_VulkanVertexID 编译链路'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_roundtrip)$' -Reason '确认新增可选离屏模块不影响默认 CPU runner'
```

| 检查 | 结果与证据 |
| --- | --- |
| graphics 配置 | 成功，新增 Offscreen 开启，/WX 保持 ON |
| M5.4 CPU + GPU | 5/5 通过、0 跳过；`out/verify/20260930-092203-c368af40` |
| 最终 GPU 产物 + 直接关联回归 | 10/10 通过、0 跳过：2 个离屏 GPU、5 个资源策略、2 个资源 GPU、1 个图形编译；`out/verify/20260930-092317-d038aff0` |
| 默认 CPU runner | 2/2 通过、0 跳过；`out/verify/20260930-092332-debd16c5` |
| 文档与差异检查 | check-spec.ps1 通过：102 个 Markdown、1012 个本地链接、元数据/表格/索引/测试入口/JSON；git diff --check 通过 |

以上覆盖 15 个不同测试，重跑项不重复计数。每次离屏 GPU 探针均通过 12 次主流程绘制、12 次计算、3 轮设备生命周期；
额外成功/失败路径不计入这两个主流程计数。逐像素检查三角形内部重心插值与背景，排除边界带以避开栅格舍入歧义；
四种 extent：64×64、72×68、80×72、88×76。
compute 以 1/64/257/1031 个元素校验整数变换及 7 个尾部哨兵，输入 binding 逆序仍正确匹配，输入 buffer 不修改。
返回字节、提交计数、pending 槽和 VMA allocation 均有断言。

失败注入覆盖提交前 std::bad_alloc、VK_ERROR_OUT_OF_HOST_MEMORY、等待超时/错误、重复 drain、
pending executor 移动、compute 的 descriptor/pipeline 延长寿命，以及带 pending draw 析构。
关闭 Memory 后的新工作拒绝且无提交；所有返回/设备/Shader 所有者销毁后 live_allocations=0。
两种离屏 GPU 模式及资源 GPU 模式均为 `errors=0 warnings=0`；验证模式开启 synchronization validation。
最终离屏输出 `pending=0 VMA=0 Memory=0`。

PPM 产物在 `out/build/windows-graphics/tests/integration/offscreen-triangle.ppm` 与
`offscreen-triangle-validation.ppm`；额外将验证版无损转为 PNG 并目视确认彩色三角形/黑色背景。
产物和验证日志留在 ignored out，不提交二进制图像。

## 开发中发现与修正

首轮构建成功、3 项 CPU 通过，2 项 GPU 探针异常退出（`out/verify/20260930-091222-1b33c5c0`）。
输出取消缓冲并关闭 CRT 交互报告后，日志显示主绘制/计算和寿命循环均已完成，失败在最后的 Memory 关闭用例
（`out/verify/20260930-091526-51846024`）。cdb 调用栈确认关闭资源之后构造输出 vector，
MSVC Debug 在 noexcept 构造函数中分配 `_Container_proxy`，失败引发 terminate，不能用外层 bad_alloc 捕获恢复。
因此入口先验证资源 open，关闭状态返回 invalid_state，测试改为验证关闭拒绝与正常回收；
不承诺任意 STL noexcept 构造的 OOM 可恢复。提交前可传播异常另以提交 seam 注入，验证无提交且没有 stranded ticket。
调试时一轮构建遇自身手工探针占用 exe 的 LNK1168（`out/verify/20260930-091419-1ee5e8aa`），
停止该探针后重新构建；未用旧程序作为通过证据。以上问题均由最终成功检查覆盖。

## 限制与下一项

未运行全量、Release、Linux/macOS、其他显卡/驱动、profiling ON 或 Tracy GPU capture。
没有模拟物理 GPU 丢失/真实 OOM；注入只验证指定失败边界。未新增管线缓存、描述符数组、多 set、顶点属性、
深度/混合、窗口或场景 Graph。同步入口每次创建资源/管线，以正确性验收为目的，不作性能基线。
M5.4 完成，M5 仍进行中；下一项为 M5.5 窗口与呈现。
