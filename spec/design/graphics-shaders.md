---
module: graphics-shaders
created_at: "2026-09-28T19:00:00+08:00"
updated_at: "2026-09-28T19:12:00+08:00"
status: accepted
---

# Slang 离线编译与最小布局反射

## 目标与边界

M5.3 提供 `dk::graphics_shaders` 和 `dk-shaderc`：单次编译一个 `.slang` 文件中的一个具名
vertex、fragment 或 compute 入口，产出 SPIR-V 1.5、布局反射及诊断。模块只依赖 Core、Memory、
IO、Profiling、Slang 和 JSON，不依赖 Device、Vulkan loader、窗口或 GPU。
管线创建、shader module、执行/读回属 M5.4；热重载、缓存、特化、ray tracing、自动管线布局合并不在此阶段。

## 接口与参数约定

公开头 `dk/graphics/ShaderCompiler.hpp` 不暴露 Slang 类型；请求包含源路径、入口名、阶段、
有序 include 搜索路径与宏定义。入口推荐带 `[shader(...)]`；也接受 Slang 从 `[numthreads]` 推断的
compute 入口。入口必须由 Slang 识别阶段，且与请求阶段一致，不能用请求强制改变源码阶段。
固定 SPIR-V 1.5、row-major matrix、默认优化、无 debug 信息，不提供浮动 profile 或任意编译器开关。
全局资源采用 `[[vk::binding(binding, set)]]`；push constant 使用 `[[vk::push_constant]] ConstantBuffer<T>`。
最小反射返回入口/阶段、compute 本地线程数、descriptor set/binding/type/count、push constant 字节范围。
普通 constant buffer、storage buffer、sampled/storage image、sampler 与固定大小 descriptor 数组为首批范围；
无法准确表达的布局（例如 ParameterBlock、未定长 descriptor 数组、入口 uniform 参数）明确拒绝，不能静默遗漏。

编译返回拥有型结果，SPIR-V words、反射列表和字符串采用调用者提供的 Memory resource；
Slang COM 对象由上游 ComPtr RAII 管理，内部编译分配属于第三方内存边界；
IO 字节、请求适配字符串与 JSON 序列化临时对象使用现有标准/三方分配器，不计入返回产物预算。
每次调用创建独立 session，不跨调用缓存已加载模块；相同输入和固定编译器配置可重复，源码/include 修改在下一次生效。
调用者保证输入文件在一次编译期间不变，不承诺跨编译器版本或机器的字节稳定。
错误使用 dk::Result，保留 source/entry/阶段和 Slang 原始诊断；成功警告单独返回。
基础设施分配异常继续抛出，CLI 捕获后以独立退出码报告。

## 输出与失败契约

库只返回内存产物，不修改文件。CLI `compile --source ... --entry ... --stage ... --output ...`
将原始 SPIR-V 原子替换到单个输出文件，成功后 stdout 输出 JSON 反射，stderr 输出诊断。
JSON 在文件提交前完成序列化；不引入两个文件同时提交的假保证。stdout 重定向由调用者负责，
stdout 写入失败不能回滚已经提交的 SPIR-V。编译/布局拒绝/输出替换前失败保留既有输出。
输出不能覆盖本次源文件或依赖文件；不自动创建父目录。
Windows 文件发布复用 IO 同目录临时文件替换契约；其他平台编译库可用性与文件发布分别验证。

## 构建与验证计划

`DK_BUILD_GRAPHICS_SHADERS` 独立开关及 `shaders` vcpkg feature，要求 Memory/IO；
`windows-shaders` 提供独立离线构建，`windows-graphics` 同时启用 Device/Shaders。固定 baseline 对应
shader-slang 2026.18（官方当前 port，无修订），不改变 CPU runner 依赖。
部署 Slang DLL 时同时复制 SPIR-V 优化器运行库 slang-glslang、官方要求的 standard modules 和 API binding 文件。
编译前检查 SPIRV_OPT 后端可用性；缺失时报错，不能接受上游缺后端仍返回产物的降级行为。
定向验证 vertex/fragment/compute SPIR-V、反射与数组/多 set/push constants、include/宏、重复编译、
语法/入口/阶段/布局错误、Memory 寿命和 CLI 输出失败保护/退出码/Unicode 路径。
编译器测试不需要 GPU；不把产物实际 Vulkan 执行写成 M5.3 验收。
实际结果见 [0046](../development/0046-slang-shader-compiler.md)，使用与参数见
[Shader 指南](../guides/shaders.md)、[dk-shaderc 参考](../commands/shaderc.md)。

参考：[Slang 编译 API](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/08-compiling.html)、
[反射 API](https://docs.shader-slang.org/en/latest/external/slang/docs/user-guide/09-reflection.html)、
[IO 契约](foundation-io.md)、[Memory 契约](foundation-memory.md)。
