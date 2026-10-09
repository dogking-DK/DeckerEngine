---
id: "0082"
created_at: "2026-10-09T19:50:44+08:00"
updated_at: "2026-10-09T20:10:17+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/physics-api.md
---

# 0082 M12.1 模拟面板与运行控制

## 目标与契约

按[编辑器设计](../design/editor.md#m121-模拟面板与运行控制)实现参数草稿、CPU/GPU选择、
异步Play/零步准备、暂停/恢复/单步/取消/Stop和实际运行状态。GUI与外部IPC共享服务；
运行中参数仅供下次启动，不改变Edit/history，不实现M12.2实时模拟视口。
为保持GPU冷初始化可响应，在[模拟服务](../design/physics-api.md)补充有限任务paused启动和有界异步单步。

## 验证计划

只构建editor、editor/simulation单元测试及直接使用的runner/ctl；选择有限任务边界、面板模型/IPC及真实GUI按钮测试。
单步测试覆盖零步初始化、精确完成、重复/越界/终态拒绝、取消/Stop竞争；复用M11数值证据，不重跑性能矩阵。
命令schema/effect发现与示例在同一已构建配置核验；文档检查后独立本地提交。

## 实际变更

- Workspace新增SimulationDraft和带观察run_id的控制接口，均走Runtime命令；仅新建实验应用参数，
  Inspector未提交草稿拒绝启动。外部IPC不覆盖本地参数，活动状态每帧读取，不增加GPU读回。
- Simulation面板提供全部布片参数、CPU/GPU选择、Play/Prepare paused/Pause/Resume/Step/Cancel/Stop，
  显示实际状态、完成拍数、活动配置与故障；终态/回收中按服务状态禁用按钮。
- simulation.run增加默认false的paused；worker启动前记录暂停意图，初始化后0拍暂停。
  有限任务step在同一互斥边界受理一个手动批次，立即进入pausing，完成后paused或succeeded；
  拒绝重入/越界/终态/旧run。Cancel/Stop可取消未领取工作，已领取工作仍保留至完成。
- 当前视口明确仍为EditWorld；GUI smoke真实点击CPU/GPU控制并核对显示状态及Edit/history，
  不将本阶段宣称为实时布片预览。新增命令文档、指南和测试选择入口。

## 验证与修复记录

单一windows-editor / RelWithDebInfo配置；均用scripts/verify.ps1显式Target和TestRegex。

1. 195608-831d31f6：首次链接因既有ShaderCompiler.obj的COFF节损坏失败，测试未运行。
   只删除该配置的单个obj及其dk_graphics_shaders.lib，未清空目录/变更源代码或依赖。
2. 195919-24aefce0：dk_editor_app/dk_editor_tests/dk_simulation_tests/dk_run/dk_ctl，
   筛选finite、task_cpu/task_gpu、workspace simulation、simulation/workbench GPU validation；
   21通过、1失败。16个有限任务测试、2个面板模型/IPC、真实CPU/GPU任务和新面板GUI通过；
   原编辑器Apply回归暴露右侧分栏高度不足，修改布局将Simulation移到左下、Inspector保留完整高度。
3. 200203-4eb5ca0d：仅重建dk_editor_app及两项GUI。原工作台26个操作已通过，退出因已知AMD层警告失败；
   新面板在较窄左栏的Stop被裁切导致超时。依据实际截图将按钮分行、参数改双列表，
   验收模式统一单列原有精确AMD Loader Message，其他warning/error及存活分配保持失败。
4. 本次已构建dk-run通过stdio调用commands.list和commands.describe，核验run/step存在、
   effect=control、undoable=false、paused布尔schema；原始输出保存out/m12-1-command-discovery。

5. 200536-ae9b8dcb：仅dk_editor_app与`^dk\.editor\.(simulation_gpu_validation|workbench_gpu_validation)$`，
   2/2通过、0失败/跳过。模拟CPU/GPU共18个真实按钮阶段及原工作台26个输入阶段通过；
   required validation下0错误、0存活分配，精确已知AMD警告单列。其余20项结果未受UI布局修改影响，未重复运行。
6. scripts/check-spec.ps1检查本阶段10个Markdown文件，370个本地链接、元数据、表格、索引、
   测试入口及JSON清单均通过；git diff --check通过。未纳入用户原有文档改动。

最终截图位于out/build/windows-editor/test-artifacts/RelWithDebInfo/
editor-simulation-78c65fd7db4f432a90319dd19613dafb/simulation-panel.ppm；无损转换PNG后检查了
按钮可见、参数列宽、说明及编辑视口布局。原Inspector的Apply/Undo/Redo/保存/重载全部恢复通过。
命令单元与真实IPC覆盖新paused启动、单步精确计数、参数/旧ID拒绝和外部状态协调，
故障与取消/Stop的在途保护沿用同一有限任务回归；没有新建GUI专属模拟状态机。

## 交付与限制

M12.1完成；M12仍进行中，下一项M12.2。当前UI运行的是有限目标实验，不按墙钟帧率限速；
从Prepare paused开始可逐拍推进。GPU编译能力不保证设备可用，失败显示fault并允许Stop。
视口仍显示EditWorld，未实现实时布片、指标采样/导出UI或粒子拖拽。
未运行全量、独立CPU-only构建或M11性能矩阵；本阶段复用原CPU/GPU数值基础，新增任务路径已由真实CPU/GPU检查验证。
保留用户原有spec/README.md与spec/guides/ai-documentation-workflow.md，独立本地提交不包含它们。
