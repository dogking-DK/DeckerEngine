---
module: runtime
created_at: "2026-09-22T13:50:31+08:00"
updated_at: "2026-09-22T13:50:31+08:00"
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
