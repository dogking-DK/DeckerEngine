---
id: "0084"
created_at: "2026-10-10T11:18:00+08:00"
updated_at: "2026-10-10T11:34:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/physics-api.md
  - ../design/scripting-luau.md
---

# 0084 M12.3 指标显示与实验导出

## 目标与设计依据

完成显式数值采样、已完成批次墙钟指标、采样状态与实验导出面板。
先更新[编辑器](../design/editor.md)和[模拟服务](../design/physics-api.md)契约。
复用现有同步诊断/导出，不引入常规 GPU 读回，不改 Edit/history。
保留用户原有 spec/README.md 和 spec/guides/ai-documentation-workflow.md，不纳入阶段提交。

## 验证计划

使用 windows-editor / RelWithDebInfo，定向验证采样身份/步数/缓存、成功批次计时发布、模型命令链、
CPU/GPU 实际 GUI 采样/导出及现有实验重放与失败保护。核验命令发现/schema 和文档；
不运行全量或双配置，不重复 M11 性能矩阵。

## 实施

- Services/Operations：simulation.sample 在精确健康暂停边界读取并缓存带步数/时间的数值；query 无隐式读回。
  worker 仅在批次成功完成时发布墙钟耗时，失败保留上一批样本。
- Workspace/Simulation 面板：Results/Next experiment 分页，显示模拟时间/进度、Current/Stale/Unavailable、
  全部数值指标；Export 复用命令，保留最后成功路径/运行/步数和失败信息。截图检查后缩短按钮标签，适配默认窄面板。
- 导出保留五文件与确定性重放内容；provenance 补齐步数/时间/尺寸/性能，返回 run_id，成功后同步数值样本。
- Luau 查询白名单加入 sample；命令参考与指南同步。

## 验证记录

- `out/verify/20261010-112225-1f249e96`：windows-editor/RelWithDebInfo 定向构建成功，8项检查通过、0失败/跳过。
  包括精确边界/缓存/失败、计时发布、Workspace/IPC，以及真实CPU/GPU按钮采样、旧样本、导出身份和重复导出文件逐字节不变。
  GUI 同步validation通过；截图已检查。该配置 Luau=OFF，experiment_gpu 未注册，不能计作通过。
- `out/verify/20261010-112607-cdbe2e51`：已有 windows-graphics/RelWithDebInfo（Luau=ON），2项通过、0失败/跳过。
  Luau query模式允许采样；Python真实IPC命令发现/describe、GPU显式样本与导出一致、300拍重放、
  CPU/GPU容差、同后端四文件逐字节重复、Lua脚本输出与Edit/history保护通过。
- `out/verify/20261010-112905-7846eb77`：Export按钮标签调整后仅重编 dk_editor_app，GUI 1项复核通过，0失败/跳过。
  截图与五文件保留在 `out/build/windows-editor/test-artifacts/RelWithDebInfo/editor-simulation-af5e6c4a02214e5aabb8e4426ac970ff`。
- 共10项不同检查通过；只对改过的GUI复核一次。两处构建均为RelWithDebInfo；未运行全量、Debug/Release矩阵、
  新一轮性能/退出延迟矩阵或独立CPU-only配置，本次未改CPU-only依赖边界和退出阈值。
- `scripts/check-spec.ps1`、`git diff --check`通过；命令参考、模块设计、Roadmap与记录索引同步。

实际验证命令（PowerShell）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_simulation_tests','dk_editor_tests','dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.(simulation\.(simulation metric|finite task (performance|diagnostic)|finite simulation commands|experiment_gpu$)|editor\.(workspace simulation|simulation_gpu_validation$))' -Reason 'M12.3 指标样本身份/过期、批次计时、模型入口、CPU/GPU真实GUI导出和实验重放'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl','dk_luau_tests') -TestRegex '^dk\.(simulation\.experiment_gpu$|luau\.Luau simulation example)' -Reason 'M12.3 现有启用Luau的GPU配置核验新采样查询权限及Python/Luau实验导出重放，编辑器配置未注册这些检查'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target dk_editor_app -TestRegex '^dk\.editor\.simulation_gpu_validation$' -Reason '仅复核截图发现的窄面板Export按钮裁切修正；其余已通过检查不重跑'
```

## 限制与下一步

采样和导出复用显式同步诊断，可能等待GPU与磁盘；常规query/显示不会隐式数值回读。
性能值为最近完成批次墙钟耗时，不是GPU timestamp或整轮基准；导出采用配置记录的固定实验相机。
本阶段无剩余实现项。下一项为M12.4真实窗口与外部客户端/脚本共同实验的一致性验收。
