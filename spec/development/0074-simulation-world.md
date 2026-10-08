---
id: "0074"
created_at: "2026-10-08T17:14:07+08:00"
updated_at: "2026-10-08T17:33:18+08:00"
status: completed
design_refs:
  - ../design/physics-api.md
  - ../design/runtime.md
  - ../design/application-services.md
  - ../design/scripting-luau.md
  - ../design/architecture.md
---

# 0074 M10.1 模拟世界与固定步长

## 目标与设计依据

依据 [Physics API](../design/physics-api.md) 建立独立 Edit/Play、固定纳秒调度、
启动/暂停/恢复/单步/停止和统一命令入口。当前无动力学求解器，数值计算留到 M10.2。
设计先于实现落盘，阶段状态见 [Roadmap](../roadmap.md)。

## 实际变更

- [Physics API](../../engine/physics/api) 新增仅依赖 Core 的 FixedStepClock，整数时间/严格步数、
  有界追赶/丢弃时间与溢出前校验；Framework/Scene 开启时自动装配，无新增依赖 feature。
- [SimulationService](../../engine/framework/services/include/dk/services/SimulationService.hpp) 拥有独立
  Project/SceneDocument，保留来源文档状态；启动构造完成后发布并采样计时基准。暂停清余量、
  恢复不追赶暂停时间；控制检查 SimulationId，旧 run_id 不会作用新运行；异常计时间隔冻结并保留 fault。
- [Operations](../../engine/framework/operations/src/SimulationOperations.cpp) 注册六条模拟命令，
  start 检查 EditGuard，后续控制检查 run_id，均不可撤销/不可加入 scene.transaction；
  capabilities.simulation 明确 fixed_step=true、solver=none。命令发现为CPU39条、截图配置40条。
- [Runtime](../../engine/framework/runtime/src/Runtime.cpp) 提供只读 Play 快照、owner pump 和
  next_pump_deadline；Windows stdio/pipe 和统一 jobs.wait 按下一拍唤醒，shutdown 丢弃 Play。
  CPU-only 的 jobs.wait 也改为 Runtime 路由，复用原资产结果 schema，直接 AssetOperations 使用方式保留。
- [Luau](../../engine/scripting/luau/src/Luau.cpp) query 可查询模拟，edit/project 可控制；
  [示例](../../examples/scripting/fixed-step.luau) 精确执行100拍。Python 使用原 Client.call，未修改 SDK/重放格式。
- 新增 [单元](../../tests/unit/SimulationTests.cpp) 与 [真实进程](../../tests/integration/SimulationTest.py)，
  同步测试选择表、设计索引、[命令参考](../commands/simulation.md)、[指南](../guides/simulation.md) 和 Roadmap。

## 验证记录

Windows/MSVC，Debug，使用已有 windows-scripting 与 windows-graphics 配置；两次 cmake --preset
均成功，全部依赖已安装，未升级三方库。所有构建/CTest 通过 scripts/verify.ps1 留存日志：

| 日志目录（out/verify 下） | 范围 | 实际结果 |
| --- | --- | --- |
| 20261008-172328-4a1cfbd4 | 初轮模拟、Luau能力、协议、资产命令、batch/stdio | 构建通过；22项中21通过、1失败、0跳过；旧命令计数断言33与新增后39不符 |
| 20261008-172655-b559c6f1 | 修正计数后资产发现与真实资产stdio | 2通过、0失败、0跳过 |
| 20261008-172806-17789cb6 | 截图开启分支：模拟命令、截图schema/取消关闭/导入失败 | 4通过、0失败、0跳过；无需GPU |
| 20261008-172900-0459c2de | 启动基准/区间溢出保护修订后全部模拟用例及Luau示例 | 9通过、0失败、0跳过 |

对应执行命令（Targets 覆盖所有被选进程夹具）：

