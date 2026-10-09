---
module: command-reference-simulation
created_at: "2026-10-08T17:23:40+08:00"
updated_at: "2026-10-09T12:09:00+08:00"
status: accepted
---

# 模拟世界命令

[返回命令目录](README.md)。Framework/Scene Runtime 提供七条基础命令；启用 RenderSimulation 后另提供 simulation.export 和 xpbd_gpu 后端。
支持默认none固定时钟与显式xpbd_cpu布片求解器；capabilities.simulation.solver="xpbd_cpu"表示已编译CPU求解器，gpu/experiment_export 两个布尔字段表示可选编译能力（不保证当前设备可用）。
实现：[SimulationOperations](../../engine/framework/operations/src/SimulationOperations.cpp)、
[SimulationService](../../engine/framework/services/src/SimulationService.cpp)。
用法见 [模拟指南](../guides/simulation.md)。

## 公共结果和状态

除 particles 分页查询和 export 文件结果外，六条控制/状态命令均返回 `{mode,run}`。mode 为 `edit`、`running`、`paused`；edit 时 run=null，其他状态为：

| run 字段 | 含义 |
| --- | --- |
| run_id | 本轮 SimulationId；非 nil UUID，每次成功 start 生成新值 |
| source | 启动时编辑文档 State：document_id、scene_id、revision、dirty、entity_count；不会随编辑刷新 |
| fixed_dt_ns | 每拍的整数纳秒时长 |
| max_catch_up_steps | 一次自动 pump 的最大步数 |
| steps | none/CPU 的已完成拍数；GPU 的已提交拍数，成功命令已等待完成；等待失败时仍保留提交数 |
| simulated_time_ns | 精确的 steps × fixed_dt_ns |
| accumulator_ns | 小于 fixed_dt_ns 的未满一拍余量 |
| dropped_time_ns | 追赶超限时丢弃的墙钟整拍时间；不算入模拟时间 |
| solver | 本轮 none、xpbd_cpu 或 xpbd_gpu |
| cloth | 任一 XPBD 后端返回完整有效布片配置；none时null |
| metrics | xpbd_cpu时返回本轮已提交数值指标；none/GPU 时 null，GPU 指标在显式导出文件中读取 |
| fault | 正常为 null；自动推进或 GPU 等待/诊断失败时为引擎错误名称，模式冻结为 paused，需要 stop/start |

计数和纳秒时间为整数，时间不超过 int64；布片/指标/粒子为浮点数。未知字段及浮点整数 token（如 count=1.0）拒绝。
所有控制 effect=control、undoable=false；不进入场景撤销历史，不允许加入 scene.transaction。
所有成功模拟操作都不修改编辑文档、revision、dirty 或历史；只有 export 写实验文件。Play 期间仍可编辑/保存/new/load，
Play 保留启动输入；Stop 保留最新 Edit，不把 Play 写回。
`scene.query`、`entity.get`、`read_scene`、编辑器视口和 `render.capture` 始终使用 Edit。

## simulation.query

参数 `{}`，effect=query、undoable=false。无需活动文档、guard 或 run_id；返回公共状态。
与其他 Runtime 命令一样，分派前会 pump：running 下查询可能观察到新拍数；paused 下不会自动推进。

```json
{"jsonrpc":"2.0","id":1,"method":"simulation.query"}
```

## simulation.start

要求已打开编辑场景且没有活动 Play。完整校验/克隆成功后发布独立运行世界，返回公共状态。

| 参数 | 必填 | 默认/范围 |
| --- | --- | --- |
| guard | 是 | 当前编辑 document_id + revision；batch --auto-guard 可补齐 |
| fixed_dt_ns | 否 | 16666667；1000000–1000000000，即 1 ms–1 s |
| max_catch_up_steps | 否 | 8；1–64，GPU 为 1–8 |
| paused | 否 | false；true 原子进入暂停态，适合严格步数实验 |
| solver | 否 | none / xpbd_cpu；graphics 构建另有 xpbd_gpu，默认 none |
| cloth | 否 | 仅 XPBD 后端允许；省略或空对象使用下文默认配置 |

