---
id: "0022"
created_at: "2026-09-23T11:41:34+08:00"
updated_at: "2026-09-23T16:38:00+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
  - ../design/foundation-jobs.md
  - ../design/assets-runtime.md
  - ../design/assets-importers.md
  - ../design/architecture.md
---

# 0022 Memory System 与 Tracy 设计准备

## 目标与基线

按用户要求先给出 mimalloc 通用 heap、PMR、智能指针、arena/pool 和多线程内存系统的具体设计，
并纳入提前接入 Tracy 的需求。基线为 0f18545，开始时工作区干净。
原 Roadmap 仅将自定义分配器列为暂缓项，没有 memory 模块或专项阶段，也尚无 Tracy 消费者。
本编号仅记录设计与路线调整，completed 不表示 M1.7 代码、性能测量或采集已经完成。

## 实际变更

- 新增 [Memory System](../design/foundation-memory.md)：每 Runtime 独立系统、域 heap、拥有型资源，
  PMR 借用规则、弱引用控制块存活、线程内 arena/local pool、共享 pool、预算、关闭闸门和失败约定。
- 新增 [Profiling](../design/foundation-profiling.md)：Tracy 薄封装与构建开关、CPU 首次采集、
  backing/使用量分层、事件配对、跨线程地址复用、采集模式和进程 client 生命周期。
- Roadmap 增加 M1.7.1–7 七个待实施小节，下一项改为 M1.7.1，M4/M5 前置补充 M1.7。
  保留原 M1–M3 验收与 M4 编号；同步 0020 的下一步，不改写其历史验证结果。
- 同步架构、Jobs、资产运行时/导入器的内存所有权与埋点边界、设计/开发索引、三方库说明。
- 未修改 C++、CMake、vcpkg 清单/baseline；未预建 target、实现接口、安装库或注册命令。

## 依据与关键决策

读取当前模块、Roadmap、spec 流程和 spec-workflow/state-contracts/build-verify skills。
核对本机固定 vcpkg 基线 port、官方最新引用及固定提交内容：

- 当前项目 baseline：`67b9e21f86e3034657a04da429a8bf274de67925`。
- 本次 `git ls-remote origin refs/heads/master`：`9e3427bc82738568947beb508e78231f99c04f4c`。
- 初次核验曾记为该提交与项目基线均是 mimalloc 3.5.3、Tracy 0.14.1。
  M1.7.1 实施时发现 Tracy 的本机工作树版本被误当成固定基线内容：67b9e21f 实际为 0.13.1#1；
  mimalloc 3.5.3 无误。此处保留设计经过并更正结论，准确升级和采集证据见 [0023](0023-tracy-cpu-profiling.md)。
- 阅读 mimalloc v3 heap 文档、v3.5.3 公共头及 Tracy v0.14.1 手册/CMake/事件接口。
  v3 heap 可多线程分配，不采用 v1/v2 的创建线程限制；arena/pool 自身线程规则单独定义。
- MemorySystem 为多实例，Tracy client 为进程共用；后者不掌管分配路由。
  默认只追踪 heap backing，临时层提供用量曲线；不将 arena 子对象和 chunk 重复计入总账。
- Tracy 在 M1.7.1 先交付 CPU capture，Memory 从 M1.7.2 开始配套内存追踪。
  生命周期基础在 heap 首节落实，多系统/worker 组合验收不依赖尚未实现的 Jobs。
- 直接读取 Tracy 固定 tag 的 CMake、options 和 Profiler.hpp：TRACY_ENABLE 默认 OFF，
  当前 portfile 未显式开启；将依赖配置检查和必要时的同版本最小 overlay 补充列入首节。
  named memory 事件内部串行入队，设计明确测量追踪开销，不声称 profiling 热路径无锁。

## 验证记录

定向 `check-spec.ps1 -Path @(本次 12 个变更 Markdown 路径)` 通过：
12 个文件、182 个本地链接，以及时间、开发编号和 JSON 清单检查；`git diff --check` 通过。
具体范围为两份新设计、架构/设计索引、Jobs、两份资产设计、0020/0022/开发索引、Roadmap、三方库说明。
设计核查包括关闭/分配竞争、weak_ptr 控制块、PMR 资源存活、跨线程释放、arena 重置和事件去重。
未构建引擎、运行 C++ 测试、安装新依赖或实际采集/benchmark：本次范围是具体设计，不是实现。

## 遗留与下一步

### 同一设计任务补充：业务自动路由

按用户对显式 thread/Assets 参数的反馈，修订为“框架入口绑定、业务隐式获取”。
保留每 Runtime 独立系统与低层显式分配；新增 ExecutionScope/DomainScope、当前资源查询、
拥有型任务 RoutingToken，以及默认捕获资源的 std 容器别名/allocator 和 scratch_vector 工厂。
容器扩容、释放和 Tracy 配对保持原资源；TLS 只用于新资源选择，跨线程移交不携带 scratch/context。
明确无上下文错误、嵌套异常恢复、系统关闭、worker 复用/缓存清理及普通/嵌套容器的接入边界。
同步 Jobs、两份资产设计、Profiling、架构和 M1.7.3/4/6 验收；未增加新编号或任何 C++ 实现。
本次补充执行定向 check-spec.ps1：8 文件/113 本地链接及时间、编号、清单检查通过；
git diff --check 通过。范围为 memory/profiling/jobs、两份资产设计、架构、Roadmap 与本记录。
上面的 12 文件/182 链接保留为初次设计的实际记录；没有构建或运行 C++ 测试。

### 下一项实施

本设计任务结束时，M1.7 实现均待开始，两份新模块设计均为 draft。
后续 [0023](0023-tracy-cpu-profiling.md) 已完成 M1.7.1，profiling 转为 accepted，Memory 仍为 draft。
当前下一项与验收范围以 [Roadmap](../roadmap.md#m17memory-与性能分析补充) 为准。
arena/pool 默认参数与 profiling 开销由后续基准决定；不预先承诺速度收益。

## 修改记录

- 2026-09-23T11:41:34+08:00：建立 Memory/Tracy 设计及七节路线，核验依赖和上游线程/采集契约。
- 2026-09-23T11:53:00+08:00：补充 client 启用风险与事件同步成本、局部资源关闭约定，完成定向文档验证。
- 2026-09-23T14:22:27+08:00：按用户反馈补充框架自动绑定/任务路由及隐式日常接口，保留显式底层入口。
- 2026-09-23T14:26:10+08:00：通过补充文档检查，明确最小上下文绑定随 M1.7.3 实施，不依赖后续集成小节。
- 2026-09-23T16:38:00+08:00：更正初次核验的 Tracy 旧基线版本，链接 M1.7.1 实施记录。
