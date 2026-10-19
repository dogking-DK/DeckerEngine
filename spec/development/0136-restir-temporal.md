---
id: "0136"
created_at: "2026-10-19T14:24:00+08:00"
updated_at: "2026-10-19T14:47:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0136 M19.3 时间 reservoir 与采样历史

依据 [M19.3 契约](../design/render-restir.md#m193-时间复用实施契约)，实现静态表面重投影、支持校正、有限历史，
分离图像累积与采样身份；保持独立 CPU oracle、RTXDI 私有桥接、全批提交和失败保护。
不变更 SDK 版本或下载新依赖，不实施空间/介质/Editor/capture/DLSS。

## 实际变更

- 先更新 ReSTIR 设计，明确两来源支持分母、祖先年龄、相机移动保留采样身份、全批提交与精确容量；未以 SDK 默认总 M 归一化代替校正。
- `RestirReference.hpp` 增加可选 `RestirTemporalSettings`（独立 version 1、M/age 界与几何阈值）；默认关闭，旧计数随机流不变，purpose 5 用于时间替换。
  CPU 初始参考和无历史 GPU 入口拒绝 temporal 配置；CPU oracle 仍不依赖 SDK/GPU。
- `path-restir-bridge.slang` 使用 RTXDI CombineDIReservoirs/FinalizeResampling 数学；引擎负责静态重投影、旧表面 BSDF/GGX 目标、完整 float UV、支持分母和祖先 age。
  不缓存最终可见性；零来源保留计数，M 截断不修改正规化 W，age 达界后整体刷新。
- `PathTraceRenderer.cpp`/`PathRestirGpu.inl` 为上一完整采样 owner 与当前线性图像分别保存引用；相机姿态变化续接 epoch，物理/投影/配置/reset 清空。
  全图初始 → 时间 → 着色、私有 ping-pong 与 Graph 读写依赖保证 tile/batch 重放；诊断和来源计数只随整批成功发布。
- `PathTraceGpuImageInfo` 保存 sampling_first_epoch、图像开始时的历史 epoch 和累计诊断；无历史非零 first sample 也正确报告采样起点。
- 完成 owner 224 bytes/pixel 不变，多 epoch 时间候选加 224 bytes/pixel ping-pong；额外两次目标评估计入候选预算。
  配置预检按实际 `min(batch_spp,target_spp)` 计费，单 epoch 尾批不虚算 ping-pong；GPU 实际 requirements/对齐仍由共享额度准入。
- 新增六组 required GPU 用例和两项 CPU 用例；同步接口/设计/指南/依赖用途/测试选择表，未扩展命令或 Editor schema。

## 验证环境与命令

Windows x64、MSVC 19.51、Slang 2026.18；RTX 4070 Laptop GPU / NVIDIA 596.49。
GPU 使用现有 windows-graphics / RelWithDebInfo，required RT/Khronos validation/synchronization validation；
CPU 使用 windows-path-media-cpu / Release，GPU targets OFF，CPU reference ON，构建树无 RTXDI 下载目录。
沿用进程级 AMD switchable layer 兼容变量并在 finally 恢复；未关闭 validation/sync 或切换工具链规避失败。

所有构建/测试经 `scripts/verify.ps1`，显式 target/regex；每轮日志保存实际列表和结果，不以旧二进制验收。

```powershell
$previous = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
        -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.restir_temporal_.*_gpu_required$' `
        -Reason '时间复用数值/状态/调度验收，实际分组筛选见各轮 summary' -Parallel 2
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previous }
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
    -Target dk_pathtracing_reference_tests,dk_restir_reference_benchmark `
    -TestRegex '^dk\.pathtracing\.(RESTIR |restir_reference_benchmark_cpu$)' `
    -Reason 'M19.3 recursive support and ancestry enumeration with initial RIS and CPU-only regressions' -Parallel 2
