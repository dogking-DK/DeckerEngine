---
module: automation-protocol
created_at: "2026-09-22T13:50:31+08:00"
updated_at: "2026-09-22T13:50:31+08:00"
status: accepted
---

# JSON-RPC 与行传输（M3.4）

dk::automation_protocol 实现 [JSON-RPC 2.0](https://www.jsonrpc.org/specification) 请求分派，
PUBLIC 依赖 dk::runtime。dk::automation_transport 提供流循环，PRIVATE/ PUBLIC 依赖协议按公开接口声明。
M3.4 为文件批处理，M3.5 复用同一协议并接入持续 stdio/同步任务状态；不建立 TCP/命名管道。

请求为 {jsonrpc:"2.0",method:string,params?:object,id?:string|integer|null}；
id 整数限 int64 范围，拒绝浮点、bool、数组/对象；字符串最多 256 个 UTF-8 字节。
named params 为当前命令配置，数组 params 返回 -32602。缺失 params 视作 {}。
额外请求字段拒绝（本工程严格配置）。缺失 id 是通知，合法通知即使业务失败也不响应。
无效 envelope 返回 id:null 的 -32600，无法解析为 -32700。
支持 1–128 个元素的 JSON-RPC batch 数组，按顺序执行，返回非通知响应数组；空数组无效。
这与 scene.transaction 不同：协议 batch 不提供原子性。

成功响应为 {jsonrpc:"2.0",id,result:{value:<command-result>}}；M3.5 将在 result/data 增加同步任务信息。
错误为 {jsonrpc:"2.0",id,error:{code,message,data?}}，data 含 engine_code、engine_name、context。
未知方法 -32601、参数错误 -32602、内部错误 -32603；业务 invalid_state=-32002、not_found=-32003、
io_error=-32004、not_supported=-32005、conflict=-32007。未知方法先于参数 schema 校验。

UTF-8 JSON Lines：每个非空行一个对象或 batch，支持 CRLF，纯空白行忽略；
单行最多 1 MiB，超长行消费到换行后报 -32700 并继续，深度/节点/重复键校验沿用 commands。
响应压缩为一行并立即 flush；stdout 不写启动 banner/日志。总批处理文件长度不限制，但每行有界。
底层流读取失败、写入失败或异常返回 3；错误命令标记批处理最终退出 1，正常 EOF 退出 0。
参数/命令/JSON 错误可恢复；资源异常不伪装成可恢复业务错误。

验证完整/通知/混合 batch、ID 及 envelope、严格解析、stdout/stderr、错误码和跨进程状态；
具体 stdio 停止和 task 保留策略在 M3.5 开始前补充。