无活动场景或已有 Play 返回 invalid_state；过期 guard 返回 conflict；非法参数返回 invalid_argument。
失败不发布 Play，不改变 Edit。克隆复制内存实体/层级/Project 映射，不固定外部资产文件字节。
CPU/GPU XPBD 要求 fixed_dt_ns<=33333333；cloth配置语义错误为invalid_argument。生成的粒子独立于Scene实体。

```json
{"jsonrpc":"2.0","id":2,"method":"simulation.start","params":{"guard":{"document_id":"<document_id>","revision":0},"fixed_dt_ns":10000000,"paused":true}}
```

## simulation.pause / simulation.resume

必填 `run_id:UUID`，无默认值。pause 将 running 变 paused，清除不足一拍余量；已暂停为 no-op。
resume 从当前墙钟重新计时，暂停时间不追赶；已 running 为 no-op，不重设时钟。
返回公共状态。pause 响应后的空闲时间不会增加步数，但 pause 分派前仍会正常 pump。
无 Play 为 invalid_state；过期 run_id 为 conflict；nil/非法 UUID 为 invalid_argument；
有 fault 时 resume 为 invalid_state，pause 仍可返回当前状态。

```json
{"jsonrpc":"2.0","id":3,"method":"simulation.pause","params":{"run_id":"<run_id>"}}
{"jsonrpc":"2.0","id":4,"method":"simulation.resume","params":{"run_id":"<run_id>"}}
```

## simulation.step

必填 `run_id:UUID`；可选 `count:integer`，默认1，范围1–10000。
仅健康 paused 状态允许；精确完成 count 拍后仍为 paused，返回公共状态。
GPU 每次 count 为 1–8（超限 invalid_argument），脚本分批到精确 N；count 不受 max_catch_up_steps 的当前取值限制。
xpbd_cpu另有20000000工作单元上限：count*(粒子数+iterations*(粒子数+约束数))，超限invalid_argument，需显式分多次请求。
CPU 一次 step 中任一拍数值失效会保留整个调用前的粒子、指标和时钟。GPU 提交前错误保留旧状态；提交后等待/设备失败会冻结 fault 并保留已提交步数，不能回滚。GPU 不默认回读数值，显式 particles/export 才检查有限值。
无 Play、running、有 fault 或模拟时间溢出为 invalid_state；过期 ID 为 conflict；
非法 UUID/count 为 invalid_argument。参数拒绝不增加计数。

```json
{"jsonrpc":"2.0","id":5,"method":"simulation.step","params":{"run_id":"<run_id>","count":100}}
```

## simulation.stop

必填 `run_id:UUID`；从 running/paused（含 fault）丢弃 Play，返回 `{mode:"edit",run:null}`。
无 Play 为 invalid_state；过期 ID 为 conflict；nil/非法 UUID 为 invalid_argument。
GPU Stop 会排空队列并释放资源。不保存、恢复旧编辑快照或取消既有截图任务。再次 start 会使用当前 Edit 和新 run_id。

```json
{"jsonrpc":"2.0","id":6,"method":"simulation.stop","params":{"run_id":"<run_id>"}}
```

示例中的身份须替换为实际响应值；精确步数从 paused=true 开始，不能以先运行后暂停替代。
配置和运行计数不写场景文件，scene.save/project.save 不是检查点；进程结束丢弃 Play。


## XPBD cloth 配置和指标

cloth所有字段可省略，额外字段拒绝；配置只在start接受。数值以float32保存，结果会反映舍入后的有效值；输入上下限按 float32 表示，允许导出配置在边界值直接重放。

| 字段 | 默认 | 范围/单位 |
| --- | --- | --- |
| columns / rows | 8 / 8 | 整数2–32；粒子索引=row*columns+column，row=0固定 |
| seed | 1 | uint32；非固定点初始高度扰动的确定性种子 |
| spacing | 0.15 | 0.01–1 m |
| height | 0.75 | -100–100 m，必须>floor_y |
| particle_mass | 0.1 | 0.001–100 kg；固定点逆质量为0 |
| compliance | 0.000001 | 0–0.01 m/N；0为硬距离约束 |
| iterations | 12 | 整数1–32，每拍迭代轮数 |
| gravity_y | -9.81 | -1000–1000 m/s² |
| floor_y | 0 | -10000–10000 m，无摩擦/反弹水平地面 |
| damping | 0.5 | 0–100 /s，全局速度阻尼 |

