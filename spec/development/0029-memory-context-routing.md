---
id: "0029"
created_at: "2026-09-28T09:04:00+08:00"
updated_at: "2026-09-28T09:29:06+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
  - ../design/foundation-profiling.md
---

# 0029 M1.7.6 线程上下文、拥有型路由与关闭集成

基线 bc1d87a，初始工作区干净。应用 spec-workflow、state-contracts、build-verify。
先细化 [Memory 设计](../design/foundation-memory.md)，实现 ThreadContext local pool 装配、
RoutingToken 和 owner 线程缓存，再以标准线程验证路由恢复、多系统关闭与延迟释放。
不新增依赖/命令，不提前实现 M4 Jobs、Runtime 自动装配或 M1.7.7 性能基准。

## 状态与提交点

token 保活系统 control/持久资源，不阻止 Closing；登记 context 与系统关闭共用共享状态中的 mutex。
context 配置候选构造完成后才发布缓存项；失败不留 lease 或改变 TLS。
退休在 owner 线程安全点逐项执行，busy 项不变；清理局部 backing 后才解除 lease。
系统不可重开且 ID 不复用，不添加冗余 generation；token 不绑定缓存项的生命周期。

## 实现与文件范围

- [Context.hpp](../../engine/foundation/memory/include/dk/memory/Context.hpp) 与
  [Context.cpp](../../engine/foundation/memory/src/Context.cpp)：新增 ThreadContextOptions、local_pool/current_local_pool、
  RoutingToken capture/from_resource/validate、ExecutionScope token 入口及 ThreadContextCache。
  cache 以系统身份复用稳定地址的 context；配置冲突拒绝，clear/退休报告 retired/busy，错线程不改变状态。
- [MemorySystem.cpp](../../engine/foundation/memory/src/MemorySystem.cpp) 把登记 mutex 放进共享 SystemState，
  context 的直接/缓存构造共用登记路径；释放 local pool/scratch 后才减少 active_contexts。
  [Resource.hpp](../../engine/foundation/memory/include/dk/memory/Resource.hpp) 仅允许 token 取得所属共享 control，
  没有扩大 heap handle 的分配语义，也没有进程全局路由或 owner TLS 缓存。
- [ContextTests.cpp](../../engine/foundation/memory/tests/ContextTests.cpp) 的 14 个用例验证多域/系统重绑定、
  submitter/worker/包装对象生命周期、异常/取消恢复、配置失败、局部分配 OOM、缓存 busy/退休/重建、
  SharedPool 结果与 weak 延迟释放，以及 32 次通过 barrier/latch 协调的登记/关闭竞争。
- [ContextProbe.cpp](../../tests/integration/ContextProbe.cpp) 在 std::jthread 复用两个系统，A 关闭后 B 继续执行，
  worker 退出后释放 A 的 Buffer/shared/weak，再关闭 B；两处 CMake 注册新增测试与探针。
- [采集脚本](../../scripts/capture-profiling.ps1) 与
  [inspector](../../tools/profiling/inspector/MemoryTraceInspector.cpp) 新增 context/context-disabled 模式，
  比较独立 heap 计数、事件配对和曲线；不改 profiling 事件适配层，不为局部对象重复记 heap alloc。

同步 README、Memory/Profiling/Jobs/架构设计、索引、Roadmap、三方库现状与工具说明。
Jobs 设计修正了“Memory 尚未实现”的旧描述；Jobs 调度仍未实现。

## 实际失败与修复

首轮新用例 11/12 通过（`out/verify/20260928-091034-7e25ee08`）。故障注入覆盖到 MSVC Debug
vector 的 noexcept 构造期 proxy 分配导致终止；调整为先构造容器再注入增长到新 chunk 的失败，
验证标准可恢复的 resize 失败及随后重试。首次 probe 编译因错误 API 名称失败，修正为仓库已有接口。
probe 编译失败证据为 `out/verify/20260928-091307-100065ef`，未运行测试。
沙箱内尝试因 vcpkg 外部 buildtrees/FileTracker 权限失败，未运行测试；按原配置授权重试，无工具链或选项降级。
对应摘要为 `out/verify/20260928-091017-669d5393`、`20260928-091239-444aa58f`、`20260928-091546-fa3595ea`。

首次真实 capture `out/profiling/20260928-092100-62075aa7` 的 idle-backing 检查失败：probe 只在对象存活时采样，
下一次采样已在退休释放之后，因此未观测到中间空闲保留量。增加对象释放后的显式安全点采样；
不放宽 inspector 的检查条件。修正后的三个配置 probe 和两种真实采集均通过。

