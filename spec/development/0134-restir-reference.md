---
id: "0134"
created_at: "2026-10-19T12:57:00+08:00"
updated_at: "2026-10-19T13:13:00+08:00"
status: completed
design_refs:
  - ../design/render-restir.md
  - ../design/render-pathtracing.md
---

# 0134 M19.1 ReSTIR CPU 参考

## 目标与设计依据

依据[ReSTIR 实施契约](../design/render-restir.md#m191-cpu-实施契约)，交付流式 reservoir、支持校正合并数学、
首表面 RIS 与现有路径的光能分工、独立数值验证及基准协议。
按用户要求为后续 RTXDI/DLSS 保持后端边界，本阶段不接入 SDK/GPU/编辑器开关。

## 实际变更

- 实施前更新模块设计，冻结 CPU 接口、光源测度、采样域、支持校正推导、失败保护及误差协议。
- `RestirReference.hpp/.cpp`：双精度流式 reservoir、支持矩阵校正合并、独立随机域，零候选计数、极值/无效输入与提交前校验。
- `PathTraceReference.cpp`：独立 `estimate_restir_reference`，首表面面积/常量环境 RIS 与 delta 太阳分工；一次 BSDF 延伸只抑制已覆盖发光，后续反射保留 MIS/RR。旧 CPU/GPU 请求结构和旧采样序列保持。
- 内部光样本冻结光域身份、测度/坐标、proposal 和目标；公开参考不暴露 SDK 类型。CPU 入口预检候选/射线/几何预算，失败不发布部分估计。
- 新增 17 项 CPU 测试及独立基准工具，覆盖可枚举期望、支持域反例、相关来源、M 压缩、mask、混合光能、GGX、相机、取消与预算；同步目标/测试选择入口。
- 更新后续 RTXDI/DLSS 方向、三方库说明和 Roadmap；无新增或升级依赖，无 SDK 接入或不可用 UI 开关。

## 验证记录

Windows / MSVC 19.51.36260.0，现有 `windows-path-media-cpu` / Release。
Graphics Device/Graph/Shaders/GPU Pathtracing 均 OFF，CPU reference ON；Release 用于高样本统计/求积与基准。

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| 原配置重新生成 | 通过，全部依赖已安装，无 baseline/版本变化 | `cmake -S . -B out/build/windows-path-media-cpu` |
| 首次定向构建 | 失败，测试中将 selection 的 normalization 字段写成函数调用；未运行测试，修正后重建 | out/verify/20261019-130248-6953fd85 |
| 初始 RIS 数学/光能/状态与基准 smoke | 16/16 通过，无跳过 | out/verify/20261019-130355-fcbec087 |
| 混合光能/活动取消补充，普通 CPU 表面路径/纹理/GGX 回归及基准 smoke | 33/33 通过，无跳过；两轮合计 48 个唯一测试 | out/verify/20261019-130540-88fe4687 |
| 单灯/100灯/1000灯 CPU 正式基线 | 48 次估计完成，6 组均值/参考门槛通过；报告完整性通过 | [原始 JSON](../benchmarks/2026-10-19-restir-reference.json)、[结果与命令](../benchmarks/2026-10-19-restir-reference.md) |
| 文档与差异检查 | 9 个 Markdown、784 个本地链接、元数据/索引/表格/测试入口/JSON 检查通过；Git 空白检查通过 | `scripts/check-spec.ps1 -Path` 限定本次 9 个文档；`git -c core.safecrlf=false diff --check` |

两次成功测试均使用 `scripts/verify.ps1`，Target 为 `dk_pathtracing_reference_tests`、`dk_restir_reference_benchmark`，
具体 TestRegex、配置、构建和结果保存在各 summary。仅对受影响链路执行，未运行全量、Debug 双配置、GPU、Editor 或 SDK 验证。
初次沙箱配置不能访问外部 vcpkg 工具链，经工具权限提升后成功；没有替换构建配置或关闭功能规避失败。

## 偏差与决策

GPU 阶段优先核验 RTXDI-Library 适配，CPU 参考保留独立 oracle；DLSS 属于重建层，不能替代采样原始线性质量验证。
初始 RIS 采用 light-only proposal；roughness=.05 的窄高光用 64 候选/65536 spp 的独立积分对照通过，
仍不代表低 spp 高光已足够低噪声。CPU 多灯同 spp 噪声下降但 wall 未下降，未宣称 GPU 加速。
支持校正数学以正规化来源、预先固定有效计数/支持域和目的支持覆盖为前提，不能推定任意时域启发式无偏。

## 遗留问题与下一步

下一阶段 M19.2 优先核验 RTXDI 版本与 Slang/桥接，再进行 GPU 首表面 RIS 和资源/epoch 调度。
本阶段没有 GPU/时间或空间会话、Scene media RIS、Editor/capture 切换、DLSS/RTXDI SDK；其实施状态以 Roadmap 为准。
用户原有 spec/README.md 与 spec/guides/ai-documentation-workflow.md 改动保留，不纳入提交。

## 修改记录

- 2026-10-19T12:57:00+08:00：创建实施设计与记录。
- 2026-10-19T13:13:00+08:00：完成 CPU 实现、48 个唯一定向测试、固定基线与文档检查，记录 SDK 后续接入边界和当前性能限制。
