---
module: scene
created_at: "2026-09-22T12:05:49+08:00"
updated_at: "2026-09-22T13:42:16+08:00"
status: accepted
---

# SceneDocument 设计

## M3.3 内存内容提交

SceneSnapshot 增加 same_content 与 logical_bytes；比较忽略 revision/来源，仅比较 SceneId 和有序实体值。
SceneDocument::stage(snapshot) 复制内容进入独立 ECS，revision=0、dirty=true、独立快照来源；不检查磁盘资源。
apply_snapshot(snapshot) 要求同 SceneId，内容相同返回 false；否则先完整构建/验证候选世界，再交换 Impl，
保留当前快照来源、revision 加 1、dirty=true。溢出或验证失败保持原状态。
此 API 支持 [服务事务](application-services.md)，不允许把历史 revision 直接写回真实文档。
快照只能从有效文档产生；持久化 codec 和来源检查维持原边界。

## 目标与阶段

按 M2.1–M2.4 逐节建立场景编辑文档：flecs 所有权、稳定身份、组件与变换层级、
工程/资产引用，以及 JSON 快照、保存和重载。各节设计与验证记录保留对应关系。
不创建 Runtime/PlayWorld，不向调用方暴露 ECS 句柄、world 或可变组件指针。

## 模块和依赖

engine/scene 建立 dk_scene / dk::scene，公开 include/dk/scene/SceneDocument.hpp。
Pimpl 私有持有 flecs::world，PUBLIC 依赖 dk::core、dk::math、dk::io、dk::asset_types，
PRIVATE 依赖 flecs 和 nlohmann-json。Scene 要求 math/io 开启，关闭时 CMake 明确报错。
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
当前 flecs 内部存储 Identity 和不可变 SceneComponents 聚合负载；持久组件名属于逻辑协议，
不暴露独立的 flecs 查询接口。聚合负载便于无分配地提交经过验证的整组组件。

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

## 场景协议、快照与安全重载（M2.4）

SceneIO.hpp 提供 serialize_scene/parse_scene、load_scene/reload_scene、save_scene、
save_project。内存编解码显式接收只读 Project，以校验所有 AssetId、种类和文件存在性。
load/save 默认使用 project.scene_path；父目录由调用方创建。单线程所有者约定保持不变。

v1 场景 JSON 顶层严格含 format="DeckerScene"、version=1、scene_id、revision、
entities。revision 为 uint64 非负整数，不存 dirty。实体数组每项严格含 components，
其键恰为五种稳定组件名，每个组件对象包含 version=1 和描述中的字段。
Transform translation/scale 为 3 个有限 double，rotation 为 [x,y,z,w]，导入验证并归一化；
归一化前后分量差不超过 8 * double epsilon 的四元数保留原值，避免已保存单位四元数
因重复归一化产生末位漂移；与当前局部 TRS 精确相同的写入直接视为无操作。
Hierarchy.parent 为 null 或非 nil EntityId；AssetReferences.items 为 {id,kind} 数组。
仅持久化局部 TRS，重算 world。未知组件/字段/版本、重复 ID/键、缺失引用、
循环、非法数值均拒绝；JSON 沿用 16 MiB/64 层限制，实体上限 10000。
输出实体按 ID 排序，保留资产引用列表顺序；不序列化 ECS 句柄，不提供迁移。

两遍恢复在独立候选文档完成：第一遍登记所有 EntityId 和局部组件；
第二遍解析父级/资产引用并拓扑重算。输入顺序可以子在父前，整体 O(N+E)。
完成校验前不触碰旧文档，reload_scene 只在成功后交换 unique_ptr；
成功后旧对象的引用/指针失效，调用方重新取值。parse_scene 为未保存的导入状态（dirty=true），
load_scene 从文件读入后 dirty=false，revision 恢复文件中的值。

SceneDocument::snapshot 返回不可变 SceneSnapshot 值，包含同一时刻的 ID/revision/实体副本，
及不可伪造的文档实例来源令牌。快照可在后续编辑后继续编码；不允许跨文档
（即使 SceneId/revision 相同）用于确认保存。公开接口不提供任意清除 dirty。
save_scene(doc, snapshot, project) 先验证来源、编码并检查引用，再交给原子 writer：
写入/刷新/关闭临时文件，读回逐字节对比并重新解析协议，全部通过才替换目标。
替换成功后只做无失败状态确认：dirty=(当前 revision != 保存快照 revision)。
因此先存新快照再存旧快照也会恢复 dirty=true；失败不更改 dirty/revision 或旧文件。
便捷重载 save_scene(doc, project) 在调用时取快照。

IO 增加带验证器的 atomic writer 重载，验证器只读临时路径，在替换之前调用；
返回错误走已有带诊断清理，抛异常由 RAII 清理后继续传播，未开始替换时旧文件仍保留。
保存继承 Windows 本地文件/NTFS 已验收范围，不承诺断电持久化、外部并发写隔离
或不可信同权限进程篡改临时路径的防护。工程 JSON 同样可经 save_project 验证后安全写入。

验收覆盖包含父子/剪切/资产/Unicode 的文件往返，子在父前、重复/坏版本、
循环/孤儿/缺失资源、失败重载保留旧对象与 revision/dirty、旧快照保存、
跨实例快照拒绝、最大 revision、真实文件共享冲突、验证器失败和异常清理。
默认与最小 Scene Debug/Release 回归，并运行独立 CPU 场景进程示例闭环。

examples/scene 提供 dk-scene-demo create/load ROOT，要求已有根目录和 mesh.bin 引用占位文件。
create 生成父子实体并覆盖 scene.json/project.json；两文件各自原子，非多文件事务。
load 从 project.json 恢复。标准输出只含场景身份/revision/数量，诊断到 stderr；
Windows 使用 wmain 支持 Unicode 路径。进程测试覆盖独立创建/重载、坏版本与缺失资产，
这只是 M2 CPU 验收入口，M3 的统一命令层后续设计。

## 后续边界

M2.3 按 [资产类型](assets-types.md) 和 [工程格式](project-format.md) 为 EntityData
加入 assets 值列表与 set_asset_references；新增第五个 dk.AssetReferences v1 描述。
引入 dk::io、dk::asset_types 和私有 JSON 依赖，Scene 的独立配置同时开启数学与 IO。

M2 不包含 Commands/Runtime、资产解码、图形设备、版本迁移、后台编辑或增量变换计算。
下一阶段由 M3 设计命令层，沿用本模块的受校验写入口和快照保存语义。

## 参考与记录

- [flecs 实体与组件](https://www.flecs.dev/flecs/EntitiesComponents.html)
- [Core](foundation-core.md)、[Roadmap](../roadmap.md)
- [0009 文档与实体身份](../development/0009-scene-identity.md)
- [0010 组件与层级](../development/0010-scene-hierarchy.md)
- [0011 工程与资产引用](../development/0011-project-assets.md)
- [0012 序列化与安全重载](../development/0012-scene-persistence.md)
