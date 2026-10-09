---
module: application-services
created_at: "2026-09-22T13:28:00+08:00"
updated_at: "2026-10-09T10:43:38+08:00"
status: accepted
---

# 场景与资产应用服务

services 提供 dk::scene_services，PUBLIC 依赖 Scene；operations 提供 dk::scene_operations，
PUBLIC 依赖 commands 和 services。framework 与 Scene 同时开启时构建；没有窗口、GPU 或传输依赖。
服务拥有捕获的工程根、只读 Project 和唯一 SceneDocument；不暴露可变文档。
操作层负责 JSON 与强类型转换、schema 和命令注册，服务负责生命周期/状态约束。

## 状态和并发约定

单线程串行调用。DocumentId 是会话 StableId，每次 new/load 生成新值，不写磁盘。
EditGuard 必须同时匹配 DocumentId 和 uint64 revision，防止重新载入旧版本的 ABA。
Core 增加 conflict=7 和稳定名称；不匹配时返回 expected/actual 上下文，不修改状态。
首次 new/load 可省略 guard；替换现有文档必须携带 guard。失败先返回，成功才整体替换。
revision 和 dirty 沿用 Scene 语义；无变化操作不递增。new/load、文件 IO 不参与撤销。

## 命令

- scene.new：可选 name（Untitled）、scene_file（scene.json）、资产清单 assets 和替换 guard。
- scene.load：manifest 相对工程路径和可选替换 guard；完整加载成功才替换。
- scene.query(offset=0,limit=128)：按 EntityId 排序分页，limit 1–256，offset 0–10000，
  返回 state/entities/offset/has_more；防止合法大场景突破单条 JSON 结构上限。
  entity.get(id) 返回单个实体，含 local TRS、parent、资产引用及按行展开的 world_matrix[16]。
- entity.create：guard 和可选显式 id；返回 state 和创建 id。
- entity.delete / set_name / set_transform / set_parent / set_assets：guard、id 和完整新值。
- scene.save(guard)：原子保存场景；project.save(guard,manifest)：单独保存清单。
  两文件不承诺跨文件原子性。保存可能产生外部效果，不能撤销。
  服务拒绝将清单直接写到场景路径（Windows 基本大小写比较）；不是文件系统沙箱或任意别名检测。

统一 state = {document_id, scene_id, revision, dirty, entity_count}。
所有 UUID 必须为规范非 nil 字符串；revision 必须为非负整数且不溢出 uint64。
TRS 为 translation[3]、rotation[x,y,z,w]、scale[3]；所有数值有限，服务/Scene 完成语义校验。
set_assets 先确认注册种类和文件，再执行单次修改。JSON schema 禁止未知字段。
单条与批量编辑复用 Edit variant（创建/删除/改名/变换/父级/资产）和统一内存事务。
服务活得比引用它的 registry 更久；SceneService 本身不启动后台线程，资产 worker 由下述资产服务拥有。

## 验证

通过同一注册表完成父子创建、名称/变换/资产编辑、查询、保存和重载；
检查 stale revision、会话 ID 更换、失败加载、非叶删除、循环父级、缺失资源均不破坏状态。
按受影响服务/事务和命令边界选择定向测试；涉及条件编译时检查独立命令配置。
记录：[0014](../development/0014-scene-services.md)。

## 事务和历史

edit_batch(guard, edits) 限 1–128 条。先快照并构建独立暂存文档，在暂存文档顺序编辑；
中途失败丢弃暂存结果，真实文档和历史不变。成功将内容作为一个提交，真实 revision 只加 1。
暂存文档 revision 从 0 起，避免真实 revision 临近上限时批内多条操作错误耗尽计数。
内容完全相同的事务是 no-op，保留 revision、dirty、redo；创建后删除也可成为 no-op。
分配、历史预算计算和结果列表准备发生在真实文档提交前；提交点仅交换已验证内容和预建历史。

历史保存 before/after 不可变内存快照，undo/redo 恢复内容但 revision 继续单调递增，dirty=true。
保存不清历史，new/load 清空历史；新编辑丢弃 redo。撤销不读取外部文件，缺失资产不影响内存回滚，
再次保存时仍检查资源。历史默认最多 64 单元、32 MiB 逻辑载荷，计入 before/after 的实体、名称和资产值，
不声称是分配器/进程内存硬上限。超限时淘汰最旧 undo；单个单元超预算则修改前拒绝。
HistoryLimits 可设更小的正数用于宿主控制和预算测试，不能超过默认上限。

scene.transaction(guard,commands[{method,params}]) 只接受六种 entity 内存编辑，params 不含内层 guard；
复用注册表 schema 和同一个 decode_scene_edit，再交服务原子执行。返回 state 和逐条 created_ids（非创建为 null）。
禁止保存、查询、new/load、history 和嵌套事务；命令发现标明 entity 编辑和事务 undoable。
history.status 返回 undo_count/redo_count/logical_bytes；history.undo/redo 要求 guard，空栈返回 invalid_state。
验证失败回滚、稳定 ID、层级恢复、no-op/redo、资产失踪后撤销、预算淘汰、revision 上限和保存状态。
记录：[0015](../development/0015-transactions-history.md)。

## 资产服务

AsyncAssetService 独立拥有 MemorySystem、Jobs 和 CPU 状态，通过 AssetOperations 注册 11 条命令。目录可独立 open；目录 guard 与 Scene guard 分离。SceneService 记录 manifest，同一清单的资产映射刷新不修改文档或历史。project.save 目标匹配活动目录时（含刚 new 的 Scene）先同步映射，成功后刷新工程 name/scene 和清单字节快照，资产映射不变则保留目录 guard。详细生命周期见 [Runtime](runtime.md)，命令见 [资产参考](../commands/assets.md)。

## M7.4 截图接入

SceneService 增加只读 read_snapshot(guard)，复制 Project 和内存 SceneSnapshot。独立可选 CaptureService 拥有后台渲染和原子输出状态，既有 SceneServices target 不链接 GPU。
M8.3 的只读快照同时携带活动 manifest 的工程相对路径（新建未保存时为空），使外部 load/project.save 后编辑器 Reload 指向当前清单；不改变业务命令 schema。
详见 [截图设计](render-capture.md)。

## M10.1 模拟服务

SimulationService 独立拥有启动时复制的 Project、来源 DocumentState 和暂存 SceneDocument。
它只读取 SceneService 取得启动快照，绝不替换编辑文档或修改历史/持久化。
控制命令使用运行身份保护，详见 [Physics API](physics-api.md) 与 [模拟命令](../commands/simulation.md)。

M10.2 SimulationService 可选拥有CPU XPBD连续粒子/约束，独立于SceneDocument；物理求解成功才提交候选时钟，查询经Operations分页返回。见 [XPBD](physics-xpbd.md)。
