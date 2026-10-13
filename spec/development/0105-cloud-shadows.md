---
id: "0105"
created_at: "2026-10-13T16:34:38+08:00"
updated_at: "2026-10-13T18:20:19+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-pipeline.md
---

# 0105 M15.5 太阳云透射阴影

## 目标与设计依据

按[云影契约](../design/render-clouds.md#云影与交互闭环)完成固定世界光空间云柱透射图，
接入[日光接收链路](../design/render-pipeline.md#m155-可选云透射阴影快照)，不修改其他光源项。
实现前冻结范围、预算、发布/失败边界和数值门槛。

## 实际变更

- 新增[CloudShadow](../../engine/render/clouds/include/dk/render/CloudShadow.hpp)纯CPU设置、投影、云底朝阳交点射线、接收坐标与预算验证；光空间固定世界区域不跟随相机。
- 新增[CloudShadowRenderer](../../engine/render/clouds/src/CloudShadowRenderer.cpp)与[积分Shader](../../engine/render/clouds/shaders/cloud-shadow.slang)：CPU double交点/风相位约化，GPU云柱Beer–Lambert积分，R32F输出和可选诊断读回。
- 完整键命中复用代次；图成功submit才发布新current。至多3活代/64MiB逻辑计费，无隐式等待；最后消费者完成且无快照/Frame引用才回收预算。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)四个显式日光/合成入口可绑定CloudShadowMap；[Lambert Shader](../../shaders/render/sunlight.slang)仅乘太阳直射，默认白图保持旧路径。Frame保留快照及图代号，Graph导入并声明fragment sampled，失败不发布使用序号。
- [GPU用例](../../tests/integration/CloudShadowCases.hpp)与[CPU用例](../../tests/unit/CloudReferenceTests.cpp)覆盖投影/数值、风/太阳/相机、sky/emissive/能量/mask/联合合成、旧图/预算/故障/在途寿命。无依赖升级或命令schema变更。

## 验证记录

已冻结均匀透射误差≤.002、真实云柱≤.02、参考相邻加倍收敛≤.005、地表双线性采样≤.03；不因失败放宽。

- `out/verify/20261013-164131-a4591a2d`构建失败：读回使用了不存在的ImageCopyRegion.extent字段，改用现有width/height；未运行测试。
- `164657-ea9c8a25`：2个CPU与surface通过；低角度CPU 256→512→1024尚未收敛（.01017），GPU误差.00902已在门槛内，但参考证据不足。
  提升CPU参考至1024→2048→4096，保持.005收敛/.02云柱门槛；未改变GPU默认256步或质量。
  failures用例因测试夹具更换seed耗尽原云场的2代预算，改用独立的错误纹理夹具，保持资源限制不变。
- `165133-5a989e7c`三个云影GPU用例3/3通过，required GPU+同步validation，无跳过，errors/warnings/live均为0。
  均匀径向/斜向误差9.245e-8/7.819e-7；真实常规/低太阳误差3.327e-5/.00948225；参考最坏收敛差.000423357。
  +world X风的一纹素正确偏移误差5.603e-5，反向偏移.101309；时间恢复逐位一致。
  地表25点双线性对独立4096步CPU直射透射最大差.00204587；sky/emissive隔离、能量比例、alpha-mask、相机移动稳定、联合衰减一次与4种零云逐位M14均通过。

- `165453-2101c33f`：补充未收集pending读回拒绝与消费者submit失败重试后，云影failures及日光4项、空气合成、普通pipeline、云合成/优化边界/历史共10项通过；截图1项超时，CTest记载3672.80s，未确认原因，不将此次截图记作通过。
- `181548-0aef8b5e`：仅重跑截图，19.92s通过，验证冻结scene/time、quality/schema、零云退化与发布失败保护；无代码修改或降低门槛。产物在`out/build/windows-editor/test-artifacts/Debug/cloud-capture-3375ed8a310d4f47a4632ce6f353c12d`。
- `165643-8e4816ea`：现有windows-graphics配置（Clouds OFF、warnings-as-errors ON）编译pipeline成功，6项CPU回归通过。

验收汇总：Windows/MSVC Debug，NVIDIA GeForce RTX 4070 Laptop GPU、驱动596.49。
21项独立检查最终均通过（8 CPU、13 GPU/真实进程）；GPU required+同步validation、无跳过，探针errors/warnings/live均为0。
原失败日志保留在各次`out/verify`目录；GPU完整成功输出另存`details.log`，不以截断的JUnit输出替代数值证据。

复现本次通过范围（从仓库根执行，GPU运行期间设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并在结束后恢复）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_reference_tests') -TestRegex '^dk\.clouds\.shadow ' -Reason 'M15.5 CPU projection and budgets'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe') -TestRegex '^dk\.clouds\.shadow_.*_gpu_composition$' -Reason 'M15.5 numeric, surface and lifetime contracts'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_composition_probe','dk_surface_sunlight_probe','dk_render_pipeline_probe','dk_aerial_perspective_probe','dk_run') -TestRegex '^dk\.(clouds\.(shadow_failures|composition|optimization_boundaries|history|capture)_gpu_composition$|render\.(sunlight_(normals|direct|diffuse|failures)|pipeline|aerial_composition)_gpu_validation$)' -Reason 'M15.5 direct consumers and shared composition regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_run') -TestRegex '^dk\.clouds\.capture_gpu_composition$' -Reason 'M15.5 isolated capture timeout retry'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_render_pipeline','dk_render_pipeline_tests') -TestRegex '^dk\.render\.pipeline\.' -Reason 'M15.5 optional API with Clouds disabled and warnings as errors'
```

CPU两项实际在`164657-ea9c8a25`与3项GPU一起筛选；上面首行给出单独复现入口。
云影表面、真实场、alpha-mask和联合介质四张33² PPM及放大联系图保存在`165133-5a989e7c/images`，已查看；
用于数值夹具和遮挡检查，不代表完整场景艺术效果或性能验收。
设计、Roadmap、开发索引、构建指南与测试选择表同步。`check-spec.ps1 -Path`检查本次8份Markdown通过，
校验588个本地链接以及元数据/表格/索引/测试入口/JSON清单；`git -c core.safecrlf=false diff --check`通过。

## 限制与下一步

首版接收体限云底以下且在显式固定覆盖区；自动配置/GUI/时间序列属M15.6，性能目标属M15.7。
没有执行全量/Release、跨平台或1080p云影性能验收；无新依赖、命令或持久化schema变化。

## 修改记录

- 2026-10-13T16:34:38+08:00：开工冻结设计与独立验收条件。
- 2026-10-13T18:20:19+08:00：完成实现、数值与失败保护验收；保留初次失败和截图超时，定向重跑通过，交接M15.6。
