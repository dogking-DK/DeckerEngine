---
id: "0026"
created_at: "2026-09-23T18:51:00+08:00"
updated_at: "2026-09-23T19:14:49+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
---

# 0026 M1.7.4 ScratchArena 与线程临时作用域

基线 f63a330，初始工作区干净。采用 spec-workflow、state-contracts、build-verify。
先细化 Memory 设计，再实现 arena、checkpoint、自动 ScratchScope、保留上限和 Tracy 用量曲线。
沿用 mimalloc/Tracy 版本；不新增命令，不实现 M1.7.5 pool 或 M1.7.6 任务 token。

## 状态与失败保护

chunk 候选完整构造后接入；checkpoint 带身份/代际/序号并按 LIFO 消费；reset 拒绝活动 scope。
作用域先于容器声明，退出前先析构对象再 rewind。context 先清理 scratch，再解除系统 lease。
Closing 拒绝新申请，保留清理路径；显式 arena 不偷偷改变 TLS，ExecutionScope 隔离不同执行入口。

## 实现

- [Arena.hpp](../../engine/foundation/memory/include/dk/memory/Arena.hpp) / Arena.cpp：线程专属 PMR、
  ScratchOptions/Snapshot/Checkpoint/Scope。活动/缓存 chunk 链，64 KiB 默认普通块、1 MiB 空闲保留上限；
  超大块不缓存，缓存块复用不增加 heap 分配。大小/对齐/累计字节溢出检查，零字节规范化为 1。
  token 绑定 arena 身份/generation/序号/深度并消费，reset 仅在无活动 token 时清空缓存/递增代际。
- [Context.hpp](../../engine/foundation/memory/include/dk/memory/Context.hpp) / Context.cpp：
  ThreadContext 可一次性配置同系统 scratch 上游；默认 ScratchScope 自动绑定当前帧，scratch_vector 返回借用 PMR vector。
  DomainScope 继承 scratch，ExecutionScope 隔离前一入口；无配置/上下文/作用域明确抛 ContextError。
  显式 ScratchScope(arena) 不改变 TLS；context 清理缓存后再解除系统 lease。ResourceHandle 增加轻量 state 查询。
- [Memory.cpp](../../engine/foundation/profiling/src/Memory.cpp)：安全点聚合 4 条固定 scratch 曲线，
  per-arena 上次贡献被替换，销毁归零；Memory.Scratch.Grow/Rewind/Reset CPU zone 不覆盖普通 bump。
  上游 heap 是唯一 alloc/free 事件来源。曲线禁用时为空函数；不改变 mimalloc 或 Tracy 的版本/feature。
- [ArenaTests.cpp](../../engine/foundation/memory/tests/ArenaTests.cpp)：15 个用例，覆盖嵌套多 chunk/对齐/代际、
  OOM 与预算回滚、保留/重用/大块释放、PMR 析构顺序、隐式/显式路由、两个系统/线程、Closing 和系统包装销毁。
  测试 sink 确认 100 次子分配只产生一次 backing 事件；不以 sink 替代实际 Tracy 验收。
- [ArenaProbe.cpp](../../tests/integration/ArenaProbe.cpp)：双线程 102 次临时申请，普通块 1024/128 字节、
  内层大块 2048 字节，总计 3 次 backing 申请。屏障协调重叠用量，不靠 sleep 制造测试竞态。
  [采集脚本](../../scripts/capture-profiling.ps1) 增加 arena/arena-disabled，
  [读回工具](../../tools/profiling/inspector/MemoryTraceInspector.cpp) 检查配对、曲线格式、峰值与结束值。
- CMake 导出 Arena.hpp、添加实现/单测和独立 probe；README 加入完整入口/业务示例及借用限制，
  更新模块设计、工具使用、三方库能力说明、索引和 Roadmap。无新命令，无 M1–M3 容器迁移。

单个 PMR deallocate 不回收游标；必须在 rewind 前销毁借用对象。新缓冲始终属于最内层 checkpoint，
外层 scratch 容器不能在内层扩容后继续逃逸使用。结果复制到拥有型容器，智能指针继续走持久域。
元数据和 Tracy 自身存储不计入 backing；used 包括 padding，retained 不包括活动块尾部，三者不是 RSS。
单 arena 峰值精确；全局 sampled-peak 为进程内安全点采样历史峰值，不代表并发瞬时精确值。

