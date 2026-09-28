---
created_at: "2026-09-28T16:00:00+08:00"
updated_at: "2026-09-28T16:15:12+08:00"
---

# Memory 与性能分析使用

[返回项目入口](../../README.md)。以下命令均在仓库根目录执行；按当前任务选择相关小节。
构建前提见[构建指南](build.md)，验证范围遵循[定向验证约定](../README.md#开发辅助-skills)。
独立配置和历史验收计数不构成每次修改的固定回归要求。

## Memory heap（M1.7.2）

模块链接 `dk::memory`。底层接口显式持有资源；日常代码可使用下节的拥有型容器和自动持久域路由。
以下函数演示预算、关闭期间释放及关闭重试，返回值供调用方处理：

```cpp
#include <dk/memory/MemorySystem.hpp>

std::expected<dk::memory::CloseResult, dk::memory::AllocationError> heap_example()
{
    namespace mem = dk::memory;
    auto system = mem::MemorySystem::create();
    if (!system) return std::unexpected(system.error());
    auto heap = system->create_heap({"assets", mem::DomainCategory::assets, 1024});
    if (!heap) return std::unexpected(heap.error());
    auto block = heap->try_allocate(256, 64);
    if (!block) return std::unexpected(block.error());

    system->begin_close(); // 拒绝新申请；现有块仍有效
    const auto busy = system->try_close(); // closing，尚有 1 个活块
    (void)busy;
    heap->deallocate(*block, 256, 64); // 原资源及原 size/alignment；允许跨线程
    return system->try_close(); // closed
}
```

原始指针不自动持有资源，必须保留一个 `ResourceHandle` 直到释放；资源可晚于创建线程和
MemorySystem 包装对象析构。`snapshot()` 返回域身份、活块、backing 请求量、峰值、预算和失败次数。
并发快照为近似采样，静止时精确；预算包含在途申请，限制请求字节而非进程 RSS。
零字节申请按 1 字节计费；错误返回固定枚举，失败不产生 alloc 事件。
完整接口与契约见 [Memory 设计](../design/foundation-memory.md)。

定向验证（先配置 windows-dev）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_memory_probe') -TestRegex '^dk\.memory\.' -Reason 'Memory heap 与关闭生命周期'
```

真实内存采集与关闭事件的对照见 [Profiling 工具说明](../../tools/profiling/README.md)；
M1.7.2 记录见 [0024](../development/0024-mimalloc-heap.md)。

## 拥有型内存与持久域路由（M1.7.3）

框架入口绑定一次 ThreadContext 和持久资源，业务函数里的 `dk::Vector`、`dk::String`、
`dk::memory::make_unique/make_shared` 自动捕获该资源。下面是可单独编译链接 `dk::memory` 的完整示例：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>

struct Mesh { dk::Vector<int> indices; };

std::shared_ptr<Mesh> build_mesh()
{
    auto mesh = dk::memory::make_shared<Mesh>();
    mesh->indices = {0, 1, 2};
    return mesh;
}

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto assets = memory->create_heap({"assets", mem::DomainCategory::assets});
    auto scene = memory->create_heap({"scene", mem::DomainCategory::scene});
    if (!assets || !scene) return 2;
    std::shared_ptr<Mesh> result;
    {
        mem::ThreadContext thread{*memory};
        mem::ExecutionScope entry{thread, *assets};
        result = build_mesh(); // 对象/control block 和 indices 都归 Assets
        {
            mem::DomainScope domain{*scene};
            dk::String label(80, 'x'); // 新对象归 Scene
            result->indices.reserve(1024); // 已有容器扩容仍归 Assets
        }
    } // 路由恢复；结果和 allocator 继续持有资源
    result.reset();
    return memory->try_close().closed() ? 0 : 3;
}
```

Runtime 的异步资产服务与 Jobs 已自动装配内存系统；独立调用者仍需绑定作用域。未绑定时隐式接口抛
`ContextError`，`try_current_resource()` 返回固定错误码；不会静默转到全局 heap。
ExecutionScope 支持嵌套另一系统；DomainScope 只切换同系统的持久域。context/scope 必须同线程、
按栈序析构，context 晚于 scope；存活 context 会使系统关闭返回 active_contexts/busy。
容器/Buffer/智能指针的释放无需 TLS，但容器自身的并发读写仍遵守标准库规则。

| 需要 | 接口与边界 |
| --- | --- |
| 显式选择资源 | `Allocator<T>{handle}`、`make_unique_in<T>`、`make_shared_in<T>`；不改写 TLS |
| 资源敏感类型的成员 | 声明 allocator_type，按 uses_allocator 构造；嵌套标准容器使用 scoped_allocator_adaptor，示例见 [测试](../../engine/foundation/memory/tests/OwnershipTests.cpp) |
| 跨域复制容器 | `dk::Vector<int> clone{source, mem::Allocator<int>{target}}`；普通复制保留源 allocator |
| 借用 PMR | `std::pmr::vector<int> values{owner.pmr_resource()}`；owner 必须晚于容器析构，跨域复制显式传目标 resource |
| 拥有字节缓冲 | `try_allocate(handle, bytes, alignment)` 返回 expected&lt;Buffer&gt;；try_resize 失败保留原内容，增长需容纳新旧块的预算 |

拥有型 allocator 的 copy/move/swap 均传播资源，移动后源容器可继续使用。普通成员不声明 allocator
感知时，显式 `_in` 只决定对象/control block 的资源，成员默认构造仍从当前作用域捕获；不会反射并改写任意类。
对象构造异常原样传播并释放存储。shared 的最后一个 weak 控制块仍持有资源；unique 的 reset/null 赋值
保留 deleter，销毁或用空 UniquePtr 替换后才释放其句柄。PMR 仅借用，不提供这一自动所有权保证。
`Buffer` 只管理字节，不对任意 C++ 对象执行 realloc；新增长字节未初始化，零长度 Buffer 仍规范化申请 1 字节。

定向验证使用上节 memory 命令；实现与验收见 [0025](../development/0025-memory-ownership-routing.md)。

## 临时内存与自动 ScratchScope（M1.7.4）

入口为线程配置一次 scratch 上游，业务函数使用 `ScratchScope` 和 `scratch_vector<T>()`，无需传入线程/heap。
返回结果仍使用持久域；临时容器必须先于 scope 析构。完整示例：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>
#include <dk/memory/Containers.hpp>
#include <dk/memory/SmartPtr.hpp>

struct Mesh { dk::Vector<int> indices; };

std::shared_ptr<Mesh> build_mesh()
{
    dk::memory::ScratchScope scratch; // 先声明，最后析构并 rewind
    auto temporary = dk::memory::scratch_vector<int>();
    temporary = {0, 1, 2};
    auto result = dk::memory::make_shared<Mesh>();
    result->indices.assign(temporary.begin(), temporary.end());
    return result; // 只移交持久结果
}

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto assets = memory->create_heap({"assets", mem::DomainCategory::assets});
    auto scratchHeap = memory->create_heap({"scratch", mem::DomainCategory::jobs});
    if (!assets || !scratchHeap) return 2;
    std::shared_ptr<Mesh> mesh;
    {
        mem::ThreadContext thread{*memory, *scratchHeap};
        mem::ExecutionScope entry{thread, *assets};
        mesh = build_mesh();
        auto usage = thread.scratch().snapshot();
        if (usage.used_bytes != 0) return 3; // 空闲 chunk 可以留待复用
    } // context 归还缓存；mesh 继续持有 Assets
    if (mesh->indices.size() != 3 || mesh->indices.back() != 2) return 4;
    mesh.reset();
    return memory->try_close().closed() ? 0 : 5;
}
```

`ScratchOptions{chunk_bytes, max_retained_bytes}` 默认 64 KiB/1 MiB。嵌套 scope 只回收内层申请，
普通 chunk 在上限内复用，超过普通 chunk 大小的申请退出时立即归还。`try_reset()` 仅在无活动 scope 时
清空缓存并更新 generation；`try_checkpoint()/try_rewind()` 提供显式、同线程、严格 LIFO 的底层接口。
`ScratchScope{arena}` 不修改 TLS，显式 PMR 容器使用 `scope.resource()`；直接 raw/PMR 申请也要求活动 checkpoint。
`DomainScope` 不切换 scratch 上游；新的 `ExecutionScope` 不继承外层 ScratchScope。
保留原 `ThreadContext{system}` 用于仅持久分配；它不会自动创建 scratch。
同一 arena 的分配归当前最内层 checkpoint；不要在内层 scope 扩容一个需要在外层继续使用的 scratch 容器，
因为新缓冲会随内层回退。需要跨作用域保留的数据先复制到持久容器。

`snapshot()` 的 used 包含对齐 padding，retained 只计完全空闲 chunk，backing 计全部 chunk 容量；
三者不能相加当总占用，也不等于 RSS。关闭系统后拒绝新申请，清理和释放仍可执行。
Tracy 只记录 chunk 的 alloc/free，用量曲线在作用域边界、增长、reset 和显式 `sample()` 处采样。
`dk/scratch/sampled-peak` 是进程内采样总 used 的历史峰值；精确的单 arena 峰值保存在 snapshot 中。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_arena_probe') -TestRegex '^dk\.memory\.arena' -Reason 'ScratchArena 定向验证'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_arena_probe -TestRegex '^dk\.memory\.arena_probe$' -Reason 'Scratch 用量采集探针'
& ./scripts/capture-profiling.ps1 -Mode arena
```

