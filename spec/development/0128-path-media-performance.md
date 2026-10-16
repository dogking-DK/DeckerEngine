---
id: "0128"
created_at: "2026-10-16T15:30:20+08:00"
updated_at: "2026-10-16T16:34:34+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/editor.md
  - ../design/render-capture.md
---

# 0128 M17.6 Sponza 介质路径性能

## 目标与设计依据

依据[路径设计](../design/render-pathtracing.md#m176-测量与优化实施契约)与[Editor](../design/editor.md#m176-缓存与交互调度)，
完成同模型/固定质量优化、真实交互和三轮性能/显存验收；不混用 M15 近似云或改变曝光掩盖误差。

## 实际变更

- PathTraceScene增加拥有型rebind，逐实体/世界变换/资产引用确认后复用冻结三角形/纹理，直接持有根owner避免长链；Reload强制create。geometry hash不变，input fingerprint v3组合完整环境/元数据/geometry，捕获schema/命令文档同步。
- 几何构建只遍历一次hash流，环境编辑不再重建CPU几何；GPU相同owner跳过深比较，继续复用纹理/BLAS/TLAS。
- 介质shader每路径结束一次atomic add，保留每路径硬界及tile超额sticky错误/整批失败；随机流、消光、majorant和散射模型不变。
- Scene media相机移动使用192像素长边，200ms静止恢复原尺寸、样本0重启。合并相机请求优先发布完整预览，渲染阶段100ms再替换；冷准备不因连续相机输入反复取消，物理输入变更仍立即失效。面板显示临时预览与实际尺寸。
- worker立即响应suspend唤醒，排空后报告Suspended；pending状态在提交后更新。测试区分API调用、GPU回收和GUI实际完成呈现。
- 新增冻结输入/独立CPU与GPU参考/质量阶梯、三轮640×360/96×54性能及完整性回放；扩展现有worker探针和真实GUI，包含媒体连续拖动与停动尺寸恢复。原始线性/二阶矩/逐tile计时以gzip归档，无新依赖。

## 验证记录

Windows Release / MSVC 19.51.36260.0 / RTX 4070 Laptop / 596.49。GPU串行；正确性required RT/validation/sync，性能validation OFF。Editor使用--background及独立复制工程；AMD switchable冲突仅在子shell临时禁用。

所有构建/CTest使用scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release，实际Target/TestRegex保存在各summary：

| 范围 | 结果 | 证据 |
| --- | --- | --- |
| 原版测量探针smoke | 1/1通过 | out/verify/20261016-153615-12ecfda3 |
| CPU重绑与最初GPU预算/探针 | 4/4通过；此时CAS性能候选随后被拒绝 | out/verify/20261016-154359-5f42e1ba |
| 最终atomic add、CPU所有权/纹理、介质数值/统计/历史/预算、捕获schema/命令、worker/GUI与报告校验 | 42/42通过，无跳过 | out/verify/20261016-161246-2a549436 |
| worker测量尺寸/来源元数据补充 | 1/1通过 | out/verify/20261016-161851-2333c540 |
| 连续相机请求合并修正，旧表面/媒体捕获一致与真实Sponza | 3/3通过 | out/verify/20261016-162124-3981815f |

主验证Target包括dk_editor_app、dk_editor_render_benchmark、dk_pathtracing_probe、dk_ctl、dk_run、dk_pathtracing_reference_tests、dk_pathmedia_benchmark、dk_protocol_tests。
42个唯一目标覆盖CPU根owner/源释放/冲突/纹理、air/cloud全部10个GPU组、progressive状态/失败/额度、GGX及mask、两类capture、真实Console/IPC及三种GUI；后续只重跑受影响链路。
捕获测试通过实际commands.list/describe确认external/不可撤销、v3输入指纹schema与输出一致；参数版本和模型版本保持。

性能原始目录out/benchmarks/media-0128：独立baseline与optimized各3轮；质量8 seed，CPU16384/65536，Sponza4×4独立参考与64..16384阶梯。质量采集约1287秒，GPU无并发测量。
四个CPU参考收敛误差0.252%–0.399%，CPU/GPU最大偏差0.0645%；同样本介质线性/二阶矩前后差为0。
Sponza云内/云上方参考达0.5%，16384 spp达2%；其余8个输入参考及候选未达，达到耗时null，不作为已认证真值。
庭院空气/稀疏云单样本wall降低约16%/18%，云内/云上方约41%/36%；天空晴空与surface wall未改善。CPU几何准备约420ms降至0.003–0.006ms，CPU heap峰值746299508降至377772972字节。
GPU自有峰值915354784字节，所有运行退出CPU/VMA存活分配0；详见[完整实测与原始归档](../benchmarks/2026-10-16-path-media.md)。

最终呈现时点验证1/1通过（out/verify/20261016-162425-74e700a0），关闭验证层GUI另一次通过。
最终真实Sponza两秒内空气/云38/37次完整新视角，GUI请求到呈现GPU完成p95=60.664ms；停动恢复736×436。
三轮worker热相机p95=21.075/20.952/21.007ms，取消至排空p95=12.050/15.697/16.033ms；新字段首构建数百ms独列。
补充验证Git归一化LF后的归档字节仍可回放；临时目录在沙箱外首次被拒绝后改为workspace out目录，8项单测全部通过。
归档含editor.json.gz与全部renderer raw，benchmark-path-media.py --verify-archive核验SHA并重算通过。
报告拒绝及归档换行单测8项通过；check-spec限定10份Markdown、527个本地链接及元数据/表格/索引/测试入口/JSON检查通过；git diff --check通过。

## 偏差与决策

- CAS循环虽然数值/预算测试通过，实际整轮约129秒，严重退化；保留out/benchmarks/media-0128/rejected-cas.json并终止其第二轮，替换为一次atomic add后重新独立测三轮。首次超额在不溢出的范围置sticky错误，后续失败计数不当有效统计。
- 33ms相机替换期限在validation GUI勉强8次/2秒、performance GUI只有6次/2秒，实际发生取消饥饿。改为完成优先/渲染100ms、冷准备不取消，收紧GUI检查至少16次/2秒，重测通过36–37次/2秒。
- tile64的GPU p95全部低于8ms，保留固定粒度，不增加常驻profiling。majorant/重要性采样需要改变样本策略，本轮没有证据要求冒险修改；收益来自缓存/计数与呈现调度。
- 没有放宽画质门槛。未收敛Sponza参考只作收敛观测；正确性依据四个收敛CPU参考、解析/统计回归与同样本前后比较。

## 遗留问题与下一步

首次编译/导入/AS及首次云场生成仍需要等待；100ms指标针对热相机/取消，不能当作首次启用时延。
高质量静态路径非实时，8个Sponza质量候选仍未达，完整PBR/去噪/wavefront不在本阶段。Capture保留4096三角形小场景额度。
未跑全量、Debug双配置或重新配置CPU-only；没有依赖/平台接入变更。原有spec/README.md与spec/guides/ai-documentation-workflow.md改动不纳入提交。

## 修改记录

- 2026-10-16T15:30:20+08:00：创建实施设计与记录。

- 2026-10-16T16:28:00+08:00：完成固定样本/参考/三轮性能与42项唯一目标检查，修正CAS退化和相机取消饥饿；归档实际未达候选并完成最终呈现验证。
