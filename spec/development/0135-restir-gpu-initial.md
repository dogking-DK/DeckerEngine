---
id: "0135"
created_at: "2026-10-19T13:44:00+08:00"
updated_at: "2026-10-19T14:18:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0135 M19.2 GPU 初始 RIS 与 RTXDI

依据 [M19.2 契约](../design/render-restir.md#m192-gpu-初始-ris-实施契约)，接入固定 RTXDI shader reservoir、GPU 首表面与初始采样、
分阶段 tile/epoch 调度和工作/内存额度。保留 CPU oracle 和旧渲染入口；不实施时空复用、介质 RIS、Editor/capture 或 DLSS。

## 依赖核验

- 最新正式发布 v3.1.0（2026-09-03），对应库 commit `f12037fa8e97ebc08e9e3edfd2de528ed1772a4b`。
- 官方 vcpkg 完整 registry 树 `664933b8d385dfc01f4e0cdff4c7c74e71f6a740` 无 RTXDI port；采用固定 archive，不更新无关 builtin-baseline。
- archive SHA256 `f91de92d7c27f9824915ee5fcf62b0da664a6eb294edb659056ff3c462823f75`。
- NVIDIA RTX SDKs LICENSE（2025-01-31）；保留下载中的许可，SDK 源码不提交到本仓库；发布分发另按该许可核查。
- 首次沙箱网络访问失败，经工具权限提升后官方 API/固定 archive 读取成功；未执行上游脚本。

## 实际变更

- 实施前更新 ReSTIR/路径设计，冻结首表面、初始 reservoir、全图两阶段、整批提交及内存/工作预算。
- `cmake/Rtxdi.cmake` 只由 GPU pathtracing target 引入，固定 archive/hash；`dk::rtxdi_shaders` PRIVATE 提供 shader include 路径，未链接 SDK C++ 或执行上游构建脚本。
- `PathTraceRenderer.hpp` 增加独立 `submit_restir_rays/view`、`configure_restir_view` 与来源配置、epoch/pass 进度；旧调用继续可用，无命令/schema/UI 变更。
- `path-restir-bridge.slang` 实际调用 RTXDI StreamSample/FinalizeResampling。引擎负责面积/环境 proposal、BSDF 目标、完整 float UV、随机域和当前可见性；delta 太阳独立，间接路径继续原 MIS/RR。
- `pathtrace.slang` 与新增 RIS shader 入口实现首表面记录/初始采样和最终着色；首命中复用、遮挡重新查询，有限值诊断拒绝异常候选。
- `PathTraceRenderer.cpp`/`PathRestirGpu.inl` 增加独立管线/描述符、Graph 资源访问和 tile/epoch 调度。batch 1..16 的全部阶段成功才发布；取消、失败、迟到结果保留旧帧。
- 完成 owner 为 224 bytes/pixel，候选同尺寸，多 epoch 增加 128 bytes/pixel 私有累积；旧帧外持者继续计费。实际 Vulkan requirements/对齐仍由共享额度准入。
- 修复固定射线上传的 -0 TMin：普通、介质及 RIS 三个入口统一为 +0，其他裁剪值不变；增加三入口对照。
- 新增五组 required GPU 用例，覆盖数值、材质/纹理、正交/透视重放、失败/取消/旧 owner/预算；同步测试选择表、依赖说明、使用指南与索引。

## 验证记录

Windows x64、MSVC 19.51、Slang 2026.18，RTX 4070 Laptop GPU / NVIDIA 596.49。
GPU 使用现有 `windows-graphics` / RelWithDebInfo，required RT、validation 与 synchronization validation 均开启。
CPU 使用现有 `windows-path-media-cpu` / Release；Graphics Device/Graph/Shaders/GPU Pathtracing OFF、CPU reference ON。
两个构建树均重新 `cmake -S . -B <目录>` 成功；既有 vcpkg 包已安装，CPU 树无 `_deps/dk_rtxdi_source-src`。

| 检查 | 结果 | 证据目录（out/verify 下） |
| --- | --- | --- |
| 首次构建 | 失败，测试误用私有相机 helper/不存在的 Device.dispatch；修正为公开相机入口和已有提交失败注入，未使用旧程序验收 | 20261019-135148-6cb2da5d |
| 初期 GPU 验证 | 依次发现共享反射 binding 9、Graph 生产平面 initialized 声明错误，修正；随后修正零 bounce/材质夹具、共享三角形边平局和 -0 TMin 上传 | 20261019-135325-88bf14d9 至 20261019-140009-1d0aaeb1 各独立 summary |
| 新增 RIS 与旧 GPU 回归 | 15/16 通过，无跳过；唯一失败是窄高光超长单 invocation device lost，后续有界工作修复。12 组旧回归全部通过 | 20261019-140319-c1cc6ccd |
| 有界工作和纹理补充 | numeric/materials/schedule/state 4 组通过，无跳过；textures 失败定位为确定性硬件过滤精度，后按下述分离协议修正 | 20261019-140711-df14397a |
| 最后受影响 GPU 检查 | numeric/textures/schedule/state 4/4 通过，无跳过；覆盖有限首表面、空场景射线计数与纹理分离协议。materials 既有通过结果仍有效 | 20261019-141033-e18747ba |
| CPU reservoir/光能/状态与基准 smoke | 18/18 通过，无跳过，确认 CPU-only 隔离 | 20261019-141517-44b639f4 |
| 文档与差异检查 | 9 个 Markdown、809 个本地链接、元数据/表格/索引/测试入口/JSON 通过；Git 空白检查通过 | `scripts/check-spec.ps1 -Path` 限定本次 9 个文档；`git -c core.safecrlf=false diff --check` |

最终 **17 个唯一 GPU 测试、18 个 CPU 测试通过**，无未解决失败或跳过。12 组旧 GPU 回归为
orthographic、orthographic_medium、cloud_surface、air_numeric、progressive_images/state/failure/budget/lost、analytic、ggx_indirect、texture_mask。
使用 `scripts/verify.ps1`；完整命令参数、构建日志、实际测试列表与逐项结果保存在每轮 summary/tests/results。

```powershell
# GPU 每轮选取受影响的后缀；最后一轮如下。
$previous = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
    $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
    & ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
        -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.restir_(numeric|textures|schedule|state)_gpu_required$' `
        -Reason 'Separate hardware filter precision from RIS statistics; verify finite surfaces and empty-scene ray accounting' -Parallel 2
} finally { $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previous }
& ./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Configuration Release `
    -Target dk_pathtracing_reference_tests,dk_restir_reference_benchmark `
    -TestRegex '^dk\.pathtracing\.(RESTIR |restir_reference_benchmark_cpu$)' `
    -Reason 'M19.2 CPU reservoir oracle and CPU-only RTXDI dependency isolation' -Parallel 2
```

