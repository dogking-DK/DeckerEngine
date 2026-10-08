---
module: automation-replay
created_at: "2026-10-08T16:34:00+08:00"
updated_at: "2026-10-08T16:57:18+08:00"
status: accepted
---

# 自动化记录与重放

## 范围和接口

M9.4 在 Python SDK 增加 Recorder、load_recording、replay 和命令行入口，复用
[Python 客户端](automation-python.md)，不新增 C++ 命令或协议，不执行记录中的 Python 代码。
从已有磁盘 Project/Scene 开始；录制和重放各使用独占、尚未加载场景的宿主，以及初始工程的独立副本。
Recorder.start(client, project_root, manifest, seed, engine_build, context) 检查输入并加载初始场景。
call/transaction/capture 记录具体参数和结果；save(path) 完成验证后原子发布完整记录。
replay(client, recording, project_root, engine_build, timeout) 预检后顺序执行，返回 ReplayReport。

支持 entity.*、scene.query/transaction/save、project.save（仅原 manifest）、history.status/undo/redo；
截图通过 capture 高层入口等待终态并校验本地产物。scene.new/load 只用于初始引导，
不记录任意文档替换、资产注册/导入/改名、任务查询、JobId 轮询/取消或 runtime.shutdown。
这些范围继续使用普通 Client；录制期间不得并发调用其他修改者。

## v1 文件和输入

UTF-8 单个 JSON，format=DeckerRecording，version=1，最多 16MiB/1024 步，未知版本/字段、
重复键、非有限数字、越界数值和未支持命令在执行前拒绝。完整文件带内容 SHA256，
用于意外损坏诊断，不提供签名或可信来源保证。保存失败保留旧记录文件。

header 包括格式/SDK 记录协议版本、IPC protocol、Runtime capabilities、命令描述摘要，
调用者明确给出的 engine_build、uint64 seed、JSON context、Python/OS 信息及原工程路径。
engine_build 是调用者的构建标识，不伪装为服务端认证；重放要求调用者提供相同标识，
同时实查能力/命令 schema 摘要。seed 初始化 Recorder 的局部 random.Random，
不修改全局 RNG，也不声称设置引擎/未来物理 RNG；重放使用已记录的具体值。

初始输入清单包括工程内全部普通文件（排除 `.decker` 派生缓存），记录规范相对路径、
字节数和 SHA256，最多 4096 文件/512MiB；包括 manifest、scene、glTF 外部缓冲/纹理和 sidecar。
存在 `.decker/asset-operations/pending.json` 时拒绝开始/继续，必须先完成已有资产恢复。
拒绝符号链接/目录连接及越界路径，不自动打包/复制输入。重放要求输入完整且无额外文件
（仅排除 `.decker`），在加载/编辑前拒绝差异。录制文件必须放在工程根外。
保存命令可以覆盖原 scene/manifest，并记录该次文件摘要；其他初始输入必须始终保持不变。
截图输出须为非输入、未存在且未在本次使用的 .ppm 路径，父目录事先存在；每步保存/截图的
提交点仍在引擎，Python 检查/写记录不提供跨文件/内存事务。

## 身份和逻辑核验

每步记录 method/已物化 params、成功值或业务错误、执行后逻辑快照摘要；文件操作另存产物摘要。
初始和最终保存完整逻辑快照；快照遍历 scene.query 全部分页并核对每页 document_id/revision，
检查实体数量/唯一 ID。逻辑状态含 scene_id、revision、dirty、entity_count 和按 EntityId 排序的
id/name/local TRS/parent/assets，不包含 DocumentId、TaskId、JobId 或派生 world_matrix。
浮点按 JSON 精确值比较，不承诺跨 GPU 或不同数学实现的位级重现。

entity.create 缺省 ID 在录制发送前由局部 seed RNG 生成显式 UUID，事务内也一样；
回放直接使用保存的 ID。跨进程只替换 guard.document_id 中原文档 ID，保留 revision 和
故意错误的 guard，绝不查询后悄悄更新 revision。先比较上一步状态再发送本步；
每步比较成功值/错误 code+engine_name 和后状态。业务错误的路径/message/TaskId 只用于诊断。
截图比对终态、版本、frame/尺寸/路径/draw_count，并验证完整 PPM；保存其 SHA256 供观察，
不把重放图像摘要相等作为跨 GPU 正确性的承诺。

## 生命周期、失败和取消

Recorder 持有 Python 值和 Client 引用，不持有引擎或连接。普通 RPC 业务拒绝可记录并继续，
异常仍返回调用者；结果未知、等待/取消失败、前后状态不一致或执行后记录容量不足使 Recorder 失效，
禁止再执行/发布完成记录。中途失败保留已提交的引擎/文件状态，不能把已有记录当作恢复日志。
save 核对末态与输入/已知输出后，临时文件 flush/fsync + replace 是记录文件提交点；
不声称掉电事务。只在完整保存后才存在可重放文件；进程崩溃恢复日志不在本阶段范围。

replay 先验证整个文件/所有步骤/输入/版本，再加载；每步保持总单调期限，
传输失败、错误/结果/状态不符立即停止，ReplayError 保留 phase、step、completed、cause。
不自动重试、跳过失败、回滚或恢复重放；超时/中断不撤销已执行操作，也不隐式取消截图作业。
外部文件/宿主并发变化无法获得跨进程原子锁，输入校验和逐步状态检查只能检测观察到的变化。

## 验证

格式/版本/摘要/路径/容量拒绝、原子保存失败、分页变化、seed/身份、业务失败、未知结果、
总期限与中途停止单元测试；真实跨进程 CPU 记录/保存/回放，包含新旧 ID、事务失败、
undo/redo、输入损坏和重放漂移。启用 Luau 时从 M9.1 脚本保存的工程开始；GPU 配置验证
记录截图、回放 Job 等待与产物。沿用 M9.1–3 已验收证据，全部子阶段完成后关闭 M9。

相关：[场景](scene.md)、[Project](project-format.md)、[0073](../development/0073-automation-replay.md)。