采集前准备匹配版本的独立工具，见 [Profiling 工具](../../tools/profiling/README.md)。
实现与验收见 [0026](../development/0026-scratch-arena.md)。Pool 用法见下节；
任务 token、worker 缓存和既有 Runtime/Jobs 自动装配仍待实现。

## Pool 与对象复用（M1.7.5）

`LocalPoolResource` 供同线程反复创建/销毁小对象，`SharedPoolResource` 支持并发申请与异线程释放。
前者由调用方保持存活，后者为可复制的拥有型句柄；两者都持有固定 heap 上游。
`ObjectPool<T>` 提供局部 unique 对象，`SharedObjectPool<T>` 提供拥有型 unique/shared 对象：

```cpp
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/ObjectPool.hpp>
#include <thread>

struct Particle { int value; explicit Particle(int n) : value(n) {} };

int main()
{
    namespace mem = dk::memory;
    auto memory = mem::MemorySystem::create();
    if (!memory) return 1;
    auto heap = memory->create_heap({"particles", mem::DomainCategory::scene});
    if (!heap) return 2;
    std::shared_ptr<Particle> result;
    std::weak_ptr<Particle> weak;
    {
        mem::LocalPoolResource local{*heap, {32, 256}};
        mem::ObjectPool<Particle> localObjects{local};
        { auto particle = localObjects.make(7); if (particle->value != 7) return 3; }
        if (!local.try_trim()) return 4; // 活对象已经销毁，归还缓存；后续还能重用

        auto pool = mem::SharedPoolResource::create(*heap, {32, 256});
        if (!pool) return 5;
        mem::SharedObjectPool<Particle> sharedObjects{*pool};
        result = sharedObjects.make_shared(42);
        weak = result;
        if (pool->try_trim()) return 6; // 仍有活对象，trim 应拒绝
    } // 工厂/外观已退出，result 和 weak 控制块继续保有池
    std::jthread consumer{[value = std::move(result)]() mutable { value.reset(); }};
    consumer.join();
    if (!weak.expired()) return 7;
    if (memory->try_close().closed()) return 8; // weak 控制块尚未释放
    weak.reset();
    return memory->try_close().closed() ? 0 : 9;
}
```

