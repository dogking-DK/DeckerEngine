---
id: "0123"
created_at: "2026-10-16T11:15:00+08:00"
updated_at: "2026-10-16T11:29:51+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-atmosphere.md
  - ../design/render-clouds.md
  - ../design/editor.md
  - ../design/render-capture.md
  - ../design/architecture.md
---

# 0123 大气云路径集成规划与介质参考

## 目标与设计依据

用户选择“制定分阶段方案，并实现第一阶段”。依据[路径设计](../design/render-pathtracing.md)建立M17六阶段，
本次实现M17.1世界空间介质基础和CPU透射参考；Editor、GPU体积路径、capture及性能优化属于后续独立验收。
不把既有surface-only兼容确认移除，也不以屏幕空间合成冒充体积路径追踪。

## 实际变更

设计先冻结物理模型、CPU/GPU顺序、输入/资源失效、采样预算、正确性与性能验收。
[Roadmap](../roadmap.md#m17大气云与路径追踪集成)登记六个独立阶段，后续GPU/上层/优化均未开始。

- [PathMediumReference.hpp](../../engine/render/pathtracing/include/dk/render/PathMediumReference.hpp)与
  [实现](../../engine/render/pathtracing/src/PathMediumReference.cpp)：不可变环境/显式时间/云场owner，世界点RGB消光及三种散射系数，
  向上舍入的全局消光majorant，空气/云最多5个有序区间，地面边界及有限距离裁剪，CPU分段透射/光学厚度。
  参数、field key、支持域和工作预算先检查，成功返回才发布值；取消返回错误，旧快照和旧场保留。
- 初测发现精确外壳切线经b²-c计算会生成约浮点误差尺度的伪区间；改为垂距差平方计算判别式，
  结合稳定q/差平方c求根，并对判别式为0返回同一根。保留近地1.8米、双壳、内外切线回归。
- [Render CMake](../../engine/render/CMakeLists.txt)在路径模块前装配CloudFields，reference PUBLIC链接纯CPU字段模块；
  只开路径参考也自动提供CloudFields。旧GPU、shader、Scene持久化及命令行为未改。
- [PathMediumTests](../../tests/unit/PathMediumTests.cpp)加入已有dk_pathtracing_reference_tests；
  M15已启用的配置额外链接CPU cloud_reference作独立透射对照，纯CPU最小配置不要求该模块。
  当前8个新用例覆盖解析、消光叠加、真实云步数加倍、区间/坐标、输入寿命、非法参数、取消及工作额度。

不新增或升级三方依赖，不更改Scene持久化和命令schema。

## 验证记录

Windows/MSVC 19.51、Release；只验证M17.1及直接受影响CPU路径链路。
14个不同CTest通过（8个新增介质用例、6个原表面用例），纯CPU隔离配置重复验证其中9个，均零跳过。
不把重复配置的9项计成新增覆盖，也不把缺少M15的纯CPU配置中未注册的交叉参考用例记作通过。

| out/verify目录 | 结果 |
| --- | --- |
| 20261016-112116-fea1ac6b | 沙箱内CMake重生成无法访问现有C:/code/third_lib/vcpkg；构建失败，未运行测试。 |
| 20261016-112131-468b5d14 | 正常本机权限构建成功，介质7/8通过；精确外壳切线伪区间失败，修正后重跑。 |
| 20261016-112352-10a078cb | 最终介质8/8通过，包括M15真实云透射与混合消光对照。 |
| 20261016-112505-c9fdbf3c | 旧表面解析、media拒绝、样本序列/区间、空场景、快照寿命6/6通过。 |
| 20261016-112549-6e5287ad | 独立windows-path-media-cpu配置9/9通过：7个介质用例加2个旧表面用例。 |

数值门槛：垂直指数/三角吸收解析光学厚度及T绝对误差<2e-6，反向/平移T误差<1e-10，
分段乘积及空气/云消光叠加<2e-6，固定真实云与M15同采样透射差<1e-9，1024→2048步T差<.005。
这些是已通过的断言阈值，不是所有输入上的全局误差证明，也不是辐亮度/多散射的质量结果。

CPU隔离配置沿windows-dev，关闭资产导入/Runtime/Jobs/runner/examples，仅新增路径参考；
Graphics全部OFF、空气/云渲染及参考开关OFF，CloudFields显式开关OFF仍能由路径依赖自动装配。
从已有固定baseline二进制缓存恢复依赖；测试程序链接中没有Vulkan/Slang，非替换工具链或关闭本次目标来绕过错误。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.medium ' `
  -Reason 'M17.1世界空间介质及M15独立参考'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(black |budgets |sample |scene |empty |counter )' `
  -Reason '原表面路径直接回归'
cmake --preset windows-dev -B out/build/windows-path-media-cpu `
  -DDK_BUILD_RENDER_PATHTRACING_REFERENCE=ON -DDK_BUILD_ASSET_RUNTIME=OFF `
  -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_FRAMEWORK=OFF -DDK_BUILD_JOBS=OFF `
  -DDK_BUILD_RUNNER=OFF -DDK_BUILD_CTL=OFF -DDK_BUILD_EXAMPLES=OFF
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(medium |black |budgets )' `
  -Reason '纯CPU依赖边界'
```

文档检查覆盖10份Markdown、本地链接、元数据/索引/测试入口/JSON，git diff --check通过。
未运行GPU/GUI/capture测试和性能基准：本阶段没有接入相应链路，不能从CPU参考通过推断GPU可用或性能提升。
未重建dk-editor.exe；当前可执行文件仍保留原surface-only能力边界。

## 遗留问题与下一步

按[Roadmap](../roadmap.md#m17大气云与路径追踪集成)完成M17.2体积路径参考、M17.3/4 GPU、M17.5上层闭环和M17.6优化。
本记录完成只代表第一阶段和规划交付；用户原有spec/README.md及AI文档指南改动不纳入提交。

## 修改记录

- 2026-10-16T11:15:00+08:00：创建方案和第一阶段记录。
- 2026-10-16T11:29:00+08:00：完成CPU介质实现、切线修正和定向/隔离验证；M17.1完成，后续阶段未开始。