metrics字段：particle_count、constraint_count、color_count；max_constraint_error、rms_constraint_error、
min_height、max_penetration、max_pin_displacement单位m；max_relative_error为相对原长的最大绝对误差；
max_speed单位m/s；kinetic_energy、gravity_potential_energy、compliant_energy单位J。
动能/重力势能仅计动态粒子，地面为零势能；compliant_energy只计非零compliance约束的弹性能。
固定点、碰撞投影和阻尼不保证能量守恒，无自碰撞、弯曲或撕裂；详见 [XPBD设计](../design/physics-xpbd.md)。

```json
{"jsonrpc":"2.0","id":7,"method":"simulation.start","params":{"guard":{"document_id":"<document_id>","revision":0},"solver":"xpbd_cpu","paused":true,"fixed_dt_ns":10000000,"cloth":{"seed":42}}}
```

## simulation.particles

参数必填run_id，offset可选默认0、范围0–1024且不能超过当前粒子数；limit默认128、范围1–256。
effect=query、undoable=false，无EditGuard；要求当前 run_id 对应 CPU/GPU XPBD 世界，none/无Play为invalid_state，
旧ID为conflict，nil/非法ID、越界分页为invalid_argument。可读取fault后保留的最后完整状态。
返回run_id、steps、simulated_time_ns、offset、total、has_more和particles数组；每项为
`{index,position:[x,y,z],velocity:[x,y,z],inverse_mass}`。offset=total返回空页。
运行中每页可能属于不同step；须核对版本或先pause再分页，不将跨拍数组拼作一个快照。
只读命令无文件输出；调用者可保存JSON用于分析，保存场景不会保存这些运行态数组。

```json
{"jsonrpc":"2.0","id":8,"method":"simulation.particles","params":{"run_id":"<run_id>","offset":0,"limit":128}}
```

## GPU 粒子查询

simulation.particles 在 GPU 运行中要求 paused 且无 fault；每次是显式同步回读，不增加拍数。
发现数值异常返回 invalid_state 并记录 fault，后续恢复/步进拒绝直到 Stop。query 不触发回读，metrics 为 null。

## simulation.export

仅 RenderSimulation 构建注册；effect=external、undoable=false，不允许 scene.transaction；Luau 仅 project 权限可用。
要求 paused、无 fault、有布片。使用 run_id 和 expected_steps 固定导出版本，不需要新的 EditGuard；目标相对启动时项目。

| 参数 | 必填 | 默认/范围 |
| --- | --- | --- |
| run_id | 是 | 当前非 nil SimulationId |
| expected_steps | 是 | 0..int64_max，必须等于当前拍数 |
| output | 是 | 1–1024 UTF-8 字节，新目录，项目相对路径；父目录必须存在 |
| width / height | 否 | 640 / 480；1–2048 |

返回 `{output,steps,simulated_time_ns,files}`；output 是规范项目相对目录，files 固定包含
config.json、metrics.json、particles.json、image.ppm、provenance.json。
CPU/GPU 导出同样格式，CPU 图像也需要 Vulkan；零步 Graph 使用当前粒子，导出不推进模拟。
config 保存完整参数、seed、N、dt 和固定视图，metrics 保存同版本拍数/整数时间/全部 XPBD 指标；
particles 保存所有位置/速度/逆质量；provenance 保存来源 document/scene/revision 与本轮 run_id。
PPM 为 RGB8；v1 视图固定适配默认布片，极端尺寸/高度可能离开画面。配置用于从零重放，不是检查点。

已有目标（包括空目录）或 expected_steps/旧 run_id 不匹配为 conflict；running/无布片/fault 为 invalid_state；
非法尺寸、绝对/越界路径、链接/重解析点、.decker 路径为 invalid_argument；父目录缺失或写入/发布失败为 io_error。
先准备完整数据，再写同父临时目录，最后 rename 发布；普通失败不留下目标半成品，不覆盖已有文件。
不承诺断电持久性或恶意并发路径替换隔离。GPU 数值/设备故障可冻结 fault，但不改变 Edit 或步数。

```json
{"jsonrpc":"2.0","id":8,"method":"simulation.export","params":{"run_id":"<run_id>","expected_steps":300,"output":"cloth-300","width":640,"height":480}}
```
