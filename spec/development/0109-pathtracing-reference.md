---
id: "0109"
created_at: "2026-10-14T10:00:00+08:00"
updated_at: "2026-10-14T10:29:36+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/architecture.md
---

# 0109 M16.1 CPU路径追踪参考

## 目标与设计依据

落实[路径追踪M16.1契约](../design/render-pathtracing.md#m161-cpu参考实施契约)：
不可变场景输入、确定性样本、Lambert直接/间接光、MIS/RR、透视相机和有界CPU参考。
本次不涉及Vulkan/AS、GPU积分、编辑器或capture命令扩展；下一阶段仍由Roadmap维护。

## 实际变更

- 先冻结模型、随机维度、深度语义、输入与工作额度、解析/统计门槛。
- 新增[CPU模块](../../engine/render/pathtracing/CMakeLists.txt)和
  [公开接口](../../engine/render/pathtracing/include/dk/render/PathTraceReference.hpp)：
  拥有型PathTraceScene、稳定来源/指纹、独立双精度求交、透视主射线、
  固定射线均值/二阶矩、拥有型线性图像与设置/样本/计数报告。
- [场景转换](../../engine/render/pathtracing/src/PathTraceScene.cpp)复制消费的世界几何/材质，
  保留完整仿射效果；[积分器](../../engine/render/pathtracing/src/PathTraceReference.cpp)实现
  常量环境、方向太阳、面积灯、余弦Lambert、NEE/MIS、RR和连续计数样本。
  预算先于积分/图像分配，取消或分配失败不发布部分结果，旧输入/输出保持有效。
- 新增默认OFF的DK_BUILD_RENDER_PATHTRACING_REFERENCE；自动接入RenderData/asset_data，
  reference不依赖Geometry实现、导入器、Vulkan或Slang。RenderData测试改按实际target存在注册，
  覆盖由reference开关间接启用的情形；没有新增/升级三方依赖。
- 新增[程序化示例](../../examples/pathtracing/src/main.cpp)、
  [使用指南](../guides/pathtracing-reference.md)、[16项参考单元测试](../../tests/unit/PathTraceReferenceTests.cpp)
  和[示例集成检查](../../tests/integration/PathTraceReferenceDemoTest.py)。同步构建指南、架构、索引、Roadmap和测试选择表。
  示例输出PPM与线性JSON，两个文件分别原子替换；未增加命令或Scene持久字段。

## 验证记录

配置使用现有windows-dev/Debug、MSVC 14.51；Graphics相关开关均OFF，
`cmake --preset windows-dev -DDK_BUILD_RENDER_PATHTRACING_REFERENCE=ON`配置通过。
正式验证均通过版本化脚本，命令如下：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @(
  'dk_pathtracing_reference_tests', 'dk_pathtracing_reference_demo',
  'dk_render_data_tests', 'dk_geometry_tests'
) -TestRegex '^dk\.(pathtracing|render\.data|geometry)\.' -Reason 'M16.1 CPU reference, input lifecycle and example verification'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(scale aware|empty degenerate)' -Reason 'M16.1 additional scale robustness and bounded scene input cases'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.mixed light' -Reason 'M16.1 mixed environment sun and emitter selection probabilities'
```

| 验证 | 结果与证据（本地out日志） |
| --- | --- |
| 初轮定向构建/测试 | 23通过、0失败、0跳过；13项参考单元、4项Geometry、5项RenderData及1项示例；`out/verify/20261014-101735-9695416c`，测试约34.12秒 |
| 新增尺度/输入边界 | 2通过；比例1e-6/1/1e4、自遮挡、世界面积与裁剪、退化/缺失/重复/下调额度；`out/verify/20261014-102306-4d6fba30` |
| 新增混合光源 | 1通过；环境/太阳/面积灯的三种两两组合及三类共同启用，MIS与BSDF-only均对照独立能量积分；`out/verify/20261014-102443-ffd6b4ce` |
| CPU-only运行/链接 | 所有Graphics模块关闭；示例dumpbin `/dependents`仅列flecs、mimalloc、Windows/MSVC运行库，无Vulkan/Slang DLL |
| 指南128spp示例 | 通过；24×24、12三角形、seed=42，输出`out/pathtracing-reference/reference.ppm`和`reference.json`；CPU积分7.648秒，非性能验收 |
| 文档/差异检查 | 定向check-spec通过：10份Markdown、707个本地链接、元数据/表格/索引/测试入口/JSON清单；git diff --check通过 |

增量合计**26个不同CTest条目通过，0失败、0跳过**；不是一次全量26项重跑。
生产源码在首轮通过后未变化，后两轮只新增测试。取消覆盖调用前与计算中，Memory额度逐步释放
覆盖初始和中途分配失败、候选释放与旧owner保活。固定样本分段加权合并与一次积分误差≤1e-12。

冻结阈值后实测（各统计组8个seed，原始输出见首轮results.xml）：

| 对照 | 实测 |
| --- | --- |
| 白炉解析0.125 | MIS均值0.124883，标准误差3.96802e-5 |
| 面积灯独立二维积分0.287349 | MIS 16384spp为0.287082；65536spp为0.287230；BSDF-only 16384spp为0.288181 |
| 面积灯64/256/1024spp集合MSE | 4.1554e-5 / 9.61042e-6 / 5.17669e-6，持续下降 |
| 多反弹颜色解析(1.12,0.22,0.105) | 无RR各通道误差≤1e-4；红通道RR起点1/3均值1.12169 / 1.12067，均通过2%与5标准误差门槛 |

失败与修复：`101230-96e3295b`发现测试成员stderr与MSVC宏冲突，改为standard_error；
`101348-67b05f17`发现reference自动启用RenderData后测试target未注册，改按target存在判断；
`101614-2fbb57df`发现示例Memory expected类型与通用Result helper不兼容，改用其自身value接口。
三轮均在构建失败后停止，没有使用旧二进制作通过证据。沙箱内编译/原子输出权限受限，
经自动审批后同一命令成功；128spp首次沙箱写入失败，提升后输出成功，无算法或路径变更。

未运行Release矩阵、全引擎回归和GPU/validation检查；本阶段仅CPU参考，
没有RT设备/AS/SBT/shader实现，不将CPU通过计为GPU验收。

## 决策与限制

参考采用世界空间平面法线与双精度暴力求交，Geometry BVH仅作测试对照。
明确拒绝金属/纹理/alpha材质，防止默认metallic=1被误解释为Lambert。
所有计算先预算，失败不发布部分候选；逐样本/反弹支持协作取消。
同一实现/平台/输入/样本区间可重现；不承诺跨CPU/GPU的浮点逐位相同。
规模与工作上限用于小场景数值验证，暴力遍历不是交互性能方案；PPM用于预览，误差使用线性RGB。
后续阶段由[Roadmap](../roadmap.md#m16渐进式路径追踪)维护，下一项是M16.2 RT能力与BDA/VMA配置。

## 修改记录

- 2026-10-14T10:00:00+08:00：建立实施契约与本记录。
- 2026-10-14T10:28:00+08:00：实现、示例与26项增量定向检查通过，记录统计证据和限制。
- 2026-10-14T10:29:36+08:00：文档/差异检查通过，完成阶段留档。
