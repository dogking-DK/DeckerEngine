# 模拟世界与固定步长

支持独立 PlayWorld、固定时钟和可选CPU XPBD布片求解器。start默认none，只推进调度拍数；
显式solver="xpbd_cpu"可推进独立粒子。场景实体保持启动输入。可选 GPU 后端与 CPU 使用同一组命令，支持统一实验导出，见下文。
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
Luau query 权限可调用 simulation.query/particles；edit/project 可调用模拟控制；文件导出仅 project。
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
M9 的记录重放白名单尚未接入模拟命令；物理实验使用独立版本化 config.json 与下述 Python 示例重放。


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

`simulation.start(solver="xpbd_gpu")` 和 step 已接入 GPU；`render.capture` 仍只截图 EditWorld。
物理实验用 simulation.export；当前没有 GPU 检查点或编辑器 Play 视口。
真实设备的容差、同步/失败与寿命验证入口：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_gpu_xpbd_probe -TestRegex '^dk\.xpbd\.gpu_validation$' -Reason 'GPU XPBD数值、同图绘制与资源寿命'
```

输出在 `out/build/windows-graphics/test-artifacts/Debug/gpu-xpbd`：对比JSON、图计划和初态/300拍PPM。
无GPU或验证层不可用时测试返回77跳过；不能把跳过当作验收通过。
接口与限制见 [GPU设计](../design/physics-xpbd-gpu.md)、[绘制设计](../design/render-simulation.md)。


## Python / Luau 完整实验与重放

构建带 Luau 的图形 runner（复用固定依赖，无窗口）：

```powershell
cmake --preset windows-graphics -DDK_BUILD_SCRIPTING_LUAU=ON
cmake --build out/build/windows-graphics --config Debug --target dk_run dk_ctl
New-Item -ItemType Directory -Force out/cloth-experiment | Out-Null
./out/build/windows-graphics/bin/Debug/dk-run.exe --project-root out/cloth-experiment --script examples/scripting/gpu-experiment.luau --script-timeout-ms 120000
```

[Luau 示例](../../examples/scripting/gpu-experiment.luau) 从 paused 启动，每批最多8拍，最后4拍到300；
导出 `out/cloth-experiment/luau-experiment` 后 Stop。重复执行需换新项目目录或自行归档旧产物，不覆盖旧目录。
默认 VM 的5秒预算不足以涵盖冷编译，示例显式配置120秒；原生命令协作取消，不承诺中途硬抢占 GPU 等待。

Python 在另一个终端连接专用宿主。先创建目录，在终端 A 启动：

```powershell
New-Item -ItemType Directory -Force out/cloth-python | Out-Null
./out/build/windows-graphics/bin/Debug/dk-run.exe --project-root out/cloth-python --pipe cloth-experiment --pipe-timeout-ms 60000
```

终端 B 从仓库根运行 [Python 示例](../../examples/automation/cloth_experiment.py)：

```powershell
$env:PYTHONPATH = "$PWD/sdk/python"
python examples/automation/cloth_experiment.py --pipe cloth-experiment --ctl out/build/windows-graphics/bin/Debug/dk-ctl.exe --output gpu --seed 42 --steps 300
python examples/automation/cloth_experiment.py --pipe cloth-experiment --ctl out/build/windows-graphics/bin/Debug/dk-ctl.exe --config out/cloth-python/gpu/config.json --output repeat
python examples/automation/cloth_experiment.py --pipe cloth-experiment --ctl out/build/windows-graphics/bin/Debug/dk-ctl.exe --config out/cloth-python/gpu/config.json --solver xpbd_cpu --output cpu
./out/build/windows-graphics/bin/Debug/dk-ctl.exe --pipe cloth-experiment --method runtime.shutdown
```

客户端等待60秒，宿主也显式设置60秒；只增加客户端等待不能改变服务端默认5秒限制。
GPU 冷编译耗时较长，同步步进期间 owner 不处理其他命令。示例要求独占控制，失败时保留活动运行供查询，
不自动重试不确定执行结果或 Stop；可用原 ticket 查询已执行结果，参见 [IPC 指南](ipc.md)。
Python --config 从零重放（0..10000拍），--solver 可切换参照后端；--seed/--steps 仅在无 --config 时使用。
当前固定视图适配默认布片，极端网格/高度可能超出画面；v1 不允许更换相机矩阵。

每个实验目录有五个文件：

| 文件 | 内容 |
| --- | --- |
| config.json | DeckerSimulationExperiment v1、solver、完整 cloth/seed、N、fixed_dt_ns、固定视图和像素尺寸 |
| metrics.json | 同版本 steps、simulated_time_ns 及全部数值指标 |
| particles.json | 按索引排列的全部 position、velocity、inverse_mass |
| image.ppm | 当前布片的 RGB8 图像 |
| provenance.json | 启动文档/场景/revision、本轮 run_id 和主要产物文件名 |

GPU 常规 step 不回读粒子；query.metrics 为 null。particles 和 export 才显式等待并读取，后者用同一零步 Graph
读取当前粒子和图像；CPU 导出图像会上传当前 CPU 数组，因此完整图像导出也需要 Vulkan。
相同构建/设备/输入重放时 config、metrics、particles、image 可逐字比较；provenance 的会话身份预期不同。
CPU/GPU 采用数值容差：默认300拍位置最大分量差<0.002 m、速度<0.02 m/s；不承诺跨设备逐位一致。
这些产物不是检查点；没有中途恢复/撤销导出。

定向验收（需上述 Luau 配置）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.simulation\.experiment_gpu$' -Reason '严格300拍、导出重放、CPU/GPU容差及失败保护'
```

产物保留在 `out/build/windows-graphics/test-artifacts/Debug/simulation-experiments/<独立目录>`。

验收测试在设备初始化明确返回不可用/不支持时以77跳过，其余错误按失败处理；跳过不算GPU验收通过。
