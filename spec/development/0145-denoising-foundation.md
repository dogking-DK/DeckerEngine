---
id: "0145"
created_at: "2026-10-23T16:58:00+08:00"
updated_at: "2026-10-23T17:14:39+08:00"
status: completed
design_refs:
  - ../design/render-denoising.md
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
  - ../design/architecture.md
---

# 0145 M20.1 降噪接口、状态与依赖基线

## 目标与设计依据

按用户确定的范围把 M20.1–M20.11 写入 [Roadmap](../roadmap.md)，本次仅完成 M20.1。
[降噪设计](../design/render-denoising.md)定义 À-trous、SVGF、A-SVGF、NRD REBLUR/RELAX、vk_oidn 的共同边界，
不创建假 GPU 实现或提前开放 UI。À-trous 独立可选，并作为两种自研时域算法的空间组件。

## 实际变更

- 新建设计及设计索引；更新 Roadmap 总览、小阶段/依赖/验收、当前下一步；关联路径/ReSTIR/架构的后续边界。
- 确定 accumulated/fresh 输入、分量和 guides、三类历史、来源 serial、能力/配置/错误、完成提交点、不可变输出和预算责任。
- 特别区分 SDK 原地历史污染与输出提交：取消/失败后旧输出保留，不能保证被 SDK 修改的历史可继续复用。
- 核验当前 Graph 没有原生 command buffer 回调；将 vk_oidn 外部 compute 桥、设备 feature 与 SDK 设备内存记账列为 M20.4 必须实现的链路。
- 新增 [固定来源清单](../dependencies/render-denoising.lock.json)，更新 [三方库说明](../third-party-libraries.md#m20-降噪依赖基线)。
  它是设计基线，当前未被 CMake 消费；现有 builtin-baseline、构建选项、命令和运行时均未改变。

## 依赖核验

- 官方 vcpkg commit `23b8a8e7409c2d6346114a88e834165bde79a51c`：完整 ports 树未截断，2886 项，
  与 baseline 2886 项匹配；没有 NRD/vk_oidn port。采用固定上游来源而非浮动分支，不升级无关依赖。
- NRD latest release 为 v4.17.3，commit `792eff196afdd350fd9c3f862119017ccb438a0e`；
  主分支 README 的 4.18 不是本次正式 release 基线。许可为 NVIDIA RTX SDKs LICENSE。
  固定上游指定 ShaderMake commit 和 MathLib v11 对应 commit，关闭 NRI 与非 SPIR-V shader。
- vk_oidn v0.2.0 对应 `81b75b54f064da71f79b821eab067de81cfe65d6`；Windows SDK SHA-256 与 GitHub asset digest、
  发布 SHA256SUMS 一致。核对 ZIP 内头文件、Debug/Release 库、CMake target 和 LICENSE/NOTICE；OIDN 2.5.1 权重来源固定。
- NRD 编译工具选择官方 registry 的 directx-dxc 2026-05-27#0（v1.9.2602.24），记录 port SHA-512，
  替代 ShaderMake 默认旧版本的隐式下载。仅核验元数据，未激活、下载或执行该工具。

## 验证记录

- 下载/哈希及原始远端核验材料保存于 `out/verify/20261023-m20-1-dependencies`；仅下载和检查包，未运行第三方程序。
- 首次下载因沙箱禁止网络套接字失败；经工具权限审核后重试成功，非依赖内容或构建失败。
- `scripts/check-spec.ps1 -Path` 定向检查本次 9 份 Markdown：通过，922 个本地链接及全局元数据、表格、索引、测试入口、JSON 清单一致性通过。
- `out/verify/20261023-m20-1-dependencies/verify-lock.ps1`：42 项通过；重新计算四个归档的哈希/尺寸，
  对照正式 release/commit、GitHub digest/发布校验和、完整 registry 名称集合、DXC 元数据和 SDK 内头文件/库/CMake 导出/许可。
  结果为同目录 `validation.json`；核验材料保存在 `official-metadata.json`、`source-audit.json`、`source-identities.json` 和 `downloads.json`。
- `git diff --check`：通过。阶段变更限于设计/开发记录、Roadmap/索引、三方库说明与固定来源 JSON；未更改 C++、shader、CMake 或 vcpkg 清单。
- 未运行引擎构建、CPU/GPU 测试、SDK 链接、shader 编译和质量/性能测试：本阶段只更改设计/来源清单，尚无运行时实现。

## 偏差与决策

采用独立模块的接口设计，不在 M20.1 建立空 C++ API 或只有构造成功的测试。
vk_oidn SDK 内存由其直接分配，不能声称已归入 VMA 预算；严格准入估计及实际分配观测在 M20.4 验收。
A-SVGF 的 ReSTIR 梯度、Scene media 分量策略分别留在 M20.9/10，不把规划能力算作本阶段支持。

## 遗留问题与下一步

下一步为 M20.2：显式 fresh 信号、分量重组与匹配采样 guides；以原始结果保持、投影/纹理/GGX 边界和资源增量为验收。
后续实际引入 SDK/host tool 前按三方库规则复核版本漂移，必要时更新锁定清单和该阶段记录。

## 修改记录

- 2026-10-23T16:58:00+08:00：建立 M20.1 设计、固定来源和核验记录。
- 2026-10-23T17:14:39+08:00：文档与 42 项依赖核验通过，M20.1 设计阶段验收完成；M20.2–11 保持待开始。
