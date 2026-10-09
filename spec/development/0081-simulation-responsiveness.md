---
id: "0081"
created_at: "2026-10-09T15:49:42+08:00"
updated_at: "2026-10-09T19:38:24+08:00"
status: completed
design_refs:
  - ../design/physics-api.md
  - ../design/physics-xpbd-gpu.md
  - ../design/editor.md
  - ../design/foundation-profiling.md
  - ../design/graphics-device.md
  - ../design/graphics-graph.md
  - ../design/graphics-resources.md
  - ../design/runtime.md
  - ../design/automation-transport.md
  - ../design/graphics-shaders.md
---

# 0081 M11.4 响应性集成与性能复测

## 目标与边界

依据[模拟响应目标](../design/physics-api.md#m112-响应性测量与后续验收目标)和
[性能协议](../design/foundation-profiling.md#m114-响应性复测协议)，
完成冷初始化/首次Graph准备的协作取消、真实GUI/IPC响应试验和同一基准复测。
保留M11.2原始基线、旧同步start/step和显式诊断契约；不提前实现M12的Play视口。

## 实际变更

- 有限任务将stop_token传到Device创建检查点、shader/pipeline之间、批次建图和Graph编译。
  取消仅退出未发布候选；以唯一context区分取消和真实错误，已提交工作仍等待completion。
  Graph依赖/裁剪/缓存和XPBD计算公式未变，模拟GPU资源仍由worker同线程创建/销毁。
- editor在设备支持时使用独立第二队列，共享原生设备与VMA寿命；各自提交域独立，最后的GUI设备视图
  在worker join后由主线程释放；不支持第二队列时保留独立设备。XPBD四个shader变体在一个批次中
  复用Slang全局会话，各入口仍有独立局部会话；取消/失败释放所有未发布结果。
- Named Pipe监听使用独立停止事件，关闭立即停止accept，已有连接保留回复排空机会；
  GUI增加安全点pump及1ms有界握手后续处理，呈现清理和模拟worker回收可重叠。
- runtime.shutdown先请求停止并返回受理，worker回收可与窗口关闭重叠，Runtime销毁前仍join。
  GUI确认关闭走同一请求；SimulationService::shutdown保留同步清理接口。后续命令仍拒绝。
- editor增加一次性工程限定的response probe，记录实际SDL事件循环时间、状态和提交/完成数，
  上限120000样本；仅退出时写JSON。记录IPC、Presenter、Viewport、GUI、Window、Model清理时间戳。
  required验证保留全部消息，单列已知AMD隐式层API版本警告；其他warning/error或存活分配失败。
  另提供互斥--no-validation用于独立GUI性能诊断，默认验证策略未变。
- 新benchmark-simulation-response.py使用真实SDK/dk-ctl/Named Pipe控制有限任务：
  runner/editor × CPU/GPU × 8/16/32，覆盖初始化/首批/稳态；
  每组四个独立进程对应pause/cancel/Stop/shutdown，记录受理、实际阶段、安全边界和完整退出。
  每控制项跨阶段至少20次、每阶段至少20个独立进程、稳态每项至少100个热query；
  暂停/取消稳定后再读，Stop检查Edit和历史未变。
  收集错误/超时立即留档，汇总拒绝缺项、重复、错身份、无心跳或超限；smoke只能partial。
  复算先使旧摘要失效，测量期间二进制变化失败；保存源码/binary hash、构建缓存、电源和全部日志。
  原采集错误扩大为每阶段每动作20次，经用户指出后停止全组合矩阵；恢复原定样本下限和覆盖范围，
  保持延迟阈值不变，默认7轮。支持hash引用已完成数据，仅补数量缺口，显式保留人工中断证据。
- 补充真实窗口/runner响应smoke和Python聚合防误报测试，Device取消原生所有权、Graph旧计划保护、
  worker初始化/提交准备取消和shutdown拒绝后续命令回归。定向选择表和复现指南同步。

## 已完成验证

使用scripts/verify.ps1显式指定下表目标/筛选。CPU-only Debug用于隔离验证，
RelWithDebInfo用于真实驱动和性能；没有执行全量或无依据的双配置矩阵。

| 配置、目标与筛选 | 结果与out/verify证据 |
| --- | --- |
| windows-scripting Debug；dk_simulation_tests/dk_run/dk_ctl；`^dk\.simulation\.(finite \|task_cpu$\|response_aggregation$\|stdio$\|pipe$)` | 16通过；新增shutdown断言误用要求dispatch成功的测试helper而失败，修复后该1项通过；161345-bc7bd170、161611-2ec2e8cd（日期均20261009） |
| windows-graphics RelWithDebInfo；dk_device_tests/dk_graph_probe/dk_run/dk_ctl/dk_simulation_tests/dk_simulation_benchmark；Device、finite、benchmark、GPU Graph、task_gpu、response | 38通过；同一测试helper失败1项，漏建device probe导致2项未运行；161008-866bcac6 |
| 同配置；dk_device_probe/dk_simulation_tests/dk_run/dk_ctl；`^dk\.(device\.gpu_\|simulation\.(finite simulation commands\|response_aggregation$))` | 修复/补齐后4/4通过，0失败/跳过；161427-852dda0e |
| windows-graphics；dk_graph_tests/dk_graph_probe/dk_run/dk_ctl；Graph预算失败保护、真实GPU执行/validation、response、task_gpu | 6/6通过；160409-4826430f |
| windows-graphics-profiling RelWithDebInfo；dk_simulation_benchmark；`^dk\.simulation\.benchmark_` | 4/4通过，真实GPU数值与绘制开启required+同步验证；161008-06d6d89b |
| windows-editor RelWithDebInfo；dk_editor_app/dk_simulation_tests/dk_run/dk_ctl；shutdown回归、真实GUI响应smoke | 2/2通过；161555-48529745 |
| 最终editor；dk_editor_app；`^dk\.simulation\.response_editor_smoke$` | 1/1通过，required validation，0 error/0 live allocations，精确AMD已知警告单列；162600-2ac28f96 |
| Graph排序取消最终版本；windows-graphics RelWithDebInfo；dk_graph_tests/dk_graph_probe/dk_simulation_benchmark/dk_run/dk_ctl；`^dk\.(graph\.\|simulation\.(response_aggregation$\|response_runner_smoke$\|benchmark_\|task_gpu$))` | 48/48通过，0失败/跳过；164112-922a7977 |
| 排序取消最终Tracy版本；windows-graphics-profiling；dk_simulation_benchmark；benchmark前缀 | 4/4通过；164111-c731ac30 |
| 独立进程夹具；windows-editor；dk_editor_app；response_aggregation/response_editor_smoke | 2/2通过；164057-cef27c33 |
| 仅采样/聚合工具调整；windows-graphics；dk_run；response_aggregation | 新夹具首次漏建临时子目录，修复后1/1通过（含5个Python测试）；171230-0b427576、171247-d6242e33；未重复引擎/GPU检查 |

早期Graph全前缀检查发现优化构建的验证临时分配失败窗口小于原测试31字节步长，
改为逐字节预算扫描后覆盖失败与成功两端；未弱化断言或更改引擎错误策略。
两个探针漏建在报告中标为未运行，补齐目标后通过，没有将跳过记作成功。
普通沙箱Python临时目录权限失败；以可运行真实桌面/IPC的权限重跑通过，未改测试绕过检查。

## 同基准复测

`python scripts/benchmark-simulation.py --skip-control`已通过，目录
`out/benchmarks/simulation-20261009-164305-264b9ca3`，2026-10-09T16:43:05至16:44:09+08:00。
三规模、三轮、CPU/OFF及GPU OFF/timestamps/Tracy共36次，9份实际capture。
全部严格300拍、有限值/固定点/地面及CPU/GPU容差通过；36份全量位置/速度数组与
M11.2目录`simulation-20261009-145527-f2453c77`对应文件逐项完全一致。
此命令只完成直接API测量，不能替代响应矩阵。具体性能与最终验收状态见[报告](../benchmarks/2026-10-09-simulation-response.md)。

## 原250ms门槛下的验收记录

功能、数值与资源寿命已通过，响应复用195个完成组，仅补45组及热query缺口。
原目录response-20261009-164410-4ecab251的r5-runner-gpu-16-initializing由代理主动终止宿主64520，
因用户要求减少过量测试而产生timeout；保留failed记录及显式排除原因，不归类为引擎自发失败。
续采目录response-20261009-171305-fe6e70f4复核同一二进制hash与验证配置；240组汇总已结束，3项退出p95失败。

完整汇总发现editor/GPU三规模的退出p95分别263.534/250.360/264.289ms，超过250ms，
其他响应分组通过；本次并非因用户要求提速而忽略这些失败。
进一步仅调整退出时窗口隐藏顺序，求解/任务/计时语义不变。
首版将隐藏放到IPC排空前，暴露连接进入下一次等待导致1500ms grace的顺序问题：
172147-f3a6f98c聚合通过、GUI smoke失败，原始response-20261009-172202-c8f5d0e4保留。
改为先排空IPC再隐藏窗口，仅构建editor并复测关闭动作，不重跑其他引擎和求解基准。

首版夹具每进程依次测四个新任务，只有pause属于进程首次冷初始化，后续动作属于暖进程。
检查原始证据后停止`response-20261009-163138-06d37890`并保留failed记录：
主动终止了其当前一次性宿主，产生Win32 109，不能计为引擎自发失败或完整验收。
夹具改为每动作独立进程，汇总拒绝缺少process_per_action标记的旧采集。
此前暖进程的重复任务退出超标仍保留为独立限制，不删除或混入新的冷态统计。

既有spec/README.md和spec/guides/ai-documentation-workflow.md改动保留，不属于本阶段。

## 首轮收尾状态（历史）

隐藏窗口的三阶段smoke通过（172455-082c6437），但63次退出定向复测仍有GPU8 p95超标；
加入单呈现同步槽后GUI smoke通过（172913-a4058949），另一轮63次退出仍未全部达标。
两处试验性GUI改动已撤回，保留此前已验证的核心实现。原始结果与数据版本全部保留在报告中。
仅恢复编辑器构建，不再重复未变化代码的测试。工具增加--actions以便以后只复测受影响控制项；
聚合6个Python用例通过（172441-e277675c），子集仍标partial，样本充分的分组检查p95，缺样/失败不伪装通过。
M11.4和M11保持进行中，未创建宣称该小阶段已验收的提交。下一步只需定位GUI完整退出尾延迟。

## 退出尾延迟定位与收尾

恢复工作后使用CDB仅作退出阶段定位；系统WPR因性能采样策略不可用，未获得ETW证据。
诊断发现NVIDIA Vulkan交换链路径加载opengl32，其进程卸载回调约59ms；并非编辑器误启用了OpenGL。
GUI设备销毁亦包含驱动等待。调试器耗时不计入正式性能数据，没有卸载非自有DLL或绕过析构。
临时文件写入/删除不足1ms，无须改写SDK协议。现有主循环只在帧头pump，GPU等待期间
到达的IPC请求会延后到下一帧；先按editor设计补充获取后/提交等待后的owner pump，
关闭时利用已有Frame放弃契约归还尚未提交图像，随后仅复测直接受影响的GUI路径。

获取后/提交后pump通过真实GUI smoke（183951-5c0590b1），但三次退出226.8/230.4/250.0ms仍接近阈值。
进一步把绘制限制为60Hz，帧间RuntimeEvents等待可立即被IPC唤醒，SDL事件最长8ms再次轮询；
不增加GPU提交来维持命令轮询。同期修复此前暴露的grace关闭竞态：独立accept停止事件只取消监听，
活动连接仍保留回复排空时间。以上先更新设计，定向验证包含空闲grace与关闭回复，以及GUI实际控制。

帧间唤醒及管道回归3/3通过（184713-187b60ea）；GPU8 smoke退出285.5/242.5/259.0ms，
受理缩短至30–33ms，但初始化样本在GUI设备销毁后又等待worker约83ms，不能以smoke通过宣称延迟达标。
最后将正常退出分成渲染完成、UI资源回收与Runtime join、呈现完成/GUI设备销毁三个安全阶段；
UI清理和worker join与呈现等待重叠，避免两设备最终销毁竞争驱动生命周期锁；异常路径保留原完整排空。


仅改变退出顺序仍未达标（185113-cefdf271的GUI安全性通过，三次退出274.6/251.4/263.0ms）。
撤回60Hz限帧，转为解决重复设备成本：编辑器在支持同族第二队列时提供已有设备寿命，
模拟独占第二队列和独立提交域，worker资源仍在自身线程回收。默认runner单设备路径保持。
主线程作用域守卫保证正常/异常退出都先join worker再释放呈现设备/窗口；无第二队列保留旧路径。
这改变首次模拟的设备创建成本，报告将显式区分宿主设备复用与runner独立冷初始化。


共享设备正式退出复测response-20261009-190745-01b710d5仍失败：已完成阶段约220–250ms，
部分initializing样本约310–335ms，Runtime join单独等待约100ms。CDB诊断191251确认
每次slang_createGlobalSession2重复初始化约86–114ms（仅作定位，非正式采样）。
因此将XPBD四变体批量编译，作用域内复用全局会话、每入口保持独立局部会话，并加入阶段间取消检查。
避免取消已到达后仍启动下一次90ms全局初始化；不通过延迟受理、移动计时点或修改阈值规避失败。

shader/共享队列验证10/10通过（191552-10e1271a），Tracy真实GPU验证1/1通过（191754-3a1a1cd2）。
批量编译后退出复测192011-1a4ce607不再出现初始化300ms阶跃，但p95仍超标，全部数据保留。
阶段日志定位两段IPC握手跨帧，以及在worker join后才开始呈现清理的串行成本。
Workspace仅对尚未分派命令的握手作1ms有界事件等待；正常退出先清理呈现、再join，
共享设备仍在join后由主线程最终销毁。只复验这些直接受影响的宿主路径。

## 400ms退出目标与最终验收

2026-10-09用户明确要求将250ms退出延迟放宽至400ms。仅调整完整进程退出p95，
保留1000ms单次上限；Stop、cancel终态、shutdown受理、其他请求和GUI心跳预算不变。
汇总脚本新增边界回归，验证400ms通过、401ms失败、其余门槛不被连带放宽；
193238-c68e5b07的response_aggregation通过（7个Python用例）。旧失败summary保持原样。
最终IPC握手与呈现/worker并行回收版本192800-c256f588定向2/2通过：
workspace IPC刷新保护与真实GUI response smoke。后续只做最终二进制的63次GPU编辑器退出采样，
以及shader批量编译变更后的36次直接API性能/数值复测；不重跑完整响应矩阵。

最终退出目录`response-20261009-193252-f1554c12`：8/16/32各21次，
p95为247.860/246.990/244.053ms，max为251.250/251.844/244.607ms；全部低于400/1000ms，
required validation下0错误、0存活分配，活动心跳max≤8.146ms。子集仍标partial，阈值失败0项。
原960进程完整矩阵按新阈值重新验证全部数据及hash后通过，另存
`response-policy-20261009-400ms`；没有覆盖原250ms失败summary，也未把不同版本样本混算p95。

最后直接API命令为`python scripts/benchmark-simulation.py --skip-control --off-dir out/build/windows-editor`，
目录`simulation-20261009-193508-f36ddf11`，36次/9份capture全部通过。
36份位置/速度数组与M11.2基线逐项一致，对比记录保存在该目录comparison-to-m11.2.json。
GPU初始化OFF中位数由约453–468ms降至156–159ms；稳态求解和首次Graph成本另列，
完整结果见[验收报告](../benchmarks/2026-10-09-simulation-response.md)。

最终链路追加验证（均由verify.ps1限定target/TestRegex执行，日期20261009）：

| 配置与验证范围 | 结果及证据 |
| --- | --- |
| windows-editor；dk_device_tests/dk_simulation_tests/dk_editor_app/dk_ctl；Device共享视图、有限任务、GUI响应 | 33通过；漏建device probe的2项未运行，后续补建；185945-74dee938 |
| windows-scripting Debug；dk_simulation_tests/dk_run/dk_ctl；finite、task_cpu、pipe | 15/15通过，CPU-only保持隔离；190259-f6652fa7 |
| windows-editor；dk_device_probe/dk_simulation_tests/dk_editor_app/dk_ctl；GPU设备、有限任务配置保护、聚合、共享队列任务 | 4通过；GUI夹具scene.new未带guard失败，改为读取已打开场景后单项通过；190431-8b4beb25、190618-28e3a077 |
| windows-editor；dk_shader_tests/dk_editor_app/dk_ctl/dk_simulation_benchmark；shaders、task_editor_gpu | 10/10通过，含批量SPIR-V一致性/失败归零、三规模300拍及控制/导出/暖退出；191552-10e1271a |
| windows-graphics-profiling；dk_simulation_benchmark；benchmark_gpu | 1/1通过，required+同步验证与数值；191754-3a1a1cd2 |
| windows-editor；dk_editor_app/dk_editor_tests/dk_ctl；workspace IPC与response_editor_smoke | 2/2通过；192800-c256f588 |
| windows-editor；dk_editor_app/dk_ctl；response_aggregation | 1/1通过（7个Python用例）；193238-c68e5b07 |

依据完整矩阵、新门槛复核、最终退出采样、直接API数值/性能及共享队列功能回归，M11.4验收完成。
未执行全量或重复双配置矩阵；最终共享设备pause/cancel/Stop不另重测20次p95，
暖进程重复任务的约293ms退出只有单次功能证据，不声明其p95。其他硬件/复杂场景等范围见报告限制。
Roadmap关闭M11.4及M11，后续阶段仍未开始；保留既有用户文档改动不纳入本阶段提交。
文档检查通过：check-spec.ps1限定本次17份Markdown，核验422个本地链接、元数据、表格、
索引、测试入口及JSON manifest；git diff --check通过。
