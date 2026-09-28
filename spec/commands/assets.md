---
module: asset-command-reference
created_at: "2026-09-28T15:02:00+08:00"
updated_at: "2026-09-28T15:02:00+08:00"
status: accepted
---

# CPU 资产命令

需要启用 Memory、Jobs、Asset Runtime、Importers、Scene 和 Framework（windows-dev 已启用）。
所有命令 undoable=false，不允许放入 scene.transaction。CPU Ready 表示网格/材质/纹理已可读，不代表 GPU 上传。
路径为工程根内规范相对路径；单源、依赖、输入、CPU 输出分别限 16、64、128、256 MiB。
动态超额返回错误，不产生 JobId；任务中的文件/导入错误通过 jobs.get/wait 查询。

| 命令 | 参数（除注明外必填） | 返回及效果 |
| --- | --- | --- |
| assets.open | manifest；可选 guard | `{guard,total}`；control，读取现有 Project 清单并建立目录会话，不加载 Scene |
| assets.catalog | 可选 offset=0（0–10000）、limit=128（1–256） | `{guard,total,offset,has_more,records:[{id,kind,path}]}`；query |
| assets.import | source；可选 unit_scale（有限正数，默认沿用 meta 或 1） | `{job_id}`；external，异步准备 CPU 产物，成功后写 meta/current，不自动登记到 Project |
| assets.register | guard、source；可选 create_meta=false、unit_scale、outputs | `{guard,total}`；external，持久登记身份，见下文 |
| assets.rename | guard、source、target | `{guard,total}`；external，同目录移动源/meta 并更新 Project，保留 ID |
| assets.load | id | AssetStatus；control，已登记 ID 才能加载；Loading 复用作业，Ready 复用拥有值 |
| assets.status | id | AssetStatus；query，未知/未登记 ID 返回 not_found |
| assets.unload | id | AssetStatus；control，递增代次、取消旧请求、释放服务当前引用；外部旧句柄仍可读 |

目录 guard 为 `{session_id,revision}`，由 open/catalog/register/rename 返回，不能使用 Scene guard。
首次 open 省略 guard；替换活动目录必须携带旧 guard。load/status/unload/register/rename/catalog 要求活动目录，
import 无需目录或活动 Scene。目录最多 10000 个记录；异步最多 1024 个源 slot。
打开另一目录或实际登记/改名成功会清空异步会话并取消旧请求，旧结果不得覆盖新会话。
每个源导入整体 mesh/material/texture，同源输出共用 Loading/Ready；unload 释放该源的整组当前数据。
已 Ready 的 load 不自动监视文件，依赖改变后使用显式 import 重建或 unload 后 load。

register.outputs 为可选 `[{key,kind}]`，最多 10000 条；省略视作 mesh/0，始终保留并登记已有 meta 的全部输出。
非空数组必须含 mesh/0。key 支持 mesh/0、material/N、texture/N，kind 分别为 mesh/material/texture。
默认要求已有 meta；显式 create_meta=true 可创建或采用旧工程单一 mesh 身份。
旧工程没有 meta 时，先 register(create_meta=true)，再 import，避免分配不同于旧工程的身份。
register 不伪造尚未实现的导入能力；未知输出/删除旧 selector 的导入仍会报错。

AssetStatus 固定字段：`id,kind,state,generation,ready_generation,job_id,artifact,error`。
state 为 unloaded/loading/ready/failed；job_id 为活动 JobId 或 null；artifact 为
`{root_id,key,cache_hit}` 或 null；error 为 `{code,message,context}` 或 null。不返回大块 CPU 字节。
接受取消时，终态变 cancelled，资产在主线程回收后变 unloaded；业务错误变 failed，可再次 load 重试。

同一清单的资产映射会同步到 SceneService，保留文档 revision/dirty/历史；project.save 不覆盖新映射。
不同清单互不影响。外部直接改动清单会使持久操作冲突，需要重新 open。
常见错误：invalid_argument（路径/数值/schema）、invalid_state（未 open/容量满/恢复未完成）、
not_found（文件或 ID 缺失）、conflict（旧 guard/输入变化/身份冲突）、io_error（写入失败）。
meta 与 current 不是跨文件事务；current 失败可能保留已提交合法 meta 和完整孤立产物，错误上下文会说明。
没有成功 Job/Ready 就不能把提交命令成功当作资产完成。

```json
{"jsonrpc":"2.0","id":1,"method":"assets.import","params":{"source":"assets/triangle.gltf"}}
```

用返回 job_id 调用 [jobs.wait](jobs.md)，成功后保存 result.root_id；再登记到已存在的工程清单：

```json
{"jsonrpc":"2.0","id":2,"method":"assets.open","params":{"manifest":"project.json"}}
{"jsonrpc":"2.0","id":3,"method":"assets.register","params":{"source":"assets/triangle.gltf","guard":{"session_id":"<session_id>","revision":0}}}
{"jsonrpc":"2.0","id":4,"method":"assets.load","params":{"id":"<root_id>"}}
```

替换占位符和最新 revision。Batch 的 --auto-guard 会为资产命令注入目录 guard；stdio 必须显式传入。
EOF/shutdown 会取消未完成作业并回收，持久调用者须先 wait 确认完成再关闭输入。
