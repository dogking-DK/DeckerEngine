---
module: assets-runtime
created_at: "2026-09-22T18:20:46+08:00"
updated_at: "2026-09-22T19:00:00+08:00"
status: draft
---

# M4 资产身份、缓存与 CPU 加载设计

## 范围与当前基线

本文件是 M4 的实施设计稿，尚未实现。当前只有
[AssetReference](../../engine/assets/types/include/dk/assets/AssetReference.hpp) 和
[Project](../../engine/scene/include/dk/scene/Project.hpp) 的注册、类型与文件存在性校验。
分步计划见 [0020](../development/0020-m4-development-plan.md)。

M4 交付源文件 → 导入 → 可重建缓存 → CPU Ready。Scene 仍只保存 AssetId/AssetKind，
GPU 上传、渲染资源和 GPU Ready 留到 M7。资产准备不修改实体、Scene revision、dirty 或撤销历史。

## 目录、所有者与依赖

以下均为计划 target，实施对应小节时才添加 CMakeLists 和构建开关：

| 模块 | 计划 target | 职责与依赖 |
| --- | --- | --- |
| assets/types（已有） | dk::asset_types | 持久 ID/种类/引用，继续仅依赖 Core |
| assets/data | dk::asset_data | 不可变 CPU 网格/材质/纹理值；依赖 types、math |
| assets/importers | dk::asset_importers | 源文件到 CPU 数据；依赖 data、IO，私有 fastgltf/stb_image |
| assets/runtime | dk::asset_runtime | 元数据目录、缓存、加载请求/发布；按阶段依赖 types/data、IO、importers、jobs，私有 xxHash |
| framework/services、operations | dk::asset_services、dk::asset_operations | Project 与资产目录适配、命令注册；复用现有服务分层 |

资产底层不依赖 Scene、Commands、Vulkan 或 Editor。AssetService 从 ProjectDescription 构造
独立目录快照；不能令 assets/runtime 反向包含 Project.hpp，或暴露 SceneService 的可变 Project。
后台工作只持有输入快照和工作结果；运行时目录及 Ready 发布归属调用者线程。

## M4.1：身份与元数据

### 文件职责

- 源文件及依赖：工程内的 glTF/GLB、buffer、图片，属于用户内容。
- `<source>.meta`：UTF-8 JSON，保存持久身份、导入设置及子资产映射，进入版本管理。
- `<project>/.cache/assets/`：可删除、可重建的导入产物，不保存唯一身份；不能据此恢复丢失的 ID。
- Project v1 的 `assets[].path` 继续指向源文件，不能悄悄改成缓存文件或带 fragment 的路径。
  同一源文件的 mesh/material/texture 子资产各有记录，以 ID 区分，path 可以相同。

计划元数据 v1 字段如下；JSON 编解码拒绝重复键、未知字段、非法 UTF-8、nil/重复 ID 和未知版本。

| 字段 | 约定 |
| --- | --- |
| format / version | `DeckerAssetMeta` / 整数 1 |
| root_id | 主网格 AssetId；等于 outputs 中 mesh/0 的 ID |
| importer | `{name:"gltf-static",version:1}`，声明支持的导入契约版本 |
| settings | v1 仅 `unit_scale`，默认 1，有限且大于 0；规范化后参与缓存键 |
| outputs | `{key,id,kind}` 数组：mesh/0、material/N、texture/N 等源内选择器到 UUID 的映射 |

实现时为元数据提供限长输入和数量限制，复用 Project 的路径/ID 约定；输出数量不能突破工程
10000 条资产记录的上限。源路径由 sidecar 位置决定，不在 meta 再存第二份路径。
导入器实现版本另写入产物并参与缓存键；升级实现不重分配 AssetId。

首次显式登记时生成 stduuid-backed ID；重复登记复用合法 meta。新增子资产先在候选 meta 中
分配 ID，完整验证后发布，不能每次导入重新编号。首版以源内索引定位子资产，名称只作诊断；
不承诺导出器重排数组后仍能识别同一个语义对象，重排需显式检查/重映射，不能靠名称猜测。

