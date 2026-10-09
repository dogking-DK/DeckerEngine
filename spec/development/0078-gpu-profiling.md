---
id: "0078"
created_at: "2026-10-09T14:06:00+08:00"
updated_at: "2026-10-09T14:26:13+08:00"
status: completed
design_refs:
  - ../design/foundation-profiling.md
  - ../design/graphics-resources.md
  - ../design/graphics-graph.md
  - ../design/physics-xpbd-gpu.md
  - ../design/physics-api.md
---

# 0078 M11.1 GPU 性能观测

## 目标与设计依据

依据 [性能设计](../design/foundation-profiling.md) 与 [提交契约](../design/graphics-resources.md)，
补齐提交/Pass 的 timestamp、延迟发布 Tracy GPU 区间和 CPU 阶段关联。保持 CPU-only。

## 实际变更

- [GpuProfiling.hpp](../../engine/graphics/device/include/dk/graphics/GpuProfiling.hpp) 提供显式队列开关、容量、
  票据拥有的纯 CPU 结果、pending/ready/unavailable/device_lost/disabled 状态和时钟元数据。
  每槽独立 query pool，只在 free 槽录入 reset，timeline 完成后无等待读查询；旧结果不受池复用/关闭影响。
- [GpuProfiling.cpp](../../engine/graphics/device/src/GpuProfiling.cpp) 按 validBits/period 换算时间，
  预分配事件和查询读回空间；容量超限只标记 dropped_zones，不改变工作。
  设置需要空闲队列，候选资源建立后才替换。提交后状态发布仍无分配；读取失败显式缺失。
- 私有 Tracy 0.14.1 适配保存原 CPU 时刻/线程，在提交完成后发出完整 GPU 事件。
  放弃/失败图、提交失败、缺失查询不发半个区间。队列按提交值顺序收集，支持槽复用后顺序变化。
  首次启用用一次单 timestamp 和 queue-idle 建立非校准对齐；重复开关复用 context。
- [GraphExecution](../../engine/graphics/graph/src/GraphExecution.cpp) 自动包围保留 Pass 的 prepare/record/finish，
  从访问阶段分类 compute/draw/transfer/other，另有整个提交区间。裁剪 Pass 不记录，计划与缓存键不变。
  [GPU XPBD](../../engine/physics/xpbd-gpu/src/GpuXpbd.cpp) 补 CPU 初始化/建图/推进 zone，
  Graph补编译/执行，既有Shader.compile与submit/wait/collect形成独立阶段，提交编号关联CPU和GPU结果。
- [探针](../../tests/integration/GpuProfilingProbe.cpp)、[采集脚本](../../scripts/capture-gpu-profiling.ps1)、
  [使用指南](../guides/gpu-profiling.md)、测试选择表、设计索引和Roadmap同步。
  专用 windows-graphics-profiling 继承graphics，开启Tracy、关闭内存事件，采用RelWithDebInfo。
  保持原fixed baseline/依赖版本，没有新增或升级三方库；模拟命令/服务语义不变。

## 验证记录

Windows / VS2026 MSVC / NVIDIA GeForce RTX 4070 Laptop GPU / driver 596.49。
Tracy开启采用RelWithDebInfo；无Tracy使用原windows-graphics Debug。timestampPeriod=1ns，timestampValidBits=64。
所有测试由 scripts/verify.ps1 显式选择目标和筛选；未运行全量、双配置全套或其他平台。

| 范围 | 结果 | 证据（out/verify/） |
| --- | --- | --- |
| 初轮Tracy构建、GPU开关与查询生命周期、CPU回绕换算 | 2通过，0失败/跳过 | 20261009-141256-d59db633 |
| 无Tracy的GPU观测、资源单元、原有Graph GPU执行/同步验证、原有300拍XPBD验收 | 14通过，0失败/跳过 | 20261009-141554-17db83fa |
| CPU-only profiling runner、CPU采集冒烟、无Tracy头文件副作用隔离 | 3通过，0失败/跳过 | 20261009-141753-aad55703 |
| 最终补齐poll/Graph失败/device-lost/析构后，Tracy开启探针 | 1通过，0失败/跳过，2.80s | 20261009-142109-b3d187c8 |
| 最终同一探针、Tracy关闭 | 1通过，0失败/跳过 | 20261009-142242-f0554e56 |

实际命令：

