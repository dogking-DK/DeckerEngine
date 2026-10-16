---
id: "0124"
created_at: "2026-10-16T11:48:00+08:00"
updated_at: "2026-10-16T12:14:04+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/architecture.md
---

# 0124 M17.2 CPU体积路径参考

## 目标与设计依据

按用户要求完成M17.2。依据[路径设计](../design/render-pathtracing.md#m172-cpu体积路径实施契约)，
在M17.1世界空间介质上建立RGB逐通道体积路径积分，覆盖表面/体积散射、太阳圆盘、NEE/MIS与多次散射。
保留原表面路径API，GPU与Editor仍属于后续阶段。

## 实际变更

设计先冻结model=4/sampler=2、介质/光源采样、边界和失败保护，再增加
[VolumePathReference.hpp](../../engine/render/pathtracing/include/dk/render/VolumePathReference.hpp)、
[积分实现](../../engine/render/pathtracing/src/VolumePathReference.cpp)及
[相函数](../../engine/render/pathtracing/src/VolumePhase.hpp)，编入原dk_render_pathtracing_reference。

- 每路径均匀选一个RGB通道并乘3补偿，analog delta tracking选择散射/吸收/null；
  Rayleigh逆CDF及Mie/云HG混合相函数使用完整混合PDF，真实事件共享表面反弹深度。
  小g使用HG逆CDF的等价稳定形式；不把近零g的采样分布改成与PDF不一致的常量近似。
- 空气全局majorant只用于无云区间，混合区间用空气加云上界；跨区间继续null计数。
  NEE以独立序列做ratio tracking，表面和介质均检查场景/mask/行星遮蔽；
  面积光、常量边界、有限太阳圆盘按类等概率采样，与BSDF/相函数使用power MIS。
  太阳采用已有L=E/(πsin²α)，不叠加旧方向光；真实多散射不使用M14/M15的增亮近似。
- 主网格near/far与物理介质边界分开；行星和网格取最近交点，二次表面起点数值外推到行星外侧。
  边界复查修正最大支持半径下“远端外球出口超2e7但近地面有效”的误拒绝；
  在受限查询已经证明近地面终止时使用该结果，真正超域的长路径仍返回not_supported。
- 单射线返回均值/二阶矩/实际统计；拥有型图像额外冻结场景、完整环境、云场hash、时间、采样/模型和相机。
  独立事件/用途/counter采样v2可按区间重组；统计包含真实多次介质散射路径数。
  几何工作预检、总tracking及每路径额度、活动取消和分配失败均不发布部分图，旧输入及旧图保留。
- [VolumePathTests](../../tests/unit/VolumePathTests.cpp)新增14个用例；
  [使用指南](../guides/pathtracing-reference.md#cpu体积路径参考)说明入口、参数、支持域与统计噪声。

没有新增依赖、命令或Scene持久化字段；M16 model=3/sampler=1及其surface-only行为保持原样。

## 验证记录

Windows/MSVC 19.51、Release，使用既有windows-path-media-cpu配置。
Graphics/Slang/Editor、空气/云GPU和旧CPU光照参考全部OFF；CloudFields仍由路径依赖自动装配。
最终48个不同CPU路径CTest通过（14个新增体积、7个M17.1介质、27个表面/纹理/GGX等回归），零跳过。
随后边界修正重跑14个体积用例，补介质顶点遮蔽断言再单独重跑1个；重复运行不计为新增覆盖。

| out/verify目录 | 实际结果 |
| --- | --- |
| 20261016-120002-0966546a、20261016-120103-79e55c80 | 编译失败，测试未运行：测试内部detail命名空间歧义、Windows stderr宏与诊断表达式冲突；修正测试代码。 |
| 20261016-120122-a119ba86 | 8/13通过；5个大气夹具的吸收层参数超过其缩短后的大气高度，修正有效域。 |
| 20261016-120247-88dba0c9 | 11/13通过；单次散射夹具直视太阳却只比较散射量，改为从大气顶向黑地面观察；寿命夹具在关闭heap后构造新资产容器，改为关闭前清理。 |
| 20261016-120456-dea068a9 | 1/2通过；单次散射解析均值通过，N/4N差0.565%超过0.5%阈值。保留阈值与seed，增加独立pixel计数以降低通道采样噪声。 |
| 20261016-120551-54538538 | 2/2通过：单次散射N/4N RGB收敛，以及真实云、空气/云混合白炉。 |
| 20261016-120739-f51d2fd5 | CPU路径模块48/48通过，16.39秒；包括原M16及M17.1链路、体积图像活动取消与云场寿命。 |
| 20261016-121050-93472b26 | 最大半径地面裁剪修正后，体积14/14通过，13.37秒。 |
| 20261016-121210-3e0d48a8 | 补充介质散射顶点的opaque遮蔽/mask穿透断言，单次散射用例通过。 |

统计固定8个seed：101+7919×i，i=0..7；不同seed作为独立集合计算标准误差。
解析均值要求逐RGB误差≤2%加1e-4，并落在5标准误差加1e-4以内。
一般夹具每seed 16384或65536样本；单次散射N/4N每seed使用16个独立pixel，每pixel N=16384、4N=65536。
相函数用独立余弦积分和采样矩检查Rayleigh、HG g=±0.99、近零g、云g=-0.95及混合分布。

| 数值场景 | 参考与结果 |
| --- | --- |
| 真空/表面 | 常量边界、Lambert白炉、GGX独立角度积分、面积灯独立平面积分均通过；MIS与continuation-only均值一致。 |
| 有限太阳 | 圆盘L、Lambert法向E/π、太阳与常量边界重叠、几何/mask阴影通过；阴影不继承相机far。 |
| RGB纯吸收 | 垂直指数解析T×边界RGB；8×65536样本均值最大相对偏差约0.46%，主相机far从无穷缩到10米不改变介质结果。 |
| 单次散射 | 独立球面/指数积分0.0186425；4N均值(0.0186444,0.0186223,0.0186049)，最大偏差约0.20%；N/4N最大通道差约0.14%，RGB均值投影差0.0758%，均低于0.5%。 |
| 空气多次散射 | 保守散射、白地面/白边界，深度8→32；32深度均值(0.997503,1.00213,1.00029)，RR开关和NEE开关通过；断言超过1000条路径发生至少两次真实介质散射。 |
| 非均匀云 | 4096→8192步确定性T差<1e-4，参考T=0.342893；随机透射和太阳云影通过统计门槛；云及混合空气/云白炉最大均值偏差<1%，断言实际重复介质散射。 |
| 失败与寿命 | 环境/样本/预算/地下/超域拒绝，分配中途失败回收、闭合heap拒绝新图、活动tracking取消、样本分区重组及图像保活旧云场均通过。 |

随机吸收/辐亮度估计遵循统计门槛；不把随机样本误差等同于M17.1确定性透射的绝对误差要求。
上述数字仅代表固定夹具，不是全参数域证明，也不是CPU/GPU性能基准。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.'
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.volume '
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.volume single scattering'
```

文档使用check-spec.ps1限定本次7份Markdown检查（695个本地链接及全局索引/元数据），与git diff --check均通过。
未运行GPU/Editor/capture、Sponza交互性能、Debug配置或全仓库测试；本阶段没有接入对应路径。
未重建dk-editor.exe，不能从本次CPU验收推断Editor联合大气/云已经可用或卡顿已经改善。

## 遗留问题与下一步

M17.3 GPU晴空、M17.4 GPU云、M17.5上层闭环和M17.6优化仍待后续执行。
CPU参考逐三角形求交、逐路径RGB采样，厚云可能触发明确工作预算；性能优化须保持该参考的物理模型和统计均值。
用户原有spec/README.md和AI文档指南改动不纳入本阶段提交。

## 修改记录

- 2026-10-16T11:48:00+08:00：创建记录并冻结实施契约。
- 2026-10-16T12:13:00+08:00：完成体积参考、独立统计验收、边界修正及失败保护；更新Roadmap和使用入口。
