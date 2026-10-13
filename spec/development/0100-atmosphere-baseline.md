---
id: "0100"
created_at: "2026-10-13T11:54:00+08:00"
updated_at: "2026-10-13T12:24:00+08:00"
status: completed
design_refs:
  - ../design/render-atmosphere.md
  - ../design/foundation-profiling.md
---

# 0100 M14.7 大气质量、性能基线与阶段交付

## 目标与设计依据

按[大气设计](../design/render-atmosphere.md)完成固定质量集、独立CPU-only与required GPU验收，
提供可复现1080p冷/热LUT、CPU/GPU/API端到端和显存报告；性能候选目标与实测结果分开。
不新增依赖，不扩大到云或RenderSimulation视口。

## 实际变更

- `DK_BUILD_RENDER_ATMOSPHERE_REFERENCE`独立构建CPU参考，GPU大气选项仍隐含参考库；只要求Scene/Math的配置真实验证无Graphics/Slang链接。
- [AtmosphereSky](../../engine/render/atmosphere/include/dk/render/Atmosphere.hpp)提供lut_profile，只读返回该代已完成构建的GpuProfile。
  profile只持有CPU数据，随候选表数值检查成功后发布；缓存命中保留同一观察，失败不覆盖旧代。默认timestamp关闭时返回disabled。
- [质量探针](../../tests/integration/AtmosphereProbe.cpp)增加4高度×单散射/MS、每组8条地平线射线，
  将真实遮挡边界与插值变化分开，对照直接高采样检查相邻变化残差；另验证profile命中/非法索引/失败旧代保持。
- [性能探针](../../tests/integration/AtmosphereBenchmark.cpp)与[驱动](../../scripts/benchmark-atmosphere.py)提供三固定1080p预设、冷编译/LUT、
  static/Sky更新/物理重建/unlit、CPU/GPU/API端到端、VMA在途和驻留观察；3轮×3预设×OFF/ON，保存2538个计时帧和18张PPM。
  计时之外单独提交显存诊断帧，wait/collect前记录AP/HDR临时资源；不把显存查询开销计入性能样本。
- [汇总回归](../../tests/integration/AtmosphereBenchmarkTest.py)拒绝缺失/重复/错阶段、NaN、缺少Sky更新/Pass、Debug/validation混入或失败/泄漏。
  nearest-rank分位数保留尾部，冷帧不混入稳态，增量包含当帧Sky更新；GPU父区间不叠加子Pass。
- 更新[指南](../guides/atmosphere-benchmark.md)、测试选择表、设计/索引和Roadmap；完整[报告](../benchmarks/2026-10-13-atmosphere.md)、
  [质量](../benchmarks/2026-10-13-atmosphere-quality.json)、[性能来源/统计](../benchmarks/2026-10-13-atmosphere-evidence.json)与预设预览纳入版本管理。

## 验证记录

使用decker-spec-workflow、decker-state-contracts与decker-build-verify；不新增依赖，未改命令。
Windows x64 / RTX4070 Laptop / driver596.49 / MSVC19.51.36260.0 / Slang2026.18。
Debug验证行为；性能必须用RelWithDebInfo优化配置（/O2 /Ob1 /DNDEBUG），并与required GPU validation分开。
仅对子进程临时禁用冲突的AMD switchable隐式层；Khronos和同步validation仍启用。

配置/正式采样命令：

```powershell
cmake --preset windows-scripting -DDK_BUILD_RENDER_ATMOSPHERE_REFERENCE=ON
cmake --preset windows-editor -DDK_BUILD_SCRIPTING_LUAU=ON
cmake --preset windows-graphics
python scripts/benchmark-atmosphere.py --build-dir out/build/windows-graphics
```

定向验证均通过scripts/verify.ps1执行，准确targets/regex见各summary.json；可复跑入口见[指南](../guides/atmosphere-benchmark.md)。

