---
module: foundation-profiling
created_at: "2026-09-23T11:41:34+08:00"
updated_at: "2026-09-23T19:13:31+08:00"
status: accepted
---

# Foundation 性能分析与 Tracy 接入设计

## 目标、边界与版本

从 M1.7.1 提供可实际采集的 CPU 分析能力，再随 [Memory System](foundation-memory.md) 接入
内存事件、使用量曲线与线程上下文。工具用于回答命令/IO/任务耗时、内存峰值、保留量与分配热点。
M1.7.1 已完成 CPU 接入、OFF/ON 定向验证和真实采集，见 [0023](../development/0023-tracy-cpu-profiling.md)。
M1.7.2 已提供 heap backing 事件与真实配对采集，见 [0024](../development/0024-mimalloc-heap.md)。
M1.7.4 已接入 arena 用量曲线，见 [0026](../development/0026-scratch-arena.md)；pool 曲线和 GPU 埋点仍为规划。
accepted 表示采用当前方案，不代表所有小节已验收或已经测得性能改进。

2026-09-23 实施核验官方最新提交 `33d78c1ed898a06938f31312167c7abefd229455`，
提供 **Tracy 0.14.1 / BSD-3-Clause（client）**。旧固定基线 67b9e21f 实际为 0.13.1#1，
初次设计误读本机工作树版本，现以固定提交的 baseline/port 内容更正，并按
[依赖政策](../third-party-libraries.md) 在接入时升级基线。
viewer/capture 工具与 client 使用匹配版本，不将网站缓存中的旧版号作为当前 registry 依据。

## 模块和构建方式

目录 `engine/foundation/profiling`，target `dk_profiling / dk::profiling`，
头文件 `include/dk/profiling/Profiler.hpp`、`Memory.hpp`，实现 `src/Profiler.cpp`、`Memory.cpp`。
模块不依赖 Memory、Core、日志或 Runtime。
Memory/Jobs/IO/Runtime 的实现按需 PRIVATE 链接它；公开模板若需要埋点则明确 PUBLIC 传递。

项目侧只使用 DK_* 宏和 dk::profiling 函数。CPU zone 宏必须在**调用处**保留源码位置、函数名和
静态 SourceLocation，不能把所有调用都归到包装函数，也不能每进入一个 zone 就分配一个 Pimpl。
启用时薄包装 Tracy 的 scope 宏，dk::profiling PUBLIC 链接 `Tracy::TracyClient` 以传递包装头所需声明；
业务公开数据结构不保存 Tracy 类型。禁用时包装头无需 Tracy，宏不求值参数且不产生 client 链接。
M1.7.1 固定 CPU 接口为 DK_PROFILE_ZONE、DK_PROFILE_ZONE_TEXT、DK_PROFILE_ZONE_VALUE、
DK_PROFILE_FRAME、DK_PROFILE_THREAD_NAME，以及 enabled/is_connected/set_thread_name。
无 Tracy 配置的 target 为 INTERFACE；ON 为包含线程名/连接状态适配的静态库。
TEXT 接受可转为 string_view 的文本并只求值一次，保持临时 string 活到 Tracy 完成复制；
空文本不发事件，超过 Tracy 字节上限的内容截到 65534 字节（边界可能切断 UTF-8 字符）。
宏只在同一词法作用域内访问 zone，scope 不跨线程；ZONE/FRAME 名称必须使用静态期字符串。
关闭时参数不求值由 DK_* 宏保证，普通 C++ 函数的实参仍遵循语言自身的求值规则。
M1.7.2 增加 `memory_enabled()` 和 `record_allocation/record_free` 固定分类接口。
CPU 或内存开关关闭时内存适配内联为空操作；CPU 关闭时不要求 Tracy 头或 client，
仅关闭内存时仍保留 CPU client。预算和关闭契约始终由 Memory 自身执行。

