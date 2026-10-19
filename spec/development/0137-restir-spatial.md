---
id: "0137"
created_at: "2026-10-19T14:58:00+08:00"
updated_at: "2026-10-19T15:10:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0137 M19.4 空间 reservoir 与支持校正

依据 [M19.4 契约](../design/render-restir.md#m194-空间复用实施契约)，实现冻结空间输入、独立输出、
来源支持归一化和当前可见性查询，保持 RTXDI 私有数学桥接、独立 CPU oracle、有限祖先年龄及整批提交。

## 设计与验证计划

- 配置独立可选，空间自身来源覆盖当前支持；邻居筛选、M 截断和 ancestry age 不依赖已抽到的光样本。
- 目标不含遮挡，完整支持校正无需额外射线；每次着色重新查询当前可见性，不缓存或清空历史。
- 增加独立 reservoir 平面和 pass 4；峰值额度、候选评估、诊断和失败恢复同步扩展。
- 定向 CPU 数学/参考、required GPU 数值/边界/调度/状态/额度、直接受影响旧路径与文档检查。
- 不升级依赖，不实施介质/Editor/capture/DLSS。验证结果完成后记录，未运行项不视作通过。

## 实际实现

- `RestirSpatialSettings` 与独立 purpose 6/7；仅渐进 Surface only 可用，默认关闭。校验邻域、阈值、M 和工作预算，再发布配置。
- `path-restir-bridge.slang` 使用现有固定 RTXDI 合并/归一化数学，应用持有完整 float UV、邻接源接收点、支持分母和全部来源最大祖先年龄。
  不引入 SDK 标准空间 pass 的默认偏差模式；未遮挡目标的支持校正与最终新阴影查询分别负责归一化和可见性。
- `PathTraceRenderer.cpp` 增加 pass 4、binding 14 冻结输入和私有空间输出。Graph 声明输入只读、输出 discard 写，整幅完成后交换；
  多 epoch 时与时间历史 ping-pong 协作，完整批次才发布最后空间结果和图像，失败/取消保留旧 owner。
- 候选峰值额外 32 bytes/pixel；参数 672 bytes，诊断与渐进读回各 64 bytes；已完成 owner 仍 224 bytes/pixel。
  目标评估上界按初始+时间+空间统一预检，shadow 查询最多每像素每 epoch 一次连续灯，不增加支持校正射线。
- `tests/RestirSpatialOracle.hpp` 是测试专用 double CPU 屏幕参考，独立 CPU 求交/BSDF/可见性与支持矩阵合并，不依赖 RTXDI/Vulkan，
  当前参考只渲染初始+空间首表面，不声称 CPU 公共入口新增时空会话。另对相关三源、零 reservoir、异支持、不同接收点遮挡的两轮空间传播穷举期望。

## 当前验证证据

- `20261019-145708-b3ddbcbb`：required GPU 初始 RIS 调度回归 1/1 通过。
- `20261019-150016-271a7018`：required GPU 空间 numeric/schedule/state 3/3 通过；CPU/GPU 空间逐像素参考、8 条时空链 N/4N、
  texture/GGX、透视/正交、空间单开/时空同开、batch 1/7/16、tile/reverse、各阶段失败/取消与旧 owner 保护。
- `20261019-150257-17cf18c5`：CPU-only Release 22/22 通过（21 数学/参考用例和 CPU benchmark smoke），无跳过。
- `20261019-150223-5c407ef4`：required GPU support/edges/budget 3/3 通过；两组 3×3 场景各用 8 条独立 seed 链、
  GPU 4096 spp 对 CPU classic MIS 65536 spp，每像素 RGB 同时满足 2%+1e-4 与 5*combined_SE+1e-4，未放宽门槛。
  support 使用单面竖直面积灯跨接收点改变支持，配薄 alpha-mask 墙；edges 使用倾斜法线、材质/GGX、凸起轮廓与背景。
  1 MiB 额度验证双 epoch 独立空间面、外持旧 owner 的拒绝/释放恢复、单 epoch 尾批与最小候选预算。
- 白炉时空链以整幅 5×5 图像链为统计单位，N=256 MSE=2.81201337e-5，4N=1024 MSE=8.53586039e-6；
  age 上限 4、来源 M 上限 7，空间结果 M≤35，持续触发整链刷新。初始+空间独立 CPU 图像参考逐像素误差≤1e-5。
- `20261019-150639-27aa5b33`：19 项中 18 通过、1 个新增零来源断言失败，无跳过；
  全黑 Lambert 原契约跳过候选生成，测试却要求 M>8。未修改渲染算法，修正夹具为两个独立断言：
  黑 Lambert 无候选 M=0；非黑接收面+背半球面积灯产生全零权重候选，M 保留且无阴影射线。
  通过项包括空间状态、时间 boundaries/schedule/state、初始 numeric/textures/state、正交表面/介质、cloud_surface、air_numeric、
  progressive images/state/failure/budget/lost、GGX 间接与 texture_disabled，覆盖共享 Graph/描述符/诊断受影响链路。
- `20261019-150931-a2263533`：修正夹具后的空间 schedule 1/1 通过，无跳过；未重复执行未受影响的长链数值。
- 最终 **24 个唯一 required GPU + 22 个 CPU** 定向检查全部通过；GPU 用例要求 validation error/warning 与存活分配为零。
  文档检查覆盖设计/索引/指南/依赖用途/本记录/Roadmap/选择表，最终 9 个 Markdown、826 个本地链接通过；`git diff --check` 通过。

## 验证命令与环境

现有 Windows x64 / MSVC 19.51 / Slang 2026.18，RTX 4070 Laptop / NVIDIA 596.49；
GPU 为 windows-graphics / RelWithDebInfo，required RT、Khronos validation 与 synchronization validation；
CPU 为 windows-path-media-cpu / Release，GPU target OFF，无 RTXDI 下载目录。
构建、测试全部经 `scripts/verify.ps1`，单个构建树串行；进程级 AMD switchable layer 兼容设置在 finally 恢复。

```powershell
$previous = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
        -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.restir_spatial_.*_gpu_required$' `
        -Reason 'M19.4 spatial numeric/support/edges/schedule/state/budget' -Parallel 2
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previous }
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
    -Target dk_pathtracing_reference_tests,dk_restir_reference_benchmark `
    -TestRegex '^dk\.pathtracing\.(RESTIR |restir_reference_benchmark_cpu$)' `
    -Reason 'M19.4 CPU spatial oracle, correlated source enumeration and RIS regression' -Parallel 2
```

GPU 新用例实际分为 numeric/schedule/state 与 support/edges/budget 两轮；共享链路回归的实际选择见对应 summary。
上述命令提供统一复现入口，不将未重复运行的长链测试写成每轮都运行。

## 限制与下一步

未运行全工程、Debug/Release 双配置、Editor/固定 capture 或 M19.8 等误差性能验收；本阶段未改变这些消费者。
空间 GPU 仅渐进 Surface only 首表面 DI，最终遮挡仍重新查询；未将目标改成可见性缓存，也未引入近似偏差模式。
来源半径/法线阈值是有界可配置筛选，当前用例证明冻结协议内均值与状态行为，不承诺所有场景降噪速度。
后续 M19.5 单独解决 Scene media 条件采样参考，DLSS 保持独立重建边界。
