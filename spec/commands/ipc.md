---
created_at: "2026-10-08T12:55:00+08:00"
updated_at: "2026-10-08T13:04:33+08:00"
---

# IPC v1 与 dk-ctl 参数

[命令目录](README.md) · [可执行使用指南](../guides/ipc.md) · [生命周期设计](../design/automation-transport.md)

本页描述传输封装和客户端参数，不增加 Runtime 业务命令。stdio 的原始 JSON-RPC 行协议保持不变。

## CLI

`dk-ctl --pipe NAME --method METHOD [--params JSON | --params-file FILE] [--timeout-ms N]`

| 参数 | 约束 |
| --- | --- |
| --pipe NAME | 必填；本机端点名 1–64 个 ASCII 字母、数字、点、横线和下划线 |
| --method METHOD | 要调用的 Runtime 命令，与 --hello 互斥 |
| --params JSON | 可选对象，默认 {}；与 --params-file 互斥 |
| --params-file FILE | UTF-8 无 BOM 对象文件，上限 1 MiB |
| --timeout-ms N | 默认 5000，1–60000 的整数 |
| --hello | 查询 session/限额，不执行命令、不预留 ticket |
| --session UUID / --request-id N | 成对指定，并提供原 method/params；显式重试，N 为正 int64 |
| --help | 输出用法 |

调用者自行维护业务 guard；CLI 不注入 guard、不自动重试。
业务副作用、持久化和撤销范围由相应命令决定；成功结果路径改为 `response.result.value`。
输出 `status` 是传输状态，`execution` 是 not_sent/unknown/received；`received` 包括业务错误。
`response.error.data.execution=unknown` 表示虽收到了错误响应，原命令是否成功仍未知。
退出 0/1/2/3 分别为成功、RPC 错误、用法错误、传输失败。

## 原始帧

每帧是 4 字节 little-endian 无符号长度，后接 UTF-8 JSON；请求 1..1048576 字节，响应 1..8388608 字节。
同一连接按请求/回复顺序工作，无流水线、通知或 batch。下例 JSON 为展示格式，实际长度按 UTF-8 字节计算。

1. 发送 `{"protocol":1,"hello":true,"reserve":true}`。
2. 接收 `{"protocol":1,"session":"<uuid>","request_id":1,"limits":{...}}`。
3. 发送 `{"protocol":1,"session":"<uuid>","request":{"jsonrpc":"2.0","id":1,"method":"scene.query","params":{}}}`。
4. 接收 `{"protocol":1,"session":"<uuid>","response":{"jsonrpc":"2.0","id":1,"result":{...}}}`，随后客户端可关闭连接。

重连时 hello reserve=false，核对 session，再携带原 request id/内容发送。
预留票据只绑定当前 server session，首个有效执行帧把该票据绑定到规范化 JSON 内容。
无效 RPC 也会消耗已绑定票据并缓存错误；缓存可按条数或逻辑字节预算淘汰。

| RPC code | 含义 / 恢复 |
| --- | --- |
| -32070 | IPC 封装、版本、hello 或 ticket 类型错误；修正调用 |
| -32071 | 票据过期或未预留；不执行，原结果可能未知，查询实际状态 |
| -32072 | 同一 ticket 内容冲突；恢复原方法及参数 |
| -32073 | session 已变；不在新 session 重放旧请求 |
| -32074 | 请求已消费但结果不可用；不得当作未执行 |
| -32075 | 响应超过 8 MiB；命令可能已执行，使用更小的查询 |

JSON 解析与业务错误继续使用[通用错误规则](README.md)。队列满、帧为空/超限/截断、超时会断开连接。
IO worker 不分派业务；owner 队列中的请求断连后仍可能执行。shutdown 回复后最多排空 1500 ms，再取消并回收 IO。
