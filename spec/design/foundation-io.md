---
module: foundation-io
created_at: "2026-09-22T11:06:07+08:00"
updated_at: "2026-09-22T11:06:07+08:00"
status: accepted
---

# Foundation IO：工程路径与二进制文件

## 目标与边界

实施 [Roadmap](../roadmap.md) 的 M1.4：UTF-8/原生路径转换、工程相对路径、
最小二进制读写；复用 [Core](foundation-core.md) 的 Result/Error。
公开头 include/dk/io/Path.hpp、File.hpp，实现在 src。
新增 dk_io / dk::io，PUBLIC 链接 dk::core，仅标准库，无新增 vcpkg 依赖。
DK_BUILD_IO 默认 ON；关闭时不创建 IO target/测试，bootstrap 显式关闭 IO。
不依赖数学、日志、Scene、窗口或 GPU。

普通写入会截断已有内容，失败可能留下空文件或部分数据。
临时文件、原子替换、旧文件保护与持久化保证属于 M1.5；
本阶段不能作为场景安全保存接口。

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

## 实施与验证

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
