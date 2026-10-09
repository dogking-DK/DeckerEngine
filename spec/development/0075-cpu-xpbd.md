---
id: "0075"
created_at: "2026-10-09T10:33:53+08:00"
updated_at: "2026-10-09T10:46:49+08:00"
status: completed
design_refs:
  - ../design/physics-xpbd.md
  - ../design/physics-api.md
  - ../design/runtime.md
  - ../design/architecture.md
---

# 0075 M10.2 CPU XPBD 参考求解器

## 目标与设计依据

依据 [XPBD设计](../design/physics-xpbd.md) 实现单一距离约束求解器与固定上沿布片、连续数据、地面边界和数值指标。
保留M10.1默认none时钟模式，显式选择xpbd_cpu；GPU求解和可视化在M10.3继续。
设计先于实现落盘；公式核对 [原作者论文](https://mmacklin.com/xpbd.pdf) Algorithm 1与式(17)(18)，
使用自己的实现和有序分色，不引入第三方求解库。

## 实际变更

- [physics/xpbd](../../engine/physics/xpbd) 提供dk::physics_xpbd，PUBLIC仅Core，无Scene/GPU依赖。
  16-byte位置/逆质量、速度与距离约束记录，独立连续数组；稳定索引、确定性贪心分色与offset表。
- CPU XPBD每拍重置lambda、拍内累计、固定点与质量权重；半隐式预测、速度阻尼、摩擦/反弹均为0的地面投影。
  动态重合约束使用初始单位方向。限制数组、颜色、dt、迭代和同步工作预算，验证数值有限/有界。
- 布片生成器提供columns/rows、间距/高度、质量/柔度、重力/地面/阻尼/迭代与uint32 seed；
  默认64粒子、210结构/剪切约束、8色；首行固定，不创建场景实体。
- 指标包含约束最大/RMS/相对误差、速度/动能/势能/柔性弹性能、地面穿透/固定点误差及数量，double归约。
  求解器在候选数组执行整批N步，普通失败保持原数组/指标；服务在求解成功后提交候选时钟，保护Edit/Play。
- [SimulationOperations](../../engine/framework/operations/src/SimulationOperations.cpp) 扩展start和run结果，
  新增simulation.particles分页；每页带run_id/steps/时间。Runtime公开read_play_particles拥有型快照。
  capabilities显示xpbd_cpu可用，start默认none兼容旧计时用例。40条CPU命令、截图配置41条。
- Luau query可查询粒子，edit/project可运行完整[布片示例](../../examples/scripting/xpbd-cloth.luau)。
  Python沿用Client.call，无SDK格式变化；同步[命令参考](../commands/simulation.md)、[指南](../guides/simulation.md)、测试入口和Roadmap。

## 验证记录

Windows/MSVC Debug，复用windows-scripting，依赖均已安装且版本未变。

```powershell
cmake --preset windows-scripting
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_xpbd_tests','dk_simulation_tests','dk_luau_tests','dk_asset_command_tests','dk_run','dk_ctl') -TestRegex '^dk\.(xpbd\.|simulation\.|luau\.(Luau XPBD |Luau simulation )|asset_commands\.asset command discovery|runtime\.assets_stdio$)' -Reason 'M10.2 CPU求解解析解、布片/边界/回滚、时钟耦合与命令/脚本/真实进程验证'
New-Item -ItemType Directory -Force out/xpbd-demo | Out-Null
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root out/xpbd-demo --script examples/scripting/xpbd-cloth.luau --script-access edit
./scripts/check-spec.ps1
git diff --check
```

构建通过，20项检查全部通过、0失败、0跳过，日志 `out/verify/20261009-104157-17fd8e00`。
6项XPBD数值/拓扑/回滚测试、8项时钟/服务/命令测试、2项Luau示例/能力、
2项真实stdio/pipe、1项资产命令发现和1项资产stdio回归。构建日志无编译warning/error。
真实runner运行指南布片脚本退出0、无stdout/stderr。新命令及start/run变更用实际注册表schema验证；
float32最小spacing返回值也通过结果schema，避免有效输入在输出舍入后被误判。

验证包括离散自由落体解析解、单边柔度与质量权重解析解、迭代lambda累计、硬约束固定点、
地面切向速度/无穿透/无向下速度、动态重合恢复；错误拓扑/容量/数值/预算、
前一拍成功但批内后续越界的整批回滚；相同seed与300拍/3×100拍一致。
真实stdio/pipe重复相同seed/N得到相同粒子与指标，分页版本一致，暂停查询不推进，Stop保留编辑态。

seed=42、默认8×8、dt=10000000 ns、300拍（3s）的本机实测：

| 指标 | 值 |
| --- | --- |
| 粒子 / 约束 / 色 | 64 / 210 / 8 |
| 最大 / RMS距离误差 | 0.000205846375863 m / 0.000055712208246 m |
| 最大相对约束误差 | 0.00111173980373，低于设计0.08上限 |
| 最大穿透 / 固定点误差 | 0 m / 0 m |
| 最低高度 | 0 m，已接触地面 |
| 最大速度 | 0.0531196112748 m/s |
| 动能 / 重力势能 / 柔性弹性能 | 0.00361261556112 / 12.5010250494 / 0.325904266322 J |

完整数组/有效配置/指标由进程夹具输出到
`out/build/windows-scripting/test-artifacts/Debug/simulation/pipe-1f651f5a2d044e34ae87999d95db4bef/xpbd-report.json`，
另有stdio产物；这些是本次运行证据，不是跨平台逐位golden或检查点。
单元测试从原始数组独立复算RMS/最大约束误差和动能，与服务指标一致。
文档检查通过161个Markdown、1652本地链接及元数据/索引/测试入口/JSON清单，diff空白检查通过。

## 偏差与决策

选择有序分色XPBD距离约束，CPU float32数据为后续GPU对照做准备，指标double归约。
这是结构/剪切弹性网络，不声称完整布料材料模型；未实现弯曲、自碰撞或断裂。
直接在拥有型暂存数组推进整批，成功才发布；小规模CPU参考接受复制开销，未虚构性能结论。
保留默认none时钟模式，xpbd_cpu显式选择；不把现有场景查询/截图切换到粒子世界。
整个阶段未出现构建/CTest失败，没有通过关闭模块或放宽既定数值阈值规避失败。

## 遗留问题与下一步

M10.2已验收，下一项M10.3：同一XPBD步骤接入GPU Graph，设计粒子缓冲输出/独立渲染Pass与CPU/GPU容差。
未运行全量引擎回归、Release、真实GPU/GUI专项或跨平台/性能矩阵；当前没有GPU/渲染实现变更。
未提供模拟配置持久化、检查点、资产网格变布片、M9 Recorder扩展；实验记录/图像闭环留M10.4。
固定dt限制1–33.333333ms，同步请求工作量有限，调用者可分批；数值失败保持原状态，OOM/进程退出仍沿用宿主边界。

## 修改记录

- 2026-10-09T10:33:53+08:00：建立求解器设计、更新运行契约并创建记录。
- 2026-10-09T10:46:49+08:00：完成CPU实现、服务/命令/脚本与20项定向验收；M10.2完成，M10继续进行中。
