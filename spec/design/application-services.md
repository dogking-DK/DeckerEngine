---
module: application-services
created_at: "2026-09-22T13:28:00+08:00"
updated_at: "2026-09-22T13:38:00+08:00"
status: accepted
---

# 场景服务和编辑操作（M3.2）

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
编辑使用 Edit variant（创建/删除/改名/变换/父级/资产），为 M3.3 批量事务复用。
本节每条编辑对应一次 Scene 调用；事务、历史和 Runtime 留待后续小节。
服务活得比引用它的 registry 更久；不存在全局单例和后台线程。

## 验证

通过同一注册表完成父子创建、名称/变换/资产编辑、查询、保存和重载；
检查 stale revision、会话 ID 更换、失败加载、非叶删除、循环父级、缺失资源均不破坏状态。
默认 Debug/Release 回归，独立命令配置保持可构建。
记录：[0014](../development/0014-scene-services.md)。
