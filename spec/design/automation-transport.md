---
module: automation-transport
created_at: "2026-10-08T12:30:00+08:00"
updated_at: "2026-10-09T19:38:24+08:00"
status: accepted
---

# M8.3 本机 Named Pipe 与 owner 分派

## 模块边界

automation/protocol 拆为仅 Commands/Core 的 JSON-RPC 验证/响应分派（注入 endpoint），
Runtime 适配和现有 JSON Lines/stdio 移入 automation/server，线协议保持兼容。
宿主 CMake 改为链接 dk::automation_server；直接使用 Runtime 版 RPC 重载时包含 RpcRuntime.hpp，
纯协议消费者继续包含 JsonRpc.hpp。JsonLines.hpp 的公开 include 路径保持不变。
automation/transport 的 NamedPipe 只传输有界字节帧，不链接 Runtime、Scene 或渲染器；
automation/client 组合协议/传输，dk-ctl 只链接该客户端。无新增三方库。
Windows 启用；非 Windows 保留原 stdio，不宣称已有 Unix socket。

## 端点、帧与生命周期

显式 `--pipe NAME` 启动服务，物理名为 `\\.\pipe\DeckerEngine.NAME`，NAME 限 1–64 个 ASCII 字母、数字、点、横线和下划线。
创建者当前用户 SID 的显式 DACL、PIPE_REJECT_REMOTE_CLIENTS、FILE_FLAG_FIRST_PIPE_INSTANCE；同名第二宿主失败。
不绑定 TCP、不使用默认 Everyone ACL；同一 Windows 账户内的程序仍视为可信调用者。
单实例字节模式、overlapped IO：4 字节 little-endian 长度 + UTF-8 JSON；请求最多 1 MiB，响应最多 8 MiB。
短读/短写循环完成；空/超限/截断帧断开当前连接。无无限 FlushFileBuffers 或 detached worker。

单 IO worker 接收/排队/发送，最多 8 个待处理帧；IO 线程不访问 Runtime。
owner 在 pump 安全点解析/分派，runner 空闲时等待 RuntimeEvents，editor 每帧及最小化时也 pump。
每个连接串行请求/响应，空闲/单次 IO 与等待 owner 默认 5 秒上限；慢客户端不会无限占用端点。
accepted 队列项断连后仍可能执行；超时不是回滚。队列满断开且不入队，但远端仍按结果未知处理。
退出停止接收，已提交回复允许短暂排空；随后 CancelIoEx、等待 overlapped 完成、join、释放句柄。
响应写完后等待客户端关闭或下一帧，避免 DisconnectNamedPipe 丢弃未读响应。
关闭立即通知独立的accept停止事件，取消空闲ConnectNamedPipe；活动连接仍使用原IO停止事件，
在grace内保留回复排空机会。避免客户端刚断开且worker重新进入监听时，无待发回复也等待整个grace。
grace到期才取消活动IO并join，不将停止监听误当作可以丢弃已提交回复。覆盖空闲grace关闭和回复排空回归。

## IPC v1 与重试身份

外层 JSON `protocol:1`。hello 为 `{protocol:1,hello:true,reserve:bool}`，返回 server session UUID、限额和可选 request_id。
每次命令先由 server 分配单调正整数 ticket；RPC id 必须等于此 ticket。
执行帧为 `{protocol:1,session:<uuid>,request:<单个 JSON-RPC 请求>}`；返回 `{protocol:1,session,response:<JSON-RPC 响应>}`。
IPC 不支持通知或协议 batch；原 stdio/batch 行协议保持原能力。

owner ledger 最多 256 条 / 32 MiB，包含预留 ticket、规范化请求和最终响应；同 token 同内容重放缓存响应和原 TaskId，
同 token 不同内容冲突。新 hello reserve 可淘汰最旧记录；已淘汰/未分配 ticket 一律拒绝，不会重新执行。
进程重启生成新 session，旧 session 拒绝。客户端只在携带原 session/ticket/内容时显式重试，不自动换身份。
ledger 在实际 Runtime 调用前记录请求，执行后发布响应；资源异常留下不可重放结果未知标记，禁止该 ticket 再次分派。
结果过大返回明确结果不可交付错误，可能已经执行；不重新执行。session/ticket 不替代 Scene guard。
32 MiB 为 ledger 的请求/响应逻辑字节预算，不是含 JSON 临时对象、IO 缓冲和分配器开销的进程内存硬上限。

## 宿主与验证

runner 新增 `--pipe NAME`，与 batch/stdio 互斥；editor 可选同名参数，启动时不默认开放 IPC。
editor 通过 Workspace 的 owner 安全点接入并刷新快照，未应用草稿保留旧 guard；外部更改会取消过期 Gizmo。
本阶段验证传输、客户端、去重、生命周期与基本编辑入口；M8.4 再验收 GUI/外部/截图完整一致性。
测试覆盖碎片帧、畸形/超限、客户端超时、断连后执行、重连去重/冲突/淘汰/重启、端点冲突、shutdown、
空闲/满队列退出、真实 dk-ctl 子进程，以及无 Scene/Renderer 的独立客户端构建。

实现依据：[Win32 Pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)、
[ConnectNamedPipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-connectnamedpipe)。
关联：[客户端](automation-client.md)、[协议](automation-protocol.md)、[Runtime](runtime.md)、[0067](../development/0067-ipc-client.md)。

M10.4：runner 显式 `--pipe-timeout-ms 1..60000` 覆盖 PipeOptions.timeout（默认5000不变，只能与 --pipe 同用）。GPU 冷编译可能超过5秒，实验宿主使用60000，客户端独立设置60秒。超时断连不取消已接受命令，保留原 ticket/未知执行状态语义；不把客户端等待期当服务端配置。
