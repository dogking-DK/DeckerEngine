# Profiling 独立工具

此目录不加入引擎的 add_subdirectory。引擎仅链接 Tracy client；capture、csvexport 和
内存检查器单独构建。使用与 client 匹配的 Tracy 0.14.1，基线/overlay 与根清单一致。

## 安装和构建

从项目根目录执行，先将 VCPKG_ROOT 设置为本机 vcpkg 路径：

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install --x-manifest-root=tools/profiling --x-install-root=out/profiling-tools/vcpkg_installed --overlay-ports=cmake/vcpkg-ports --triplet=x64-windows --host-triplet=x64-windows
```

默认 CPU 采集只需要上述工具。内存采集还需要本目录的 inspector：Tracy 0.14.1 的 csvexport
不导出逐分配数据，检查器使用 TracyServer 读回 `.tracy` 内的地址配对、大小、分类和线程。
它依赖该版本的内部读取 API，升级 Tracy 时须同步检查器并重新验证。

安装过程在 vcpkg buildtrees 中准备应用过补丁的源目录。列出候选后，将实际目录填入变量：

```powershell
Get-ChildItem -LiteralPath "$env:VCPKG_ROOT/buildtrees/tracy/src" -Directory -Filter 'v0.14.1-*.clean'
$tracySource = 'C:/path/to/vcpkg/buildtrees/tracy/src/v0.14.1-xxxxxxxxxx.clean' # 替换为上一步实际目录
$toolInstall = (Resolve-Path out/profiling-tools/vcpkg_installed).Path
cmake -S tools/profiling/inspector -B out/profiling-tools/inspector -G 'Visual Studio 18 2026' -A x64 "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_MANIFEST_MODE=OFF "-DVCPKG_INSTALLED_DIR=$toolInstall" -DVCPKG_TARGET_TRIPLET=x64-windows "-DDK_TRACY_SOURCE_DIR=$tracySource"
cmake --build out/profiling-tools/inspector --config Release --target dk_memory_trace_inspect --parallel 4
```

二进制缓存命中时可能没有 buildtrees 源码；这种情况需在独立工具安装树中从源构建该 port，
或指向另一个由相同 overlay 准备的源目录。不要把未应用 port 补丁的上游 checkout 当作等价输入。
检查器构建固定版本并使用 MSVC UTF-8 编码；无 GUI 或 viewer 依赖。

## 采集模式

先配置、构建所需探针。每个构建目录内串行执行配置/构建：

```powershell
cmake --preset windows-profiling -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_memory_probe', 'dk_profiling_probe') -TestRegex '^dk\.(memory\.probe|profiling\.smoke)$' -Reason '有界采集探针'
& ./scripts/capture-profiling.ps1 -Mode cpu
& ./scripts/capture-profiling.ps1 -Mode memory

cmake --preset windows-profiling -B out/build/windows-profiling-cpu-only -DDK_PROFILE_MEMORY=OFF -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_memory_probe -TestRegex '^dk\.memory\.probe$' -Reason 'CPU 开启且关闭内存事件'
& ./scripts/capture-profiling.ps1 -Mode memory-disabled -BuildDir out/build/windows-profiling-cpu-only
```

| Mode | 预期检查 |
| --- | --- |
| cpu（默认） | 134 个 CPU 区间、2 个线程，保留原有源码位置/动态文本检查 |
| memory | 36 次申请全部配对，assets 35 次/2241 字节、scene 1 次/32 字节，2 次异线程释放，0 个活块 |
| memory-disabled | 同一 heap 工作负载完成 36 次申请，capture 中内存事件为 0 |
| arena | 102 次临时申请只产生 jobs 3 次/3200 字节 chunk 事件，全部配对；4 条 scratch 曲线的峰值/结束值及 Memory 格式正确 |
| arena-disabled | 同一 arena 工作负载完成，capture 中内存事件和 scratch 曲线均为 0 |
| pool | 192 次局部/共享对象申请；backing 事件与独立 probe 计数一致并全部配对；八条曲线格式/峰值/结束值匹配 |
| pool-disabled | 相同池工作负载完成，无内存事件和 pool 曲线，保留 CPU 区间 |

M1.7.4 的双线程 arena 探针单独构建/采集：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_arena_probe -TestRegex '^dk\.memory\.arena_probe$' -Reason 'Arena 采集探针'
& ./scripts/capture-profiling.ps1 -Mode arena
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_arena_probe -TestRegex '^dk\.memory\.arena_probe$' -Reason 'Arena 内存采集禁用路径'
& ./scripts/capture-profiling.ps1 -Mode arena-disabled -BuildDir out/build/windows-profiling-cpu-only
```

`dk/scratch/used` 峰值 2912、retained 峰值 1024、backing 峰值 3200，三个当前量最终归零；
`dk/scratch/sampled-peak` 最终保留 2912。后者为安全点采样的总 used 历史峰值，非并发瞬时精确值。
检查器额外参数 `arena` 选择此负载；默认仍验证 M1.7.2 的 heap 探针。
两种 arena 模式还读回 8 个 CPU 区间/2 个线程，覆盖 Probe、Grow、Rewind 和 Reset；内存采集关闭不关闭 CPU zone。

