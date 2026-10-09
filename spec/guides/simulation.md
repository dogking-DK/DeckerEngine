# 模拟世界与固定步长

支持独立 PlayWorld、固定时钟和可选CPU XPBD布片求解器。start默认none，只推进调度拍数；
显式solver="xpbd_cpu"可推进独立粒子。场景实体保持启动输入。另有独立GPU求解/可视化C++模块和示例，见下文。
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
Luau query 权限可调用 simulation.query/particles；edit/project 可调用全部模拟命令。
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


## CPU XPBD 布片实验

[完整脚本](../../examples/scripting/xpbd-cloth.luau) 从空编辑场景生成独立8×8布片，固定上沿，
设seed=42与10 ms步长，推进300拍并检查距离误差、固定点和地面边界：

```powershell
New-Item -ItemType Directory -Force out/xpbd-demo | Out-Null
./out/build/windows-scripting/bin/Debug/dk-run.exe --project-root out/xpbd-demo --script examples/scripting/xpbd-cloth.luau --script-access edit
```

此示例只做数值验收，不打开渲染窗口，成功时stdout为空。粒子存在连续数组中，Scene entity_count仍为0。
算法、缓冲布局、参数单位、有限迭代误差及边界见 [CPU XPBD设计](../design/physics-xpbd.md)。

外部Python可保留本次运行的有效配置、指标与粒子结果：

```python
import json
from pathlib import Path
from decker import guard

edit = client.call("scene.query").value["state"]
run = client.call("simulation.start", {
    "guard": guard(edit), "solver": "xpbd_cpu", "paused": True,
    "fixed_dt_ns": 10_000_000, "cloth": {"seed": 42},
}).value["run"]
final = client.call("simulation.step", {"run_id": run["run_id"], "count": 300}).value["run"]
page = client.call("simulation.particles", {"run_id": run["run_id"], "limit": 256}).value
assert not page["has_more"] and page["steps"] == final["steps"]
Path("xpbd-result.json").write_text(json.dumps({"run": final, "particles": page}, indent=2), encoding="utf-8")
client.call("simulation.stop", {"run_id": run["run_id"]})
```

较大布片需分页，暂停态下取得稳定快照；位置/速度为float32有效值，指标用double归约。
step工作量超限时显式分批，不自动改变步长或减少请求步数。每批失败保持该批开始前的全部粒子和计数，
更早的成功批次保留。相同输入/seed/步数在同一构建中可重复；跨CPU/GPU使用设计中的数值容差对照，不承诺逐位一致。
结果JSON是分析产物，不是可加载检查点，也尚未接入M9 Recorder。

## GPU XPBD 与布片图像

`windows-graphics` 开启 `DK_BUILD_PHYSICS_GPU` 和 `DK_BUILD_RENDER_SIMULATION`，不影响 CPU-only 预设。
从仓库根运行：

```powershell
cmake --preset windows-graphics
cmake --build out/build/windows-graphics --config Debug --target dk_xpbd_demo
./out/build/windows-graphics/bin/Debug/dk-xpbd-demo.exe out/xpbd-gpu-demo
```

[示例源码](../../examples/simulation/src/main.cpp) 使用 seed=42、8×8布片、dt=10000000 ns，
严格推进300拍（3秒）；输出目录生成 `cloth-initial.ppm` 和 `cloth-300.ppm`。
程序会覆盖输出目录内这两个同名文件；PPM为RGB图像，可用支持PPM的查看器打开。
图中青色棋盘为布片、灰色网格为地面。位置由GPU直接传入vertex shader，示例仅显式读回最终图像。
可视化使用默认正交相机；换网格尺度时同步设置 ClothView.view_projection 和 floor_y。

C++ 调用 `GpuXpbdSolver::create(heap,queue,cpu,shader)` 上传初态，然后 `advance(queue,dt,count)` 推进。
每批 count=0..8 且最多4096计算Pass；count=0适合暂停展示。`ClothRenderer::render` 将推进与绘制放进一次Graph提交。
frame.physics().steps() 为从GPU初始化起成功提交的拍数，wait确认完成；它不是数值健康或GPU完成的隐含证明。
`GpuXpbdOptions.readback` 可显式申请粒子诊断读回；`ClothView.image_readback` 控制图像读回，默认均关闭。
在wait成功前读回会报错；数值发散的诊断读取也会报错，调用者应停止并重建实验。

现有 `simulation.start`/`step` 和 `render.capture` 命令仍分别使用CPU模拟和EditWorld截图；
GPU命令/脚本实验与配置、指标、图像统一导出由M10.4继续，当前没有GPU检查点或编辑器Play视口。
真实设备的容差、同步/失败与寿命验证入口：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_gpu_xpbd_probe -TestRegex '^dk\.xpbd\.gpu_validation$' -Reason 'GPU XPBD数值、同图绘制与资源寿命'
```

输出在 `out/build/windows-graphics/test-artifacts/Debug/gpu-xpbd`：对比JSON、图计划和初态/300拍PPM。
无GPU或验证层不可用时测试返回77跳过；不能把跳过当作验收通过。
接口与限制见 [GPU设计](../design/physics-xpbd-gpu.md)、[绘制设计](../design/render-simulation.md)。