## 定向验证结果

Windows x64、VS 2026 / MSVC 19.51、CMake 4.2.1；warnings-as-errors 开启。
共享登记 mutex/context 销毁影响 heap/ownership/arena/pool，因此 Debug 回归选中整个 Memory 链路；
Tracy 条件代码与真实采集使用已有 RelWithDebInfo profiling/CPU-only 配置，不运行全引擎双配置。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_context_probe','dk_memory_probe','dk_arena_probe','dk_pool_probe') -TestRegex '^dk\.memory\.' -Reason 'Shared SystemState registration mutex and ThreadContext destruction affect heap ownership scratch pool and new routing integration'
& ./scripts/verify.ps1 -Target dk_memory_tests -TestRegex '^dk\.memory\.context (cooperative|shared)' -Reason 'Added cancellation restoration and shared-pool result retirement cases in Debug'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_memory_tests','dk_context_probe') -TestRegex '^dk\.memory\.context' -Reason 'M1.7.6 optimized Tracy-enabled token cache tests and multi-system capture probe'
& ./scripts/verify.ps1 -Target dk_context_probe -TestRegex '^dk\.memory\.context_probe$' -Reason 'Verify final context probe sampling in Debug'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_context_probe -TestRegex '^dk\.memory\.context_probe$' -Reason 'Sample local pool idle backing explicitly before context retirement in capture probe'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_context_probe -TestRegex '^dk\.memory\.context_probe$' -Reason 'Verify final context probe with memory profiling disabled'
cmake --build out/profiling-tools/inspector --config Release --target dk_memory_trace_inspect --parallel 4
pwsh -NoProfile -File scripts/capture-profiling.ps1 -Mode context -Port 18093
pwsh -NoProfile -File scripts/capture-profiling.ps1 -Mode context-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18094
```

| 检查 | 结果 | 本地证据（out 不入 Git） |
| --- | --- | --- |
| Debug Memory 回归，含最初 12 个新增用例/4 个 probe | 79/79 通过，0 失败/跳过 | `out/verify/20260928-091606-c05b4c98` |
| Debug 补充协作取消、SharedPool 结果组合 | 2/2 通过 | `out/verify/20260928-092031-b1cef4be` |
| Tracy ON 14 个新增用例与 context probe | 15/15 通过 | `out/verify/20260928-091750-8a5a7fe8` |
| 最终采样 probe：Debug / ON / CPU-only | 各 1/1 通过 | `out/verify/20260928-092246-9185adbd`、`20260928-092202-cac8ef02`、`20260928-092247-642b7ae2` |
| 真实 Tracy Memory ON capture/readback | 11 次申请全部配对，3 次异线程 free，8 条曲线 | `out/profiling/20260928-092247-750c0acb` |
| CPU ON / Memory OFF capture/readback | 0 内存事件/0 曲线；CPU 区间保留 | `out/profiling/20260928-092324-d1574ea9` |

Debug 的独立测试覆盖合计 **81 项**，来自 79 项回归和后加 2 项；probe 采样修正后单独重验，
不把重复测试相加。最终 ON/OFF capture 均为 15 个 CPU 区间、2 个线程。
ON 的 assets/scene/jobs 分别 2/3/6 次申请，累计 104/304/2384 字节，最终 live 为 0。
scratch used/backing/retained 峰值为 128/2048/2048 字节，local pool live/backing/idle 为 4/336/336 字节；
当前曲线归零，sampled-peak 保留 128/4。这些是当前 STL/负载的观测，不是布局保证或速度结论。

更新后的 inspector 另读回既有 Pool capture `out/profiling/20260924-101851-38a72924`，
旧模式检查通过；这是工具兼容验证，不是重新采集旧客户端。
`check-spec.ps1 -Path` 限定本次 11 个 Markdown，306 个本地链接、时间戳、开发编号及 JSON 清单检查通过；
`git diff --check` 通过。

## 限制与交接

未运行全引擎测试、其他平台、ASan/TSan、非法析构 death tests、吞吐基准或 Tracy GUI 人工检查。
Readme 新增代码为入口片段，完整可执行示例以已编译/运行的 ContextProbe 为准。
context 及局部借用仍不可跨线程迁移；缓存无自动后台清理，宿主必须在 worker 安全点调用退休或退出 worker。
系统 ID 不复用且不可重开；未来支持重开时须引入系统 generation，当前 token 不引用缓存代际。

M1.7.6 完成；M1.7 仍进行中，下一项 **M1.7.7 重复工作负载与性能基线**，下一记录编号 0030（开工前重查）。