```powershell
cmake --preset windows-scripting
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_simulation_tests','dk_luau_tests','dk_protocol_tests','dk_asset_command_tests','dk_run','dk_ctl') -TestRegex '^dk\.(simulation\.|luau\.(Luau simulation |Luau capability )|protocol\.|asset_commands\.|runtime\.(stdio_interactive|batch_))' -Reason 'M10.1 新模拟世界与时钟、脚本能力、Runtime 作业等待及持续输入调度直接回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_asset_command_tests','dk_run') -TestRegex '^dk\.(asset_commands\.asset command discovery|runtime\.assets_stdio$)' -Reason '复验新增六条模拟命令后的能力计数，并验证统一 jobs.wait 的真实资产 stdio 链路'
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_protocol_tests','dk_simulation_tests') -TestRegex '^dk\.(simulation\.simulation commands|protocol\.capture )' -Reason 'Runtime 增加模拟及统一作业等待，需要验证截图开启分支的编译、注册与关闭失败保护；不启动 GPU'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_simulation_tests','dk_luau_tests','dk_run','dk_ctl') -TestRegex '^dk\.(simulation\.|luau\.Luau simulation )' -Reason '启动计时改为克隆提交后采样，并保护注入时间区间溢出；复验世界、命令、脚本及空闲调度'
./scripts/check-spec.ps1
git diff --check
```

最终有效证据覆盖23项CPU检查与4项截图配置检查（其中模拟命令在两配置各验一次）。
真实 stdio/pipe 以 max_catch_up_steps=1 启动，无输入0.5 s后观察超过5拍，
能证伪仅在下一次请求到来时追赶的实现；暂停后状态完全不变，严格123拍得到1230000000 ns。
新 runner 实查六条 commands.list/describe 的 effect/undoable/schema，Luau完整示例也通过。
另按指南实际执行 `dk-run --project-root out/simulation-demo --script examples/scripting/fixed-step.luau --script-access edit`，退出0、stdout/stderr为空。
非空层级/TRS 场景验证 Play 副本、编辑/历史/保存/重载独立、Stop与重启身份；
坏guard/参数/ID/状态、负时间/溢出保护及shutdown通过。
文档检查通过159个 Markdown、1626个本地链接与元数据/索引/测试入口/JSON清单；diff空白检查通过。

## 偏差与决策

实时模式有界追赶并显式统计丢弃的墙钟时间；严格实验使用 paused=true 与 step(N)。
编辑服务从不切换到 Play，从所有权上保证 Stop 不覆盖用户编辑，运行期间允许继续编辑/保存/new/load。
复用 SceneDocument::stage 建立独立初始世界，不为尚未选择的求解器设计虚假接口或空 GPU target。
原CPU资产等待在服务内部阻塞，因此将其路由提升到 Runtime，才能同时驱动模拟；资产结果/错误语义保持不变。
首轮唯一失败是既有全局命令计数过期，同步单元与资产stdio中的断言后定向复验通过。

## 遗留问题与下一步

M10.1 已完成，下一项 M10.2：选择一个 CPU 参考求解器，建立连续数据布局、边界条件和数值指标。
当前只有调度拍数，没有物理动力学、seed消费、GPU模拟/可视化、指标导出、配置持久化或检查点；
M9记录重放白名单不接受模拟控制，M10.4再接实验闭环。场景/截图始终读取Edit。
未运行完整引擎回归、Release、真实GPU/GUI专项、跨平台或性能基准；当前不改变渲染实现，不需要这些检查。
非Windows stdio仍同步等待输入；Luau/batch只在命令安全点推进，长脚本循环无后台模拟线程。
OOM、进程退出和命令结果分配失败沿用宿主边界，不承诺控制已提交后的回滚。

## 修改记录

- 2026-10-08T17:14:07+08:00：创建设计与本阶段记录。
- 2026-10-08T17:32:08+08:00：完成模块、命令/脚本、owner调度、定向验收与文档；M10.1完成，M10进行中。
