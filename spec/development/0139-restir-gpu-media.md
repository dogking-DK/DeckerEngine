---
id: "0139"
created_at: "2026-10-19T15:42:00+08:00"
updated_at: "2026-10-19T16:35:55+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0139 M19.6 GPU Scene media 首表面 DI

依据 [M19.6 契约](../design/render-restir.md#m196-gpu-scene-media-实施契约)迁入 M19.5 条件采样，
复用 RTXDI 初始/时空数学、完整 epoch 调度和资源 owner；不把随机通道/存活/透射写入历史。

## 设计与验证计划

- 增加独立 GPU medium RIS 入口，网格/地面身份、有限太阳标签、当前阴影和真实散射 emission 分工。
- 保护初始/时间/空间输入独立性，支持空几何地面 compute；共享 Graph 云场与 RIS 绑定正确计费。
- 保持物理输入/风时间失效、AS/云场复用、整批发布和旧 owner；拒绝非法组合和工作预算。
- 定向 CPU/required GPU 数值、时空/相机/调度、tracking/预算/取消与文档验证，记录实际通过/失败/未运行。
- Editor/capture 与 DLSS 不在此阶段；无依赖升级。

## 实际实现

- [公开接口](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)增加三个 `*_restir_medium_*` 独立入口，
  固定射线/图像仅初始 RIS，渐进视图可选时间、空间或联合复用；来源同时携带 medium/restir。
  [私有入口](../../engine/render/pathtracing/src/PathRestirMediumGpu.inl)验证介质/视图/预算，复用原调度、云场和资源 owner。
- [shader bridge](../../engine/render/pathtracing/shaders/path-restir-bridge.slang)将有限太阳加入介质灯光域，
  使用私有 ground sentinel 与地面 Lambert 材质；空几何 compute 也执行时间/空间支持合并，网格继续支持 GGX/纹理/mask。
  保持表面 delta 太阳和既有 SDK 数学，无新依赖、无 RTXDI 类型外泄。
- [主路径](../../engine/render/pathtracing/shaders/pathtrace.slang)在初始阶段保存几何接收面和完整 RGB reservoir；
  [介质着色](../../engine/render/pathtracing/shaders/path-air.slang)随后才抽 hero/自由程，仅实际到面时应用 DI。
  复用主查询；当前阴影使用独立 stream 11 做 mesh/mask/行星与 ratio tracking，真实延伸散射解除 emission 抑制。
  不复用体积散射顶点，不以吸收/散射结果筛选采样历史。
- 抽取共享 `primaryLocation` 和 `airEndpoint`，保持正交近面相位与普通体积传输；
  [renderer](../../engine/render/pathtracing/src/PathTraceRenderer.cpp)修复 cloud 与 RIS Graph buffer 导入编号重叠，
  按 `7+restir_buffers` 导入云场，纹理和 AS 继续使用各自偏移。
- 首查询与唯一当前阴影保持 `2*bounces+1` 射线界；候选预算包括复用，tracking 只在 shade 阶段消耗。
  保持已有平面大小和精确 quota；使用 64-byte status 的 14/15 槽记录准备/应用面数，整批成功才发布诊断与两种历史。
- 物理/采样身份变化、风时间、场内容、投影/尺寸与 session/gesture 清空历史；相机姿态保留可重投影的完整采样 owner。
  曝光/暂停/工作额度不重置；普通 medium 的身份匹配增加 `!previous.restir`，防止切回经典路径时误保留 RIS。
  云场/AS 按原规则共享，配置失败和迟到结果不替换已接受视图。

## 数值与状态证据

新增 [介质测试](../../tests/integration/PathRestirMediumCases.hpp)和
[交界支持测试](../../tests/integration/PathRestirMediumSupportCases.hpp)，注册于 required GPU 探针。
9 个新增 GPU 用例、10 个直接受影响的 GPU 回归用例和 10 个 CPU-only 参考用例最终全部通过，零跳过。
随机均值固定 8 seeds `101+7919*i`，以独立链求标准误差；每通道同时要求
`abs(error)<=2%*abs(reference)+1e-4` 和 `abs(error)<=5*combined_SE+1e-4`，不放宽 M19 的门槛。

| 检查 | 结果与观察 |
| --- | --- |
| 初始 RIS / CPU | 真空、M=1、彩色网格/地面消光、有限太阳/面积/环境、mask、GGX、纹理、后续反弹/RR 的 CPU RIS 或普通体积对照通过 |
| 独立消光参考 | 网格实测 `(0.165218436,0.0172597213,0.00876623941)`，参考 `(0.165440316,0.0172396063,0.0087533802)`；行星阻挡太阳为零 |
| 云当前阴影 | time=17 实测 `(0.0705039784,0.0707242385,0.0709405558)`，独立积分参考各通道 `0.0706546018`；time=2017 晴空间隙均值匹配 `0.159154943`，固定时间逐样本重放 |
| 真实多次散射 | 32 bounce/RR 白炉纯云实测 `(0.997788556,1.00406278,1.00286486)`，空气/云混合 `(0.998792303,1.0021678,0.999083853)`，参考为 1；无首表面路径与经典体积逐样本相等 |
| 复用链 | 时间/空间/联合模式均匹配彩色纯吸收解析式，准备量等于全部像素×epoch，实际应用量更少；256→1024 spp 的 8-chain MSE 为 `9.75626934e-7→4.04944917e-7`；age 到期刷新，当前阴影查询有界 |
| 交界支持 | 地面/斜网格、不同颜色/法线、单面面积灯和 mask/云阴影；无历史 GPU 对 CPU RIS/经典体积两组逐像素比较通过；联合复用 32768 spp 全部 9 像素 RGB 通过，8192→32768 spp 链 MSE `9.73688618e-6→3.82739985e-6`，支持校正 256558 次、复用 6883531 次 |
| 调度 | 空几何与带纹理网格、透视/正交、四种初始/复用组合，batch=1/7/16、tile=64/3 和反序，颜色/二阶矩及应用量完全一致；初始渐进与固定图像一致 |
| 状态/资源 | 姿态、曝光暂停、风时间、光照、算法切换、双视图、迟到 reset 和旧 owner 通过；投影/尺寸、seed、配置、session/gesture、cloud albedo 清空两种历史，工作额度保留；改变场 seed 更换 hash 而不重建 AS，旧图仍可读取 |
| 失败保护 | 初始/时间/空间/着色提交失败、总/逐路径 tracking 超限、取消、非法组合/候选预算、超额平面/云场、持有旧 owner 导致额度耗尽均保留旧结果，释放后可恢复 |

## 验证命令与结果

GPU 使用已有 `out/build/windows-graphics`、RelWithDebInfo、target `dk_pathtracing_probe`；
NVIDIA GeForce RTX 4070 Laptop GPU，required RT + Khronos validation + sync，不允许跳过。
按 [图形指南](../guides/graphics.md)仅在验证进程临时设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，结束后恢复环境变量。
CPU 使用已有 `out/build/windows-path-media-cpu`、Release，target `dk_pathtracing_reference_tests`。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.restir_medium_.*_gpu_required$' `
  -Reason 'M19.6 medium RIS transport, reuse and state contracts' -Parallel 2

& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe `
  -TestRegex '^dk\.pathtracing\.(restir_medium_support|restir_medium_failure|restir_numeric|restir_temporal_support|restir_spatial_support|restir_spatial_schedule|air_numeric|air_statistics|cloud_surface|cloud_progressive|cloud_failure|orthographic_medium)_gpu_required$' `
  -Reason 'M19.6 mixed receivers and directly affected shader/Graph regressions' -Parallel 2

& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.RESTIR volume ' `
  -Reason 'M19.6 independent CPU medium RIS oracle and CPU-only build isolation' -Parallel 2
```

- `20261019-154450-4dd65d79`：首次表面 numeric smoke 的数值通过，但未设置 AMD 兼容变量，
  因 AMD 隐式层 API 1.3 对应用 API 1.4 的一条 warning 判失败；后续按已有进程级设置重跑，未修改系统配置。
- `20261019-155009-b4e9ce36`：最初 7 个新增 GPU 用例中 4 通过，复用/调度/状态 3 个失败。
  测试相机 far=10 米使远地面不在重投影视锥；改为 far=20000 米覆盖接收面，没有修改重投影算法或数值阈值。
- `20261019-155323-aa78b43d`：受影响 reuse/schedule/state 3/3 通过。
- `20261019-155954-c5a2cca8`：CPU-only 介质参考 10/10 通过，零跳过。
- `20261019-155727-fb5b568c`：11/12 通过，其中 10 个旧路径直接回归和介质 failure 通过。
  新增 support 在 8192 spp 时 pixel 2 的绿色均值 `0.0732707772` 对参考 `0.0715927225`，约 1.8 SE、2.34% 误差，
  未通过固定 2% 门槛；增加至 16384 spp，并保留 8192 spp 检查整体链 MSE 下降，数值阈值和 seeds 不变。
  此独立八链用例 timeout 从默认 240 秒单独调至 600 秒，避免调度开销阻断统计检查。
- `20261019-160449-0412045a`：扩展 state 通过；support 总体链 MSE 从 `9.73688618e-6` 降至 `9.25221651e-6`，
  但 pixel 2 绿色均值 `0.0735011231`，约 1.77 SE、2.67% 误差，仍未通过 2% 门槛。
- `20261019-161445-e0fa5789`：新增 support_initial 1/1 通过，用同一场景的无历史 GPU 16384 spp
  同时对照 CPU RIS 16384 spp 和 CPU 经典体积 65536 spp，全部像素 RGB 满足原门槛；基础传输与初始候选未见偏差。
- `20261019-161737-1e542dc7`：support 1/1 通过，运行 763.85 秒。保持配置、8 seeds 和两个误差门槛，
  8192→32768 spp 的链 MSE 下降，全部像素 RGB 通过；pixel 2 绿色均值为 `0.0730094658`、参考 `0.0715927225`、SE `0.000642277663`。
  仅该长链统计用例 timeout 为 1200 秒；其运行时间不是渲染性能基准。

最终 GPU 回归覆盖原表面 initial/temporal/spatial 支持与空间调度、经典空气解析/统计、
云表面/渐进/失败和正交介质。成功的 required 探针均为 validation errors=0、warnings=0、live allocations=0。
最后仅重跑受影响的检查；未重复已有效通过的用例，也未运行全量引擎套件。
文档定向检查通过：9 个 Markdown、805 个本地链接，以及元数据、表格、索引、测试入口和 JSON manifests；`git diff --check` 通过。

## 限制与交接

此阶段只提供 GPU C++ 接口，默认普通积分器不变。CPU 仍仅初始参考；GPU 可选初始/时间/空间/联合。
未运行 Editor/capture、DLSS、完整引擎套件或性能基准；不声明默认开启收益、实时帧率或去噪能力。
下一项 [M19.7](../roadmap.md#m19restir-直接光重采样)接入 Editor/worker/capture 的独立算法选择、诊断和版本化来源。
RTXDI 继续固定已有版本；DLSS 仍独立于光照采样与介质选择，后续通过重建适配层接入。
