---
id: "0023"
created_at: "2026-09-23T16:14:00+08:00"
updated_at: "2026-09-23T16:48:00+08:00"
status: completed
design_refs:
  - ../design/foundation-profiling.md
  - ../design/project-foundation.md
---

# 0023 M1.7.1 Tracy CPU 性能分析

## 目标与范围

从 00f5c26 开始，按 M1.7 每次一个小节的约定实现 M1.7.1。
接入 dk::profiling、ON/OFF 构建、调用处 CPU zone/线程命名、Runtime/IO 埋点和真实 capture。
Memory、域路由、arena/pool 与内存事件留到后续小节，本次不预建这些能力。

## 设计与依赖核验

采用 [Profiling 设计](../design/foundation-profiling.md)。
2026-09-23 本次查询官方 vcpkg master 为 `33d78c1ed898a06938f31312167c7abefd229455`，
抓取并直接读取该提交的 baseline 与 port；Tracy 为 0.14.1，两份清单已固定该基线。
旧固定基线 67b9e21f 的 Tracy 实际为 0.13.1#1；0022 初次设计误把本机较新的工作树 port
当作固定基线内容，旧“两个基线版本一致”结论在此更正。mimalloc 仍为后续选型，不在本次安装。
同版本最小 overlay 保留官方原 port/补丁，只为 client 显式启用 TRACY_ENABLE；工具使用独立清单。
对比旧/新 baseline：此前已集成七个库、M4 选型和清单预留的版本均未变；Tracy 从 0.13.1#1
变为 0.14.1，工具侧 ppqsort 从 1.0.6 变为 1.0.6#1。仅 fetch 官方索引，没有切换本机 vcpkg checkout。
overlay 内七个原始 port/patch 文件先以 git blob hash 对照官方固定提交，只有 portfile.cmake
新增一行 `-DTRACY_ENABLE=ON`；保留 MIT port 许可证及升级移除条件。

## 实际实现

- dk::profiling 开启时链接 Tracy::TracyClient，关闭时为无依赖的头文件空操作；宏参数不求值。
- DK_PROFILE_ZONE/TEXT/VALUE/FRAME 保留调用处位置；线程名与连接状态支持受控采集探针。
- windows-profiling 复用开发模块集合，采用 RelWithDebInfo；不修改已有命令和协议。
- 测试探针支持有界等待连接、命名 worker、嵌套 zone 与异常退栈；真实 capture 用匹配 csvexport 读回。
- 默认构建验证无 Tracy 链接/依赖、无参数副作用；ON 验证 CPU zone 和受影响 runner/IO 链路。

代码入口：[Profiler.hpp](../../engine/foundation/profiling/include/dk/profiling/Profiler.hpp)、
[Profiler.cpp](../../engine/foundation/profiling/src/Profiler.cpp) 和 [模块 CMake](../../engine/foundation/profiling/CMakeLists.txt)。
宏保留调用处 SourceLocation 和 RAII 生命周期，静态名称不逐次创建；动态文本只求值一次，
延长临时 string 到复制结束，空文本不发事件，超长文本截到 Tracy 的 65534 字节上限。
ZONE/FRAME 名称必须具有静态生命周期；普通函数调用不承诺禁用时消除实参计算。
线程内嵌套用词法子块，不允许跨线程析构同一 zone。

[CMake 选项](../../cmake/Options.cmake) 默认关闭分析，调用栈深度默认 0、合法范围 0–64。
ON 客户端导出宏必须包含 TRACY_ENABLE、TRACY_ON_DEMAND、TRACY_NO_CRASH_HANDLER，
不通过只给消费方定义宏掩盖依赖禁用问题。OFF target 为不含 Tracy 的 INTERFACE。
Runtime、IO 和 runner PRIVATE 链接；一个进程共享 vcpkg 的 TracyClient DLL，不按 Runtime 新建 client。
VS 分组继承 Engine/Foundation 和 Tests。内存追踪开关尚未实现，不预置空效果选项。

[ProfilingProbe](../../tests/integration/ProfilingProbe.cpp) 覆盖线程命名、128 个嵌套工作区间、
异常退栈、frame 宏、动态文本边界与断开后继续运行；无参数时不等 viewer，--capture 有界等连接。
[禁用探针](../../tests/integration/ProfilingDisabledTest.cpp) 在 ON 配置也独立编译，
不继承 Tracy include/link，验证副作用和仅供启用模式使用的未声明表达式全部被消除。
[采集脚本](../../scripts/capture-profiling.ps1) 用匹配的 capture/csvexport 启动本地受控进程，
保存 capture、CSV、进程日志及带时间/提交/工作区/配置的 summary，失败会停止自建进程。
工具依赖与引擎独立，未安装 GUI。

## 验证记录

环境：Windows x64、VS2026/MSVC 19.51、CMake 4.2.1，vcpkg x64-windows。
ON 使用 RelWithDebInfo + /WX；OFF 使用 windows-dev Debug。
由于宏条件编译与依赖图不同，本节需要 OFF/ON，但只验证 profiling 与直接受影响链路。

配置与工具安装成功（日志在 out/profiling/configure-on.log、configure-off.log、tools-install.log）：

```powershell
cmake --preset windows-profiling -DDK_WARNINGS_AS_ERRORS=ON
cmake --preset windows-dev -DDK_ENABLE_PROFILING=OFF
& "$env:VCPKG_ROOT/vcpkg.exe" install --x-manifest-root=tools/profiling --x-install-root=out/profiling-tools/vcpkg_installed --overlay-ports=cmake/vcpkg-ports --triplet=x64-windows --host-triplet=x64-windows
```