| 选项/配置 | 语义 |
| --- | --- |
| DK_ENABLE_PROFILING=OFF | 默认轻量构建；不查找、安装或链接 Tracy；所有埋点编译消除 |
| 专用 profiling preset | 开启分析，优先 RelWithDebInfo + PDB；优化下测量，Debug 用于行为调试 |
| DK_PROFILE_MEMORY=ON | 默认 ON，仅在 profiling 开启时有效；启用 heap backing alloc/free，默认无逐分配 plot |
| DK_PROFILE_CALLSTACK_DEPTH=0 | 默认不采集每次分配/zone 调用栈；诊断 preset 可提高深度 |
| profiling vcpkg feature | tracy，default-features=false，启用 on-demand；不自动构建 GUI 工具 |
| 独立工具配置 | 按需 cli-tools / 匹配版本 viewer；与被测程序分开，不拉 GUI 依赖进入 CPU Runtime |

通过 imported target 使用 port 编好的设置，不只在消费目标上定义宏却让 TracyClient 使用另一套配置。
本次源码核验发现 v0.14.1 的 TRACY_ENABLE 默认 OFF，当前 portfile 没有显式传入 ON。
M1.7.1 已使用仓库内[最小 overlay port](../../cmake/vcpkg-ports/README.md)，保留官方相同版本/
源码校验/补丁，只补充 `-DTRACY_ENABLE=ON`。配置时核验导出的 TRACY_ENABLE、TRACY_ON_DEMAND、
TRACY_NO_CRASH_HANDLER；升级官方 port 时检查是否可以移除 overlay。
不通过降级库或仅在消费端定义 TRACY_ENABLE 绕过；实际产物和 capture 验证前不推定可用。
首版不启用 fibers，不安装 Tracy crash-handler，避免改变现有异常/崩溃处理。
完整启动期 capture 若需要 non-on-demand，必须使用另一独立 build/install 配置重新构建 client；
不能用源文件局部 #define 改变预编译依赖语义。开关覆盖全体消费目标，避免 ODR/ABI 不一致。

进程内只存在一个 Tracy client 实例，所有 exe 内的库/未来 DLL 共享它；不在每个 Runtime
各编译一次 TracyClient.cpp。**共享观测后端不等于共享 MemorySystem**：多个 Runtime 仍独立拥有域和预算。
宿主保持 client 晚于所有会发事件的线程、资源、延迟释放对象；首版使用库默认进程生命周期，
不由某个 Runtime 调用 ShutdownProfiler，禁用库卸载和静态引擎对象晚析构这种未验证生命周期。

## 埋点接口与最早交付

CPU 包装：`DK_PROFILE_ZONE("literal")`、`DK_PROFILE_ZONE_VALUE(id)`、
`DK_PROFILE_FRAME("literal")`、`set_thread_name(...)`；named allocation/free 已在 M1.7.2 接入，scratch plot 在 M1.7.4 接入。
静态名称表达稳定操作，例如 Runtime.Dispatch、IO.Read、Scene.Save；运行时 command/JobId 使用 zone value/text，
不为每条请求制造新的 source location。禁用分析时，格式化文本等额外参数计算也必须消除。

| 阶段 | 采集范围 | 能回答的问题 |
| --- | --- | --- |
| M1.7.1 | runner 主线程、Runtime 分派、直接 IO/保存路径与受控多线程探针 | 首个 CPU 时间线与可导出的 capture |
| M1.7.2 | heap 分配/释放、域 backing 使用量 | 谁分配了持久内存，是否正确释放 |
| M1.7.4–5 | arena/pool 用量、高水位、增长/重置/trim zone | 临时空间和池保留量是否过大 |
| M1.7.6–7 | 线程 context、移交探针、重复工作负载 | 跨线程生命周期与各策略实际开销 |
| M4 | 导入/解码/hash/cache、Jobs worker/等待/主线程发布 | 作业排队、执行、发布分别耗时多久 |
| M5–M7 | Vulkan GPU zone、上传/Pass/frame-slot/fence | CPU/GPU 对齐与帧资源复用；另做专项验收 |

Jobs 的排队、执行与发布各自为线程内 zone，通过 JobId 关联；不能在主线程打开一个 RAII zone、
到 worker 线程关闭。CPU-only 程序不伪装成固定帧循环，FrameMark 留给有真实 frame 的模块。
首版不包装所有 mutex；M4 队列若测出等待热点，再对相关锁局部接入 Tracy lock 工具。

## Memory 事件契约

1. 仅 HeapResource 的实际上游申请记录 `TracyAllocN`，成功后、指针发布前发事件；
   分配失败不发，规范化后的 backing 请求大小与域账本一致。
