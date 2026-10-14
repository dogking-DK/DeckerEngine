---
id: "0110"
created_at: "2026-10-14T10:52:20+08:00"
updated_at: "2026-10-14T11:09:04+08:00"
status: completed
design_refs:
  - ../design/graphics-raytracing.md
  - ../design/graphics-device.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
  - ../design/render-pathtracing.md
---

# 0110 M16.2 RT设备能力与BDA/VMA

## 目标与设计依据

实现[光追设备契约](../design/graphics-raytracing.md#m162设备能力与bda实施契约)：
三态RT请求、supported/enabled/缺项/limits、确定性选卡、BDA buffer与VMA配置。
本阶段不构建AS、SBT或光追shader，不升级依赖。

## 实际变更

- [Device.hpp](../../engine/graphics/device/include/dk/graphics/Device.hpp)增加RayTracingMode、
  RayTracingSupport及Device实际启用查询；能力包含三个扩展、三个feature、AS/RT原生limits，
  固定缺项数组无需堆分配，保存的属性链pNext清空。公开设置追加字段，原调用默认disabled。
- [Selection.cpp](../../engine/graphics/device/src/Selection.cpp)将required加入纯选卡策略；
  显式adapter拒绝不回退，if_available保留原排序。无效模式在创建loader前拒绝。
- [Device.cpp](../../engine/graphics/device/src/Device.cpp)逐设备枚举扩展、按支持查询feature/property，
  WSI与RT启用链合并；仅完整请求增加三个扩展/BDA+AS+RT feature，
  校验九个本Device入口，VMA仅此时启用BUFFER_DEVICE_ADDRESS标志。
  原RAII提交点和共享queue view不变，驱动/入口/VMA失败不重试非RT，不发布半设备。
- [Resources.hpp](../../engine/graphics/device/include/dk/graphics/Resources.hpp)、资源策略和工厂增加
  ShaderDeviceAddress usage与Buffer::device_address()：分配前检查enabled，查询检查owner/lost/usage及非零地址，
  不改变内容/访问状态；Buffer share/move继续保活同一分配及Device。AS/SBT专用usage仍拒绝。
- [DeviceTests](../../tests/unit/DeviceTests.cpp)覆盖六项逐一缺失×三种请求、显式选卡、属性快照、
  精确启用链、VMA标志、缺九个入口、失败/取消清理、WSI链合并和共享surface owner；
  [资源单元](../../tests/unit/GraphicsResourceTests.cpp)更新BDA描述策略并保持未实现usage拒绝。
- 新增[required RT探针](../../tests/integration/RayTracingDeviceProbe.cpp)及CMake/测试选择表入口：
  无窗口、required validation与同步验证，三种模式、三种内存角色、地址稳定性/普通buffer拒绝、
  queue销毁后的owner保活、VMA/Memory零残留；缺硬件/能力/验证层为失败而不是77。
- 同步[Graphics指南](../guides/graphics.md#可选rt设备与bda)、设计与Roadmap，修正普通设备示例筛选以免误选新增required探针。
  不新增模块target、命令或第三方依赖；本次新增target仅测试程序。

## 验证

主要构建沿用windows-graphics/RelWithDebInfo，MSVC 14.51与现有固定依赖；CPU隔离使用windows-dev/Debug。
下列范围通过scripts/verify.ps1执行，原始summary.json/results.xml/build.log/tests.log留在out/verify。

| 运行编号（均20261014前缀） | 范围 | 结果 |
| --- | --- | --- |
| 105753-6771a3bd | Device/资源/RT探针目标编译 | 失败；测试中的有符号比较告警及Catch条件表达式导致编译停止，测试未运行；已修复 |
| 105850-9678f3d4 | 34项CPU单元与5项真实GPU Device/资源/RT检查 | 38通过、1失败、0跳过；唯一失败是资源validation探针将13条旧AMD隐式层API警告计为失败；GPU error=0 |
| 110208-3907f406 | 23项Device单元（含新增WSI）+required RT+资源validation | 25通过、0失败、0跳过；两个GPU探针均零warning/error/live allocations |
| 110223-61002d29 | CPU-only dk_run版本、stdio交互 | 2通过、0失败、0跳过；原筛选中的batch_success无匹配，实际选择仅此两项，不计batch验证 |
| 110633-1b16f153 | 非RT离屏draw/compute/readback与validation | 1通过、0失败、0跳过，像素/整数结果与预期一致，零warning/error/live allocations |

关键复现命令（第一次全范围与后续补验分别列出）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_device_tests','dk_graphics_resource_tests','dk_raytracing_device_probe','dk_device_probe','dk_graphics_resource_probe'
) -TestRegex '^dk\.(device\.|graphics\.(unit\.|gpu_resources))' -Reason 'M16.2 RT capability and BDA regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_device_tests','dk_raytracing_device_probe','dk_graphics_resource_probe'
) -TestRegex '^dk\.(device\.(unit\.|rt_required_validation$)|graphics\.gpu_resources_validation$)' -Reason 'M16.2 WSI and required RT validation'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target dk_offscreen_probe -TestRegex '^dk\.offscreen\.gpu_validation$' -Reason 'M16.2 non-RT draw compute and readback'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_run -TestRegex '^dk\.(bootstrap\.version$|runtime\.stdio_interactive$)' -Reason 'M16.2 CPU-only isolation'
```

首次警告来自本机AMD manifest声明API 1.3.287；读取其disable_environment后，后两次GPU检查仅在
验证进程设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并finally恢复。
manifest位于DriverStore的u0407082.inf_amd64_5e872c9a82fd665c/B406401/amd-vulkan64.json；
未改注册表/持久环境、未关闭Khronos或同步验证。复验无警告；不将初轮失败写成通过。

真实required RT结果：NVIDIA GeForce RTX 4070 Laptop GPU，驱动596.49，Vulkan 1.4.329，
两条同族queue。required/if_available均supported=1、enabled=1，disabled为supported=1、enabled=0。
AS limits：geometry/instance=16777215、primitive=536870911、scratch alignment=128，
每set/每stage AS descriptor=1048576。RT limits：handle size/alignment=32/32、base alignment=64、
max stride=4096、recursion=31、dispatch invocation=1073741824、hit attribute=32。
这些是实测设备上限，不是引擎已支持的场景规模或已完成AS/trace的证明。

CPU-only缓存全部Graphics开关OFF；dk-run的dumpbin `/dependents`无Vulkan/Slang DLL，
版本与stdio进程通过。未重跑M16.1积分器、全引擎、Release矩阵或编辑器GUI；
WSI合并用fake dispatch验证，真实GPU验收为无窗口设备和离屏路径。

增量合计43个不同CTest条目通过（23 Device单元、12资源单元、6 GPU与2 CPU-only进程检查），
不是单轮全量43项重跑。最终生产源码自首轮完整编译后未改变；后续仅补测试与严格warning检查。
定向check-spec通过：11份Markdown、684个本地链接、元数据/表格/索引/测试入口及JSON清单；
git diff --check通过。

## 边界与下一步

BDA随RT整组启用，不开放独立BDA-only模式、capture/replay、host/indirect AS build、ray query或纹理非一致索引。
只返回借用地址，不自动将地址关联到提交寿命；调用者沿现有Buffer owner/retain契约保活。
required RT验收成功不等于存在BLAS/TLAS或shader管线；下一项是[Roadmap](../roadmap.md#m16渐进式路径追踪)的M16.3。

## 修改记录

- 2026-10-14T10:52:20+08:00：建立实施契约与开发记录。
- 2026-10-14T11:05:29+08:00：能力/BDA实现、CPU策略、required RT与资源/CPU隔离通过；补验非RT离屏链路。
- 2026-10-14T11:09:04+08:00：非RT离屏与文档/差异检查通过，完成M16.2，共43个不同条目的增量通过证据。