客户端导出宏实际包含 TRACY_ENABLE、TRACY_ON_DEMAND、TRACY_NO_CRASH_HANDLER、TRACY_IMPORTS。
工具端 capstone 5.0.9、zstd 1.5.7、ppqsort 1.0.6#1 安装成功，未引入引擎客户端清单。

定向验证命令（从仓库根目录执行；初次 ON 的 TestRegex 为 `^dk\.(profiling\.|runtime\.|bootstrap\.version$|io\.)`）：

```powershell
$env:TRACY_ONLY_LOCALHOST = '1'
$env:TRACY_ONLY_IPV4 = '1'
$env:TRACY_NO_EXIT = '0'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_profiling_probe','dk_profiling_disabled_test','dk_run','dk_io_tests') -TestRegex '^dk\.(profiling\.|runtime\.|bootstrap\.version$|io\.(binary files roundtrip|atomic save creates))' -Reason '最终包装头与直接 Runtime/IO 链路'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration Debug -Target @('dk_profiling_probe','dk_profiling_disabled_test','dk_run','dk_io_tests') -TestRegex '^dk\.(profiling\.|runtime\.|bootstrap\.version$|io\.(binary files roundtrip|atomic save creates))' -Reason 'OFF 无副作用及协议/IO 路径'
pwsh -NoProfile -File scripts/capture-profiling.ps1
```

以下目录均相对 `out/verify/`；不把重复执行累计为新的独立测试数：

| 证据目录 | 范围 | 结果 |
| --- | --- | --- |
| 20260923-162155-b93c229e | 初次 ON，profiling、runner/Runtime 与 IO；32 项 | 31 通过、1 既有符号链接权限跳过、0 失败 |
| 20260923-162653-526e0f45 | OFF，2 profiling、4 runner/Runtime、2 IO；8 项 | 8 通过 |
| 20260923-163017-4b3059c1 | ON，工作区间计时修订后的 2 profiling | 2 通过 |
| 20260923-163220-48b3f665 | OFF，新增文本边界后的 2 profiling | 2 通过 |
| 20260923-163221-e2e47f47 | ON，最终动态文本包装头与 8 项直接链路 | 8 通过 |
| 20260923-163555-96e57ca5 | ON，仅最终异常探针的 smoke | 1 通过 |
| 20260923-163733-8fd5d9e0 | OFF，仅最终异常探针的 smoke | 1 通过 |

最终真实采集 `out/profiling/20260923-163722-ae040c43/`：
`cpu.tracy` 可由 Tracy 0.14.1 csvexport 读回；summary=passed，**134 个 CPU 区间、2 个线程 ID**。
其中 128 个 Probe.Step、Worker/Capture 嵌套调用、Exception 已闭合；位置均指向探针调用行，
主区间文本、临时 80 字节字符串、空文本和 65536→65534 字节截断均匹配。
客户端断开后输出 checksum=8128 并正常退出；普通 smoke 无 viewer 也正常退出。
窗口/GUI 未参与验收；CSV 不导出线程名称，未声称验证过 viewer 的名称显示。

dumpbin /DEPENDENTS 检查实际 EXE：OFF runner/probe 和 ON 构建内的独立 disabled test 均无
TracyClient.dll，ON runner 含该依赖；OFF vcpkg status 无 Tracy。
证据在 `out/profiling/linkage-check.log`。协议进程测试保持 stdio JSON 行、批处理及错误退出行为。

### 验证期间发现并解决

- 首次两次 capture 超时：localhost 的默认监听落在 IPv6 ::1，而工具连接 127.0.0.1；
  preset/脚本补充 TRACY_ONLY_IPV4=1。失败 summary 在 out/profiling/20260923-162653-7bc891a5
  和 20260923-162807-f64d8025。
- 初始算术区间太短，原始 capture 有 131 区间，但 csvexport 的统计重建省略了零时长区间，
  Probe.Step 只导出 126。核对上游 ReconstructZoneStatistics 后为探针加入受控等待。
- 新增文本用例后 2 秒 capture 在最后的源码名称查询完成前断开，导出出现 `???`；
  Windows sleep 按调度粒度取整，128 次等待约 2 秒，现改为 5 秒 capture 以完成元数据查询。
  最终验证全部通过；受控等待只属于探针，不在引擎热路径，也不能当性能基准。
- 最终动态文本边界检查同时覆盖临时 string 生命周期，避免包装为 string_view 后临时对象提前销毁。

最终定向 check-spec.ps1 检查 15 个 Markdown、269 个本地链接、时间/开发编号和 JSON 清单通过；
工具 manifest 另行成功解析，根/工具 baseline 一致。git diff --check 通过。
暂存后检查曾将上游补丁的空上下文行（单个空格）判为尾随空白；未改写补丁，
仅在 .gitattributes 为 cmake/vcpkg-ports/tracy/*.patch 保留 LF 并允许这类 diff 上下文/末尾空行。
补丁 blob 内容与固定官方提交一致，源码/文档规则保持不变；最终暂存 diff 检查通过。
生成的 slnx 中 dk_profiling 位于 Engine/Foundation，两项 profiling 探针位于 Tests，未修改生成文件。
没有运行全引擎测试、GUI 或性能 benchmark。

## 下一步

M1.7.1 完成并独立本地提交；父阶段 M1.7 仍进行中。
下一节 **M1.7.2**：mimalloc v3 heap、域/ResourceHandle、对齐/预算/关闭闸门及 Tracy backing 事件。
开始时重新核验 vcpkg 最新基线；开发编号取当时最大值加一（当前预计 0024）。
本节未验证 Linux/macOS、GPU、内存事件、非零调用栈深度或性能开销，均不计入本节结果。
