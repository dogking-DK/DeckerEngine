---
id: "0077"
created_at: "2026-10-09T11:45:28+08:00"
updated_at: "2026-10-09T12:10:00+08:00"
status: completed
design_refs:
  - ../design/physics-api.md
  - ../design/physics-xpbd-gpu.md
  - ../design/render-simulation.md
  - ../design/physics-xpbd.md
  - ../design/scripting-luau.md
  - ../design/automation-python.md
  - ../design/automation-transport.md
  - ../design/runtime.md
  - ../design/architecture.md
---

# 0077 M10.4 脚本化物理实验

## 目标与设计依据

按 [模拟服务设计](../design/physics-api.md) 接入 GPU 控制与统一配置/指标/图像导出。
复用 [GPU XPBD](../design/physics-xpbd-gpu.md) 与 [布片绘制](../design/render-simulation.md)，不新增依赖和检查点。

## 实际变更

- [SimulationService](../../engine/framework/services/include/dk/services/SimulationService.hpp) 与私有 [GpuSimulation](../../engine/framework/services/src/GpuSimulation.cpp) 延迟装配 GPU；start 支持 xpbd_gpu，step 每批1..8拍，自动追赶最多8拍。普通批次等待退休而不回读。提交前失败保持旧时钟，提交后等待失败保留提交步数并冻结 fault；Stop/shutdown 排空资源，保持最新 Edit。
- [SimulationExport](../../engine/framework/services/src/SimulationExport.cpp) 实现 simulation.export：paused、健康、run_id/expected_steps 匹配后用零步 Graph 回读当前粒子与图像，输出完整配置、指标、粒子、PPM 和来源身份。只发布新目录，拒绝已有文件/目录、越界和链接；同父暂存目录写齐后 rename，失败清理临时目录。
- [XpbdSolver::evaluate](../../engine/physics/xpbd/include/dk/physics/Xpbd.hpp) 用原始拓扑和固定点测量外部数组，不修改 CPU 求解器。输入 schema 的 float32 边界可接受完整有效配置原样重放。
- [SimulationOperations](../../engine/framework/operations/src/SimulationOperations.cpp) 同步 schema/effect/发现，Runtime 增加 gpu/experiment_export 编译能力。Luau export 仅 project 可用，CPU-only 保留基础命令且不创建 Vulkan。
- [Python 示例](../../examples/automation/cloth_experiment.py) 和 [Luau 示例](../../examples/scripting/gpu-experiment.luau) 严格分批300拍并导出；Python 支持 config.json 从零重放及后端覆盖。M9 场景 Recorder v1 保持原格式。
- [runner](../../apps/runner/src/main.cpp) 增加 --pipe-timeout-ms（1..60000，默认5000不变），长 GPU 实验显式使用60000；只改变宿主配置，不改变传输协议或票据重试语义。
- 命令参考、能力参考、模拟/IPC指南、设计、Roadmap 和测试入口已同步。无新增/升级三方库；图形验证在原 windows-graphics 中开启已有 Luau 选项。

## 验证记录

Windows / VS2026 / Debug，现有固定 vcpkg baseline。配置：
`cmake --preset windows-graphics -DDK_BUILD_SCRIPTING_LUAU=ON`。
以下均通过 scripts/verify.ps1，目标、筛选和完整日志见各目录 summary.json；未执行全量或 Release。

