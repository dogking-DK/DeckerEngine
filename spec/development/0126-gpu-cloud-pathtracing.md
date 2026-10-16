---
id: "0126"
created_at: "2026-10-16T14:32:00+08:00"
updated_at: "2026-10-16T14:55:32+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/architecture.md
  - ../design/render-clouds.md
---

# 0126 M17.4 GPU云路径传输

## 目标与设计依据

按用户要求完成M17.4，依据[GPU云契约](../design/render-pathtracing.md#m174-gpu云实施契约)，
实现世界空间非均匀云自由程、多次散射、自遮蔽及场景几何遮蔽，保持统一状态提交与资源额度。

## 实际变更

- [PathTraceRenderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)新增通用介质单射线/视图/渐进接口，
  旧air接口继续拒绝enabled云；所有体积结果统一为info.medium，保存完整输入和场hash，旧surface模型不变。
- [路径介质shader](../../engine/render/pathtracing/shaders/path-air.slang)按球壳分至多5段，跨段延续null counter，
  云段采用空气+云密度硬界，空气段用空气上界；analog自由程、三分量相函数、真实多次散射和NEE ratio tracking共用原MIS/RR。
  表面、体积顶点和有限太阳同时受到云/空气、行星、几何/mask遮蔽，无M15近似增亮或屏幕历史叠加。
- [云密度shader](../../engine/render/pathtracing/shaders/path-cloud.slang)显式repeat线性插值原RG8/R8数据；
  CPU双精度先约简固定风时间/中心的周期坐标，GPU传播相位，倒尺度高低位与乘法余项减少长距离舍入。
  支持域沿既有GPU云尺度限制，并拒绝float中塌缩的壳厚/正系数；不要求shaderFloat64。
- CPU快照只读公开已有不可变field owner，无新GPU依赖。renderer缓存最近成功使用的field，
  也复用其他视图/在途代持有的相同key/hash；上传buffer计入既有AllocationBudget，时间/光照编辑不重新上传。
  prepare候选不替换缓存，成功configure/submit才提交；失败/取消/旧代迟到不推进completed_spp或污染旧帧。
- 新增[五组GPU云验收](../../tests/integration/PathCloudCases.hpp)，扩展原探针/CTest及CPU owner断言；
  更新模块、架构、索引和[使用指南](../guides/pathtracing-gpu.md)。无新命令、schema、依赖或上层Scene media模式。

## 验证记录

windows-editor/Release，NVIDIA GeForce RTX 4070 Laptop，required RT/Khronos validation及sync。
探针关闭前强制检查validation错误/警告、VMA与Memory归零；所有列出的GPU测试均实际运行，无缺GPU跳过。
子进程临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，运行后恢复。构建/CTest统一使用scripts/verify.ps1。

| out/verify证据目录 | 范围与实际结果 |
| --- | --- |
| 20261016-143441-64935cf0 | 首次构建失败：Graph typed BufferId不能直接作compiled索引；测试未运行 |
| 20261016-143547-688409a5 | 修正编译后共享空气数值1项通过 |
| 20261016-143905-73b83999 | 首轮云5项，4通过/表面1失败；新buffer改变Graph按类型分组的compiled索引，见下条修正 |
| 20261016-144100-ba715911 | 修正buffer/image/AS/SBT导入索引后26项通过：云表面、5空气及20表面/纹理/GGX/渐进回归，含磁盘纹理资产 |
| 20261016-144403-74100ca2 | windows-path-media-cpu/Release，21项CPU介质/体积参考通过；Graphics/Slang/大气与云光照关闭 |
| 20261016-144452-371f334c | 最终云传输5项与空气失败1项通过；增加体积顶点几何遮蔽、云内整图CPU对照、旧场复用及float塌缩拒绝 |
| 20261016-144755-697ef725 | 实际纹理+云buffer/descriptor indexing组合表面测试1项通过 |
| 20261016-144925-67362b13 | 重建dk_editor_app/dk_run/dk_ctl，后台真实Editor和capture旧表面兼容2项及云表面1项通过；GUI自动启动使用--background |
| 20261016-145252-d7b459b6 | 最终云表面/体积几何遮蔽1项通过；体积视线偏离太阳，隔离散射光贡献 |

最终去重53项检查通过：30项GPU模块（5云、5空气、20表面/纹理/GGX/渐进），21项CPU介质/体积，2项Editor/capture；0跳过。
Editor/capture验证的是现有表面模式，未提前接入Scene media。定向文档检查8份通过，git diff --check通过。

代表命令（每次运行的完整参数/构建/结果见上述目录，初次失败不记为通过）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.cloud_.*_gpu_required$' `
  -Reason 'M17.4 GPU云路径、状态与额度验收'
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(volume |medium )' `
  -Reason 'M17.4 CPU介质owner与隔离边界'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_pathtracing_probe','dk_editor_app','dk_run','dk_ctl') `
  -TestRegex '^dk\.(editor\.pathtracing_gpu_validation|pathtracing\.(capture|cloud_surface)_gpu_required)$' `
  -Reason 'M17.4共享参数与元数据变更后的上层表面兼容'
```

数值结果采用固定8 seed=101+7919*i，线性RGB，不用截图观感替代积分验收：

- 真实云纯吸收，独立4096/8192步确定性透射差<=1e-4；云下参考T=0.34289336，云内T=0.39921417。
  云上/外壳切线T=1，双壳T=0.93442759；65536 spp×2 pixel/seed的GPU均值均通过2%及5标准误差门槛。
  同样本8192 spp的CPU/GPU透射在这些夹具上逐值一致；这不代表任意输入都逐位一致。
- N=16384至4N=65536、4 pixel/seed，吸收均方误差1.7947748e-5→4.2889373e-6（0.239倍）。
  上述“对确定性参考nRMSE”包含采样噪声，与同样本CPU/GPU配对误差分别报告。
- 16384 spp×2 pixel/seed，云白炉均值(0.99746157,0.99907887,1.003437)，CPU/GPU nRMSE=3.00834e-5；
  CPU统计93613条路径包含至少两次真实介质散射。GPU不虚构未暴露的介质事件统计。
- 混合Rayleigh/Mie/云，6反弹、有限太阳、NEE/MIS、RR和continuation-only三组CPU/GPU nRMSE分别
  2.68665e-5、3.36696e-5、3.94430e-5，均值通过2%及5联合标准误差；云GGX反射nRMSE=1.14796e-4。
- 表面云影解析参考0.054573174；65536 spp×4 pixel/seed的均值(0.054563099,0.054431522,0.054659773)。
  几何遮挡变黑，透明mask恢复光；云内离轴体积散射的CPU/GPU nRMSE=1.09632e-4，几何遮蔽时为零。
- 7×7/19 spp云内图对CPU绝对误差<=2e-4，3/7/16样本批次与不同tile逐值一致；
  时间变化重置历史且复用场、seed变化保留新旧场、在途旧代丢弃、旧完成图保持来源和线性内容。
- 1MiB额度夹具成功资源峰值15120字节；2MiB级新云场候选明确拒绝，旧视图/缓存/完成帧保持。
  tracking耗尽、提交失败、取消、非法域与恢复均通过；并验证负1e9秒时间及噪声公共周期、世界点系数不超过保守上界。

## 偏差与决策

采用packed storage buffer的显式线性过滤，而非M15的硬件3D纹理缓存：使过滤与CPU参考一致，并让上传/在途/缓存共用renderer真实资源额度。
最近的成功场在释放视图后仍保活，renderer析构释放；外部完成帧仅需线性结果与完整来源，无需保留噪声场。
这不是空间密度跳跃优化，M17.6可在同质量约束下评估更紧的保守上界和GPU布局。

## 遗留问题与下一步

Editor Scene media/Console/IPC/capture联合接入属于M17.5，Sponza固定质量性能优化属于M17.6。
本阶段未跑Sponza性能基线、全引擎套件或双配置，不宣称GUI云路径已接入或交互帧率已经改善。

## 修改记录

- 2026-10-16T14:32:00+08:00：创建记录，冻结接口、场缓存提交点、数值域及统计验收门槛。

- 2026-10-16T14:54:53+08:00：实现和53项去重检查完成，更新设计/指南/Roadmap与最终失败、恢复和统计证据；本地提交M17.4。
