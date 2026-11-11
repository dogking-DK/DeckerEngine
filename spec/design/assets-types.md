---
module: assets-types
created_at: "2026-09-22T12:28:05+08:00"
updated_at: "2026-11-11T20:28:55+08:00"
status: accepted
---

# 资产身份与引用（M2.3）

engine/assets/types 提供 dk::asset_types，仅 PUBLIC 依赖 Core。
AssetKind 固定 mesh/material/texture/voxel 四种持久文本值；未知值返回 invalid_argument。
`dk::asset_types` PRIVATE 使用 magic_enum 生成名称与严格区分大小写的解析，未知枚举名称仍为空。
`asset_kind_names()` 返回静态存储的只读 string_view span，供命令 schema 使用同一反射列表；
不接受数字文本、首尾空白或嵌入 NUL，不在公开头文件暴露第三方类型。
AssetReference={AssetId, AssetKind}，ID 必须非 nil；不包含句柄、指针、加载状态或设备资源。
资产记录包含 ID、种类和工程相对路径；注册与文件诊断由 Project 负责。
同一路径可拥有不同种类记录，身份仅由 AssetId 决定，重复 ID 被拒绝。

Scene 的 EntityData 新增 assets 列表（每实体最多 64，ID 不重复）。
set_asset_references 先验证 ID/种类/数量，再提交不可变组件负载，失败不改 revision；
不依赖工程的编辑允许尚未解析的引用，显式 Project 校验和 M2.4 保存/加载必须解析引用。
新增 dk.AssetReferences v1 属性描述，字段 items 类型 asset_references。

M2.3 只验证类型、注册表与普通文件存在性，不读取资产内容或推测文件扩展名格式。
资产导入、解码、缓存和 GPU 创建在 M4 及后续实现。
验证 nil/重复引用、类型不匹配、未登记 ID、缺失文件和成功路径。
关联：[Scene](scene.md)、[工程格式](project-format.md)、[0011](../development/0011-project-assets.md)。

## M4 衔接

M4 不改变上述已实现的 AssetReference 持久语义，也不将 CPU/GPU 句柄加进 Scene 引用。
CPU 数据计划放到独立 assets/data，M4.1.1 的 meta 身份/只读目录已位于 assets/runtime；
types 继续只依赖 Core。一个 glTF 源的网格/材质/纹理通过 meta 映射为各自的 AssetId，
源内选择器不写入现有 AssetReference。具体方案见 [资产运行时设计稿](assets-runtime.md)。
meta v1、登记候选与兼容验收见 [0031](../development/0031-asset-metadata-catalog.md)；持久提交、CPU 导入/加载尚未实现。

## M23.5 体素身份

`voxel` 对应 `.dkvoxel` 描述及其命名 `.vdb` grid，meta 使用 `voxel-vdb` v1 / 唯一 `voxel/0`。
ID 和 kind 继续独立于文件格式、CPU 数据和 GPU generation；Scene/Project 不新增设备字段。
Scene v2 / Project v1 的字段结构不变，旧 reader 对未知 kind 明确拒绝。详情见[体素资产契约](render-voxels.md#m235-资产实施契约)及 [0163](../development/0163-voxel-assets.md)。