| 验证范围 | 证据目录（out/verify/） | 结果 |
| --- | --- | --- |
| 图形构建下 CPU数值/服务/脚本回归；targets dk_run、dk_ctl、dk_simulation_tests、dk_xpbd_tests、dk_luau_tests、dk_asset_command_tests | 20261009-115124-c00f7c68 | 17通过，0失败/跳过 |
| 命令数量/发现 | 20261009-115612-f4dca9a5 | asset discovery 1通过；同次 GPU 实验因服务端5秒限制失败，见下文修复 |
| CPU-only 同上核心回归及真实 stdio/pipe；windows-scripting | 20261009-115638-266ca46d | 19通过，0失败/跳过 |
| GPU真实实验与 runner IPC回归；targets dk_run、dk_ctl，筛选 ^dk\.(simulation\.experiment_gpu\|ipc\.runner_client)$ | 20261009-120244-4f59fc2c | 2通过，0失败/跳过 |
| 最终 CPU-only runner IPC回归 | 20261009-120542-4154d838 | 1通过，0失败/跳过 |
| float32 下边界完整配置原样重放，dk_simulation_tests 定向筛选 | 20261009-120742-4fba008e | 1通过，0失败/跳过 |
| 最终设备诊断/无设备跳过分支接入后 GPU实验复验，^dk\.simulation\.experiment_gpu$ | 20261009-120728-ae7f4e30 | 1通过，0失败/跳过，69.37秒 |

[真实实验验收](../../tests/integration/SimulationExperimentTest.py) 包含：0拍导出不推进、300拍与精确3000000000ns、最后不足8拍、暂停等待不增加拍数、粒子尾页、过期身份/超批次/错误尺寸、覆盖拒绝与坏路径、修改 Edit 后 Stop 保持最新内容/历史、Python 重放与 CPU 参照、独立 Luau 进程。
同环境 GPU/Python重放/Luau 的 config/metrics/particles/image 四份文件逐字相同；provenance 的会话身份预期不同。
位置最大分量差 `1.0579824447631836e-05 m`，速度最大分量差 `3.129243850708008e-05 m/s`，
分别小于0.002与0.02容差。约束误差<0.002、穿透和固定点位移均为0。

第一次完整成功产物在 `out/build/windows-graphics/test-artifacts/Debug/simulation-experiments/2d493908ac884bd8981278d2fddd0464`。
人工检查转换后的 image.png，布片下垂、固定上沿与地面接触可见；PPM原始字节保持不变。
Vulkan同步验证开启，无 validation error；只容许并报告已有 AMD switchable API1.3/应用1.4 loader warning。
本机 GPU 测试实际通过，未走77跳过；无设备分支仅限定设备初始化 not_found/not_supported，其他错误仍失败。

早期验证：20261009-115411-6d6f9b6f 因测试夹具向 entity.create 传入不支持的 name 失败，已修正；
20261009-115612-f4dca9a5 的 GPU 首批图编译超过服务端默认5秒，连接关闭，宿主仍继续执行，
已通过显式服务端等待参数修复。后续有效通过结果如上，不将这些失败算作通过。

文档检查 scripts/check-spec.ps1 与 git diff --check 已通过；最终提交前再核对索引和差异。

## 偏差与决策

M9 场景 Recorder v1 不混入运行时身份和跨设备浮点逐字比较；物理实验采用独立版本化配置，从零重放。
GPU每批最多8拍；不承诺多批事务、硬抢占或检查点。v1 固定视图适配默认布片，极端布片尺度可能离开画面。
完整 CPU 图像导出也需要 GPU，CPU-only 的粒子/指标查询仍可独立使用。
文件发布只承诺同进程普通失败保护，不承诺断电持久性或外部恶意并发替换路径隔离。

## 遗留问题与下一步

本阶段及 M10 无未完成验收项。检查点、编辑器Play视口、跨设备逐位一致和发布打包需另立需求。
未执行其他 GPU/平台、Release、全工程回归；未注入新服务的 device-lost/中途磁盘写失败，
底层 GPU 提交/寿命失败保护沿用 [0076](0076-gpu-xpbd.md) 的既有证据，本次以真实命令/导出边界验收为主。
工作区另有 spec/README.md 与 AI 文档流程指南改动，非本阶段变更，保留不纳入提交。

## 修改记录

- 2026-10-09T11:45:28+08:00：创建记录并先更新设计。
- 2026-10-09T12:02:00+08:00：确认 IPC 服务端等待上限导致断连，增加显式配置。
- 2026-10-09T12:10:00+08:00：完成实现、真实 GPU/Python/Luau 与 CPU-only 验收，关闭 M10.4/M10。
