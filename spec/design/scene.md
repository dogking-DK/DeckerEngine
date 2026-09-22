---
module: scene
created_at: "2026-09-22T12:05:49+08:00"
updated_at: "2026-09-22T12:27:09+08:00"
status: accepted
---

# SceneDocument 设计

## 目标与阶段

按 M2.1–M2.4 逐节建立场景编辑文档。本次 M2.1 只实现 flecs 所有权、SceneId、
实体创建/删除、稳定 EntityId 索引和 revision/dirty 基础；组件/层级在 M2.2，
资产/工程在 M2.3，持久化与清洁状态确认在 M2.4 实施。
不创建 Runtime/PlayWorld，不向调用方暴露 ECS 句柄、world 或可变组件指针。

## 模块和依赖

engine/scene 建立 dk_scene / dk::scene，公开 include/dk/scene/SceneDocument.hpp。
Pimpl 私有持有 flecs::world，PUBLIC 依赖 dk::core，PRIVATE 依赖 flecs。
当前 vcpkg 锁定基线的 flecs 为 4.1.4；由已有 scene feature 提供，不变更基线。
DK_BUILD_SCENE 默认 OFF，windows-dev 预设启用；启用时自动补充 scene feature。
基础 bootstrap 和各独立 Foundation 命令显式关闭 Scene，避免隐式扩大依赖。

## 身份和 API（M2.1）

- create() / create(SceneId) 返回 Result<unique_ptr<SceneDocument>>；
  指定 SceneId 必须非 nil，自动生成使用 Core/stduuid。
- 文档不可复制/移动，unique_ptr 可转移所有权；析构释放该文档所有 flecs 数据。
  外部使用持久 ID，不存储可复用的 flecs 运行时编号。
- create_entity() 返回新 EntityId；create_entity(EntityId) 恢复指定非 nil ID。
  同一文档重复 ID 返回 invalid_argument，跨文档可复用导入的同一持久 ID。
- destroy_entity(EntityId) 删除实体和索引；缺失 ID 返回 not_found。
  M2.1 实体尚无层级；M2.2 在此基础上明确父节点删除策略。
- id()/revision()/dirty()/entity_count()/contains() 查询值；
  entity_ids() 返回按 ID 排序的副本，顺序不依赖哈希表或 flecs 运行时顺序。
- validate() 交叉检查索引和真实 ECS Identity 组件数量、存活性及 ID 一致性，
  为集成测试和后续快照校验提供诊断，不暴露底层 world。

## 编辑状态与错误

新文档 revision=0、dirty=true（尚无持久化基线）。
成功创建/删除每次 revision 加一；拒绝的编辑不更改实体、索引或 revision。
计数到 uint64 最大值后拒绝进一步编辑，不回绕。M2.4 接入保存确认和干净状态，
不在 M2.1 提供无文件依据的清除 dirty 接口。

单线程所有者模型；调用方协调所有读取/编辑，不引入后台 ECS progress/观察器。
对 nil/重复 ID 和缺失实体先验证再更改；先预留索引项，再创建/设置 ECS 组件，
C++ 异常路径销毁已建实体、回收索引项，提交后才更新 revision。
资源异常继续传播；flecs 内部不可恢复错误/进程终止不承诺可恢复回滚。
新文档初始化失败由 RAII 销毁半成品，现有文档不受影响。

## 验证

验证自动/指定 SceneId、nil 拒绝、生成/导入实体 ID、重复/缺失编辑状态不变、
真实 ECS/索引一致、删除后旧 ID 不会指向新实体、跨文档隔离、排序与反复创建/销毁。
消费者测试只 include dk 头并链接 dk::scene，确认 flecs 私有依赖和 DLL 布署。
Debug/Release 全量回归；独立仅 Core/Scene/Catch2（关闭数学/IO/日志/示例/runner）
验证 M2.1 边界。M2.2 引入数学后再调整独立配置。

## 组件与层级（M2.2）

本节新增公开 EntityData 值副本：id、UTF-8 name（可空，上限 1024 字节、禁止 NUL）、
双精度 Trsd local、optional<EntityId> parent。每个实体始终拥有 Identity/Name/Transform/Hierarchy；
缺省名称空、单位 TRS、无父级。world_transform 返回派生 Transformd，保留剪切。
场景现在 PUBLIC 依赖 dk::math；DK_BUILD_SCENE 要求 DK_BUILD_MATH=ON，关闭时配置明确报错。

固定持久组件名 dk.Identity、dk.Name、dk.Transform、dk.Hierarchy，首版版本均为 1。
Components.hpp 显式描述字段名称、类型和只读属性；无 RTTI 自动反射或任意组件扩展。
EntityId 只读，世界矩阵为派生值不持久化。名称不是身份，也不绑定 flecs 名称路径。

entity(id) 读取副本；set_name、set_local_transform、set_parent 是唯一写入口。
TRS 输入由数学库校验，四元数规范化后保存；相同规范值为无操作，不递增 revision。
set_parent(nullopt) 解绑，显式 nil/缺失父级、自指向/循环返回错误；重挂保留 local。
destroy_entity 仅允许叶节点，非叶拒绝且不改变 revision；调用方显式自底向上删除。

层级以稳定 ID 组件保存，不使用会隐式级联删除的 flecs ChildOf。
修改变换/父级时先复制组件候选，按根至叶的拓扑队列计算所有 world（无递归栈深限制）。
发现循环、缺失引用或非有限组合时不提交。成功后用已准备好的不可变 shared_ptr 交换 ECS
组件负载和派生矩阵，提交不分配内存；查询不暴露这些内部指针。此基线以正确性为先，
层级编辑 O(N+E)，尚不做脏子树增量更新。普通创建、名称和叶删除无需重建整个 world。

验证多级传播、非均匀缩放剪切、负/零缩放、规范化、重挂/解绑、无操作、
缺失实体、坏名称、循环、非叶删除、溢出回退和属性描述；默认 Debug/Release 回归，
独立 Scene 配置加入数学并继续以警告即错误构建。

## 后续

工程/资产引用、JSON 快照与原子重载在各子阶段开始前扩展设计。
不把预留阶段当作当前已实现的功能。

## 参考与记录

- [flecs 实体与组件](https://www.flecs.dev/flecs/EntitiesComponents.html)
- [Core](foundation-core.md)、[Roadmap](../roadmap.md)
- [0009 文档与实体身份](../development/0009-scene-identity.md)