M1.7.5 Pool 采集：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target dk_pool_probe -TestRegex '^dk\.memory\.pool_probe$' -Reason 'Pool 采集探针'
& ./scripts/capture-profiling.ps1 -Mode pool
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling-cpu-only -Configuration RelWithDebInfo -Target dk_pool_probe -TestRegex '^dk\.memory\.pool_probe$' -Reason 'Pool 内存采集禁用'
& ./scripts/capture-profiling.ps1 -Mode pool-disabled -BuildDir out/build/windows-profiling-cpu-only
```

池的标准库布局不写死在检查器中；脚本保存 `pool-measurements.json`，作为 inspector 的 `pool <测量文件>` 参数。
测量含上游申请次数/累计字节、采样 backing/idle-backings；对象逻辑峰值固定为 local 512/shared 1024 字节。
检查事件数小于 192 子申请数、全部配对且归零、曲线 Memory 格式与结束值；CPU 区间为实际 Grow 次数加 Probe/Worker/两次 Trim。
MSVC RelWithDebInfo 本次结果为 21 次上游申请、25 区间、2 线程；其他 STL 不要求相同 chunk 数量。

脚本要求 PowerShell 7，默认 localhost IPv4 端口 18086；并行采集必须指定不同 `-Port`。
内存检查器位置可用 `-MemoryInspector` 改写，默认取上述 Release 产物。
所有分配与释放位于一次稳定 on-demand 连接内，不包括启动前存活对象，也不推断晚连接的泄漏情况。
失败预算申请不产生事件；两个 MemorySystem 的同类别在 Tracy 中聚合，域明细使用引擎 snapshot。

结果保存到 `out/profiling/<本次运行>/`：capture、进程日志、summary.json；内存模式附带检查器 JSON。
计数字节是累计申请量，不是峰值或 RSS。该工作负载含受控等待，只验收采集正确性，不是吞吐基准。
M1.7.2 的实际验证与限制见 [0024](../../spec/development/0024-mimalloc-heap.md)。
M1.7.4 的 arena 曲线与禁用验证见 [0026](../../spec/development/0026-scratch-arena.md)。
M1.7.5 的 pool 验证与 MSVC Debug 构造兼容处理见 [0027](../../spec/development/0027-memory-pools.md)。

M1.7.6 context/关闭集成采集：先在 profiling 和 cpu-only 目录分别构建 `dk_context_probe` 的 RelWithDebInfo，
重新构建 inspector，然后运行：

```powershell
& ./scripts/capture-profiling.ps1 -Mode context -Port 18093
& ./scripts/capture-profiling.ps1 -Mode context-disabled -BuildDir out/build/windows-profiling-cpu-only -Port 18094
```

标准线程在同一 Tracy client 内执行两个系统的三次任务。A 进入 Closing 后，在 worker 退休其 context，
B 继续执行；worker 退出后主线程释放 A 的 Buffer/shared/weak 结果，再关闭 B。
probe 的独立 heap 快照计数写入 `context-measurements.json`；inspector 的额外参数为
`context <measurement.json>`，核对三类 backing 计数、全部配对、三次异线程释放、8 条 scratch/local pool 曲线归零。
sampled-peak 保留历史峰值。禁用内存采集时应无内存事件和曲线，CPU 任务/关闭区间仍存在。
这是生命周期验收，性能基线属于 M1.7.7；记录见 [0029](../../spec/development/0029-memory-context-routing.md)。

## M1.7.7 重复负载与基线

[benchmark-memory.ps1](../../scripts/benchmark-memory.ps1) 独立于固定时长的 smoke capture 脚本。
它要求先构建同优化级别的三种 `dk_memory_benchmark`，步骤见 [README](../../README.md#memory-重复工作负载与性能基线m177)。
默认 512 测量轮、32 预热轮、batch 16、三次重复；N 为本机逻辑线程数，运行时可显式覆盖。
OFF 不启动 collector；CPU/Memory 每次建立独立连接，负载结束后排空固定版本 Tracy client，capture 自动结束。
整个测量顺序运行；程序和工具超时/异常时保存失败摘要并清理本次启动的进程，不回退到未连接测量。

inspector 的额外参数为 `benchmark <inspection-input.json>`，输入来自独立资源快照累计计数，
检查总数/分类、全部配对、raw heap/PMR/管线的跨线程释放下界和 12 条局部曲线。
共享池的 backing 线程归属与具体 chunk 数可随 STL 和调度变化，不写死；每次记录和核对实际值。
CPU-only 必须无内存事件和曲线；csvexport 另核对 case/batch/pipeline/import 区间数。
`summary.json` 保存 CPU/OS/内存/电源计划、编译器/优化选项、依赖基线、二进制与 DLL 哈希和每次检查结果。
`baseline.csv` 保存各配置完整矩阵的吞吐中位数/范围、batch 分位延迟、峰值/保留量和相对 OFF 耗时比。
原始文件留 out，版本化报告见 [2026-09-28 基线](../../spec/benchmarks/2026-09-28-memory.md)。
