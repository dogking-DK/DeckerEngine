---
module: runtime
created_at: "2026-09-22T13:50:31+08:00"
updated_at: "2026-10-03T21:44:26+08:00"
status: accepted
---

# Runtime

## 所有权与分派

dk::runtime 位于 framework/runtime，拥有 SceneService 和 CommandRegistry；构造先创建服务，
再注册操作，析构先销毁 registry，保证捕获引用有效。PUBLIC 依赖 services/commands，PRIVATE 依赖 operations/profiling。
framework 和 Scene 同时启用才构建；基础 CPU 配置不链接日志、窗口、GPU 或脚本。
DK_BUILD_RENDER_CAPTURE 条件接入独立 RenderServices/Operations，启用时才链接渲染链路。
M1.7.1 为 create/dispatch 和 runner 入口增加可关闭的 [CPU zone](foundation-profiling.md)，
dispatch 使用动态文本记录 method；不改变命令/schema/guard/stdout 契约。

Runtime::create(existing_root) 捕获既有工程根；dispatch(method,params,auto_guard=false)
为串行状态修改安全点，拒绝同实例重入。宿主必须在同一线程调用，未声明线程安全。
每次命令通过注册表校验并调用统一服务，事务由服务负责；异常复位调度标志，资源异常交进程入口处理。

## 批处理入口

dk-run 支持 --help/--version 和 --project-root ROOT --batch FILE [--auto-guard]。
Windows 使用 wmain 接收原生路径；文件内容 UTF-8、按行 JSON-RPC，无 BOM；批文件按调用者 cwd 解释。
batch 读取直到 EOF，逐条输出/刷新响应，命令失败继续后续行并最终返回 1；输入文件/根目录错误为 2。
--auto-guard 只在 batch 显式开启：命令 schema 含 guard、请求省略 guard 且有活动文档时，
在串行执行点注入当前 document_id/revision；显式 guard 保持原样。方便从空场景开始的可重放离线脚本。
协议和服务默认仍要求显式 guard；此选项不是过期请求自动重试。stdio 不允许 auto-guard。

transport/protocol 位于 automation，依赖 runtime，Runtime 不依赖传输。stdout 只写 JSON 响应，
stderr 写诊断；未连接日志模块时同样可运行。batch 按行串行处理；持续 stdio 使用下述显式事件循环；GPU 只在提交截图作业后由 worker 延迟初始化。
资源不足、输出失败等入口致命异常报告 stderr 并返回 3；不能宣称响应失败意味着命令未执行。

验收：独立进程从空工程创建父子/变换、保存两文件，第二进程加载查询并比较持久状态；
错误命令继续、显式过期 guard 拒绝、bootstrap 兼容。按本次影响选择进程用例；依赖边界变化时检查无日志/示例/Catch2 CPU 配置。
记录：[0016](../development/0016-cpu-runtime-cli.md)。

## 同步命令任务

Runtime::dispatch 返回 CommandExecution{TaskId, Result<Json>}；开始实际分派前生成 TaskId，
完成后记录 succeeded/failed、method、可选 error_code 和执行后 DocumentState。
TaskId 继续使用 stduuid-backed StableId，不与请求 id 混用。不保存大结果、不写磁盘，重启清空。
固定最多 256 条终态元数据，预留存储；按完成顺序淘汰最旧项。tasks.list/get 查询前已完成的任务，
查询本身随后作为一个任务登记，所以列表不会包含自己的新记录。不存在虚构 pending/running 或等待/取消接口。
无效 JSON/envelope、未知命令、非对象参数、已停止调度不会产生任务；进入命令 schema 后的错误会产生 failed 任务。

runtime.capabilities（明确 async_tasks=false、限额/guard/事务能力）、tasks.list、tasks.get(id)、
runtime.shutdown。shutdown 是不可撤销控制命令，返回 stopping=true，当前响应先刷新，再结束输入循环。
同一协议 batch 内的后续有效命令返回 invalid_state，不继续修改场景；停止后 Runtime 拒绝新分派。

## 持续 stdio 与退出

