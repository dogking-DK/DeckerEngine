---
id: "0086"
created_at: "2026-10-10T14:33:00+08:00"
updated_at: "2026-10-10T14:38:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/editor-interaction.md
  - ../design/physics-api.md
---

# 0086 布料视口持续导航卡顿修复

## 目标与设计依据

排查用户报告的布料模拟期间视角移动卡顿，修复M12.2预览发布链路。
先更新[编辑器](../design/editor.md)、[交互](../design/editor-interaction.md)与[模拟服务](../design/physics-api.md)。
保留用户原有spec/README.md及spec/guides/ai-documentation-workflow.md，不纳入提交。

## 原因与状态契约

worker在绘制期间收到新视角时丢弃完成图；GUI先提交新视角再要求领取图与最新请求完全相等。
持续输入可使每个完成图都被拒绝，窗口循环仍运行但布料画面停住。原smoke只移动一次后松开，未覆盖持续输入。
改为同run发布最近完成图，保留实际矩阵/尺寸/步数和单调视图序号；请求及完成结果各保留一个。
Stop仍优先、旧run不发布、失败保留上一图像；导入和描述符创建成功后提交显示状态，不改Edit/history或求解步数。
维持运行中33ms采样上限与批次公平性，不添加主线程等待或常规readback。

## 验证计划

在已有windows-editor/RelWithDebInfo增加确定性worker回归和真实窗口持续环绕/平移检查，先复现再验证修复。
只运行预览相关单元与simulation_gpu_validation；后者同时覆盖CPU/GPU运行/暂停、恢复、Stop及活动关闭。
不重跑全量、双配置或M11性能矩阵；持续输入只观察发布序号，像素检查放在测量区间外。

## 实际变更与验证记录

复现轮：`out/verify/20261010-143443-8a70db03`构建成功，4项中2通过、2个新增回归点失败，符合预期。
真实CPU窗口连续环绕1003.6ms只发布1次新视角，最长无更新610.0ms；worker新用例在第二视图阻塞时领取不到已完成第一图。
原合并/错误隔离与模拟批次公平性2项通过，说明既有测试没有覆盖完成图的持续发布。

[AsyncSimulation](../../engine/framework/services/src/AsyncSimulation.cpp)不再以最新请求相等作为完成结果发布条件；
[SimulationViewport](../../engine/editor/src/SimulationViewport.cpp)接收同run单调前进的完成图，尺寸/序号/步数取自结果。
新run清空已发布序号及请求缓存；模拟图不再把当前输入相机冒充已完成图的相机，编辑拾取相机仅服务Edit。
[SimulationSmoke](../../engine/editor/src/SimulationSmoke.cpp)在CPU/GPU运行和暂停中各持续环绕/平移1秒，
测量实际发布视图变化次数与最长间隔，要求至少5次且最长小于400ms；该门槛用于发现显示饥饿，不是跨设备帧率承诺。
停止输入后检查最终图像变化和视角收敛，暂停时步数不变，运行时求解仍前进。

修复后执行（复现轮使用相同Target/TestRegex，仅Reason不同）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_simulation_tests','dk_editor_app') -TestRegex '^dk\.(simulation\.finite task preview|editor\.simulation_gpu_validation)' -Reason '修复后台和视口丢弃已完成视角，验证预览回归及CPU/GPU持续导航、暂停、Stop与关闭'
```

`out/verify/20261010-143600-b97db122`：构建成功，4通过、0失败、0跳过，测试16.72秒。
三项worker回归覆盖新视图绘制期间上一图可用且保留真实矩阵/尺寸、最新请求合并、暂停/错误隔离、相机更新不饿死模拟批次。
真实窗口在RTX 4070 Laptop GPU / 596.49驱动上得到以下持续输入观察（每项约1秒）：

| 求解后端/状态 | 环绕发布次数 / 最大间隔 | 平移发布次数 / 最大间隔 |
| --- | --- | --- |
| CPU运行 | 29 / 58.8ms | 29 / 58.5ms |
| GPU运行 | 26 / 43.3ms | 26 / 66.7ms |
| CPU暂停 | 160 / 28.1ms | 162 / 28.6ms |
| GPU暂停 | 135 / 142.4ms | 161 / 34.6ms |

运行期间保留约30Hz生成策略。以上为短时预览发布观察，不是主窗口FPS、输入到显示延迟或完整性能分位数。
每次输入结束后最终视角和像素变化检查通过；相机导航期间运行步数前进、暂停步数不变。
原窗口检查同时通过指标/导出、暂停稳定、缩放/最小化、Edit切换、Stop/restart和活动关闭；源Scene/history未改变。
窗口累计2157帧；Vulkan同步验证errors=0、liveAllocations=0，只有既有已知Loader提示1条。
产物：`out/build/windows-editor/test-artifacts/RelWithDebInfo/editor-simulation-d5661ec61b774eb28ffcdeb9c4aa9f5c`。

文档检查与git diff --check通过。没有新增依赖、命令、CTest入口或改变退出400ms政策。

## 限制与遗留

本次确认并修复持续输入导致完成图被丢弃的卡顿，未改变物理求解或预览采样频率。
真实窗口夹具为8×8布片；没有重跑大规模布片性能基线、全量、双配置或独立CPU-only构建。
本任务无剩余实现项；更大布片的计算吞吐和相机更新频率提升属于后续性能工作。