2. 释放在存储交还 mimalloc **之前**记录匹配 `TracyFreeN`，然后释放、结清统计；
   不允许先 free 再发事件，否则其他线程复用同地址时会出现 alloc/free 顺序错误。
3. 分配与释放可来自不同线程，但地址、named pool 和追踪模式保持一致。
   自动路由仅决定新对象的资源；既有对象的分配/释放标签取自其保存的 resource，不读取当前 TLS 域。
   `DK_PROFILE_MEMORY` 是构建配置，不在块存活期间按域任意切换或只采样一半事件。
4. named pool 标签首版采用适配层中唯一的静态字符数组，分类为 `dk/heap/general`、`assets`、
   `scene`、`render`、`jobs`、`other`；同类别的多个 Runtime 在 Tracy 内存视图聚合。
   系统/域实例明细保存在引擎快照，后续需要时以 zone value 关联，不把临时 string.c_str() 交给 Tracy。
5. 不分别在 PMR、smart pointer、Buffer、heap 每层重复发同一 allocation。
   arena/pool 的 chunk 已在 backing 轨道中；子对象默认只更新 logical/used/retained/peak 曲线。
   曲线在安全点或固定低频采样，跨线程先聚合再发布；不每次 bump 就发 plot。

因此一个 64 KiB arena chunk 内分出 100 个对象，heap 轨道只记录这一个 64 KiB 申请。
对象退出后的 arena used 下降，chunk 若继续保留，backing 仍保持；真正归还上游时才出现 free。
预算、backing 请求量、logical 使用量与进程 RSS 是不同指标，UI/报告不能相加当总内存。

M1.7.4 的 `record_scratch_sample(previous, current)` 在安全点替换每个 arena 的上一笔贡献，
使用短 mutex 汇总为固定 `dk/scratch/used`、`dk/scratch/retained`、`dk/scratch/backing` 和
`dk/scratch/sampled-peak` 曲线，Memory 格式/阶梯展示。retained 是完全空闲 chunk，backing 包括全部 chunk；
sampled-peak 是进程内采样总 used 的历史最大值，不代表瞬时精确峰值或 RSS，arena 快照另保存自身精确峰值。
checkpoint、rewind 前后、chunk 增长/复用、reset 和显式 sample 时更新；销毁撤销贡献。
普通 bump 不发事件、不加全局锁；Memory.Scratch.Grow/Rewind/Reset 提供操作级 CPU zone。
DK_PROFILE_MEMORY=OFF 时曲线适配为空函数，CPU zone 仍按 DK_ENABLE_PROFILING 控制。

逐对象 arena/pool 追踪留作以后诊断模式：必须使用独立 logical named pool，并解决地址复用和
checkpoint 部分 rewind 的配对。`TracyMemoryDiscard` 只能清空对应整个命名池，
**不能用于包含其他 arena/Runtime/外层 scope 活对象的共享标签**。首版不依赖 discard 实现正确性。

追踪适配不回调日志、用户 observer 或 dk::memory 分配；Tracy 自身事件缓存保持其独立分配路径。
固定名称在静态存储中建立，不在 hook 中创建字符串。生产路径仅调用固定适配器，不向用户开放 observer；
测试 sink 仅供内部确定性探针，不可重入 Memory。不把随意丢失 alloc/free 事件当作防递归方案。
Memory 正确性不依赖 viewer 在线或成功接收事件。

## 采集模式、退出和开销

日常 profiling preset 使用 on-demand，未连接时不无限累积从启动开始的事件；在受控 workload
开始前连接 viewer/capture 工具。晚连接不会重建此前所有存活分配，报告注明捕获区间，
不能把未配对历史或断线后的记录直接判为泄漏。严格内存配对验收在一次稳定连接内完整运行 workload。
跨连接仍使用相同 allocator 标签；不自行依据 IsConnected 丢弃某一侧事件来“修复”账本。

短命工具通过有界测试握手在 capture 就绪后启动工作；正常 runner/CTest 不启用 TRACY_NO_EXIT，
避免用户未打开 viewer 时进程无法退出。采集文件写入忽略目录 `out/profiling/`，
记录输入、提交、编译配置、依赖版本、采集范围及是否包含启动前存活对象。
本机 preset 和采集脚本设置 TRACY_ONLY_LOCALHOST=1、TRACY_ONLY_IPV4=1；后者保证与
工具使用的 127.0.0.1 一致，避免 localhost 只监听 ::1。直接从终端/VS 启动时须自行传入这些环境。
不修改 stdout 的 JSON-RPC 协议或添加性能日志。