目录操作概念接口为 `inspect_source`、`prepare_registration`、`commit_registration`、
`rename_source`；名称在 M4.1 落实头文件时固定。输入/输出均为 dk 值类型与 Result，
不暴露 JSON 或导入器内部对象。目录快照携带会话 ID 和单调 catalog revision，更新需要匹配两者。

三方库和固定基线核验集中见 [选型说明](assets-importers.md#已确定的三方库与基线)。
xxHash 的 XXH3-128 在 M4.1.2 首次用于恢复记录的文件摘要，M4.3 复用同一个内部 ContentDigest 封装；
先放在 assets/runtime 实现侧，不为尚无调用者的通用 hash 模块创建空 target。

### 兼容与提交点

已有无 meta 的 M2 工程仍可做文件校验和场景保存/加载。进入 M4 托管链路时必须显式登记：
可无歧义采用已有单个 mesh AssetId；已有 ID/kind/path 与 meta 冲突时返回 conflict，不能静默覆盖。
新的 ProjectDescription 先经 Project::create 校验，提交时整体替换只读 Project；不引入可变索引后门。
持久清单更新复用 save_project；目录 revision 与 Scene revision 分离，加载/重命名不伪造场景编辑。

`.meta` 和 Project 清单是两个持久文件。登记/重命名先准备候选、校验版本和目标冲突，
再保存带前后路径及摘要的操作记录；文件变更成功且清单替换完成后，才发布内存目录。
失败优先回滚；回滚失败返回原错误和恢复信息，并将受影响记录标记需恢复，禁止继续写入。
重启通过操作记录检查实际文件状态，能唯一判断时恢复，否则报告 conflict；不自动删除冲突文件。
操作记录位于工程 `.decker/asset-operations/`，属于恢复数据，不能随缓存一起清理。
该机制不承诺跨文件原子性、任意断电恢复或多个进程同时写同一工程；首版串行单写者。

重命名首版只允许**同目录、保持扩展名的文件改名**，同时处理源、meta 和所有相关 Project 路径，
保留全部 AssetId。大小写等价改名按平台语义拒绝，跨目录移动与 URI 重写延后。
目标已存在、meta 丢失/损坏、重复 ID 或未恢复操作均拒绝；外部单独移动源文件须诊断，不补造身份。

## M4.3：缓存与依赖

缓存按输入内容寻址，计划 key 为以下规范化输入的 XXH3-128：摘要算法标识、缓存格式版本、导入器实现版本、
支持的契约版本、settings、源字节摘要、按稳定顺序排列的依赖 URI/字节摘要，以及输出 ID 映射。
不使用 mtime/大小作为唯一正确性依据。散列实现确定为 xxHash，采用 `XXH3_128bits` 默认参数，
不使用随机 seed 或自定义 secret。摘要固定 16 字节，对外文本为 32 个小写十六进制字符。
持久字节使用 `XXH128_canonicalFromHash` 生成的规范大端编码，禁止直接保存 `XXH128_hash_t` 内存；
该编码独立于产物数值块的小端格式。CacheKey 表示输入版本，不能替代或重新生成 AssetId。

摘要包装支持分块输入，使用 `XXH3_128bits_reset/update/digest` 默认参数接口，与一次性计算保持一致；
状态通过 `XXH3_createState/freeState` 配对并由 RAII 管理，检查分配/接口错误，块间检查 IO 错误和取消。
对已读取输入快照分块消费，避免为散列再复制整份数据，也不能重读另一版本后给旧产物盖上新摘要。
构建输入描述采用带类型/长度边界的确定性编码，固定字段与依赖顺序，数值表示在 M4.3.1 固定；
禁止无分隔地拼接字符串，不将时间戳、线程完成顺序或进程地址混入 key。
依据：[xxHash 0.8.3 的 XXH3-128、流式与规范编码接口](https://github.com/Cyan4973/xxHash/blob/v0.8.3/xxhash.h)。

缓存 manifest 与恢复记录显式保存 `hash_algorithm: "xxh3-128"` 和各自格式版本，禁止把其他算法的摘要
当作当前摘要解释。缓存算法/版本不兼容时视为未命中并重建，保留未知条目；恢复记录算法未知时返回
not_supported 并保留记录，不猜测文件状态或自动删除。首版不维护双摘要，也不自动切换算法。
XXH3-128 用于本地内容变化与缓存意外损坏检测，不提供密码学抗碰撞或真实性认证；
真实性要求若在未来出现，应另行设计。性能收益在实施时按实际资源与 IO 路径测量，当前没有工程基准结果。

M4 的源依赖为 buffer/图片文件，不递归解释任意其他资产工程。按需请求时重新核验依赖；
缺失或损坏、设置/源内容/导入器变化、产物版本不兼容均不能命中有效缓存。首版不做目录监视。
解析、散列和导入消费同一份已读取字节快照，避免 key 与产物来自不同版本；发布前重新检查输入。
检测到变化返回 conflict 供显式重试，不无限自动重建；外部并发写文件不承诺文件系统级一致快照。

产物使用自描述 manifest 和定长小端数值块，记录格式版本、类型、长度、摘要、依赖与输出 ID；
不能序列化 Eigen/容器的内存布局。先在本次独占临时目录写完并读回验证，再发布不可变 key 目录，
最后原子替换小型 current 索引。每步失败保留旧索引；碰到已存在的 key 先校验，禁止覆盖未知内容。
源与 meta 从不由缓存清理删除；失败或取消只清理本次拥有的临时文件。

删除缓存后的下次导入必须重建相同 ID；损坏条目视为无效并报告重建原因。
首版仅支持显式清理未使用磁盘条目，不做复杂 LRU。内存句柄拥有只读数据，卸载只解除目录拥有权，
已有消费者仍可读到原代数据，最后一个持有者释放后回收；禁止悬空引用。

## M4.4：加载状态与发布

查询描述 `AssetId/kind/state/request_generation/ready_generation/diagnostic`，
结果句柄包含 AssetId、产物 key 和只读数据所有权；CPU Ready 不等于 GPU 可用。

| 操作 | 状态及保证 |
| --- | --- |
| 首次 load / 显式 retry | Unloaded 或 Failed → Loading；参数拒绝不改变状态 |
| 同一 ID/同一输入正在加载 | 复用现有作业；首版不建立多个独立取消订阅者 |
| 当前代成功且输入仍有效 | 候选整体发布为 Ready；mesh 所需材质/纹理一并有效 |
| 当前代失败 | Loading → Failed，保留定位错误；旧句柄仍有效，不把旧数据宣称为最新 Ready |
| 当前代取消 | Loading → Unloaded；作业终态 cancelled，与失败分开 |
| 新请求覆盖、unload 或会话替换 | 递增 generation 并取消旧作业；旧结果不能回写当前目录 |

generation 不回绕；新结果只在所属目录会话和 generation 均匹配、输入仍有效、未接受取消时发布。
提交点前接受取消则不发布；已发布后取消返回“已终止”，不撤销成功。发布后再更新作业成功终态，
查询不得观察到 succeeded 却取不到对应资产。首版由主线程轮询 completion 完成该原子可观察步骤。
磁盘已有完整不可变产物可在取消后保留为未引用缓存，但 current/Ready 不得随后偷偷切换。

## 定向验证与实施前检查

- M4.1：身份往返、冲突、合法重命名和每个文件提交点失败；Project v1 与旧无 meta 工程兼容。
- M4.3：逐项改变 key 输入、仅修改图片、损坏/删除缓存、写入中断；失败保留旧索引且 ID 不变。
  摘要封装首次落地时验证官方已知向量、空输入、分块/一次性结果一致及规范字节/十六进制编码；
  所属小节验证算法标识不匹配时的缓存重建/恢复拒绝，缓存编码测试区分不同字段边界。
- M4.4：复用请求、取消/完成竞争、旧代晚到、卸载后旧句柄、会话切换、CPU-only 生命周期。

CPU 产物 manifest 在 M4.2.2 固定，缓存索引/预算和恢复记录格式在所属小节先补充再实现；这些细节不阻塞
M4.1.1 的 meta/身份设计。测试 target 和 CTest 名称在创建时加入验证 skill 的参考，当前不得声称可运行。

关联：[导入器](assets-importers.md)、[任务队列](foundation-jobs.md)、
[已有资产类型](assets-types.md)、[工程格式](project-format.md)、[IO](foundation-io.md)。
