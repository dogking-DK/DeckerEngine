---
id: "0085"
created_at: "2026-10-10T14:11:00+08:00"
updated_at: "2026-10-10T14:21:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/editor-interaction.md
  - ../design/physics-api.md
  - ../design/render-simulation.md
---

# 0085 M12.4 GUI/IPC/脚本一致性验收

## 目标与设计依据

用户“完成M124”按Roadmap当前下一项M12.4执行。
先更新[编辑器](../design/editor.md)、[交互](../design/editor-interaction.md)、
[模拟服务](../design/physics-api.md)和[可视化](../design/render-simulation.md)的验收边界。
串起同一真实窗口的GUI操作、SDK/IPC控制、300拍导出与Python脚本重放，保持最新Edit/history。
保留用户原有spec/README.md和spec/guides/ai-documentation-workflow.md，不纳入本阶段提交。

## 验证计划

使用已有windows-editor/RelWithDebInfo。新跨入口CPU/GPU检查、现有实时视口/活动关闭、
图像交接与GUI响应smoke；不运行全量/双配置，不重跑完整M11矩阵。
响应smoke按实际样本报告partial与门槛结果，沿用完整退出p95≤400ms政策，不从少量样本声称完整p95验收。
## 实施

- [SimulationConsistencyDriver](../../engine/editor/src/SimulationConsistencyDriver.cpp)在实际ImGui输入与present完成之间保存固定检查点。
  每个后端依次观察initial/prepared/stepped/sampled/advanced/exported/running/paused/stopped/replayed，20个检查点全部可供外部比对。
  准备/单步/采样/导出/恢复/Stop均点击原有控件，不在夹具中直接写Workspace或SimulationService。
- [外部夹具](../../tests/integration/EditorSimulationConsistencyTest.py)使用SDK/dk-ctl补齐299步、编辑场景、暂停和拒绝旧身份/错误步数，
  对照GUI编辑版本/名称/历史与视口完成步数；GUI暂停画面连续8个呈现帧签名不变。
- Stop后核对最新Edit与history，执行Undo/Redo恢复同一内容，再用既有cloth_experiment.py在同一个编辑器管道重放。
  config/metrics/particles/image逐字节相等，provenance和回执匹配新的run_id；重复导出失败保留有效文件，源scene.json不变。
- 工作台新增受一次性标记和互斥参数保护的验收入口，复用既有窗口/资源回收；未改变普通模拟状态机、命令或采样策略。
  CMake、测试选择表与模拟指南同步。

## 验证记录

首轮`out/verify/20261010-141646-b41cf290`编译失败：同一声明中两个CTAD std::array长度不同，MSVC拒绝推导；
拆为独立声明后修复。该轮未执行测试，未使用旧程序作为通过证据。

修复后执行：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_editor_app','dk_ctl','dk_run','dk_image_transfer_probe') -TestRegex '^dk\.(editor\.simulation(_consistency)?_gpu_validation|graphics\.image_transfer_validation|simulation\.response_editor_smoke)$' -Reason 'M12.4 修复夹具数组类型推导编译错误后，执行尚未运行的四项定向验收'
```

`out/verify/20261010-141801-eccb329c`：构建成功，4通过、0失败、0跳过，总测试时间59.56秒：

| 检查 | 实际结果 |
| --- | --- |
| simulation_consistency_gpu_validation | CPU/GPU两轮20个已呈现检查点；精确300拍、过期/当前采样、GUI导出、IPC暂停、Stop和Undo/Redo保护、同窗Python重放通过；20.75秒 |
| simulation_gpu_validation | 真实CPU/GPU实时图像、数值/导出、相机、缩放/最小化、Stop/restart及活动关闭通过；8.36秒 |
| graphics.image_transfer_validation | 跨队列交接、失败保护、生产者先销毁与消费者寿命通过；0.77秒 |
| response_editor_smoke | GPU 8×8、初始化/首批/稳态、pause/cancel/Stop/shutdown共12个进程的定向响应回归通过；29.65秒 |

联合验收产物：`out/build/windows-editor/test-artifacts/RelWithDebInfo/simulation-consistency-ce2dd8e08a524a55a8c5914ac9cea149`，
包含report.json、20个检查点、CPU/GPU导出与重放各一份、日志和最终workbench.ppm/png。
截图确认回到最新编辑态revision=7，Hierarchy保留外部名称；同步validation错误0、liveAllocations=0，只有既有精确AMD Loader提示1条。

响应证据：`out/benchmarks/response-20261010-141822-77e6add9`，summary为partial、failures=[]。
真实GUI活动心跳共961样本，三个阶段p95为7.070/7.114/7.231ms，最大75.742ms；
query 27样本p95=41.932ms、max=85.349ms。控制项每类仅3样本，Stop完成最大254.558ms、完整退出最大297.854ms；
不把这些小样本最大值或统计p95当成完整M11分位数验收。退出政策仍为p95≤400ms、单次≤1000ms。
未重跑完整矩阵、三规模性能基线、独立CPU-only/双配置；本阶段只增加验收宿主路径，未改求解和普通控制算法。

文档检查和git diff --check通过；Roadmap关闭M12.4及父M12，已有结果未重复执行。

## 限制与下一步

联合脚本路径为已交付Python SDK/example；Luau采样和重放的现有证据见M12.3，本次没有更改或重测Luau。
GPU结果只验证当前设备/驱动，同后端确定性文件要求逐字节一致；跨CPU/GPU的容差沿用既有数值验收。
M12范围无剩余实现项，下一项为M13.1实验计划与产物索引。
