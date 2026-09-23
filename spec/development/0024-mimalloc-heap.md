---
id: "0024"
created_at: "2026-09-23T16:59:00+08:00"
updated_at: "2026-09-23T17:48:09+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
---

# 0024 M1.7.2 mimalloc heap 与内存事件

从 46ea9b2 开始，工作区干净。本节实现 MemorySystem、域/ResourceHandle、对齐 heap、
预算和关闭闸门，配套 Tracy backing 事件；不提前实现 M1.7.3 的 PMR/Buffer/智能指针/路由。

## 设计与依赖

使用 spec-workflow、state-contracts、build-verify。实现细化已先写入
[Memory 设计](../design/foundation-memory.md)。官方 vcpkg master 查询仍为
33d78c1ed898a06938f31312167c7abefd229455，mimalloc 3.5.3，无须升级基线。
核对官方 v3 heap 文档，支持同一 heap 多线程分配和异线程释放；禁止 destroy 活块或全局 override。

## 实施结果

- 新增 [dk::memory](../../engine/foundation/memory/CMakeLists.txt)，公开
  [MemorySystem](../../engine/foundation/memory/include/dk/memory/MemorySystem.hpp) 与
  [ResourceHandle](../../engine/foundation/memory/include/dk/memory/Resource.hpp)。SystemId/DomainId 稳定，
  域名称复制，分类、预算和关闭状态按实例隔离；不自动创建进程全局 MemorySystem。
- 对齐 heap 采用 mi_heap_malloc_aligned/mi_free；参数校验、乘法溢出检查、零字节规范化、
  预算 CAS 预留和失败撤销均返回固定枚举。metadata 和 Tracy 自身分配不计入 backing 账本。
  快照提供 reserved/backing/peak/live/count；并发时非事务采样，不能用快照决定删除。
- 每资源原子 gate 同时管理入场、Closing 和排他维护。分配先取得操作 ticket，再检查系统状态；
  关闭拒绝新申请、允许已入场申请结束、保留合法释放。零活动操作且无活块时才 mi_heap_delete。
  try_close 可先关闭空闲域，busy 不回滚；资源关闭和系统关闭竞争只删除一次。
- registry 用冷路径 mutex，ResourceControl 拥有共享系统状态而不回持 registry，避免循环引用。
  System 析构发起关闭；保留的 handle 允许在创建线程/System 结束之后异线程释放。
  本节裸指针调用者必须保存 owner，丢弃最后 owner 但仍有活块属于契约错误，不强制释放活对象。
- 新增 [Memory profiling 适配](../../engine/foundation/profiling/include/dk/profiling/Memory.hpp)，
  固定六类静态标签：general/assets/scene/render/jobs/other。alloc 在发布指针前记录，
  free 在地址归还后端前记录；预算拒绝/后端失败无事件，同一类别跨系统聚合。
  DK_PROFILE_CALLSTACK_DEPTH 默认 0；不逐申请发重复 plot，不修改 stdout 协议。
- 增加 DK_BUILD_MEMORY（默认 OFF，windows-dev/profiling ON）和 memory feature，
  mimalloc、profiling 和 Threads::Threads PRIVATE 链接，无 override。DK_PROFILE_MEMORY 默认 ON，仅 profiling ON 时发事件；
  禁用 CPU 或内存采集时适配为空，预算和生命周期契约照常执行。
- 新增 [15 个单元用例](../../engine/foundation/memory/tests/MemoryTests.cpp) 与
  [真实采集探针](../../tests/integration/MemoryProbe.cpp)。内部 backend/sink seam 只供测试，
  用 latch/barrier 确定性制造 OOM、地址复用、分配/释放/删除竞争，不向业务公开 observer。
- 扩展 [采集脚本](../../scripts/capture-profiling.ps1) 的 memory/memory-disabled 模式；
  独立 [检查器](../../tools/profiling/inspector/MemoryTraceInspector.cpp) 使用匹配的 TracyServer
  读回内存事件。工具不进入引擎解决方案，不把 CPU CSV 数量当作内存验证。
  原 cpu 模式继续可用；使用方式见 [工具说明](../../tools/profiling/README.md)。
- 同步 README、Memory/Profiling/工程/架构设计、依赖说明、索引和 roadmap。
  README 中历史独立 CPU 配置显式关闭新 memory 模块，保持其原有依赖范围。

## 状态契约与测试覆盖

创建域仅在 heap 和 metadata 准备成功后发布到 registry，失败不消耗 DomainId、不残留候选 heap。
分配失败只更新失败次数，不修改现有数据/live/peak 或发 alloc；已预留预算必须撤回。
释放在操作闸门内完成事件、后端 free 和账本结清；关闭排他检查在持有 gate 后读取 live，
避免在“观察到零”之后又有已入场申请提交的竞争。关闭状态只能 Open → Closing → Closed。

用例覆盖：域扩容/名称复制/多实例隔离，对齐 1–4096、零字节、非法参数/溢出、预算满/回收复用，
heap 创建失败和申请失败回滚，单域关闭、系统关闭/延迟释放和 move 赋值，
真实同 heap 4 线程共 512 次申请及异线程释放，4 路预算争用最多 2 次成功，
申请/释放进行中关闭 busy、资源与系统同时关闭只删一次、16 次确定性同地址事件复用。

## 验证结果

环境：Windows x64、VS2026/MSVC 19.51、CMake 4.2.1，x64-windows 动态 mimalloc 3.5.3，
Tracy 0.14.1。引擎定向构建开启 DK_WARNINGS_AS_ERRORS。仅因条件编译差异覆盖
profiling OFF / CPU+Memory ON / CPU ON 且 Memory OFF，不执行全引擎回归。