| 范围 | 结果 | out/verify证据 |
| --- | --- | --- |
| CPU独立参考6项+环境跨进程1项 | 7通过，0失败/跳过；Graphics/Slang OFF | 20261013-120037-fa49ff6c |
| 天空/T/MS/真空/缓存/连续性、Lambert、AP、采集、天空样例 | 14通过；筛选另选中未构建的日光样例1项未运行 | 20261013-120115-169eff8a |
| 补建日光样例 | 1通过，0失败/跳过 | 20261013-121240-67e639e5 |
| 最终RelWithDebInfo采集required GPU与汇总6个Python用例 | 2个CTest通过，0失败/跳过 | 20261013-121611-be420e55 |
| 天空/AP完整数值日志归档 | 2通过，0失败/跳过 | 20261013-121627-68da47d2 |

质量：Sky最大nRMSE0.459072%、p99 1.40604%；AP最大nRMSE0.0134668%、p99 0.0763697%，
地平线邻接变化残差最大4.50389718%，均通过既有门槛。Sky全图单点最大10.1371%单列于报告，不从平均隐藏；
全图无单点8%门槛，显式地平线/连续性门槛独立通过。GPU验证诊断为0、CPU/VMA活分配回零。

最终矩阵`out/benchmarks/atmosphere-20261013-121828-a16bd1e6`：18进程全通过，源/二进制hash前后一致，
shader指纹fnv1a64:fb97c111419f22bb。晴空/日落/高空动态Sky增量GPU p95三轮中位数2.709/2.792/1.593ms，
**1.5ms候选均未达**。在途device-local资源约101.016MiB、host-visible约7.911MiB；block与资源字节、CPU heap分开。
GPU冷物理重建主要为MS约25ms；冷lit/AP延迟编译约0.67–0.69s，不能用稳态GPU代替启动成本。

交付检查：check-spec.ps1定向扫描10份Markdown、613个本地链接，metadata/表格/索引/测试入口/JSON均通过；
git diff --check通过，最终18份原始JSON/PPM及全部被测源码hash与版本化证据逐项一致。

## 失败、修复与偏差

首次构建（20261013-120004-7005d8ef）因unsigned到枚举的列表收缩转换失败，改为显式static_cast后构建通过。
首次质量regex额外包含dk_sunlight_demo而target列表没有构建它，CTest未运行该项；保留原未运行记录，随后单独构建/验证，不将其冒充通过。
文档初检发现开发索引新行与表格之间多一个空行，修复后定向文档检查通过。

初始完整矩阵`atmosphere-20261013-121331-dcba8dc4`的计时/质量通过，但显存只在wait之后观察，漏掉Graph已回收临时资源；
因此补充独立在途诊断并重新跑全部18进程，报告仅使用最终矩阵，不跨轮混合。旧目录保留用于追溯。
JUnit默认截断成功输出为1024字节，为完整质量报告另运行天空/AP数值两项并复制LastTest.log；
该日志为本机GB18030编码，归档JSON转为UTF-8。未重复昂贵的MS CPU参考。

## 限制与下一步

不改变质量门槛或降低LUT/AP尺寸；AP GPU成本与冷编译仍有优化空间。候选目标未达不阻止已定义的基线交付，不能宣传性能达标。
本轮未改Editor/IPC/脚本/capture，沿用0099真实GUI闭环验收；未做全量、跨设备、Tracy轨迹或GUI呈现/IPC排队延迟测量。
VMA为安全点采样，未声称瞬时精确峰值/系统显存；CPU heap不是RSS。驱动/OS缓存未清理、GPU未锁频。
M14交付节点D完成，下一项为M15.1；云、全PBR/IBL、几何阴影和独立Simulation视口大气不在交付范围。

## 修改记录

- 2026-10-13T11:54:00+08:00：冻结采集协议、观测生命周期和参考集。
- 2026-10-13T12:24:00+08:00：完成质量/性能采集、在途显存修正和阶段文档归档。
