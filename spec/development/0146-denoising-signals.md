---
id: "0146"
created_at: "2026-10-23T17:23:01+08:00"
updated_at: "2026-10-23T17:58:35+08:00"
status: completed
design_refs:
  - ../design/render-denoising.md
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
---

# 0146 M20.2 路径追踪降噪输入信号

## 目标与设计依据

完成 [Roadmap M20.2](../roadmap.md#m20路径追踪降噪)，按[降噪信号契约](../design/render-denoising.md#m202-信号生产实施契约)
生成显式 fresh 分量、匹配采样 guides 和完整帧来源。仅实施输入生产；不提前接入滤波器或 SDK。

## 实施计划

1. CPU-only 信号布局/来源描述、渐进 API 可选开关和拥有型结果。
2. 独立 shader 变体生产分量、累计 guides、代表性几何和显式 motion reference；Classic/ReSTIR 保留原始采样运算。
3. 完整批次发布、配置冻结、取消/失败/预算与外持 owner 接入。
4. 数值/来源/投影/纹理/mask/多 tile/多 epoch 与状态定向 GPU 验证，更新设计和阶段状态后独立本地提交。

## 状态与失败保护

信号平面随 PathTraceCompletedFrame 所有，候选未完成不公开。启用改变时重建共同采样起点；普通调度参数不改变原始估计器。
分配/提交/校验失败、取消或过期候选不推进 source serial，不替换旧完成帧。GPU 在途 owner 按原 submission 寿命退休。

## 实际变更

- 新增 CPU-only `dk_render_denoising_types` INTERFACE target，仅链接 Math；布局版本 1 为 336 字节/像素。
  公开 signal options、motion reference 与完整来源身份；GPU buffer 仍由路径 completed frame 持有，异步读回提供类型化 span。
- 路径侧按需编译独立信号 shader 变体；Off 没有信号平面或信号计算。Classic 和 ReSTIR 共用原随机数、BRDF/MIS/可见性与 raw 补偿累积。
  BRDF lobe 比例按实际入射方向传播，fresh 六类补偿和独立于 raw；累计 baseColor/normal 与 raw 采样覆盖一致。
- 保存本批首样本的几何/材质/有效性、主射线和后续命中距离，motion 使用显式消费来源矩阵；ReSTIR 相机移动后的随机 epoch 单独记录。
- 原始与信号在完整批次成功后共同发布；source serial 仅在提交点递增。多 epoch 信号单独 ping-pong，取消/失败不污染旧输出。
  开关切换重建共同采样起点；reference 与普通调度更新冻结到下一批。输入范围、候选/旧代/读回/暂存纳入原有预算。
- 新增四组 required GPU 信号用例，扩展测试选择表；同步设计、架构/索引、GPU 指南和 Roadmap。没有新三方依赖或命令/schema 变更。
  工作区已有 spec/README.md、spec/research/ 改动不属于本阶段，保持不纳入提交。

## 验证记录

配置为现有 `out/build/windows-editor`、Release、本机 RTX 4070 Laptop；所有 GPU 用例要求 RT、Khronos validation 与同步校验。
只在测试子进程设置已支持的 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，不更改系统配置。

- `out/verify/20261023-173715-90a55341`：初次 CMake 自动再生成因沙箱不能访问外部 vcpkg 目录失败；权限审核后使用同一构建继续。
- `out/verify/20261023-173735-df8d92ba`：引擎编译成功，新增测试的 Eigen 表达式使用同组 auto 推导导致编译失败，未运行测试；改为显式 Vec3d。
- `out/verify/20261023-173926-f7399689`：几何、纹理、状态 3 项通过；数值用例的 Lambert 夹具误设非零 metallic 被输入校验拒绝。修正夹具并增加独立 GGX delta-sun diffuse/specular 解析对照。
- `out/verify/20261023-174203-1d9bb4dd`：13 项通过，0 失败/跳过。构建 target `dk_pathtracing_probe`，筛选
  `^dk\.pathtracing\.(signal_.*|analytic|progressive_images|progressive_state|progressive_failure|progressive_lost|tile_group_images|restir_temporal_schedule|restir_spatial_schedule|restir_medium_schedule)_gpu_required$`。
  覆盖 fresh 与独立样本区间逐值对照、BRDF 端点/混合/间接/RR、分量重组、Classic/ReSTIR 多 epoch、Off raw 逐值/射线统计一致、
  空场景/纹理/sRGB/mask/累计覆盖、投影/运动、完整批次/取消/提交失败/额度/旧 owner、原有介质和设备丢失路径。
- `out/verify/20261023-174705-838e6650`：2 项通过，0 失败/跳过。构建 `dk_pathtracing_probe`、`dk_editor_render_benchmark`，筛选
  `^dk\.(pathtracing\.signal_geometry_gpu_required|editor\.restir_worker_gpu_validation)$`。
  补充透视/正交 ReSTIR 相机移动后 epoch=4 与 source_serial/motion 的独立对照，并验证现有 worker 对扩展公开类型的消费。
  至此与上一轮去重后共 14 项 GPU 检查通过。
- `out/verify/20261023-175638-28646433`：3 项通过，0 失败/跳过，target `dk_pathtracing_probe`，筛选
  `^dk\.pathtracing\.(signal_textures|signal_state|progressive_budget)_gpu_required$`。
  最终审查将新增预检限制在 signals 开启时，保持 Off 原有失败路径；补充纹理+时空 ReSTIR 组合和带 stop token 的在途释放。
  最终共 **15 个不同 GPU 检查通过**；已有数值/运动/worker 结果继续有效，不重复其未改动链路。
- CPU-only：`cmake -S . -B out/build/windows-path-media-cpu --graphviz=out/verify/20261023-m20-2-cpu-targets.dot` 配置/生成通过。
  全部包已安装，无下载；target 图仅有 `render_denoising_types -> math`，没有 Graphics、Slang、RTXDI、NRD 或 vk_oidn。
  这证明配置/依赖隔离，不宣称新增 CPU 降噪实现或已运行 CPU 数值测试。
- `scripts/check-spec.ps1 -Path` 最终通过 10 份 Markdown、900 个本地链接及全局元数据/表格/索引/测试入口/JSON；
  日志为 `out/verify/20261023-m20-2-check-spec.log`。`git diff --check` 通过。

未运行全量、Debug、其他设备、真实 GUI 新控件、滤波质量或性能基准：本阶段没有滤波器/UI，相关后端仍待后续阶段。

## 遗留与下一步

M20.3 后续实现 À-trous；Scene media 信号分解和 NRD 特定距离编码仍按各自阶段验收。

## 修改记录

- 2026-10-23T17:23:01+08:00：细化 M20.2 输入/状态与验证设计并开始实现。
- 2026-10-23T17:58:35+08:00：信号生产与 15 项定向 GPU 验收完成，CPU-only 配置隔离通过；不启用滤波或 SDK。