配置分别为 `cmake --preset windows-dev -DDK_WARNINGS_AS_ERRORS=ON`、
`cmake --preset windows-profiling -DDK_WARNINGS_AS_ERRORS=ON`，以及在后者增加
`-B out/build/windows-profiling-cpu-only -DDK_PROFILE_MEMORY=OFF` 的隔离配置。
详细 configure 日志位于 out/profiling/memory-configure-off.log、memory-configure-on.log、
memory-configure-cpu-only.log。测试均通过 scripts/verify.ps1 指定 target 和正则选择：

| 配置 / 目标及筛选 | 实际结果 | out/verify 证据目录 |
| --- | --- | --- |
| OFF Debug，dk_memory_tests，`^dk\.memory\.` | 14/14 通过（初始用例集合） | 20260923-171605-e8509dc2 |
| ON RelWithDebInfo，memory tests/probe、profiling probe/disabled，`^dk\.(memory\|profiling)\.` | 17/17 通过 | 20260923-172335-8045e5f6 |
| OFF Debug，memory probe + disabled header | 2/2 通过 | 20260923-172540-449b3c04 |
| CPU-only RelWithDebInfo，budget + competing close + memory probe | 3/3 通过 | 20260923-172741-9dbcd0fe |
| OFF Debug，新增 competing close 用例 | 1/1 通过 | 20260923-172742-8191523c |
| ON RelWithDebInfo，新增 competing close + 更新后的 disabled header | 2/2 通过 | 20260923-173321-b52e34dc |
| OFF Debug，memory target 自行声明 Threads 依赖后的 probe | 1/1 通过 | 20260923-174722-44d8b500 |

每个目录的 summary.json 保存精确命令参数/正则、选中测试、配置和提交状态，所有通过记录均无跳过。
最终 15 个不同的 memory 用例均覆盖 OFF/ON；补充验证与先前结果有重叠，不能简单相加为独立测试数。
新增 competing close 后只重跑新增用例及更新的隔离头测试，其余实现未变化。

实际执行三次最终采集：

```powershell
& ./scripts/capture-profiling.ps1 -Mode memory
& ./scripts/capture-profiling.ps1 -Mode memory-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18087
& ./scripts/capture-profiling.ps1 -Mode cpu -Port 18088
```

| 模式 | 读回结果 | out/profiling 证据目录 |
| --- | --- | --- |
| memory | 36 次申请全部配对；assets 35 次/2241 字节、scene 1 次/32 字节；2 次跨线程释放，0 活块 | 20260923-173711-7ddcc3df |
| memory-disabled | 工作负载完成 36 次申请；内存事件 0 | 20260923-173709-64d4632f |
| cpu | 原有 134 区间/2 线程及文本/源码位置检查通过 | 20260923-173710-bc44904c |

内存 workload 在一次稳定连接内执行，包含预算拒绝、零字节、多系统、创建者退出后的释放。
上述字节是累计请求量，不是峰值/RSS。capture、summary.json 与解码 JSON 均保存在忽略目录，
元数据记录基线 46ea9b2 加当前工作区；没有宣称这些文件采自提交后的干净版本。

补充检查：

- bootstrap 重新配置通过，DK_BUILD_MEMORY=OFF、DK_ENABLE_PROFILING=OFF，不自动安装 mimalloc/Tracy；
  仅配置，不重跑既有 bootstrap 测试。日志 out/profiling/memory-bootstrap-configure.log。
- dumpbin /dependents 确认 OFF memory probe 含 mimalloc-debug.dll、无 TracyClient；ON 含 mimalloc.dll
  与 TracyClient.dll。证据 out/profiling/memory-dependents-off.log、memory-dependents-on.log。
- vcpkg mimalloc 构建缓存确认 MI_OVERRIDE=OFF；无注入或全局分配替换。
- 独立检查器 Release 构建通过，最终 /utf-8 构建无编译警告；见 inspector-build-utf8.log。
- README 的 heap_example 原文提取到 out/memory-readme-example，以 /W4 /WX Debug 单独编译链接，
  实际执行返回 closed；该目录保留 configure/build 日志。
- check-spec 对本次 11 个 Markdown 文件检查通过（264 个本地链接、时间戳、开发编号和 JSON 清单）；
  git diff --check 通过，未以文档检查替代编译/运行验证。

## 发现并修复的问题

首次构建（out/verify/20260923-171507-fc7d2000）因 mimalloc 版本宏编码判断错误失败，测试未运行。
安装头的 MI_MALLOC_VERSION 为 30503（major × 10000 + minor × 100 + patch），已修正 v3 检查并通过后续构建。
独立 TracyServer 配置发现 PPQSort 需要 Threads::Threads，补充 find_package(Threads) 后通过。
Windows 默认代码页对上游 Unicode 注释产生 C4819，设置 /utf-8 并重建后无警告。
首次内存采集已通过，但 summary 的嵌套 JSON 超过 PowerShell 默认深度；改为 Depth 8 后重新采集，
最终报告完整保留 pool 明细。上述首次失败/告警不作为通过证据替代品。

## 限制与下一项

未运行全引擎回归、其他平台/静态 triplet、ASan/TSan、GUI viewer 验收、非零调用栈深度或性能基准；
本节证明行为与采集正确性，不宣称吞吐提升。无需改变 M1–M3 公共容器 ABI，现有库自有分配不自动纳管。
PMR、Buffer、智能指针、自动路由、arena/pool、ThreadContext/Jobs 和 logical 曲线仍未实现。

M1.7.2 独立本地提交，下一节为 M1.7.3 拥有型分配器、PMR、Buffer 和持久域路由。
M1.7 总阶段仍进行中；下一开发记录编号 0025，开工时重新扫描确认。
