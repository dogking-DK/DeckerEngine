# 模拟世界与固定步长

M10.1 提供独立 PlayWorld、运行控制和固定纳秒时钟。当前没有物理求解器，
步数表示调度拍数，场景内容保持启动输入；CPU 求解与模拟可视化分别在后续阶段接入。
完整字段见 [模拟命令](../commands/simulation.md)，内部约定见 [Physics API](../design/physics-api.md)。

## 严格执行 N 步

Luau 示例从暂停态启动，严格推进100拍（每拍10 ms），检查时间为1 s，再 Stop：

```powershell
cmake --preset windows-scripting
cmake --build out/build/windows-scripting --config Debug --target dk_run
New-Item -ItemType Directory -Force out/simulation-demo | Out-Null
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root out/simulation-demo --script examples/scripting/fixed-step.luau --script-access edit
```

[完整示例](../../examples/scripting/fixed-step.luau) 成功时不输出 stdout、不保存文件。
Luau query 权限可调用 simulation.query；edit/project 可调用六条模拟命令。
一条 step(count=N) 消耗一条脚本命令预算，N 最多10000。

外部 Python/IPC 在已有活动场景上使用同样流程（Client 启动见 [Python 指南](python.md)）：

```python
from decker import guard

edit = client.call("scene.query").value["state"]
run = client.call("simulation.start", {
    "guard": guard(edit), "paused": True, "fixed_dt_ns": 10_000_000,
}).value["run"]
result = client.call("simulation.step", {"run_id": run["run_id"], "count": 100}).value
assert result["run"]["steps"] == 100
assert result["run"]["simulated_time_ns"] == 1_000_000_000
client.call("simulation.stop", {"run_id": run["run_id"]})
```

## 实时运行

start 默认 running；resume 恢复暂停的同一轮模拟。Windows runner 的 stdio/pipe 即使无输入也会
按下一拍唤醒；编辑器沿用逐帧 pump。自动推进每次最多8拍（start 可设1–64），超载整拍计入
dropped_time_ns，不无限追赶。暂停清除不足一拍余量，恢复不补算暂停时长。
实时运行的拍数受宿主调度影响，精确实验使用 paused=true + step。
batch/Luau 仅在命令安全点 pump，长脚本循环不在后台模拟；非 Windows stdio 仍同步等待输入。

每轮运行都有独立 run_id；控制必须使用当前 ID，旧 ID 拒绝。simulation.query 返回源文档、
步数、精确时间、余量和丢弃时间；fault 非 null 时停止推进，需要 stop/start。
运行态与调用线程同属 Runtime owner，不存在额外模拟线程。

## 编辑与运行

Play 从启动时的内存场景完整克隆，包括未保存编辑、实体身份、TRS 和层级。
后续编辑/撤销/保存/new/load 不改变这份 Play；Stop 只销毁 Play，保留当前编辑状态与历史。
原来的场景查询、保存、视口和截图始终使用 Edit。C++ 宿主可用
Runtime::read_play_scene(run_id) 取得拥有型只读 Play 快照，即使 Stop 后该副本仍有效。

场景保存不含运行配置、步数或检查点，Stop/shutdown 不自动保存。
M9 的记录重放白名单尚未接入模拟命令；实验配置、指标与图像的重放在 M10.4 接入。
