---
id: "0046"
created_at: "2026-09-28T19:00:00+08:00"
updated_at: "2026-09-28T19:14:00+08:00"
status: completed
design_refs:
  - ../design/graphics-shaders.md
---

# 0046 M5.3 Slang 编译工具

## 目标与实际实现

按 [Shader 设计](../design/graphics-shaders.md) 完成独立 CPU 编译库、dk-shaderc、最小布局反射和失败诊断。
核验 [官方 shader-slang port](https://github.com/microsoft/vcpkg/blob/master/ports/shader-slang/vcpkg.json)
当前为 2026.18、无 port 修订，与既有固定 baseline `33d78c1ed898a06938f31312167c7abefd229455` 一致。
不升级其他依赖，不使用浮动版本。

- [ShaderCompiler.hpp](../../engine/graphics/shaders/include/dk/graphics/ShaderCompiler.hpp) 和
  [实现](../../engine/graphics/shaders/src/ShaderCompiler.cpp) 提供单入口 vertex/fragment/compute 编译。
  每次创建独立 Slang global/session，使用 ComPtr RAII；直接 SPIR-V 1.5、默认优化、无 debug、row-major、保留入口名。
- 返回 Memory 拥有的 words、反射、diagnostics 和依赖路径，不暴露 Slang 类型；
  支持 storage/uniform buffer、sampled/storage image、sampler、固定 descriptor 数组、多 set、push constant 和 compute 本地尺寸。
  拒绝重复 binding、ParameterBlock、入口 uniform、未定长/无法表达的布局，保留源诊断和成功警告。
- [dk-shaderc](../../tools/shaderc/src/main.cpp) 支持中文路径、include/import、宏；单文件 SPIR-V 原子发布，JSON stdout、诊断 stderr。
  完成反射/JSON 序列化后才替换输出；拒绝覆盖源/include/import。stdout 的 shell 重定向不属于文件事务。
- 新增独立 `DK_BUILD_GRAPHICS_SHADERS` / `shaders` feature / windows-shaders 预设，graphics 预设同时启用 Device/Shaders。
  [部署脚本](../../cmake/DeploySlang.cmake) 配合 target 部署 compiler/优化器 DLL、standard modules 和 API bindings。
- 增加 [三角形](../../shaders/common/triangle.slang)、[compute](../../shaders/common/transform.slang)、
  [编译器测试](../../tests/unit/ShaderCompilerTests.cpp) 和 [进程测试](../../tests/integration/ShadercProcessTests.cmake)。
  同步设计/架构、依赖说明、指南、CLI 参考、测试选择表与 Roadmap。
  Shader 源文件在 .gitattributes 中固定 LF，与 C++ 源文件保持一致。

Slang 内部及临时 IO/请求/JSON 适配使用三方/标准分配器，返回数据使用指定 Memory heap。
不创建 Vulkan 对象，不影响此前 vk::raii 资源所有权，也不增加 Runtime 命令注册。

## 验证

环境：Windows x64、MSVC 19.51.36257、Debug /W4 /WX、动态 CRT、profiling OFF；
Slang 官方预编译 2026.18（运行时 build tag 同版本），不需要 GPU。

```powershell
cmake --preset windows-graphics -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_shader_tests', 'dk_shaderc') -TestRegex '^dk\.(shaders|shaderc)\.' -Reason '验证 M5.3 编译与进程链路'
cmake --preset windows-shaders -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-shaders -Target @('dk_shader_tests', 'dk_shaderc') -TestRegex '^dk\.(shaders|shaderc)\.' -Reason '最终验证独立离线编译配置'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version|runtime\.batch_roundtrip)$' -Reason '确认默认 CPU runner 隔离'
```

| 检查 | 结果与证据 |
| --- | --- |
| Graphics 配置首轮编译器与 CLI | 8/8 通过，0 跳过；`out/verify/20260928-185834-a3e737bc` |
| 独立 Shader 配置首轮 | 8/8 通过，0 跳过；`out/verify/20260928-190030-2a84730c` |
| 入口/警告补充验证 | 9/9 通过，0 跳过；`out/verify/20260928-190236-7ed1987b` |
| 修复优化器部署后的最终 Shader 配置 | 10/10 通过，0 跳过；`out/verify/20260928-191213-0933e5ec` |
| 默认 CPU runner | 2/2 通过，0 跳过；`out/verify/20260928-190256-4a491faa` |
| SDK SPIR-V 校验 | Vulkan SDK 1.4.321.1 `spirv-val --target-env vulkan1.3` 校验 vertex、fragment、transform compute、bindings compute 四份最终产物全部通过，编译 stderr 为空；位于 `out/shaders` |
| 依赖边界 | windows-shaders Device=OFF，安装目录无 Vulkan/volk/bootstrap；dumpbin 显示 shaderc 只依赖 mimalloc、Slang 和系统/CRT，Slang DLL 只依赖系统 DLL；windows-dev runner 无 Slang/Vulkan 导入 |
| 独立部署回归 | 实际构建目录的程序、mimalloc/Slang/优化器 DLL、标准模块与 API bindings 搬移后，PATH 仅保留 Windows/System32，成功且无 stderr；缺优化器时返回 1、无成功 JSON、保留旧产物；`out/verify/20260928-191325-df36a046` 1/1 通过 |

覆盖 SPIR-V magic/version/OpEntryPoint、反射已知 set/binding/type/count/字节大小/线程组、重复编译字节一致、
同进程 include 更新、跨进程 include/import 更新、宏、成功 warning、错误原文、缺入口/阶段、重复 binding、
不支持布局、Memory close 后产物仍可读、预算耗尽后零活分配、Unicode CLI、输出失败保留、源/依赖防覆盖。
编译产物四份都通过 SDK 校验；这不等于 GPU 执行验收。

开发中问题已修正：Slang imported target 起初仅目录可见，跨目录部署生成失败，
改用 find_package GLOBAL 后配置成功；补充测试发现 `[numthreads]` 本身可让 Slang 识别 compute，
最初“必须显式 shader attribute”的约定不准确，修正为接受 Slang 识别且与请求相符的阶段。
该轮 2/3 通过、1 失败记录在 `out/verify/20260928-190144-a454dd5d`，后续通过已覆盖修正。
独立搬移检查又发现 Slang 在缺少 slang-glslang 时会输出 spirv-opt 加载错误却返回成功，
开发环境 PATH 可能掩盖遗漏。补齐匹配版本的优化器部署，并通过 checkPassThroughSupport 在编译前拒绝缺失后端；
加入完整/缺后端两种隔离包回归，最终 10/10 和部署复验均通过。

`scripts/check-spec.ps1` 通过（99 个 Markdown、978 个本地链接、元数据/表格/索引/测试入口/JSON），
`git diff --check` 通过。

## 限制与下一项

未运行全量、Release、Linux/macOS、profiling ON、GPU shader 执行或跨平台 host 工具。
最小反射不包含成员偏移/顶点输入/跨阶段合并；不提供热重载、缓存或任意 Slang 开关。
Windows 原子输出沿用 IO 契约，无双文件/断电/并发路径事务保证。
M5.3 完成，下一阶段为 M5.4 离屏绘制与 compute 执行/读回。
