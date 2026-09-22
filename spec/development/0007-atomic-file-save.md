---
id: "0007"
created_at: "2026-09-22T11:30:13+08:00"
updated_at: "2026-09-22T11:45:58+08:00"
status: completed
design_refs:
  - ../design/foundation-io.md
---

# 0007 同目录临时文件与安全替换

## 目标与设计依据

完成 M1.5，先扩展 [IO 设计](../design/foundation-io.md)；
基线为 e2a8297，开始时工作区干净。范围止于安全字节保存，M1.6 集成验收留待下一步。
复用 dk::io 与 Core，不新增三方库、不变更 vcpkg baseline 或公开构建选项。

## 实际变更

- [File.hpp](../../engine/foundation/io/include/dk/io/File.hpp) 新增
  write_file_bytes_atomic(path, span<const byte>) -> Result<void>，
  与 M1.4 普通截断写入独立；Windows 本地普通文件支持创建、覆盖与清空保存。
- [AtomicFile.cpp](../../engine/foundation/io/src/AtomicFile.cpp) 捕获并验证目标/父目录，
  基于 Core/stduuid 生成同目录临时名，CREATE_NEW 独占创建，碰撞最多重试 32 次。
  不区分大小写排除候选与目标同名，避免目标不存在时被当作临时文件提前创建。
- 分块 WriteFile 处理短写和零进展，FlushFileBuffers、CloseHandle 均检查结果；
  只在上述操作成功后 MoveFileExW 同卷替换，不启用复制回退，不提前删除目标。
  成功重命名后没有可能将结果改报失败的操作。
- 局部 RAII 管理临时文件，正常失败显式关闭/删除；清理失败保留主错误，
  Error.context 记录附加清理诊断和临时路径。异常展开尽力清理，无全局可变开关。
- [AtomicFileInternal.hpp](../../engine/foundation/io/src/AtomicFileInternal.hpp)
  是模块私有的每次调用操作适配器；测试包装真实 Win32 后端注入故障，
  不向公开 API 暴露测试开关，也不让其他引擎模块依赖 IO 私有头。
- [AtomicFileTests.cpp](../../engine/foundation/io/tests/AtomicFileTests.cpp)
  新增 13 项 Windows 安全保存测试；由现有 dk_io_tests 编译并自动发现。
  测试产物在构建目录 tests/unit/test-artifacts/<config>/atomic-io/<随机 ID>。
- [IO CMake](../../engine/foundation/io/CMakeLists.txt) 和
  [单元测试 CMake](../../tests/unit/CMakeLists.txt) 加入实现及模块内部测试；
  README、架构、索引与 roadmap 同步 M1.5 完成和下一项 M1.6。

## 验证环境与命令

沿用 [0006](0006-foundation-io.md) 的 Windows x64 / VS 2026 / CMake 4.2.1 /
MSVC 19.51 环境，测试目录位于本机 D: NTFS。
开发配置依赖未变化；独立 IO 配置只使用 stduuid、Catch2 和 vcpkg CMake 辅助包。

```powershell
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
cmake --build out/build/windows-io-only --config Debug
ctest --test-dir out/build/windows-io-only -C Debug --output-on-failure
```

| 配置 | 构建 | CTest 实际结果 |
| --- | --- | --- |
| windows-dev Debug | 通过 | 注册 86，85 通过、1 跳过、0 失败 |
| windows-dev Release | 通过 | 注册 86，85 通过、1 跳过、0 失败 |
| windows-io-only Debug | 警告即错误，通过 | 注册 36，35 通过、1 跳过、0 失败 |

windows-io-only 复用 0006 独立缓存，DK_BUILD_MATH/LOGGING/RUNNER=OFF，
DK_BUILD_IO/WARNINGS_AS_ERRORS=ON，构建时自动重新配置。
86 项包括 26 项 IO/安全保存、42 项数学/Transform、16 项 Core 和 2 项探针。
三组跳过的均为目标符号链接用例：本机没有创建符号链接权限；不计入通过。

交付检查通过：20 份 Markdown、189 个本地链接、实际文档时间戳、开发编号唯一性、
CMake/vcpkg JSON、独立配置开关以及 git diff --check。

已实际覆盖：

- Unicode 文件名、相对路径、150000 字节跨块、缩短/清空/重复保存、无临时残留。
- 空路径、目录、dot/dot-dot、尾随分隔符、缺失父目录、尾随点/空格、
  ADS、保留设备名、非法字符/NUL、UNC/设备命名空间拒绝。
- 对旧目标与新目标分别注入创建、部分写入、零写入、刷新、关闭、替换失败；
  检查旧字节不变/新目标不存在、没有临时残留。部分写入实际完成 4096 字节后返回错误。
- 短写继续完成；候选碰撞不修改其他文件，32 次耗尽不清理其他保存的文件，
  候选与不存在目标的大小写别名不提前创建目标。
- 同时注入主写入错误和清理关闭/删除错误，验证主错误与临时路径仍可诊断；
  检查真实残留后只删除该测试持有的临时文件。
- 真实 Windows 共享冲突、只读目标拒绝替换，旧文件保持且临时文件被清理；
  释放共享冲突后重试成功。硬链接用例验证替换文件身份，其他链接保留旧内容。

## 偏差与决策

首轮 Debug 回归发现 Windows absolute 转换会消除末尾点/空格，导致校验时丢失原始拼写。
已把文件名校验提前到转换之前，回归通过；该失败已修复，不隐藏首轮结果。
检查期间补充候选与目标同名保护，并在设计先行更新后加入回归用例。

采用 MoveFileExW 同目录重命名；ReplaceFileW 文档允许部分失败改变目标状态，
不符合本阶段简洁的失败保护契约。替换不保留旧 ACL、时间或备用数据流，
新文件继承父目录 ACL；目标重解析点拒绝，UNC/网络驱动器及非 Windows 返回 not_supported。

## 遗留问题与下一步

已完成 M1.5 的本地 NTFS 正常运行与可恢复错误边界。
符号链接拒绝测试仍需具备创建权限的环境重跑；非 Windows 分支未进行本机编译/执行。
故障注入不等同于真实磁盘耗尽、设备故障或断电测试；未验证崩溃恢复、超长路径、
其他文件系统/过滤驱动或外部并发目录变化。临时内容刷新不承诺目录重命名断电持久化，
强制终止可能留下临时文件，不提供自动恢复/全局清扫。

下一项 **M1.6 Foundation 集成验收**，先写 CPU 集成案例设计，再创建 0008，
串联 ID、Transform 和安全保存/重载。M1 尚未整体完成。

## 修改记录

- 2026-09-22T11:30:13+08:00：先写安全保存设计与本记录。
- 2026-09-22T11:39:12+08:00：完成实现和首轮 Debug 回归，记录路径校验修复及权限跳过。
- 2026-09-22T11:44:41+08:00：完成候选同名保护及 Debug/Release、独立 IO 验证，同步文档与下一阶段。
- 2026-09-22T11:45:58+08:00：完成文档与配置检查，准备 M1.5 独立本地提交。
