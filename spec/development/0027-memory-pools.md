---
id: "0027"
created_at: "2026-09-23T19:24:00+08:00"
updated_at: "2026-09-23T19:56:02+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
---

# 0027 M1.7.5 Pool、ObjectPool 与受控 trim

基线 26f9bb7，工作区初始干净。应用 spec-workflow、state-contracts、build-verify。
先细化 Memory 设计，再实现 LocalPoolResource、SharedPoolResource、PoolAllocator/ObjectPool 和观测。
无新依赖，不扩大 ResourceHandle 的 heap 语义；context/任务装配留到 M1.7.6。

## 实现与状态契约

- [Pool.hpp](../../engine/foundation/memory/include/dk/memory/Pool.hpp) 与
  [Pool.cpp](../../engine/foundation/memory/src/Pool.cpp)：稳定地址的 LocalPoolResource/SharedPoolResource，
  分别封装标准 unsynchronized/synchronized PMR pool，mimalloc heap 继续提供受预算约束的 backing。
  提供 PMR、统计、显式采样、Open/Closing/Closed 与受控 trim；零字节归一化、对齐/溢出检查和错误返回。
- [PoolAllocator.hpp](../../engine/foundation/memory/include/dk/memory/PoolAllocator.hpp) 与
  [ObjectPool.hpp](../../engine/foundation/memory/include/dk/memory/ObjectPool.hpp)：共享池 allocator 持有 owner，
  copy/move/swap/rebind 保留资源身份；ObjectPool 支持构造失败回收、allocator-aware 构造及拥有型 deleter。
  SharedObjectPool 的 shared/weak 控制块保活池，允许异线程销毁；局部对象必须先于池并在同线程销毁。
- live 与在途 gate 共同阻止 trim，释放完成后才减少 live；排他维护不能覆盖并发 begin_close。
  Closing 拒绝新申请（含缓存命中），允许已有对象回收；在途操作或活对象使 close 返回 busy，随后可重试。
  构造/申请失败不发布用户块，logical/live 不变；标准池内部保留量可变化，预算仍约束实际 heap backing。
- [Memory profiling adapter](../../engine/foundation/profiling/src/Memory.cpp) 增加 local/shared 的
  live、backing、idle-backing、sampled-peak 八条曲线；显式安全点采样，不逐对象发送内存事件。
  Grow/Trim CPU zone 配合原 heap 事件区分逻辑用量、实际申请与可回收保留量。
- [PoolProbe](../../tests/integration/PoolProbe.cpp)、[采集脚本](../../scripts/capture-profiling.ps1) 和
  [trace inspector](../../tools/profiling/inspector/MemoryTraceInspector.cpp) 增加 pool/pool-disabled 模式，
  使用 probe 独立统计核对 STL 实际 backing，不写死 chunk 大小或次数。

相关边界与使用方式已同步 [Memory 设计](../design/foundation-memory.md)、
[Profiling 设计](../design/foundation-profiling.md)、[README](../../README.md) 和 [Roadmap](../roadmap.md)。
不改变 HeapResource、ResourceHandle、Arena 或现有拥有型 heap API；无命令、协议、依赖版本变更。

## 实际失败与修复

首轮 Debug 构建通过，17 项中 9 通过/8 失败（`out/verify/20260923-193028-a04b9a92`，
其中禁用 profiling 头文件检查通过）：标准 pool 的 release 不释放 MSVC Debug 容器 proxy，
trim/close 后 backing 仍为 16 字节。改为排他维护时销毁整个标准后端，后续申请通过冷路径互斥延迟重建；
`20260923-193316-0dd92439` 的 16 个 Pool 用例通过。

追加重建故障用例后，MSVC Debug 标准 pool 的 noexcept 构造仍向上游申请 proxy，
模拟 OOM 导致进程终止（`20260923-193508-6271f058`：16/17 通过）。
仅在 MSVC iterator Debug 模式，为构造元数据提供 control 内固定 256 字节兼容区；
当前 STL 使用 16 字节，用户块/chunk 仍经过受预算约束的 heap。销毁旧后端后才能复用兼容区。
未关闭迭代器检查或删减故障用例；最终验证覆盖首次申请、trim 后重建失败及恢复。
该区属于 bootstrap control，不计入 heap backing；STL 升级须重跑故障测试，超出兼容区属于不支持的布局。