局部对象必须在池之前、同一线程销毁；共享 unique 的 deleter 和 shared/weak 控制块拥有池。
`make` 构造失败归还槽位，普通成员不自动改路由；allocator-aware 类型按标准 uses_allocator 传播。
共享容器使用 `std::vector<T, mem::PoolAllocator<T>>` 并显式传入 `PoolAllocator<T>{pool}`；
其复制/移动/swap 传播 pool owner，默认空 allocator 不能申请，也不查询 TLS。
借用 PMR 使用 `pool.pmr_resource()`，调用方需保持 owner；不同池不能相互释放指针。

`try_trim()` 在有活块（含 weak 控制块）或在途操作时返回 busy，不影响已有数据。
成功时清空标准后端，下次申请延迟重建；`try_close()` 先禁止新申请，待对象释放后重试关闭。
Shared 的 try_close 返回 CloseResult；Local 返回 expected&lt;CloseResult, AllocationError&gt;，可报告错线程。
关闭上游同样禁止缓存分配，但已有对象仍可释放。Pool 不会隐式接管现有 dk 容器或持久工厂。

PoolOptions 沿用标准 `{max_blocks_per_chunk, largest_required_pool_block}` 提示，实际值通过 options() 查询；
上游 heap budget 才是申请预算。统计中的 logical_live 是用户请求量，backing 是实际上游占用；
idle_backing 仅在完全空闲时报告保留量，不能当作活跃池的可用槽容量，也不等于 RSS。
`try_sample()` 在安全点发布 local/shared 各四条 Tracy 曲线；忙时返回 busy，不逐对象采集全局曲线。
当前 MSVC Debug 构造期的 iterator proxy 使用 control 内固定保留区，属于 bootstrap 元数据；
标准池 chunk 和运行期元数据继续经过上游，见 [设计与工具链限制](../design/foundation-memory.md#pool-与对象复用)。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_pool_probe') -TestRegex '^dk\.memory\.pool' -Reason 'Pool 定向验证'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_pool_probe -TestRegex '^dk\.memory\.pool_probe$' -Reason 'Pool 采集探针'
& ./scripts/capture-profiling.ps1 -Mode pool
```

独立采集工具准备见 [Profiling 工具](../../tools/profiling/README.md)，验收见 [0027](../development/0027-memory-pools.md)。
ThreadContext 的 local pool 装配、拥有型任务路由与关闭集成见下一节。

## 线程上下文与拥有型任务路由（M1.7.6）

`RoutingToken::capture()` 保存当前持久资源和系统所有权；`from_resource(handle)` 显式选择路由。
token 可复制到另一个线程，不携带提交线程的 arena、pool、context 或 TLS 帧；没有隐式绑定时 capture 抛 ContextError。
worker 显式拥有 `ThreadContextCache`，按系统复用稳定 context，并在入口配置本线程局部资源：

```cpp
// 提交端已有 ExecutionScope；local_heap 是同系统的持久 heap handle。
auto token = dk::memory::RoutingToken::capture();
// 在 worker 自身线程创建 cache，并在每次回调建立以下作用域：
dk::memory::ThreadContextCache cache;
dk::memory::ThreadContextOptions options{
    .scratch_upstream = local_heap,
    .local_pool_upstream = local_heap,
};
{
    auto& context = cache.acquire(token, options);
    dk::memory::ExecutionScope execution{context, token};
    dk::memory::ScratchScope scratch;
    auto temporary = dk::memory::scratch_vector<int>();
    temporary.resize(32, 7);
    dk::memory::ObjectPool<int> objects{dk::memory::current_local_pool()};
    auto local = objects.make(temporary.front());
    // 持久结果使用拥有型容器/Buffer/智能指针，捕获 token 的持久域。
} // 回调结束、异常或协作取消均先析构局部对象，再回退 scratch/路由。
auto retired = cache.try_retire_closed(); // owner 线程安全点，返回 retired/busy；保留 Open 项。
```

完整可执行的双系统示例为 [ContextProbe.cpp](../../tests/integration/ContextProbe.cpp)。
同系统缓存复用要求相同的 `ThreadContextOptions`，配置冲突明确报错；`try_clear()` 可先清理闲置项再换配置。
两种清理都保留有 execution/domain scope、scratch checkpoint 或 pool 活块的 busy 项。
借用 context/resource 引用在清理后失效，所有局部对象必须同线程且早于 cache 析构；cache 不可复制/移动。

token 不阻止系统关闭，Closing 后禁止新的绑定/分配。已移交的 owning 结果及 weak 控制块可在 worker 退出后释放；
释放后重试 `MemorySystem::try_close()`。未执行的取消直接丢弃 token，不创建 context。
框架必须在所属线程安全点退休关闭系统的缓存；Memory 不创建/唤醒/join worker，也不实现 Jobs 调度。

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests','dk_context_probe') -TestRegex '^dk\.memory\.context' -Reason '线程路由、缓存退休与多系统关闭'
& ./scripts/capture-profiling.ps1 -Mode context -Port 18093
& ./scripts/capture-profiling.ps1 -Mode context-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18094
```

采集前在对应目录构建 `dk_context_probe` 的 RelWithDebInfo，以及 [独立 inspector](../../tools/profiling/README.md)。
实现/验证记录见 [0029](../development/0029-memory-context-routing.md)；重复工作负载和性能基线见下一节。

## Memory 重复工作负载与性能基线（M1.7.7）

`dk_memory_benchmark` 使用确定性数据校验 heap、heap PMR、arena、local/shared pool 的成批申请/回收，
并运行 scratch/local pool 临时对象转 owning 容器的跨线程管线。它是独立 CPU 合成负载，不要求 M4 Jobs 或资产导入器。
默认直接运行是快速 smoke；参数为 `--rounds`、`--warmup`、`--batch`、`--max-threads`，
仅用于故障验证的 `--allocation-budget` 限制矩阵 heap，失败必须清理后退出。`--capture` 等待本地 Tracy 连接。

先配置三个同优化级别的目录；已有正确配置可直接构建：

```powershell
cmake --preset windows-dev -DDK_ENABLE_PROFILING=OFF -DDK_PROFILE_MEMORY=ON -DDK_WARNINGS_AS_ERRORS=ON
cmake --preset windows-profiling -B out/build/windows-profiling-cpu-only -DDK_PROFILE_MEMORY=OFF -DDK_WARNINGS_AS_ERRORS=ON
cmake --preset windows-profiling -DDK_PROFILE_MEMORY=ON -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason '优化 OFF 基线及正确性'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'CPU-only 基线及正确性'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_memory_benchmark -TestRegex '^dk\.memory\.benchmark_' -Reason 'CPU+Memory 基线及正确性'
```

按 [工具说明](../../tools/profiling/README.md) 准备匹配的 capture/csvexport/inspector 后运行：

```powershell
pwsh -NoProfile -File scripts/benchmark-memory.ps1 -Rounds 512 -Warmup 32 -Batch 16 -MaxThreads 32 -Repetitions 3
```

`MaxThreads` 替换为本机 N；省略时默认为逻辑处理器数（最多 128）。脚本检查编译器、配置、profiling 开关和输入，
依次执行 OFF/CPU/Memory，并按重复轮次轮换顺序；不自动配置/构建，也不并发运行测量。
每个进程覆盖去重后的 1/2/4/N 线程、32/8、256/64、4096/256 字节/对齐；arena/local pool 仅同线程回收。
三配置逐场景 checksum 必须一致，全部资源最终归零；采集逐次检查 backing 计数、跨线程 free、CPU 区间和曲线。

结果写入 `out/benchmarks/<run>/`：原始 JSONL、trace/CPU CSV、环境与二进制 SHA256、`baseline.csv/json` 和 `report.md`。
吞吐以校验过的逻辑请求计，管线以结果数计；latency 是各 worker 的 batch 分布。
跨线程计时包含 barrier，arena 按批回收；峰值/保留量不等于 RSS。CPU/Memory 实际连接采集，开销含本地采集器竞争。
正式结果与限制见 [基线报告](../benchmarks/2026-09-28-memory.md) 和 [0030](../development/0030-memory-baseline.md)。

## Tracy CPU 性能分析（M1.7.1）

默认 `DK_ENABLE_PROFILING=OFF`，宏不求值参数，不链接或自动安装 Tracy。
专用预设继承 windows-dev 的模块集合，以 RelWithDebInfo 编译并保留符号：

```powershell
cmake --preset windows-profiling -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_profiling_probe', 'dk_profiling_disabled_test', 'dk_run') -TestRegex '^dk\.(profiling\.|runtime\.|bootstrap\.version$)' -Reason 'CPU profiling 与 runner 协议'
```

启用 on-demand 后，连接前的事件不会保留。普通 runner 和测试无需启动 viewer 即可退出；
已有埋点覆盖 Runner.Entry、Runtime.Create/Dispatch、IO.Read/Write/AtomicSave。
手动运行被测程序时先设置下面的进程环境；preset 的环境不会自动传给从其他终端或 VS 启动的进程：

```powershell
$env:TRACY_ONLY_LOCALHOST = '1'
$env:TRACY_ONLY_IPV4 = '1'
$env:TRACY_NO_EXIT = '0'
```

客户端使用 Tracy 0.14.1。独立安装同版本命令行工具，不引入 GUI 依赖到引擎构建：

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install --x-manifest-root=tools/profiling --x-install-root=out/profiling-tools/vcpkg_installed --overlay-ports=cmake/vcpkg-ports --triplet=x64-windows --host-triplet=x64-windows
pwsh -NoProfile -File scripts/capture-profiling.ps1
```

[采集脚本](../../scripts/capture-profiling.ps1) 需要 PowerShell 7，使用本机 IPv4 端口 18086（可用 `-Port` 改写），
在探针连接后运行有界工作负载，断开后验证正常退出；通过 tracy-csvexport 读回 `.tracy`，
检查两个线程、嵌套区间、调用位置、异常退出和动态文本。日志、CSV、capture 与 summary.json
保存在 `out/profiling/<本次运行>/`。这是采集正确性验证，探针有受控等待，不作为性能基准。
交互查看可自行使用同版本 Tracy viewer 打开文件；本次验收使用命令行工具。

模块链接 `dk::profiling` 后使用包装头，公开模板含埋点时需要 PUBLIC 传递依赖：

```cpp
#include <dk/profiling/Profiler.hpp>
#include <string_view>

void import_mesh(std::string_view asset_name)
{
    DK_PROFILE_ZONE("Assets.ImportMesh");
    DK_PROFILE_ZONE_TEXT(asset_name); // 支持临时 string；关闭时连参数表达式也不执行
    // 实际工作；同一词法作用域只放一个 zone，子块可继续嵌套。
}
```

ZONE/FRAME 名称使用静态期字符串；TEXT 立即复制文本，空文本忽略，最多 65534 字节，截断按字节。
线程命名使用 `DK_PROFILE_THREAD_NAME("worker")`；普通 `set_thread_name` 函数的实参仍按 C++ 规则求值。
`DK_PROFILE_CALLSTACK_DEPTH` 默认 0，允许 0–64；按诊断需要增加深度会增加开销，尚无性能基准。
`DK_PROFILE_MEMORY` 默认 ON，仅在 `DK_ENABLE_PROFILING=ON` 时启用 heap backing 事件。
设为 OFF 可保留 CPU 区间而关闭内存事件；GPU 观测尚未接入，Jobs 已提供 Submit/Execute/Publish CPU zone。
依赖使用[最小 Tracy overlay](../../cmake/vcpkg-ports/README.md) 显式启用客户端，配置时核验导出的宏，
避免只编译消费方埋点却链接禁用的 client。详见 [Profiling 设计](../design/foundation-profiling.md)。
