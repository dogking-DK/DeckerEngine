---
id: "0089"
created_at: "2026-10-10T16:08:00+08:00"
updated_at: "2026-10-10T16:19:00+08:00"
status: completed
design_refs:
  - ../design/experiment-management.md
---

# 0089 M13.1 实验计划与产物索引

## 目标与设计依据

按[实验管理设计](../design/experiment-management.md)完成计划版本、稳定展开、执行前预算拒绝、
实验身份和产物来源核验。批处理调度/恢复/报告留在后续阶段。
原有 spec/README.md 和 spec/guides/ai-documentation-workflow.md 改动保留，不纳入本次提交。

## 实际变更

- [experiments.py](../../sdk/python/decker/experiments.py)提供 DeckerExperimentPlan v1、完整默认配置、
  float32归一、固定排序笛卡尔展开、显式seed/后端、计数/总步数/工作量预算预检。
  plan_id 和 experiment_id 使用规范化内容SHA256；JSON字段/版本/有限数值/结构额度均严格校验。
- [experiment_index.py](../../sdk/python/decker/experiment_index.py)提供新建索引、登记和完整重新核验。
  嵌入计划及完整配置，保存各次attempt的来源、运行环境、runner摘要、相对产物路径/大小/摘要；
  核对run_id、步数、整数时间、cloth、粒子/约束数量及图像尺寸。缺失/篡改/错配不成为verified。
  单目录排他锁、同目录临时文件和os.replace构成索引提交点；失败保留旧索引和产物。
- [SDK测试](../../sdk/python/tests/test_experiments.py)覆盖稳定身份和关键拒绝/故障路径；
  [真实进程夹具](../../tests/integration/ExperimentIndexTest.py)验证CPU查询数值产物和原GPU export五文件。
  CMake注册三项定向CTest，测试选择表同步。未更改C++服务、命令schema、求解器或M9 Recorder。
- [示例计划](../../examples/automation/cloth-plan.json)和[使用指南](../guides/experiments.md)提供离线准备、
  登记/核验API与格式边界。README/SDK入口、设计/开发索引和Roadmap同步。
  无新增/升级依赖。

## 验证记录

Windows / Python 3.13 / 现有VS2026构建树，使用版本化verify脚本，仅构建dk_run和dk_ctl。

```powershell
$env:TEMP = (Resolve-Path out/test-temp).Path
$env:TMP = $env:TEMP
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Configuration Debug -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.(unit|index_cpu)$' -Reason 'M13.1 定向验证计划身份/预算/索引故障保护及真实 CPU-only 数值证据'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_run','dk_ctl') -TestRegex '^dk\.experiments\.index_gpu$' -Reason 'M13.1 核验真实 GPU export 的配置、运行身份、来源和文件摘要'
```

| 范围 | 证据目录（out/verify/） | 实际结果 |
| --- | --- | --- |
| CPU-only定向SDK及真实runner | 20261010-161514-85a2c4d4 | 2通过、0失败/跳过，4.03秒；其中unit包含13个unittest |
| GPU原生导出索引核验 | 20261010-161724-d1b22186 | 1通过、0失败/跳过，2.56秒 |

unit包含跨Python进程展开一致、顺序/seed/后端/输出身份、float32上下界/归一重复、非法height/floor组合、
超预算在物化笛卡尔积/创建输出前拒绝、错误版本/重复键/非有限数、错误来源/配置/步数、文件缺失/篡改、
索引缓存身份篡改、重复登记、图像截断/尺寸错配、路径拒绝、写入故障与锁冲突下旧索引/产物保护。

两个真实进程用例分别运行两组4×4布片、seed42、迭代4、精确3拍、spacing0.1/0.15；
完整计划配置与引擎run.cloth一致。先故意用错误run_id登记，确认旧索引字节不变，再登记/重新核验成功。
CPU使用无RenderSimulation的windows-scripting配置，保存原始sample/particles查询回执；
GPU使用原simulation.export生成96×72图像及配置/指标/粒子/来源五文件。
GPU同步validation错误0，允许并记录既有AMD Loader警告；没有走77跳过。
这两种配置用于CPU-only与真实GPU边界，不是要求所有测试执行Debug/Release矩阵。

持久验收产物分别位于：

- `out/build/windows-scripting/test-artifacts/Debug/experiment-index/2f3310cc68cc427c88065fba8b9bacc6`
- `out/build/windows-editor/test-artifacts/RelWithDebInfo/experiment-index/d3710ced76ab48a4a5e603deb093c6f1`

各含plan.json、report.json、原始回执、index/index.json与两个独立结果目录。
示例CLI实际执行 `python -m decker.experiments examples/automation/cloth-plan.json --output out/m13-plan-example`
及 `python -m decker.experiment_index out/m13-plan-example`，稳定得到8项/2400拍/6729600工作单元、verified=0。

早期直接unittest因沙箱临时目录的子目录写入/清理权限失败；改用工作区out/test-temp后13项通过。
首轮verify `20261010-161457-e5dd2d36` 在CMake再生成时被沙箱拒绝访问既有vcpkg目录，构建失败且未运行测试。
获准在沙箱外执行相同目标后得到上述有效证据，未更改依赖或关闭模块规避问题。
文档检查和git diff --check通过；最终提交前复核受影响文档与差异。

## 偏差与决策

CPU-only数值目录在本阶段定义和验证；真实进程夹具负责序列化现有查询回执，尚未提供产品批量执行器或自动收集器。
旧单次config v1保持独立且不写plan_id/run_id；批次身份只存在索引。环境未观察到的设备/驱动显式记null。
证据verified只代表文件/身份一致，不代表数值误差合格、可跳过恢复或模拟任务终态。

## 遗留问题与下一步

M13.1范围无剩余实现项；下一项M13.2接串行批量执行、进度与自动数值收集。
未运行全工程测试、M12 GUI、Luau或性能矩阵；本次没有修改其实现。
中断恢复、失败重试、取消/unknown状态、比较报告、数值容差重跑属于M13.2–4，尚未实现/验收。
没有测试断电、恶意文件系统并发、其他平台或GPU驱动，不对此作保证。
