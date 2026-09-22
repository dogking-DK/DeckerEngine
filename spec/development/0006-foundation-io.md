---
id: "0006"
created_at: "2026-09-22T11:06:07+08:00"
updated_at: "2026-09-22T11:25:49+08:00"
status: completed
design_refs:
  - ../design/foundation-io.md
  - ../design/project-foundation.md
---

# 0006 工程路径与二进制文件 IO

## 目标与设计依据

完成 [Roadmap](../roadmap.md) 的 M1.4，依据先行 [IO 设计](../design/foundation-io.md)。
用户要求将当前 Git 提交到本地，先创建检查点 b0b7255，
包含 M1.1–M1.3、stduuid/Eigen/Transform 及记录 0002–0005。
提交前仅清理 8 个原有文件的多余末尾空行，未改变其行为；提交后工作区干净。
本次 M1.4 作为后续独立本地提交；不推送远程。

## 实际变更

- [Path.hpp](../../engine/foundation/io/include/dk/io/Path.hpp)、
  [Path.cpp](../../engine/foundation/io/src/Path.cpp)：严格 UTF-8/原生路径转换，
  拒绝非法 Unicode、空串和 NUL。ProjectPaths 固定 canonical 工程根，
  解析相对路径并拒绝词法越界；不依赖之后 CWD 的改变。
- [File.hpp](../../engine/foundation/io/include/dk/io/File.hpp)、
  [File.cpp](../../engine/foundation/io/src/File.cpp)：ByteBuffer、有大小上限的二进制读取、
  普通创建/截断写入。按 64 KiB 分块，默认读取上限 64 MiB，超限返回错误，
  达到上限后最多探测一个额外字节；不按文件声明大小预分配。
- [IoInternal.hpp](../../engine/foundation/io/src/IoInternal.hpp)：内部错误构造，
  复用 Core ErrorCode，包含操作、UTF-8 路径和可用系统诊断。
  写入检查 open/write/flush/close，不自动创建父目录。
- [IO CMake](../../engine/foundation/io/CMakeLists.txt) 建立 dk_io / dk::io，
  PUBLIC 链接 dk::core；[选项](../../cmake/Options.cmake) 新增默认 ON 的 DK_BUILD_IO，
  foundation 条件加入 IO，bootstrap 显式关闭；无新增 vcpkg 包或 baseline 变更。
- [IoTests.cpp](../../tests/unit/IoTests.cpp) 新增 13 项测试，
  独立 dk_io_tests 只链接 dk::io 与 Catch2，不依赖日志或数学。
  产物位于各构建目录 tests/unit/test-artifacts/<config>/io/<随机 ID>，
  保留供排错；不写入示例工程、源码目录或用户文件。
- 同步 README 接口/独立构建命令、架构、工程基础、设计/开发索引和 roadmap。
  后续只构建数学的示例显式关闭 IO；历史开发记录命令保持原样。

## 验证环境与命令

Windows x64，CMake 4.2.1，Visual Studio 18 2026，项目编译器 MSVC 19.51.36257.0。
vcpkg x64-windows：stduuid 1.2.3、Catch2 3.13.0#1；开发预设仍包含
Eigen 5.0.1、fmt 12.1.0、spdlog 1.17.0、nlohmann-json 3.12.0#2。
vcpkg 依赖复用 MSVC 19.44 的缓存；当前项目链接和测试通过，未变更工具链策略。

完整开发配置（新增 CMake/源文件触发自动重新配置）：

```powershell
cmake --build --preset windows-debug
ctest --preset windows-debug
cmake --build --preset windows-release
ctest --preset windows-release
```

Debug 和 Release 构建成功，各 **73/73** CTest 通过：
13 项 IO、42 项数学/Transform、16 项 Core、日志流分离、版本探针。

无数学、日志或 runner 的独立配置，开启警告即错误：

```powershell
cmake --preset windows-dev -B out/build/windows-io-only -DDK_BUILD_MATH=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-io-only --config Debug
ctest --test-dir out/build/windows-io-only -C Debug --output-on-failure
```

配置/构建成功，**23/23** 通过（13 IO + 10 Core）；仅安装 stduuid、Catch2
及 vcpkg CMake 辅助包，验证依赖边界和 PUBLIC 使用要求。

最小探针回归：

```powershell
cmake --preset windows-bootstrap
cmake --build --preset windows-bootstrap-debug
ctest --preset windows-bootstrap-debug
```

配置/构建成功，**1/1** 通过；DK_BUILD_IO=OFF 时不构建 IO，仍只安装 stduuid
及 vcpkg CMake 辅助包。以上实际构建/测试均通过，无待修复失败。

IO 测试覆盖中文/空格/非 BMP 路径、非法 UTF-8 和原生孤立代理字符、NUL，
根目录不存在/非目录、相对规范化/绝对路径和越界拒绝、CWD 改变后的稳定解析；
空文件和零上限、全部字节值、1/65536/150000 字节跨块读取与精确上限、
缩短/清空覆盖、缺失文件/父目录、目录误用、Windows 独占句柄造成的共享冲突。
共享冲突时读写返回 io_error，并带系统诊断；该打开失败案例未截断旧数据，
不据此宣称普通写入具有通用旧文件保护。

交付检查：19 份 Markdown 的 174 个本地链接、实际文档时间戳、开发编号唯一性、
CMake/vcpkg JSON 和缓存开关检查通过；模板日期占位符不作为实际时间解析。
git diff --check 与 git diff --cached --check 均通过。

## 偏差与决策

按设计完成当前子阶段，未引入新的三方库。
读取限制针对结果字节数，不保证 vector 容量、流缓冲等精确内存峰值。
ProjectPaths 只保证词法范围，子路径符号链接仍由 OS 解析；预检查和打开存在竞态，
不提供访问隔离或并发快照。资源分配异常继续传播。

## 遗留问题与下一步

下一项为 **M1.5 同目录临时文件与安全替换**；在其开始前扩展 IO 设计并创建 0007。
普通写入不是安全保存，失败可能留下空文件或部分数据；flush/close 成功不代表落盘持久化。
未验证 Linux/macOS、超长路径、符号链接竞态、磁盘耗尽/物理故障或断电恢复；
本阶段没有原子替换实现，不把这些情况记为已通过。
M1.6 再串联 ID、Transform 与安全保存进行 Foundation 集成验收；M1 尚未整体完成。

## 修改记录

- 2026-09-22T11:06:07+08:00：提交此前成果，先写本次设计和记录。
- 2026-09-22T11:24:00+08:00：完成 M1.4 实现、Debug/Release、独立 IO 和 bootstrap 验证，同步下一阶段。
- 2026-09-22T11:25:49+08:00：完成文档链接、元数据、预设和暂存差异检查，准备独立本地提交。