```

GPU 按 numeric/materials、schedule/state、boundaries/support 分组执行，没有再重复运行未改变的长链统计；
上述 GPU 命令为统一复现入口，各轮实际筛选如下表对应 summary。

## 验证结果

| 检查 | 实际结果 | out/verify 证据 |
| --- | --- | --- |
| 首次定向构建 | 来源视图是值类型却误写解引用，编译失败；未运行旧二进制，修正后重建 | 20261019-142703-7aa06642 |
| 时间阶段/调度/状态与初始 RIS 回归 | 4/4 通过，无跳过，required RT/validation/sync | 20261019-142802-912f43d2 |
| 初版长链与 GGX | materials 通过；numeric 极小视域重投影触发失配，与假设每轮必复用的纯数学 oracle 不一致 | 20261019-143006-ae5a54e5 |
| 正常尺度白炉长链 | 1/1 通过；固定 Lambert 支持域隔离重投影精度与数学，未放宽门槛 | 20261019-143133-70d3f595 |
| 重置/额度与变支持/遮挡 | boundaries/support 2/2 通过，无跳过 | 20261019-143335-2fa41153 |
| 纹理历史/各阶段取消及旧路径回归 | 15/15 通过，无跳过；含时间 schedule/state、初始 textures/state 与 11 组旧路径 | 20261019-143634-e805046e |
| CPU 递归数学、拒绝非法接口与原 RIS/基准 | 20/20 通过，无跳过 | 20261019-143835-58fc2d9e |
| 视差显露、来源序号与尾批预算 | boundaries/numeric 通过；旧 state 夹具仍假设单 epoch 分配双缓冲，修正为实际超额尺寸并补成功尾批 | 20261019-144027-546cf420 |
| 精确尾批额度恢复 | state 1/1 通过，64² 单 epoch 可容纳、72² 实际超额被拒绝、外持 owner 释放恢复 | 20261019-144155-7d3552bc |
| 极小视域诊断补充 | boundaries 1/1 通过，记录实际失配并保持跨硬件合法回退；无 validation warning/error 或泄漏 | 20261019-144453-d77b5bed |
| 文档与差异检查 | 9 Markdown、817 本地链接、元数据/表格/索引/测试入口/JSON 通过；Git 空白检查通过 | `scripts/check-spec.ps1 -Path` 限定本次九文档；`git -c core.safecrlf=false diff --check` |

11 组旧路径为 orthographic、orthographic_medium、cloud_surface、air_numeric、progressive_images/state/failure/budget/lost、ggx_indirect、texture_disabled。
初始 RIS numeric/schedule/textures/state 均覆盖。最终 **21 个唯一 GPU 测试、20 个 CPU 测试通过**，无未解决失败或跳过。
逐项按最后一次结果核对，不将早期含失败的轮次记作整轮通过。

## 数值与状态证据

- CPU 枚举六轮所有新提案/替换分支，支持域交替 `{0,1}/{1,2}/{0,2}`，包含零 reservoir、M 上限 1/4、age 上限 2/5。
  每轮期望为 2、概率和为 1（1e-10 绝对门槛），验证递归校正和整体刷新；没有仅依靠误差条判断无偏。
- 白炉 8 个 seed（101+7919*i），256/1024 spp 顺序真实 GPU 三阶段会话；每条链和独立 CPU 合并 oracle 均值差 ≤1e-5。
  MSE 从 `9.54079741e-5` 降到 `5.76902249e-5`；1024 spp RGB 均值约 (.603704631,.402469743,.201234872)，
  解析值 (.600000024,.400000006,.200000003)，原 2%+1e-4 和 5SE+1e-4 同时通过。
  256 epoch 每条链 227 次复用、28 次整体过期、1 次无历史；1024 epoch 的最大 age=8、M≤72。
- GGX 先积累 64 epoch，再移动/旋转相机并积累 1024 新 spp；8 条独立链与 CPU 初始 RIS 65536 spp 对照通过。
  RGB 均值 GPU (1.25611857,.514305316,.154756075)，参考 (1.26450434,.514707244,.152681132)，均满足原双门槛。
- 单面竖直面积灯使同一 receiver primitive 两侧支持不同，alpha-mask 细遮挡单独改变当前可见性。
  8×4096 spp 时间链对照独立 CPU 普通 MIS 65536 spp，2469 次支持校正、23354 次复用；
  RGB 均值 (.199127326,.155312365,.0889365841) 对照 (.199789643,.155509066,.0889125198)，双门槛通过。
- 透视/正交平移旋转后 origin 继续、停动后继续累积；曝光保留，投影/尺寸/seed/环境/候选/复用/gesture/冻结场景变化清空。
  遮挡显露中心像素与同 epoch 的无历史新候选逐位一致；越界历史不复用，双视图结果相互独立。
- batch 1/7/16、tile 64/3、逆序、纹理与空场景一致；时间/着色提交失败、三个阶段在途取消、第二私有 epoch 取消及 reset 迟到保护通过。
  已完成旧帧不可变且外持计费，1 MiB 配额拒绝后释放旧 owner 可恢复。

## 偏差、限制与下一步

极小视域会放大 GPU 浮点位置/投影误差，按契约拒绝不匹配历史并使用新候选；实测 256 epoch 为 217 复用、19 失配、20 过期。
不能用“每轮必复用”的数学 oracle 要求同链逐位匹配；诊断测试只要求总决策数正确，不强制其他硬件发生同样的失配次数。
白炉改用正常几何视域后，每轮匹配与 CPU oracle 一致；GGX 的窄视域仍按独立链光能门槛检查，不假设所有重投影成功。
每阶段全图完成后才能进入下一阶段，时间模式默认关闭；没有在本阶段声称性能提升或实时性。
未运行全量、Debug 双配置、Editor/GUI、其他硬件/平台、SDK 发布打包或正式等误差 GPU 性能基线。
下一阶段 M19.4 空间复用；介质 RIS、Editor/capture、DLSS 仍按后续阶段推进。
用户原有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 保留，不纳入提交。

## 修改记录

- 2026-10-19T14:24:00+08:00：先建立时间复用估计器、状态与预算契约。
- 2026-10-19T14:47:00+08:00：完成实现、21 GPU/20 CPU 定向验证、尾批与极小视域诊断及文档检查，记录数值、早期失败及实际边界。
