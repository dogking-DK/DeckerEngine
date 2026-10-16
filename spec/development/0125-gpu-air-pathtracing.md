---
id: "0125"
created_at: "2026-10-16T13:46:09+08:00"
updated_at: "2026-10-16T14:22:45+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/architecture.md
---

# 0125 M17.3 GPU晴空与空气路径

## 目标与设计依据

按用户要求完成M17.3，依据[GPU空气契约](../design/render-pathtracing.md#m173-gpu空气实施契约)，
把M17.2 CPU参考接到已有RT/Graph及渐进链路；环境与几何GPU所有权分离，保持失败保护。
GPU云、Editor/capture和交互优化仍按M17.4–6推进。

## 实际变更

- 在[PathTraceRenderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)提供显式空气单次/渐进入口，
  采用M17.2 model=4/sampler=2。无几何走compute，有几何走现有RT；复用Lambert/GGX、mask、面积灯，
  实现空气analog自由程、真实多次散射、太阳圆盘、NEE/ratio tracking/MIS/RR与行星边界。
- [空气shader](../../engine/render/pathtracing/shaders/path-air.slang)传播径向向量/高度，保留地球尺度下的近地精度；
  稳定小太阳采样、叉积圆盘命中、小参数log与补偿距离累加，出壳前先比较光学距离避免稀薄介质除法溢出。
  CPU/GPU使用一致的相函数方位基底及随机编号，GPU不要求shaderFloat64。
- 单次读回新增错误字，渐进保留每tile 4字节读回/整批发布；每路径及每dispatch tracking额度耗尽都拒绝候选。
  完成图携带完整环境/介质/体积设置、几何身份及原场景来源，不能把部分候选或model=4误报为表面结果。
- 拆分GPU几何owner与场景wrapper；独立geometry_fingerprint排除环境/revision，命中后完整比较消费内容，
  两个弱缓存复用AS/纹理且不延长GPU资源寿命。环境变更重置历史但不重建相同几何，材质/纹理改变仍重建。
  弱缓存可保留至多两个CPU控制块；准备失败不发布缓存。所有GPU资源沿用统一AllocationBudget。
- 新增[五组空气验收](../../tests/integration/PathAirCases.hpp)、CPU几何身份测试，并更新既有上传/分配失败夹具：
  故障注入使用实际变化的输入，避免缓存命中绕过上传；失败准备仍要求Memory精确回到旧值，成功准备允许有界弱缓存控制块。
  测试填满heap的占位块使用作用域清理，断言失败也不会以heap关闭异常掩盖原因。
- 更新模块设计、架构、索引与[GPU使用指南](../guides/pathtracing-gpu.md)。无新依赖、命令或schema。

## 验证记录

配置为windows-editor/Release，RTX 4070 Laptop，required RT/Khronos validation与sync，零跳过。
执行期间仅对子进程临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束后恢复环境。
所有构建/测试均通过scripts/verify.ps1，证据位于out/verify以下目录。

| 证据目录 | 范围与结果 |
| --- | --- |
| 20261016-140725-e0175f30 | 22项GPU回归，17通过、5失败；失败均为旧夹具对重复prepare必上传的假设，后续修复并复测 |
| 20261016-141025-4e52b099 | 最终空气实现5项通过，表面/GGX/纹理寿命3项通过；2项分配夹具因成功弱缓存控制块使旧断言失败，未清理占位块造成0xc0000409；后续修复 |
| 20261016-141644-4d5bbc84 | 分配与纹理分配2项复测通过，含初始/中途失败、旧帧和恢复、最终Memory归零 |
| 20261016-141818-5df4b758 | 空气整图CPU对照、3/7/16分批/tile一致性、每路径额度与地下拒绝补充后2项通过 |
| 20261016-140755-0b2781eb | windows-path-media-cpu/Release，CPU几何身份/快照及介质/体积23项通过；Graphics/Slang/大气光照/云光照模块全部关闭 |
| 20261016-141947-1b66e88a | 重建dk_editor_app/dk_run/dk_ctl，真实后台Editor及capture旧表面模式2项通过；Editor自动启动使用--background |

GPU探针最终去重25项通过：5项空气与20项直接受影响的表面/纹理/GGX/渐进链路。
required探针检查validation错误/警告为0、退出VMA与Memory分配为0。
共50项去重检查通过（25 GPU模块+23 CPU+2 Editor/capture），0跳过；上层回归验证旧表面模式，未接入空气模式。

代表命令（旧失败项已按上表单独复测，不把初次失败运行记为通过）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(air_.*|lifetime|allocations|texture_lifetime|texture_allocations|ggx_lifetime)_gpu_required$' `
  -Reason 'M17.3 GPU空气与共享资源回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(geometry identity|scene snapshots|volume |medium )' `
  -Reason 'M17.3几何身份与纯CPU体积参考边界'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.(editor\.pathtracing_gpu_validation|pathtracing\.capture_gpu_required)$' `
  -Reason 'M17.3共享参数布局与结果元数据变更后的Editor/capture表面兼容性'
```

数值结果来自最终空气实现的results.xml，8 seed固定为101+7919×i：

- RGB纯吸收，65536 spp×2 pixel/seed，CPU/GPU nRMSE=4.4054e-8；解析T积分绝对误差≤1e-4。
  GPU均值(0.264379133,0.389180660,0.548034690)，解析(0.264377594,0.388308401,0.547520218)，各通道满足2%及5标准误差门槛。
- 各向同性单次散射，顶向地面、近轴小太阳，解析值0.0186424708；16384与65536 spp×16 pixel/seed，
  RGB均方误差1.09610e-8→3.30225e-9（0.301倍），各通道满足2%/5标准误差。球面小太阳修正低于验收容差。
- 低太阳（y=.08）、Rayleigh/HG混合、6次反弹/RR，16384 spp×4 pixel/seed，CPU/GPU nRMSE=1.98596e-4（约0.01986%），
  均值通过2%/5标准误差。固定7×7相机19 spp整图对CPU绝对误差≤2e-4；同GPU的3/7/16批次逐值一致。
- 1MiB GPU资源额度峰值885568字节；环境变更新增GPU分配与提交均为0，纹理变更重建；
  释放外部完成帧后额度恢复，弱缓存不保留GPU资源。包括近地1.8米、R=1/1e7、切向/高空、far不裁介质、极稀薄系数和微小太阳边界。

早期定位记录：135640首次运行发现两个读回写同一buffer缺少写屏障，改为两个Graph pass；
140053测试字段拼写导致构建失败，未运行旧二进制；140159散射对照失败，修正GPU相函数方位基底及测试中单散射解析式后通过。
另一次构建工具自动审批超时，按返回指示重试后正常执行。

## 遗留问题与下一步

GPU云仍明确拒绝，M17.4继续接入云密度与传输；M17.5接Editor/capture联合模式，M17.6做Sponza固定质量性能优化。
本阶段未跑Sponza性能基准、完整引擎回归或双配置矩阵；不宣称交互帧率改善或体积云已可用。

## 修改记录

- 2026-10-16T13:46:09+08:00：创建记录，先冻结实施契约。
- 2026-10-16T14:22:45+08:00：完成GPU空气/几何复用与失败保护，50项去重检查和文档检查通过；本地提交本阶段。