GPU 环境变量只沿用本机 AMD switchable layer 规避并在 finally 恢复，没有关闭 validation/sync。
构建/外部工具链访问通过工具权限提升完成，没有切换配置或降级模块绕过失败。

## 实测偏差与决策

- roughness=.05、64 候选、65536 spp 的单 invocation 曾触发 device lost。新增每像素每次提交 2^18 候选上限并测试拒绝；
  高样本按连续区间 16×4096 分批，总样本与 seed 不变，nRMSE 为 `6.5155333e-7`，全部均值门槛通过。渐进模式每 invocation 只执行一个 epoch。
- 同计数器 CPU/GPU 逐 seed nRMSE 在已测场景最高 `2.6336872e-5`（间接光），低于 2%；各 RGB 均同时通过 2%+1e-4 和 5SE+1e-4。
  非纹理独立解析/积分参考不变；不能由同计数器一致性独自推定无偏。
- 任意 UV 的双线性过滤产生 `0.00263842007` 最大绝对硬件/CPU 差，满足已有 M16.7.1 的 .005 契约。
  先独立验证过滤，再让 RIS 对同 GPU BSDF-only 白炉满足原 2%/5SE 门槛；另用二进制可表示过滤权重验证严格 CPU/GPU 光能、mask 阴影与面积光。
  未增加 RIS 统计容差，未裁掉高权重或修改物理 BSDF。
- 正交/透视 19 spp、batch 1/7/16、tile 64/3 与逆序遍历均和无历史入口逐位一致；纹理/空场景也完成阶段对照。
- 已发布帧包含最终完整采样平面，外持期间不可变且计费；单阶段失败、第二 epoch 取消、seed 变化迟到与内存拒绝后恢复通过。

## 遗留边界

下一阶段为 M19.3 时间复用；空间复用、介质 RIS、Editor/capture 切换、DLSS 均未实施。
未运行全量、Debug 双配置、Editor/GUI、其他 GPU/平台或发布打包；本阶段没有正式 GPU 等误差性能基线，不声称 2× 加速。
RTXDI 源码留在构建树，当前运行时 shader 编译依赖其 Include 路径；可分发打包另行实施。
用户原有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 改动保留，不纳入提交。

## 修改记录

- 2026-10-19T13:44:00+08:00：建立实施契约、记录和固定依赖事实。
- 2026-10-19T14:18:00+08:00：完成 GPU 初始 RIS、调度/额度/失败保护、17 GPU 与 18 CPU 定向验证，记录实际限制。
