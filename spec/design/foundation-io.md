---
module: foundation-io
created_at: "2026-09-22T11:06:07+08:00"
updated_at: "2026-09-22T11:40:25+08:00"
status: accepted
---

# Foundation IO：工程路径与二进制文件

## 目标与边界

实施 [Roadmap](../roadmap.md) 的 M1.4 路径/二进制 IO 和 M1.5 安全保存；
复用 [Core](foundation-core.md) 的 Result/Error。
公开头 include/dk/io/Path.hpp、File.hpp，实现在 src。
dk_io / dk::io PUBLIC 链接 dk::core；基础 IO 使用标准库，安全保存使用 Windows API，
无新增 vcpkg 依赖。
DK_BUILD_IO 默认 ON；关闭时不创建 IO target/测试，bootstrap 显式关闭 IO。
不依赖数学、日志、Scene、窗口或 GPU。

普通写入会截断已有内容，失败可能留下空文件或部分数据。
M1.5 新增独立安全保存接口，不改变普通写入语义；
该接口保存已准备好的字节，场景序列化/内容校验由上层负责。

## 路径接口

- `path_from_utf8(string_view) -> Result<filesystem::path>`：
  严格 UTF-8；拒绝空串、NUL、过长编码、孤立续字节、截断序列、
  代理区码点和超出 U+10FFFF 的编码。经 char8_t 路径构造，
  Windows 使用原生宽字符，不依赖 ANSI 代码页。
- `path_to_utf8(path) -> Result<string>`：输出 generic UTF-8（/ 分隔符），
  拒绝空路径、NUL 和无法表示的 Unicode。
  不执行 NFC/NFD、大小写或文件名合法性归一化。
- `ProjectPaths::create(root)`：根必须为已有目录，允许相对 root；
  创建时 canonical 成绝对路径，消解根目录自身的符号链接及 dot/dot-dot。
  root() 提供只读引用，其生命周期随对象。
- `ProjectPaths::resolve(relative)`：只接受非空、无 root_name/root_directory
  的路径；lexically_normal 后拒绝剩余 ..，允许 a/../b，. 返回根目录。
  返回固定根目录下的绝对路径，子路径不必存在，不受之后 CWD 改变影响。

resolve 仅做词法解析，不检查子路径符号链接的实际目标，也不是访问隔离。
原生路径的文件名限制、大小写、符号链接与设备命名遵循宿主 OS。
低层文件 API 接受 filesystem::path；相对路径依赖调用当时的 CWD，
工程稳定路径应先 resolve。本模块面向调用方指定的普通文件，不提供跨平台文件名协议。

## 文件接口

| 接口 | 契约 |
| --- | --- |
| `ByteBuffer` | vector<byte>，保留 NUL、CR/LF 等原始字节 |
| `read_file_bytes(path, max_bytes = 64 MiB)` | Result<ByteBuffer>；分块读取，结果不超过调用上限 |
| `write_file_bytes(path, span<const byte>)` | Result<void>；创建或截断，空 span 写出空文件 |

读写前验证路径并检查当前文件类型，拒绝目录和其他已识别的非普通文件，
跟随符号链接；写入不创建父目录。预检查与打开存在竞态，不宣称文件快照或访问隔离。
读取不按 file_size 预分配，达到上限后最多探测一个额外字节。
max_bytes=0 只允许空文件，实际容量还受 vector::max_size() 限制；
超限返回 invalid_argument，不返回部分内容。限制逻辑结果大小，不承诺精确内存峰值。

写入分块检查 open/write/flush/close 状态；成功表示数据交给 OS 并正常关闭，
不保证磁盘同步持久化，也不能回滚已经发生的截断/写入。
没有文本解码、换行替换、终止字符或格式校验。

## 错误与生命周期

非法路径/编码、目录当文件、工程路径越界、读上限超出为 invalid_argument；
缺失文件/根目录/父目录为 not_found；其他系统错误为 io_error。
错误保存操作、UTF-8 路径及可用的系统 category/code/message。
不新增 ErrorCode，不用 assert 检查外部输入；bad_alloc 等资源错误继续传播。

流由局部 RAII 管理，显式检查写入关闭；错误路径析构释放句柄。
输入不修改，无全局可变状态；独立文件可并发，同一文件的读写由调用方协调。
ProjectPaths 创建后只读，不改变全局 CWD。

## 安全保存接口（M1.5）

File.hpp 增加 write_file_bytes_atomic(path, span<const byte>) -> Result<void>。
首版后端支持 Windows 本地普通文件；其他平台返回 not_supported，绝不退化为截断写入。
基础路径和普通 IO 的跨平台入口不变。没有公开临时文件对象或可变全局配置。

流程与所有权：

1. 验证非空 Unicode 路径；捕获绝对路径并 canonical 父目录，父目录必须存在。
   接受相对目标；拒绝空文件名、dot/dot-dot、尾随空格/点、ADS、保留设备名、
   非普通目标和目标重解析点。仅接受常规驱动器路径；UNC/设备命名空间、
   网络驱动器返回 not_supported。父目录可含符号链接，由 canonical 消解。
