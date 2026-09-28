---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-09-28T16:15:12+08:00"
---

# CPU Runtime 与命令使用

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
构建前提见[构建指南](build.md)，验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## CPU 批处理

完整命令用法见 [命令参考](../commands/README.md)，按发现、场景、实体、历史和运行时分类。

先按 windows-dev 构建，然后在现有工程目录执行示例：

```powershell
New-Item -ItemType Directory -Force out/demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --batch examples/automation/create-scene.jsonl --auto-guard
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --batch examples/automation/load-scene.jsonl
```

第一进程用一个事务创建父子实体和变换，分别保存 scene.json/project.json；第二进程重新加载并查询。
输入 UTF-8 JSON Lines，每行一个 JSON-RPC 2.0 请求或 1–128 项协议 batch；通知没有响应。
stdout 每行一个 JSON 响应，result 含 task_id、status 和命令返回值 value，stderr 仅诊断。
--auto-guard 显式允许离线顺序脚本为省略 guard 的命令注入当前状态，显式 guard 始终保留。
默认仍要求编辑 guard，协议 batch 与 scene.transaction 的原子事务不同。

退出码：0 全部成功，1 批处理含可恢复请求错误（继续后续行），2 参数/启动文件错误，3 致命流/资源错误。
单行上限 1 MiB，超长行排空后报告错误；详细错误映射见 [协议设计](../design/automation-protocol.md)。

仅构建 CPU Runtime、保留进程验收而关闭日志/示例/Catch2：

```powershell
cmake --preset windows-dev -B out/build/windows-runtime-cpu -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-runtime-cpu --config Debug
ctest --test-dir out/build/windows-runtime-cpu -C Debug --output-on-failure
```

Release 替换配置名。该配置装配 stduuid、magic-enum、Eigen、flecs、JSON，未链接窗口/GPU。

## 持续 stdio 服务

```powershell
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/demo --stdio
```

从 stdin 逐行发送 UTF-8 JSON-RPC，进程立即刷新每条响应；不必关闭 stdin。
可先发送下面两行，查看能力和创建场景：

```jsonl
{"jsonrpc":"2.0","id":1,"method":"runtime.capabilities"}
{"jsonrpc":"2.0","id":2,"method":"scene.new"}
```

读取第二条响应的 result.value，其中 document_id/revision 构成下一次编辑的 guard。
每次编辑完成后用返回的 state 更新 guard；stdio 不允许 --auto-guard。
commands.list / commands.describe 提供实际注册能力和参数/结果 schema。

每次命令分派同步返回；后台作业由返回的 JobId 单独跟踪。命令成功为 result.status=succeeded，
已分派命令失败在 error.data 返回 task_id/status=failed。
tasks.list 枚举最近 256 个已完成任务，tasks.get({id:任务UUID}) 查询元数据；不保留大结果，进程重启后清空。
JSON-RPC 请求 id 用于关联响应，task_id 用于查询同步执行记录。tasks 不提供 wait/cancel；
启用 Jobs/Assets 时，通过 jobs.get/wait/cancel 操作后台 JobId，详见[作业命令](../commands/jobs.md)。
runtime.shutdown 在输出响应后正常关闭，也可关闭 stdin；stdio 的可恢复请求错误不会改变正常退出码 0。
启动错误仍为 2，致命流/资源错误为 3；输出失败时应查询场景状态，不能据此假定编辑未执行。

完整交互及重启验收见 [RuntimeStdioTest.ps1](../../tests/integration/RuntimeStdioTest.ps1)，
协议定义和限制见 [automation-protocol](../design/automation-protocol.md)。

## 命令层独立验证

启用 FRAMEWORK 和 Scene 时，`register_scene_commands(registry, service)` 注册
scene.new/load/query/save、project.save、entity.create/delete/get/set_name/set_transform/set_parent/set_assets。
编辑和保存参数使用 `guard: {document_id, revision}`；new/load 替换现有场景也必须携带 guard。
从返回的 state 读取最新 guard；保存到磁盘后，重新载入会生成新的 document_id。
scene.query 支持 offset（默认 0）和 limit（默认 128，最多 256），返回 has_more 和稳定 ID 排序的实体页。
变换使用 translation[3]、rotation[x,y,z,w]、scale[3]，查询 world_matrix 按行展开。
场景和工程清单分别保存。详见 [应用服务设计](../design/application-services.md)。

`scene.transaction` 接收一个 guard 和 1–128 条 `{method, params}`，内部只允许六种 entity 编辑，
内层不传 guard；失败整体回滚，成功只增加一次 revision。单条编辑也进入同一历史机制。
`history.status` 查询历史，`history.undo/redo` 使用最新 guard；撤销重做保留实体 ID 并继续递增 revision。
历史默认最多 64 单元/32 MiB 逻辑载荷，保存保留历史，new/load 清空；文件保存不支持撤销。

`dk::commands` 的 `CommandRegistry` 注册参数/结果 schema、effect 和同步 handler。
`commands.list` 枚举能力，`commands.describe` 返回完整契约；未知命令、参数错误和
handler 契约错误分别返回结构化 Error。支持的 schema 子集与上限见
[命令设计](../design/commands.md)。

```powershell
cmake --preset windows-dev -B out/build/windows-commands-only -DDK_BUILD_SCENE=OFF -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_JOBS=OFF -DDK_BUILD_MEMORY=OFF -DDK_BUILD_ASSET_RUNTIME=OFF -DDK_BUILD_ASSET_IMPORTERS=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=ON -DDK_WARNINGS_AS_ERRORS=ON -DDK_VCPKG_FEATURES=
cmake --build out/build/windows-commands-only --config Debug
ctest --test-dir out/build/windows-commands-only -C Debug --output-on-failure
```

Release 替换配置名即可；此配置不构建 Scene、Eigen、IO、窗口或 GPU。