```powershell
cmake --preset windows-graphics-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics-profiling -Configuration RelWithDebInfo -Target @('dk_gpu_profiling_probe','dk_graphics_resource_tests') -TestRegex '^dk\.(profiling\.gpu_validation$|graphics\.unit\.GPU timestamp)' -Reason 'M11.1 GPU timestamp and lifecycle plus bounded wrap conversion'
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration Debug -Target @('dk_gpu_profiling_probe','dk_gpu_xpbd_probe','dk_graphics_resource_tests','dk_graph_probe') -TestRegex '^dk\.(profiling\.gpu_validation$|xpbd\.gpu_validation$|graphics\.unit\.|graph\.gpu_)' -Reason 'M11.1 no-Tracy GPU and affected regressions'
cmake --preset windows-profiling
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_run','dk_profiling_probe','dk_profiling_disabled_test') -TestRegex '^dk\.(profiling\.(smoke|disabled_no_side_effects)|bootstrap\.version)$' -Reason 'M11.1 CPU-only isolation'
# 最终新增用例后仅重跑该探针，分别使用ON RelWithDebInfo与OFF Debug目录
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics-profiling -Configuration RelWithDebInfo -Target dk_gpu_profiling_probe -TestRegex '^dk\.profiling\.gpu_validation$' -Reason 'M11.1 explicit poll and failure coverage'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration Debug -Target dk_gpu_profiling_probe -TestRegex '^dk\.profiling\.gpu_validation$' -Reason 'M11.1 no-Tracy final coverage'
& ./scripts/capture-gpu-profiling.ps1
& ./scripts/check-spec.ps1
git diff --check
```

CPU-only的CMakeCache确认全部Graphics/Render及PHYSICS_GPU为OFF、profiling为ON；生成slnx不含graphics/Vulkan/Slang目标。
探针用seed42、8×8、dt10ms、4批×2拍、96×96绘制；OFF/ON的全量位置、速度、RGBA字节相同，
并对CPU参考检查位置2e-3m/速度2e-2m/s容差。图内包含真实预测/分色/地面/速度计算与布片绘制，非空命令计时。
原有300拍CPU/GPU及同步回归另行通过，未以8拍代替原数值契约。

失败/寿命覆盖：两槽耗尽、forced timeout不读查询、保留旧票据复用槽、改变采集配置冲突、
已录制区间放弃、Graph回调失败、驱动submit失败、query NOT_READY/错误缺失、区间不平衡、
容量省略、显式poll、显式close、隐式析构、设备丢失冻结/关闭和票据晚于队列存活。
合成device-lost前先等待真实工作完成，避免测试夹具销毁仍在途的GPU资源；丢失路径不伪造完成值。
同步验证errors/warnings均0，仅单列既有AMD隐式层API1.3/应用1.4 loader提示；全部Memory分配释放。

真实采集最终产物：`out/profiling/gpu-20261009-142353-125b9da2/`。
匹配Tracy0.14.1实际读回gpu.tracy，**928个GPU区间、277个CPU区间**；
工作负载每个Pass的次数一致，CSV整数耗时与原始查询差≤2ns；各批次的graph.execute、graphics.submit、
graphics.wait value匹配Submission。abandoned/failed submit/failed graph/omitted/nested未进入capture。
summary、gpu-profile.json、GPU/CPU CSV与日志完整保存；这不是性能基线或采集开销结论。

初期失败与处理：新独立配置缺少本目录的nlohmann_json imported target，补find_package后生成成功；
沙箱内vcpkg配置拒绝访问外部缓存、MSBuild启动及回环capture停滞，终止后使用已授权的本机工具权限完成。
未用旧二进制作为失败构建的通过证据。两轮capture验收脚本先后发现未覆盖poll及CSV value为“十进制 [十六进制]”，
补齐实际poll用例并修正解析，最后完整重采集通过。初期文档表格空行/末尾空行已修复。

## 偏差、限制与下一步

采用自管timestamp池和延迟Tracy事件，避免上游即时VkCtxScope对放弃录制产生不可退休查询。
Foundation不增加Vulkan依赖；timestamp功能与Tracy编译开关独立。私有事件协议固定到0.14.1，升级时须重做capture验证。

GPU时长包含区间内屏障/执行停顿，不等于纯shader成本；父/子不能相加。
非校准CPU/GPU对齐有采样窗口误差与漂移，alignment_window_ns显式报告；不用于请求延迟或精确排队时长。
原生区间须短于时间戳回绕周期，Tracy时间线还需不跨相对校准点的回绕周期；本次只验证64bit设备，低位设备只覆盖纯换算单元。
未验证其他GPU/平台、真正硬件device-lost、Tracy中途重连、多队列、窗口呈现或性能开销。

M11.1已验收；M11保持进行中，下一项M11.2多规模冷/热启动与响应性基线。
工作区原有spec/README.md和spec/guides/ai-documentation-workflow.md改动保留、不纳入本次提交。

## 修改记录

- 2026-10-09T14:06:00+08:00：创建记录，先更新五份相关模块设计。
- 2026-10-09T14:26:13+08:00：完成原生计时、Tracy、失败/寿命、CPU-only与文档验收。
