---
id: "0169"
created_at: "2026-11-12T15:24:00+08:00"
updated_at: "2026-11-12T16:06:43+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
---

# 0169 M23.11 体素与既有 Scene media 路径

## 目标与状态契约

实施 [M23.11](../roadmap.md#m23实体体素渲染) 与[介质组合契约](../design/render-voxels.md#m2311-既有介质组合实施契约)。将类型化原生体素接入既有空气/云 Classic、CPU initial RIS 和 GPU 四 DI；保持自由程/条件 RIS 分工及完整 epoch 发布，失败不污染已完成 owner。介质信号和上层装配保持既有边界。

## 验证计划

CPU 对独立外露面 oracle 验证事件/材质/预算；真实 GPU 固定与渐进、透视/正交、四 DI、云/空气、条件 RIS、内容失效与失败保护；定向旧 mesh 介质回归。使用 Debug VDB 配置，不默认执行全部长时统计。

## 实施与验证记录

- CPU 移除体素介质拒绝，endpoint/BSDF/原点偏移/发光权重使用类型化命中；原有 hero channel、自由程、有限太阳、MIS/RR、独立阴影流保持。追加 `voxel_regions` 与 `prepared_voxel`，几何工作预算合计三角形测试和稀疏访问。
- Slang `AirVertex` 保存完整 payload；复用 surface v2 恢复 voxel instance/cell/face/value/position，ground kind=3 不作为几何命中。RGB reservoir 仍先准备，仅无真实事件到达首表面时消费；所有反弹和最终阴影查询同一混合 TLAS。
- 修复介质固定射线 6 float4 步长的读回，以及视图诊断的 volume pixel 随机域和相机移动后实际 epoch。继续拒绝 Scene media 表面信号，保留完整 epoch 和冻结 owner 保护。
- 新增四项 required GPU 模式 `voxel_medium_numeric/schedule/reuse/state`；CPU 对照独立外露三角面，覆盖无/零介质、彩色空气/云、事件顺序/ground/预算/取消。GPU 覆盖 Lambert/GGX、面积灯/有限太阳/RR、双向混合遮挡、四 DI、双投影、调度重放、云阴影积分/独立链、阶段失败/tracking/取消/迟到与内容/介质来源失效。新增入口同步测试选择表。

### 验证结果

最终 46 项配置/检查取得通过证据：32 CPU（7 体素 +25 无 VDB 介质）、14 required GPU。CPU 两项旧长测超时后以相同二进制独立执行通过；其他夹具失败均已定向关闭，无跳过。所有 GPU 检查为 RTX 4070 Laptop、required validation/sync，零错误/警告且 CPU/VMA owner 回收通过。文档检查和 `git diff --check` 通过。

- 首轮 `out/verify/20261112-152810-c27ab039`：构建通过、6/10 测试通过。四项失败源于新夹具：云尺度小于已有 GPU 支持域、主射线处密度为零、固定射线误读只在渐进发布的统计。修正尺度和统计断言后，`out/verify/20261112-153416-c158bd23` 的 fixed numeric/state 两项通过；schedule 发现正交起点落到 ground 以下，收窄有效视野后 `out/verify/20261112-153636-81f34917` 通过。
- CPU 云夹具探测先因持有旧场时达到两代缓存额度中止，改用独立临时 cache 后 `out/verify/20261112-153806-1ec2fb97` 测得 seed 10 主射线附近密度 0.715303。最终固定此种子并移除探测代码，保留非零密度、真实散射/吸收和条件 RIS 断言。没有放宽支持域、数值阈值或旧采样器。
- 最终专项 `out/verify/20261112-153943-eebca388`：7 CPU +4 required GPU，11/11 通过。RTX 4070 Laptop、required validation/sync，errors=0、warnings=0、CPU/VMA owner 无泄漏。四 DI 的云纯吸收首表面均值约 (0.117095,0.154447,0.116988)，独立 primary×shadow 积分参考 (0.117134,0.156179,0.117134)；均满足 2%+1e-4 与 5 个独立链标准误+1e-4，128→512 spp 的链均值 MSE 和由约 3.6577e-4 降至 8.6240e-5。统计以四条独立链为观测，未把相关像素当独立样本。
- 旧体素/介质 GPU 回归 `out/verify/20261112-154311-9d2e4112`：7/8 通过；剩余 `restir_medium_failure` 的旧 32² 额度夹具未计入 surface v2，按同等 completed owner 字节量调整为 30²，保留 1 MiB、64²拒绝和外持 owner 释放恢复断言。`out/verify/20261112-154748-6ef42267` 复验 1/1 通过；未改引擎额度。
- 严格无 VDB GPU 回归 `out/verify/20261112-155016-53afb550`：严格编译及 mesh `air_numeric/restir_medium_numeric` 2/2 通过。无 VDB CPU 回归 `out/verify/20261112-154047-092af415`：23/25 CTest 通过，旧云统计与单散射收敛两项触及 240 秒限制。旧云统计原样独立复验 `out/verify/m23-11-cpu-cloud-long.log` 已通过全部 74 条断言；单散射原样独立复验 `out/verify/m23-11-cpu-single-scattering-long.log` 在 283.39 秒通过全部 277 条断言。两项均保持原 Debug 二进制、采样数和断言，不把 CTest 超时写成 CTest 通过。选择表记录旧大样本 Debug 统计不作默认门禁，后续按端点/材质/投影/失败链路选择。

### 验证命令

```powershell
$env:VCPKG_ROOT='C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_voxel_path_tests,dk_pathtracing_probe -TestRegex '^(dk\.voxels\.path_cpu\..*|dk\.pathtracing\.voxel_medium_(numeric|schedule|state|reuse)_gpu_required)$' -Reason 'M23.11 final nonzero cloud fixture and independent chain transmittance acceptance' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(voxel_numeric|voxel_lighting|voxel_state|voxel_restir_reuse|voxel_signals|voxel_restir_state|restir_medium_failure|restir_medium_schedule)_gpu_required$' -Reason 'M23.11 directly affected surface diagnostics and mixed media publication regressions' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\..*[Vv]olume' -Reason 'M23.11 no VDB no GPU medium CPU regression' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(air_numeric|restir_medium_numeric)_gpu_required$' -Reason 'M23.11 strict no VDB GPU air and media RIS regressions' -Parallel 4
& 'out/build/windows-path-media-cpu/bin/Debug/dk_pathtracing_reference_tests.exe' 'RESTIR volume real cloud shadow changes with frozen wind and scattering retains energy'
& 'out/build/windows-path-media-cpu/bin/Debug/dk_pathtracing_reference_tests.exe' 'volume single scattering agrees with independent spherical quadrature and N4N convergence'
```

## 边界与下一步

未运行全量引擎、Release、Editor/Console/IPC/capture 或性能基线；后两项按 M23.12/13 推进。未新增体素密度介质、透明/发光体素或介质降噪信号。无新增三方依赖、无 CUDA 接入；保留现有 NanoVDB/Slang 路线。
