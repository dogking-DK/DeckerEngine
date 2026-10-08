---
module: automation-client
created_at: "2026-10-08T12:30:00+08:00"
updated_at: "2026-10-08T13:04:33+08:00"
status: accepted
---

# dk-ctl 与轻量 IPC 客户端

`dk_automation_client` / `dk::automation_client` 仅依赖协议、NamedPipe 传输及 Core/Commands 的 JSON 值校验，
不链接完整 Runtime、Scene、Renderer 或 Physics。`DK_BUILD_CTL` 默认 ON，在 Windows 且 Commands 可用时构建。
可在 Framework=ON、Scene/Math/IO/Memory/Assets/Renderer=OFF 的配置单独构建。

客户端一次 call 建立连接、hello 握手、取得 ticket、发起命令、读取响应然后关闭；同一绝对 deadline 覆盖所有步骤。
默认 timeout_ms=5000，范围 1–60000。连接失败或命令写入前失败为 not_sent；开始命令写入后超时/断连/非法响应为 unknown；
收到合法对应 session/id 的响应为 received。unknown 不能解释为没执行；不会自动重发或自动注入 guard。
显式 retry 携带旧 session 与 request_id，服务拒绝已淘汰/过期的 ticket，不以新 ticket 重试。

dk-ctl 参数：`--pipe NAME --method METHOD [--params JSON | --params-file FILE] [--timeout-ms N]`；
默认 params={}。`--hello` 只发现 session/能力，不预留命令 ticket。
`--session UUID --request-id N` 必须成对指定，用于显式重试；参数文件 UTF-8，最多 1 MiB，不接受 BOM。
Windows 使用 wmain/UTF-8 转换，文件路径支持 Unicode。默认 stdout 一个 JSON 对象，stderr 仅诊断；
输出包含 transport 状态、execution、session/request_id 和可用的 response。退出 0 成功、1 RPC/业务错误、2 用法错误、3 传输失败。
成功 response 保留 Runtime 的 Task envelope；业务值仍在 response.result.value。

验证真实子进程发现、查询/guard 编辑/Undo/保存、失败退出码、Unicode 参数文件、未知端点、重试元数据、
超时与断连语义；检查独立构建 target 依赖中没有 Runtime/Renderer。
本阶段不提供自动重试、后台守护进程、端点扫描或远程网络控制。

关联：[传输](automation-transport.md)、[命令参考](../commands/README.md)、[0067](../development/0067-ipc-client.md)。
