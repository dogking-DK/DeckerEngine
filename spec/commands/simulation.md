---
module: command-reference-simulation
created_at: "2026-10-08T17:23:40+08:00"
updated_at: "2026-10-08T17:23:40+08:00"
status: accepted
---

# 模拟世界命令

[返回命令目录](README.md)。Framework/Scene Runtime 均提供这六条命令，无 GPU 前置条件。
M10.1 仅提供独立 PlayWorld 和固定时钟，当前无动力学求解器（capabilities.simulation.solver="none"）。
实现：[SimulationOperations](../../engine/framework/operations/src/SimulationOperations.cpp)、
[SimulationService](../../engine/framework/services/src/SimulationService.cpp)。
用法见 [模拟指南](../guides/simulation.md)。

## 公共结果和状态

六条命令均返回 `{mode,run}`。mode 为 `edit`、`running`、`paused`；edit 时 run=null，其他状态为：

| run 字段 | 含义 |
| --- | --- |
| run_id | 本轮 SimulationId；非 nil UUID，每次成功 start 生成新值 |
| source | 启动时编辑文档 State：document_id、scene_id、revision、dirty、entity_count；不会随编辑刷新 |
| fixed_dt_ns | 每拍的整数纳秒时长 |
| max_catch_up_steps | 一次自动 pump 的最大步数 |
| steps | 已完成固定拍数；当前每拍只推进时钟，不计算动力学 |
| simulated_time_ns | 精确的 steps × fixed_dt_ns |
| accumulator_ns | 小于 fixed_dt_ns 的未满一拍余量 |
| dropped_time_ns | 追赶超限时丢弃的墙钟整拍时间；不算入模拟时间 |
| fault | 正常为 null；自动推进失败时为引擎错误名称，模式冻结为 paused，需要 stop/start |

所有数值为整数，时间不超过 int64；未知字段及浮点整数 token（如 count=1.0）拒绝。
所有控制 effect=control、undoable=false；不进入场景撤销历史，不允许加入 scene.transaction。
所有成功操作都不修改编辑文档、revision、dirty、历史或文件。Play 期间仍可编辑/保存/new/load，
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
| max_catch_up_steps | 否 | 8；1–64 |
| paused | 否 | false；true 原子进入暂停态，适合严格步数实验 |

无活动场景或已有 Play 返回 invalid_state；过期 guard 返回 conflict；非法参数返回 invalid_argument。
失败不发布 Play，不改变 Edit。克隆复制内存实体/层级/Project 映射，不固定外部资产文件字节。

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
count 不受 max_catch_up_steps 限制；后者只限制实时 pump。
无 Play、running、有 fault 或模拟时间溢出为 invalid_state；过期 ID 为 conflict；
非法 UUID/count 为 invalid_argument。失败不增加计数。

```json
{"jsonrpc":"2.0","id":5,"method":"simulation.step","params":{"run_id":"<run_id>","count":100}}
```

## simulation.stop

必填 `run_id:UUID`；从 running/paused（含 fault）丢弃 Play，返回 `{mode:"edit",run:null}`。
无 Play 为 invalid_state；过期 ID 为 conflict；nil/非法 UUID 为 invalid_argument。
不保存、恢复旧编辑快照或取消既有截图任务。再次 start 会使用当前 Edit 和新 run_id。

```json
{"jsonrpc":"2.0","id":6,"method":"simulation.stop","params":{"run_id":"<run_id>"}}
```

示例中的身份须替换为实际响应值；精确步数从 paused=true 开始，不能以先运行后暂停替代。
配置和运行计数不写场景文件，scene.save/project.save 不是检查点；进程结束丢弃 Play。