2. 使用 Core 的 stduuid 生成 .dk-save-<uuid>.tmp，同目录 CREATE_NEW 独占创建；
   碰撞最多重试 32 次，已有候选文件不覆盖、不删除。不用先检查再创建。
   候选名与目标名按 Windows 不区分大小写比较，同名时直接跳过，即使目标不存在。
   临时文件句柄不继承、不共享；内容未完成前目标保持原样。
3. WriteFile 分块写入，处理短写；零进展视为 io_error。
   FlushFileBuffers 刷新临时文件后检查 CloseHandle，任何失败均不执行替换。
4. MoveFileExW 使用 MOVEFILE_REPLACE_EXISTING；不使用 COPY_ALLOWED，
   不删除目标后再重命名，不回退复制。目标可以不存在，也可被完整新文件替换。
   替换前再次检查目标类型；调用方仍须协调同一路径/目录的并发修改。
5. 成功的重命名为提交点，之后不执行可能将结果改报为失败的操作。
   成功后临时路径已消失；新目标拥有临时文件的元数据。

失败与清理：

- 创建失败不触碰候选或目标；获得临时文件所有权后，所有正常错误路径关闭句柄并删除本次临时文件。
  RAII 在资源异常展开时尽力清理；不扫描或清理其他保存任务的临时文件。
- 保留主错误码；如果关闭/删除清理也失败，在 Error.context 追加清理错误和临时文件路径，
  便于定位残留。不隐藏清理失败，也不为清理失败回滚/删除目标。
- 本地文件系统在运行中的写入、刷新、关闭和拒绝重命名错误时，旧目标保持原字节；
  新目标在提交前失败时仍不存在。分配异常继续传播，析构清理不能保证上报诊断。
- 缺失父目录为 not_found，路径/类型无效为 invalid_argument，
  权限/共享冲突/写入/刷新/关闭/替换/候选耗尽为 io_error。
  不支持的平台或路径类型为 not_supported；生成临时 ID 的错误沿用 Core 结果。

原子性与持久化边界：

- 同目录避免跨卷复制；可见性依赖本地文件系统的同卷重命名语义，
  验收范围为 Windows 本地 NTFS。读者可能仍持有旧文件句柄；未允许删除共享的读者可使保存失败。
  不承诺对任意文件系统、过滤驱动、远程服务、恶意路径竞态或外部并发写入的事务隔离。
- 临时内容在提交前刷新；目录重命名的断电持久化和设备实际落盘仍不承诺。
  强制终止可能留下 .dk-save-*.tmp；不实现启动时恢复或自动清扫。
- 替换文件身份，而不是原地改写。旧目标的 ACL、时间、备用数据流不保留；
  新文件默认继承父目录 ACL。其他硬链接仍指向旧文件。目标符号链接/重解析点拒绝。
  上层如需版本校验、保留元数据、备份或恢复协议，需要另行设计。
- 不采用 ReplaceFileW，其文档列出的部分失败可能已经改变目标名称/状态，
  不适合本阶段简单的失败保护契约。

验证采用公开接口的真实文件场景和 IO 模块内部的每次调用独立平台操作适配器。
测试包装真实后端，在部分写入、刷新、关闭、替换及清理位置注入错误，
观察旧字节、新目标缺失和目录残留；不向公开接口暴露故障参数。
内部操作接口仅由 IO 自身实现/测试引用，不作为其他引擎模块的依赖。
覆盖候选碰撞及耗尽、短写/零进展、Windows 共享冲突/只读文件、
中文/空/大字节序列、无父目录、非法目标、重复保存、错误诊断。
Debug/Release 全量和仅 Core/IO（警告即错误）回归后同步 0007 与 roadmap。

## M1.4 实施记录

1. 先将此前成果按用户要求本地提交为 b0b7255，再写本设计和 0006。
2. 实现路径、文件接口、开关和独立 dk_io_tests（仅链接 dk::io/Catch2）。
3. 覆盖中文/空格/非 BMP、非法 UTF-8/NUL、根目录约束、dot/dot-dot、
   绝对/驱动器相对路径拒绝、CWD 改变后解析稳定；
   空文件、所有字节值、跨块 IO、缩短覆盖、精确上限/超限、
   缺失文件/父目录、目录误用、Windows 文件共享冲突。
4. 测试产物位于构建目录 test-artifacts/<config>/io 独立子目录。
5. Debug/Release 全量回归、关闭数学/日志/runner 的独立 IO 配置和 bootstrap；
   如实记录未验证的平台与故障。
6. 完成后同步文档并单独本地提交 M1.4，不推送远程。下一阶段为 M1.5。

## 参考与记录

- [filesystem](https://learn.microsoft.com/en-us/cpp/standard-library/filesystem-functions?view=msvc-170)
- [ifstream](https://learn.microsoft.com/en-us/cpp/standard-library/basic-ifstream-class?view=msvc-170)
- [Windows 文件共享](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [0006 文件 IO](../development/0006-foundation-io.md)
- [0007 安全保存](../development/0007-atomic-file-save.md)
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- [ReplaceFileW 失败语义](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
