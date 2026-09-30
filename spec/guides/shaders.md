---
created_at: "2026-09-28T19:03:00+08:00"
updated_at: "2026-09-30T09:24:00+08:00"
---

# Slang 离线编译

[返回项目入口](../../README.md)。M5.3 提供 `dk::graphics_shaders` 和 `dk-shaderc`，
编译 vertex、fragment、compute 到 SPIR-V 1.5，并返回最小布局反射。
工具无需 Vulkan loader、驱动、GPU 或窗口；执行产物的管线与读回见 [M5.4 离屏指南](offscreen.md)。

## 构建和验证

```powershell
cmake --preset windows-shaders -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-shaders -Target @('dk_shader_tests', 'dk_shaderc') -TestRegex '^dk\.(shaders|shaderc)\.' -Reason '验证离线 shader 编译与工具'
```

独立预设启用 DK_BUILD_GRAPHICS_SHADERS、Memory 和 IO，自动选择 `shaders` feature，Device 保持关闭。
`windows-graphics` 同时启用 Device、Shaders 和 Offscreen，默认 `windows-dev` 不启用这些模块。
固定 vcpkg shader-slang 2026.18，动态 CRT。构建会复制 `slang-compiler.dll`、`slang-glslang.dll`（SPIR-V 优化器）、
`slang-standard-module-*`、`slang.slang` 和 `gfx.slang` 到程序旁；移动程序时也须保留这些文件及其 DLL 依赖。
自定义可执行目标链接 `dk::graphics_shaders` 后调用 `dk_deploy_slang(target)` 部署 Slang 数据。

## 编译仓库示例

从仓库根执行，三个入口可直接用于后续管线实验：

```powershell
New-Item -ItemType Directory -Force out/shaders | Out-Null
$shaderc = '.\out\build\windows-shaders\bin\Debug\dk-shaderc.exe'
& $shaderc compile --source shaders/common/triangle.slang --entry vertexMain --stage vertex --output out/shaders/triangle.vert.spv > out/shaders/triangle.vert.json
& $shaderc compile --source shaders/common/triangle.slang --entry fragmentMain --stage fragment --output out/shaders/triangle.frag.spv > out/shaders/triangle.frag.json
& $shaderc compile --source shaders/common/transform.slang --entry computeMain --stage compute --output out/shaders/transform.comp.spv > out/shaders/transform.comp.json
```

[triangle.slang](../../shaders/common/triangle.slang) 含使用 SV_VulkanVertexID 的彩色三角形 vertex/fragment；
[transform.slang](../../shaders/common/transform.slang) 使用 64×1×1 线程组、两个 storage buffer 和 16 字节 push constant。
SV_VulkanVertexID 直接使用 Vulkan VertexIndex，避免 SV_VertexID 引入 BaseVertex/shaderDrawParameters 额外特性；
映射见 [Slang SPIR-V 文档](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/a2-01-spirv-target-specific.html)。
M5.3 负责编译、SPIR-V 和反射；示例的实际 GPU 执行与读回已由 [M5.4](../development/0047-offscreen-execution.md) 验证。
JSON 重定向文件与 SPIR-V 没有共同原子提交，自动化脚本必须检查 `$LASTEXITCODE`。

## C++ 调用

```cpp
#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/memory/MemorySystem.hpp>

auto system = dk::memory::MemorySystem::create();
if (!system) return 1;
auto heap = system->create_heap({"shaders", dk::memory::DomainCategory::render});
if (!heap) return 1;
auto shader = dk::graphics::compile_shader(
    {"shaders/common/transform.slang", "computeMain", dk::graphics::ShaderStage::compute}, *heap);
if (!shader) return 1; // 记录 shader.error().message/context。
auto json = dk::graphics::shader_reflection_json(*shader);
// shader->spirv、bindings、push_constants 均为拥有型数据，无需保留 Slang session。
```

源码使用显式 `[[vk::binding(binding, set)]]` 固定布局；push constant 使用
`[[vk::push_constant]] ConstantBuffer<T>`。宏/include 参数、完整 JSON 字段、错误、阶段约定与输出保护见
[CLI 参考](../commands/shaderc.md)。库只返回内存产物，不写文件；输出拥有者可晚于编译器 session 或 Memory 关闭请求销毁。
Slang 内部和短期 IO/序列化适配使用三方/标准分配器，返回的引擎拥有型数据使用所传 Memory resource。

设计与限制见 [graphics-shaders](../design/graphics-shaders.md)，验收记录见
[0046](../development/0046-slang-shader-compiler.md)。尚未验收 Linux、Release、跨平台 host 工具或热重载。
