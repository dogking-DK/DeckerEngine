---
created_at: "2026-10-02T23:20:00+08:00"
updated_at: "2026-10-02T23:20:00+08:00"
---

# GPU Graph 声明与校验

[返回项目入口](../../README.md)。接口契约见 [Graph 设计](../design/graphics-graph.md)。
当前可声明资源、Pass、依赖和输出，并在 CPU 上校验；尚无 compile/execute 或 GPU 绑定入口。

## 构建与验证

`windows-graphics` 开启 `DK_BUILD_GRAPHICS_GRAPH`，其他配置可显式开启，同时要求
`DK_BUILD_GRAPHICS_DEVICE` 和 Memory。消费者链接 `dk::graphics_graph`，
包含 `dk/graphics/Graph.hpp`；不要求 Slang、SDL3 或创建 Vulkan 设备。

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_graph_tests `
  -TestRegex '^dk\.graph\.' -Reason 'GPU Graph 声明与结构校验'
```

## 声明顺序

1. 以开放的 Memory ResourceHandle 创建 `graph::Graph::create(resource)`。
2. `declare_buffer/declare_image` 返回类型化 ID；默认 transient 内容未初始化。
   external 目前只是逻辑导入声明，`initialized=true` 表示调用者保证整个资源内容有效。
3. `add_pass(PassDesc)` 复制名字和访问列表；每项 `Use` 包含资源 ID 及 AccessDescription。
   使用 M5 的 stage/access/layout 枚举。buffer 指定 offset/size，image 指定 aspect/mip/layer。
4. 完整写入访问范围时设置 `full_overwrite=true`；其他访问保留内容，需要已初始化数据。
   声明完整写入时不得同时带读访问。每个 Pass 的同 buffer 访问合并，image 子资源不能重叠。
5. `mark_output(id)` 标记需要保留的完整资源；外部副作用通过 PassDesc.side_effect 表达。
   `add_dependency(before, after)` 增加显式先后关系，`remove_dependency` 可修正依赖。
6. `validate()` 检查内容、资源依赖和显式边；失败通过 Error 的 message/context 定位。
   它不缓存成功状态，每次修改后需要重新校验。

读写依赖保持 Pass 声明顺序。buffer 同步按整个对象，image 按子资源相交；
覆盖写仍然需要等待先前读写。显式边可以重排互不依赖的 Pass，反转已有资源依赖会报告循环。
一个 Pass 内不支持隐藏的状态切换，需拆成多个 Pass。

[GraphTests.cpp](../../tests/unit/GraphTests.cpp) 的
`graph declares upload compute draw and readback without a device` 是完整公共接口例子：
导入 upload → 写 transient mesh → compute 更新 → draw 写 color → transfer 到 external readback。
验证整个声明链不触发 loader、设备、窗口、命令录制或 GPU 提交。

## 句柄与失败

不要将临时图 ID 作为持久资源身份。跨图、默认和 reset 前的 ID 会被拒绝；
move 保留转入图的身份。reset 成功后所有旧 ID 失效；失败则原图保留。
查询得到的 name/span 是只读借用，图发生修改、移动赋值或销毁后不要继续使用。
声明参数或分配失败不会发布半个 Pass，不会改变已存在的资源和依赖。

尚未支持执行计划、裁剪、transient 分配、barrier 规划、GPU 回调、实际 external owner 绑定、
状态导入导出或完成跟踪；依赖与后续步骤见 [Roadmap](../roadmap.md)。