## 验证

Windows x64、VS2026/MSVC 19.51、CMake 4.2.1，warnings-as-errors 开启。
Debug 为默认定向验证；因本节新增 profiling 条件编译与真实事件适配，补充既有 ON/CPU-only 构建树的相关探针。
没有全引擎回归；没有新依赖安装或版本更新。

主要命令（记录最后有效代码的结果；辅助增量检查有重叠，不把次数相加当独立用例数）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_memory_probe','dk_profiling_disabled_test') -TestRegex '^(dk\.memory\.|dk\.profiling\.disabled_no_side_effects)' -Reason 'M1.7.4 arena and directly affected context ownership close paths; profiling disabled header'
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_arena_probe') -TestRegex '^dk\.memory\.arena' -Reason 'Final OFF arena paths after adding operation CPU zones'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_memory_tests','dk_arena_probe') -TestRegex '^dk\.memory\.arena' -Reason 'Final arena verification including growth rewind reset CPU zones'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_arena_probe -TestRegex '^dk\.memory\.arena_probe$' -Reason 'Final scratch CPU zones with memory event and plot collection disabled'
cmake --build out/profiling-tools/inspector --config Release --target dk_memory_trace_inspect --parallel 4
& ./scripts/capture-profiling.ps1 -Mode arena -Port 18089
& ./scripts/capture-profiling.ps1 -Mode arena-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18090
```

| out/verify 目录 | 结果 |
| --- | --- |
| 20260923-185931-62aabb35 | 初次 Debug 49/49：15 新 arena、32 既有 heap/ownership、memory probe、禁用头测试 |
| 20260923-190306-ba43f405 | 新 arena probe 与增加 scratch sample 的无 Tracy 头测试，2/2 |
| 20260923-190810-b53364d9 | 最终 Debug arena 单测与 probe，16/16 |
| 20260923-190710-4026025a | 最终 Tracy ON RelWithDebInfo arena 单测与 probe，16/16 |
| 20260923-190706-d0fbc489 | 最终 CPU-only RelWithDebInfo arena probe，1/1 |

以上均无失败/跳过；未发生编译失败。中间 ON/CPU-only 增量结果亦通过，完整日志保存在 out/verify。
最终 capture 证据位于 out/profiling（忽略的本地产物）：

- `20260923-191128-94ce8125`：3 次 jobs chunk 申请/释放全部配对、累计 3200 字节、0 残留；
  used 峰值 2912、retained 峰值 1024、backing 峰值 3200，三个当前量最终均为 0；
  sampled-peak 最终为 2912。四条曲线各 17 个样本、Memory 格式；不是每个子分配一笔 heap 事件。
- `20260923-191130-93b1d7dd`：相同工作负载，内存事件 0、scratch 曲线 0。
- 两种采集均读回 **8 个 CPU 区间 / 2 个线程**：Probe 1、Grow 3、Rewind 3、Reset 1；区间已闭合，源码行有效。

README 的完整 scratch 示例提取至 out/memory-scratch-example；独立 CMake 工程链接本次 dk_memory，
Debug /W4 /WX 编译与执行成功（exit_code=0），日志为该目录 configure.log、build.log、run.log。
示例验证业务无需 memory 参数，临时容器退出释放使用量，持久结果在 context 销毁后仍有效。
check-spec 对本次修改的 Markdown、本地链接、时间戳、开发编号和清单检查通过；git diff --check 通过。

未运行其他平台、ASan/TSan、错误析构 death tests、吞吐/延迟基准或 GUI 视觉检查；
不能从功能/采集通过推断性能提升。性能参数和开销对比归 M1.7.7，pool/任务集成分别归 M1.7.5/6。

## 交接

M1.7.4 完成后独立本地提交；M1.7 仍进行中。
下一项 **M1.7.5 LocalPool/SharedPool、ObjectPool 和受控 trim**，下一可用编号 **0027**。