dk-run --project-root ROOT --stdio 持续读取 stdin，每条执行后即时响应，不等待 EOF；
正常 EOF/shutdown 返回 0，即使先前有可恢复请求错误；输入/输出/致命异常返回 3。
--stdio 与 --batch 互斥，--auto-guard 仅用于 batch。Windows 协议流设二进制，保持 UTF-8 字节与控制字符校验。
不后台启动，不绑定网络端口；传输断开不保证最后一次修改未执行，客户端应重新查询状态。

验证真实子进程在 stdin 打开时完成多轮请求/错误恢复/通知/批请求/保存/任务查询与 shutdown；
进程超时、stderr、退出码和重启加载均检查。预算淘汰、任务失败、EOF 和 shutdown 批边界有单元覆盖。
记录：[0017](../development/0017-stdio-delivery-a.md)。

Windows stdio 使用专用 ReadFile reader、最多 8 行队列、每行 1 MiB（超限继续读至换行再报错），
reader 不触碰服务/stdout。退出置 stop、唤醒满队列，再重复 CancelSynchronousIo + 完成事件等待以覆盖
“检查 stop 后、进入 ReadFile 前”的取消竞态，确认退出才 join，禁止 detach。EOF 与 shutdown 共用回收路径。
批文件/内存流保留原入口，每行之间 pump；持久 stdio 新入口在没有后续输入时也发布后台完成。

## 异步资产与 Jobs 装配

Jobs+资产导入/运行时启用时装配独立 AsyncAssetService；每个实例拥有 MemorySystem、Assets/Jobs heap、
主线程 ThreadContext、JobQueue 和 AsyncAssets。每次服务入口绑定 Assets ExecutionScope，worker 自动捕获。
registry 先于服务销毁；关闭时先 join 队列再销毁候选/目录/上下文，最后关闭 MemorySystem。
async_tasks 仍为 false；capabilities 新增 async_jobs、job_limits。关闭不保存 Scene。

`assets.open(manifest, guard?)` 独立打开已有 Project 清单，不加载 Scene；替换活动目录必须匹配旧目录 guard。
`assets.catalog(offset=0,limit=128)` 返回 guard、total、分页 records；这是 register/rename 的 guard 来源。
import 可在目录未打开时工作，完成只发布 meta/cache；显式 register 将输出映射写入 Project。
register/rename 成功后重置异步资产会话，取消旧代并释放 Ready；旧外部句柄仍有效。
SceneService 记录其打开/保存的 manifest，并在同一清单的资产命令成功后同步只读 Project 资产映射，
保留 Scene、document_id、revision、dirty、历史。project.save 执行前同步，避免覆盖刚登记映射；
不同清单互不影响。目录会话不自动切换到 scene.new/load 的工程，调用者显式 assets.open。
project.save 的目标匹配活动资产清单时，即使 Scene 刚 new 也先合入当前目录映射；
保存成功可更新工程 name/scene 字段和清单字节快照，资产映射未变则目录 guard 保持。
这样不会先覆盖映射再因快照不符报错；使用新目录须显式 assets.open。

所有 jobs/assets 参数通过 Operations schema 注册。jobs.wait 的 timeout_ms 为 0–1000（默认 0），
等待时 pump completion，不重入分派。运行时在 handler 前后检查队列 fatal，不能被通用 registry 包装为业务失败。
RuntimeEvents 维护序列号和条件变量，输入队列与 worker 通知它；主线程先采样序列再 pump/pop/wait，避免丢失唤醒。

TaskId 表示一次同步命令执行，JobId 表示后台作业，两者使用各自查询接口。
公开调用见[Runtime 命令](../commands/runtime.md)、[资产命令](../commands/assets.md)和[作业命令](../commands/jobs.md)。
后台接入与进程验收证据见 [0039](../development/0039-assets-jobs-commands.md)、[0040](../development/0040-cpu-assets-delivery.md)。

## M7.4 截图接入

Runtime 条件装配 CaptureService 与 RenderOperations；jobs 命令按 JobId 路由到资产/截图队列，idle pump 和 wait 消费两者。CPU-only 配置保留原依赖。
详见 [截图设计](render-capture.md)。