关闭分析的目标是没有 Tracy client、额外线程与事件调用；预算/所有权检查仍是 Memory 自身逻辑。
开启分析有事件缓存、同步和传输成本，内存分配事件在高频场景尤其需要测量。
v0.14.1 的 named memory 事件内部使用串行队列和锁；Memory 自身没有全局分配锁，
不等于开启 Tracy 后的路径无锁。适配器遵守 alloc 发布前/free 存储归还前的顺序，
并验证地址复用；不再在引擎层给所有 heap 加一把额外全局锁。
性能基线同时保留 OFF、CPU zone only、CPU + memory 三种配置，调用栈另行测量；
不使用带全量追踪的数据声称无追踪版本的绝对吞吐。

## 验证与实施顺序

M1.7.1 定向构建 OFF/ON 的 profiling 探针与受影响 runner/IO 链路：禁用后无依赖且参数不求值；
开启后能采集真实 CPU zone、正确源码位置和线程名称；未连接/断开时正常结束且 stdout 保持协议。
至少生成一次可由匹配 viewer/capture 工具读取的 capture，不能仅凭链接成功标记“性能分析可用”。

当前采集入口为 [capture-profiling.ps1](../../scripts/capture-profiling.ps1)，工具安装与调用见
[README](../../README.md#tracy-cpu-性能分析m171)。独立工具 manifest 与引擎保持同基线/overlay。
脚本启动并管理自己的探针和工具进程，使用 IPv4 localhost，记录提交/工作区、配置和 workload；
无连接/不退出时有超时和进程清理，失败也保存日志。探针通过受控等待避免零时长 zone，
5 秒采集留出 Windows 调度粒度与最终元数据查询时间；这不是吞吐或绝对耗时基准。
验收使用 csvexport 读回 134 个 CPU 区间，验证调用处位置、两个线程 ID、异常关闭、动态文本边界。
线程命名通过包装调用接入，CSV 不展示线程名；未做 GUI 视觉验收，不声称已检验 viewer 中的名称显示。

M1.7.2 已通过测试 sink 检验失败无 alloc、跨线程配对和同地址复用顺序，再用真实 Tracy capture
验证 adapter；sink 不替代客户端验收。真实探针读回 assets 35 次/2241 字节、scene 1 次/32 字节，
合计 36 次申请全部配对、2 次异线程释放、0 个存活块；CPU 开启且内存关闭时同负载无内存事件。
这里的字节是累计请求量，不能作为同时存活峰值。独立 [内存检查器](../../tools/profiling/README.md)
使用匹配版本的 Tracy server 解码事件；不依赖仅导出 CPU 数据的 csvexport 来推断内存正确性。
脚本 `-Mode memory/memory-disabled` 保存检查结果，默认 `-Mode cpu` 保持原有 134 区间验证。
M1.7.4 的 `-Mode arena/arena-disabled` 验证 102 次临时申请只产生 3 次 jobs chunk 分配/释放，
4 条曲线读回 peak/结束值正确，内存采集关闭时事件和曲线均为零。M1.7.5 再补 pool 对应验收。
M1.7.6 验证多 Runtime 共用 client、其中一个关闭不影响另一实例、延迟释放仍记录。
针对实际条件编译路径验证 OFF/ON 是必要范围，不扩大为全引擎双配置回归。

参考：[Tracy v0.14.1 手册](https://github.com/wolfpld/tracy/blob/v0.14.1/manual/tracy.tex)、
[CMake 导出](https://github.com/wolfpld/tracy/blob/v0.14.1/CMakeLists.txt)、
[事件宏](https://github.com/wolfpld/tracy/blob/v0.14.1/public/tracy/Tracy.hpp)、
[事件队列实现](https://github.com/wolfpld/tracy/blob/v0.14.1/public/client/TracyProfiler.hpp)、
[M1.7 路线](../roadmap.md#m17memory-与性能分析补充)、
[0022 设计记录](../development/0022-memory-profiling-design.md)。