## 定向验证

环境：Windows x64、VS 2026 / MSVC 19.51、CMake 4.2.1，warnings-as-errors 开启。
仅验证 Pool 及其直接影响的 profiling 路径；追加 RelWithDebInfo 是为覆盖 Tracy 条件编译与实际采集。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_pool_probe') -TestRegex '^dk\.memory\.pool' -Reason 'MSVC Debug constructor proxy bootstrap storage preserves recoverable heap OOM and lazy rebuild semantics'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_memory_tests','dk_pool_probe') -TestRegex '^dk\.memory\.pool' -Reason 'M1.7.5 pool profiling conditional code and capture probe'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_pool_probe -TestRegex '^dk\.memory\.pool_probe$' -Reason 'Pool memory profiling OFF must retain CPU zones with no memory events or plots'
cmake --build out/profiling-tools/inspector --config Release --target dk_memory_trace_inspect --parallel 4
& ./scripts/capture-profiling.ps1 -Mode pool -Port 18091
& ./scripts/capture-profiling.ps1 -Mode pool-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18092
```

| 检查 | 结果 | 本地证据（out 下不入 Git） |
| --- | --- | --- |
| Debug Pool 用例及双线程 probe | 17/17 通过，无失败/跳过 | `out/verify/20260923-193930-f3e031a7` |
| Tracy ON RelWithDebInfo 同组检查 | 17/17 通过，无失败/跳过 | `out/verify/20260923-193929-901f94a0` |
| CPU ON / Memory OFF probe | 1/1 通过 | `out/verify/20260923-194041-0f6ac965` |
| Tracy ON 真实采集与读回 | 通过，25 个 CPU zone、2 个线程、8 条曲线 | `out/profiling/20260923-194042-ae8e5b34` |
| Memory OFF 真实采集与读回 | 通过，25 个 CPU zone、2 个线程，0 内存事件/Pool 曲线 | `out/profiling/20260923-194052-e2b85f5a` |
| README Pool 完整示例 | 独立 Debug 编译通过，运行 exit 0 | `out/memory-pool-example` |

`check-spec.ps1 -Path` 限定本次 10 个 Markdown，287 个本地链接、时间戳、开发编号与 JSON 清单检查通过；
`git diff --check` 通过。文档检查覆盖 README、两个索引、Memory/Profiling/架构设计、Roadmap、三方库说明和工具说明。

16 个新增 [PoolTests](../../engine/foundation/memory/tests/PoolTests.cpp) 覆盖归一化/对齐/溢出、
warm reuse、大块、预算/backend 失败、错误线程、构造回滚、allocator 传播、weak 保活和 busy trim；
并验证四线程申请/异线程释放、并发延迟重建、释放/申请在途时 trim/close，以及维护期间 begin_close 不丢失。
重复配置运行不计为新增用例。

真实采集的 192 次对象申请产生 21 次上游申请，累计 6064 字节；全部释放配对，
其中 8 次 backing 在异线程释放，最终 live bytes 为 0。
local/shared 的 logical 采样峰值为 512/1024 字节，backing 采样峰值为 1600/3176 字节，
idle-backing 采样峰值为 360/976 字节；live/backing/idle 曲线最终归零，历史峰值保留。
这些是该工作负载的观测结果，不是跨 STL 的布局保证或性能提升结论。

更新的 inspector 另读回既有 heap `20260923-173711-7ddcc3df`（36 次申请）与
arena `20260923-191128-94ce8125`（3 次申请）trace，均通过。
这是旧采集文件的工具兼容回归，不是重新采集旧客户端工作负载。

## 限制与下一项

未执行全引擎测试、其他平台、ASan/TSan、误用终止的 death tests 或速度基准；未手动查看 Tracy GUI。
标准 pool 可将大请求直通上游，trim 仅允许零活块/零在途，未实现逐 chunk 回收或自定义 size class。
PMR 原始指针仍为借用；普通对象成员不自动改用池。ThreadContext 装配、自动任务路由、线程退休
与多系统关闭组合场景属于下一项 **M1.7.6**，后续独立工作负载和开销报告属于 M1.7.7。
本节完成，下一开发记录预期为 0028，开工前重新扫描编号。
