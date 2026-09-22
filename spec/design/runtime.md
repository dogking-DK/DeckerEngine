---
module: runtime
created_at: "2026-09-22T13:50:31+08:00"
updated_at: "2026-09-22T14:05:59+08:00"
status: accepted
---

# CPU Runtime（M3.4）

dk::runtime 位于 framework/runtime，拥有 SceneService 和 CommandRegistry；构造先创建服务，
再注册操作，析构先销毁 registry，保证捕获引用有效。PUBLIC 依赖 services/commands，PRIVATE 依赖 operations。
framework 和 Scene 同时启用才构建；不链接日志、窗口、GPU 或脚本。

Runtime::create(existing_root) 捕获既有工程根；dispatch(method,params,auto_guard=false)
为串行状态修改安全点，拒绝同实例重入。宿主必须在同一线程调用，未声明线程安全。
每次命令通过注册表校验并调用统一服务，事务由服务负责；异常复位调度标志，资源异常交进程入口处理。

dk-run 保留 --help/--version，新增 --project-root ROOT --batch FILE [--auto-guard]。
Windows 使用 wmain 接收原生路径；文件内容 UTF-8、按行 JSON-RPC，无 BOM；批文件按调用者 cwd 解释。
batch 读取直到 EOF，逐条输出/刷新响应，命令失败继续后续行并最终返回 1；输入文件/根目录错误为 2。
--auto-guard 只在 batch 显式开启：命令 schema 含 guard、请求省略 guard 且有活动文档时，
在串行执行点注入当前 document_id/revision；显式 guard 保持原样。方便从空场景开始的可重放离线脚本。
协议和服务默认仍要求显式 guard；此选项不是过期请求自动重试。M3.5 的 stdio 不允许 auto-guard。

transport/protocol 位于 automation，依赖 runtime，Runtime 不依赖传输。stdout 只写 JSON 响应，
stderr 写诊断；未连接日志模块时同样可运行。没有隐藏事件循环和 GPU 初始化。
资源不足、输出失败等入口致命异常报告 stderr 并返回 3；不能宣称响应失败意味着命令未执行。

验收：独立进程从空工程创建父子/变换、保存两文件，第二进程加载查询并比较持久状态；
错误命令继续、显式过期 guard 拒绝、bootstrap 兼容。默认和无日志/示例/Catch2 CPU 配置双配置测试。
记录：[0016](../development/0016-cpu-runtime-cli.md)。

## M3.5 持续服务与同步任务

Runtime::dispatch 返回 CommandExecution{TaskId, Result<Json>}；开始实际分派前生成 TaskId，
完成后记录 succeeded/failed、method、可选 error_code 和执行后 DocumentState。
TaskId 继续使用 stduuid-backed StableId，不与请求 id 混用。不保存大结果、不写磁盘，重启清空。
固定最多 256 条终态元数据，预留存储；按完成顺序淘汰最旧项。tasks.list/get 查询前已完成的任务，
查询本身随后作为一个任务登记，所以列表不会包含自己的新记录。不存在虚构 pending/running 或等待/取消接口。
无效 JSON/envelope、未知命令、非对象参数、已停止调度不会产生任务；进入命令 schema 后的错误会产生 failed 任务。

新增 runtime.capabilities（明确 async_tasks=false、限额/guard/事务能力）、tasks.list、tasks.get(id)、
runtime.shutdown。shutdown 是不可撤销控制命令，返回 stopping=true，当前响应先刷新，再结束输入循环。
同一协议 batch 内的后续有效命令返回 invalid_state，不继续修改场景；停止后 Runtime 拒绝新分派。

dk-run --project-root ROOT --stdio 持续读取 stdin，每条执行后即时响应，不等待 EOF；
正常 EOF/shutdown 返回 0，即使先前有可恢复请求错误；输入/输出/致命异常返回 3。
--stdio 与 --batch 互斥，--auto-guard 仅用于 batch。Windows 协议流设二进制，保持 UTF-8 字节与控制字符校验。
不后台启动，不绑定网络端口；传输断开不保证最后一次修改未执行，客户端应重新查询状态。

验证真实子进程在 stdin 打开时完成多轮请求/错误恢复/通知/批请求/保存/任务查询与 shutdown；
进程超时、stderr、退出码和重启加载均检查。预算淘汰、任务失败、EOF 和 shutdown 批边界有单元覆盖。
记录：[0017](../development/0017-stdio-delivery-a.md)。
