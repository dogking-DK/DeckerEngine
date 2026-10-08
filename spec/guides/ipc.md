---
created_at: "2026-10-08T12:55:00+08:00"
updated_at: "2026-10-08T13:04:33+08:00"
---

# 本机 IPC 与 dk-ctl

[返回项目入口](../../README.md)。Windows Named Pipe 服务由一个 runner 或编辑器进程持有，
业务命令、schema、guard 和撤销语义与 [命令参考](../commands/README.md) 相同。

## 构建和启动

完整开发配置生成 `dk_run`、`dk_ctl`；编辑器配置额外生成 `dk_editor_app`。
在 VS2026 中重新生成并加载方案后，Apps 下选择对应启动项目，
在项目属性 → 调试 → 命令参数中填写下面的参数，工作目录设为仓库根目录。

```powershell
cmake --preset windows-dev
cmake --build out/build/windows-dev --config Debug --target dk_run dk_ctl
New-Item -ItemType Directory -Force out/ipc-demo | Out-Null
.\out\build\windows-dev\bin\Debug\dk-run.exe --project-root out/ipc-demo --pipe demo
```

该终端持续运行服务，另开终端调用客户端。`--pipe` 与 runner 的 `--batch`/`--stdio` 互斥，
不支持 `--auto-guard`。NAME 限 1–64 个 ASCII 字母、数字、点、横线、下划线；
实际路径为 `\\.\pipe\DeckerEngine.NAME`。同一名称只能有一个宿主，只允许本机当前 Windows 用户访问。

编辑器已打开工程时，可用相同客户端调用：

```powershell
.\out\build\windows-editor\bin\Debug\dk-editor.exe --root projects/demo --pipe editor-demo
.\out\build\windows-dev\bin\Debug\dk-ctl.exe --pipe editor-demo --method scene.query
```

编辑器默认不开放端点；外部编辑刷新快照/历史，已有草稿仍用原 guard。
`runtime.shutdown` 是明确退出指令，不会自动保存场景或 Inspector 草稿。

## 查询与显式 guard

下面代码用于空的 runner 工程。参数文件 UTF-8、无 BOM，路径支持中文；避免终端的 JSON 引号转义差异。

```powershell
$ctl = '.\out\build\windows-dev\bin\Debug\dk-ctl.exe'
& $ctl --pipe demo --hello
$created = & $ctl --pipe demo --method scene.new | ConvertFrom-Json
$state = $created.response.result.value
$params = @{guard=@{document_id=$state.document_id;revision=$state.revision}}
$file = Join-Path $PWD 'out/ipc-demo/create-params.json'
[IO.File]::WriteAllText($file,($params | ConvertTo-Json -Depth 16 -Compress),[Text.UTF8Encoding]::new($false))
$reply = & $ctl --pipe demo --method entity.create --params-file $file | ConvertFrom-Json
& $ctl --pipe demo --method scene.query
```

每次写命令从最新查询结果取得 guard；修改成功后旧 revision 失效。
返回对象的 `response.result.value` 是业务结果，`response.result.task_id` 是 Runtime TaskId。
`--hello` 返回 session 和传输限额，不创建业务任务。

## 超时、断连和重试

`--timeout-ms` 默认 5000，范围 1–60000；同一绝对截止时间覆盖连接、握手、发送和接收。
服务端单次 IO/等待 owner 默认 5000 ms，客户端设置更长时间不会延长服务端该限制。
stdout 除 `--help` 外始终输出一行 JSON；退出码 0 成功、1 RPC/业务错误、2 用法错误、3 传输错误。

| execution | 含义 |
| --- | --- |
| not_sent | 未尝试发送命令帧；hello 成功也属于此类 |
| unknown | 已开始发送但未收到有效回复；请求可能已经执行 |
| received | 收到匹配 session/id 的 RPC 回复；还需检查 result/error |

超时和断连不会回滚。客户端不会自动重试。保存完整输出中的 session/request_id，以及原方法和参数；
需要重试时使用同一凭据和相同内容：

```powershell
& $ctl --pipe demo --method entity.create --params-file $file --session $reply.session --request-id $reply.request_id
```

仍在缓存内时返回原响应和原 TaskId，不再执行。不同内容复用同一 ticket 返回冲突；
最多保留 256 条/32 MiB，超限淘汰后拒绝重试，新进程的 session 也拒绝旧票据。
结果未知且凭据已经失效时，先通过查询核对实际状态，再决定下一次业务操作，不能把新 ticket 当成恢复原请求。
协议错误码和原始帧见 [IPC 协议参考](../commands/ipc.md)。

```powershell
& $ctl --pipe demo --method runtime.shutdown
```

## 只构建客户端

```powershell
cmake --preset windows-client
cmake --build out/build/windows-client --config Debug --target dk_ctl
.\out\build\windows-client\bin\Debug\dk-ctl.exe --help
```

该配置只启用 Core/Commands/协议/管道客户端，无 Scene、Runtime、Vulkan、SDL 或 Renderer。
使用工程现有固定 baseline，不额外引入三方库。本阶段仅实现 Windows Named Pipe，Unix socket、网络远程控制尚未实现。
