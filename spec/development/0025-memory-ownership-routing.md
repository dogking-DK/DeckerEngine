---
id: "0025"
created_at: "2026-09-23T18:23:00+08:00"
updated_at: "2026-09-23T18:39:45+08:00"
status: completed
design_refs:
  - ../design/foundation-memory.md
---

# 0025 M1.7.3 拥有型内存接口与持久域路由

基线 e7d6cc1，初始工作区干净。采用 spec-workflow、state-contracts、build-verify。
先细化 [Memory 设计](../design/foundation-memory.md)，再实现 PMR、Allocator/Buffer、容器别名、
unique/shared 工厂和最小 ThreadContext/ExecutionScope/DomainScope。
无新三方库、命令、既有模块容器迁移；不提前实现 scratch/pool/异步 token。

标准行为核对 [allocator 要求](https://eel.is/c++draft/allocator.requirements)、
[uses_allocator 构造](https://eel.is/c++draft/allocator.uses.construction)、
[memory_resource](https://eel.is/c++draft/mem.res.private)。具体契约以模块设计为准。

## 实现与边界

- [Resource.hpp](../../engine/foundation/memory/include/dk/memory/Resource.hpp) 增加借用 pmr_resource，
  内部 control 实现标准 PMR 接口；不同域不相等，空 handle 使用 null_memory_resource，不切换全局 PMR。
- [Allocator.hpp](../../engine/foundation/memory/include/dk/memory/Allocator.hpp) 保存拥有型 handle，
  默认捕获当前资源，显式构造不依赖 TLS。copy/move/swap 全部传播资源，移动保留源 allocator 可用，
  rebind 同域；溢出抛 bad_array_new_length，分配失败抛 bad_alloc。
- [Containers.hpp](../../engine/foundation/memory/include/dk/memory/Containers.hpp) 提供 dk::Vector/dk::String
  标准容器别名；不用继承包装或替换标准容器算法。跨域 clone 显式指定 allocator/resource。
- [Buffer.hpp](../../engine/foundation/memory/include/dk/memory/Buffer.hpp) 与 Buffer.cpp 实现 move-only 字节所有者。
  try_allocate 返回 expected；try_resize 分配候选、复制、交换后释放旧块。OOM/预算失败保留地址/内容/长度，
  增长需要同时容纳新旧块；零长度仍计费 1 字节，新字节未初始化，不用于重定位任意 C++ 对象。
- [SmartPtr.hpp](../../engine/foundation/memory/include/dk/memory/SmartPtr.hpp) 提供隐式和显式 unique/shared 工厂，
  对象构造异常原样传播且释放存储；uses_allocator 构造支持资源敏感类型、pair 和 const 结果。
  shared 使用 std::allocate_shared，最后 weak 控制块保留 owner；unique 保存原类型释放信息，不允许数组或隐式基类转换。
- [Context.hpp](../../engine/foundation/memory/include/dk/memory/Context.hpp) 与 Context.cpp 提供 ThreadContext、
  ExecutionScope、DomainScope、ContextError 和 try_current_resource/current_resource。无上下文明确拒绝，
  不采用进程全局默认域。作用域验证成功后才发布 TLS，嵌套/异常恢复上一帧；显式工厂不偷偷覆盖 TLS。
- ThreadContext 在 MemorySystem 的冷路径 mutex 下登记，持有共享系统状态；active_contexts 独立报告关闭 busy。
  关闭禁止新登记和隐式工作，存活 context 时系统暂不删除 heap。context 必须晚于 scope 析构且同线程使用；
  在系统包装析构后仍可安全退出。ResourceHandle/Buffer/allocator/智能指针继续独立维持原资源与合法释放。
- CMake 导出本节公开头、编译新增源，测试归入现有 dk_memory_tests。mimalloc/Tracy/vcpkg baseline 不变。
  更新设计/索引/roadmap/README/三方库当前能力说明。PMR、工厂和 Buffer 不重复发送 Tracy 事件。

## 状态与异常保证

Buffer 只在候选成功后交换；unique 构造失败回收其申请，共享对象/控制块由标准 allocate_shared 处理。
PMR 是借用，不能因它保存了指针就认为 owner 存活；对象内 owner 必须比 PMR 容器晚析构。
普通拥有型 allocator 后续扩容和释放只使用捕获资源；切域或换线程不迁移已经存在的容器。
显式 `_in` 的普通成员仍服从自身构造方式，资源敏感类需声明 allocator_type，嵌套容器使用 scoped_allocator。

ThreadContext 注册与关闭串行化；scope 构造拒绝空资源、错系统/线程和 Closing，失败不发布半个帧。
所有 scope 严格 LIFO、不可复制/移动；错序/错线程析构是 terminate 的契约错误，不承诺恢复非法生命周期。
系统进入 Closing 后不会重开，释放不重新查询 TLS；context/system 控制对象与真正 heap 活块分别计数。
当前不缓存/重置 context，使用不复用的 SystemId；generation、RoutingToken 和自动 worker 缓存在 M1.7.6。

## 验证

环境沿用 Windows x64、VS2026/MSVC 19.51、CMake 4.2.1、Debug、DK_WARNINGS_AS_ERRORS=ON。
只验证 memory 及其直接改动的 heap 关闭链路，不运行全引擎或惯例式 OFF/ON/Release 矩阵。

新增 [OwnershipTests.cpp](../../engine/foundation/memory/tests/OwnershipTests.cpp) 的 **17 个用例**：

- PMR 稳定身份/过对齐/显式跨域复制，allocator copy/move/swap/rebind、移动后源容器复用和标准异常。
- Buffer 零长度、对齐、预算/OOM 强失败保证、内容保留、跨域 move 赋值和异线程释放。
- unique/shared 构造异常回收、过对齐、const 结果、异线程析构、最后 weak 控制块延迟回收。
- 无上下文拒绝、两系统/多域嵌套异常恢复、失败不改 TLS、换域/线程后容器保留原资源。
- 隐式普通成员、显式 allocator-aware 与 pair 构造、scoped_allocator 嵌套容器的实际归属。
- 两线程独立路由、错线程入口、context 阻止系统关闭、新登记拒绝、系统包装先析构、backing 事件无重复。

实际命令（首次失败也保留日志，不使用旧产物宣称通过）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_memory_tests', 'dk_memory_probe') -TestRegex '^dk\.memory\.' -Reason 'M1.7.3 owning APIs and heap/context close regression; include affected probe binary'
& ./scripts/verify.ps1 -Target dk_memory_tests -TestRegex '^dk\.memory\.ownership unique and shared' -Reason 'Correct test cleanup to replace the owning deleter rather than assign nullptr'
& ./scripts/verify.ps1 -Target dk_memory_tests -TestRegex '^dk\.memory\.ownership (factories|explicit factories|unique and shared|weak pointer|wrappers)' -Reason 'Verify const-aware placement construction plus factory rollback ownership and event regressions'
```

| out/verify 目录 | 结果与处理 |
| --- | --- |
| 20260923-182831-1111dfe5 | 首次编译失败，未运行测试；过对齐测试类型的隐式 padding 触发 /W4 /WX C4324，改为显式填满测试存储 |
| 20260923-182942-5c033704 | 构建通过，31/32 通过、1 失败；测试误以为 unique = {} 会替换 deleter，实际上选到 nullptr 赋值 |
| 20260923-183027-e9dc88a1 | 改为明确 UniquePtr 空对象移动赋值后，复验 1/1 通过；仅修改测试的 owner 清理，不改标准 reset 行为 |
| 20260923-183353-10f745d2 | 增加 const 工厂用例后的编译失败，未运行测试；construct_at 不接受 const T，需保留原始存储与正确对象构造 |
| 20260923-183431-e346aa32 | unique 用 uses_allocator 参数 + placement new，保持构造回滚；相关工厂、控制块、事件及 const 用例 6/6 通过 |

最终 **17 个新用例、15 个既有 heap 用例及 1 个 probe** 均有通过证据。
补充测试存在重叠，上表不简单相加为独立用例数。const 修复只影响工厂模板，复验其构造、异常、所有权和事件用例；
其他代码未改变，保留已有通过结果。没有跳过项。

README 的完整路由示例提取到 out/memory-routing-example，通过独立 CMake 工程链接 dk_memory，
Debug /W4 /WX 编译和执行均返回成功；日志为该目录的 configure.log/build.log。
示例在入口绑定 Assets，业务无 memory 参数构造 Mesh；切换 Scene 后已有 indices 扩容仍使用 Assets。
check-spec 对本次 8 个 Markdown、246 个本地链接、时间戳、开发编号与清单检查通过；git diff --check 通过。

未运行 Release、其他平台、ASan/TSan、非法析构的 death tests、性能基准或新 Tracy capture；
本节未改事件适配和追踪宏，确定性 sink 验证上层只透传到底层唯一事件路径，0024 的 capture 保留为历史证据。
并发验证覆盖有效生命周期，不据此声称消除所有数据竞争；普通容器的并发访问仍需调用者同步。

## 交接

本节完成后独立本地提交。M1.7 仍进行中，下一项 **M1.7.4 ScratchArena/ScratchScope**，下一记录编号 0026。
scratch、pool、RoutingToken、worker 缓存与 Runtime 自动装配未实现；既有 Scene/Runtime 容器没有自动纳管。
