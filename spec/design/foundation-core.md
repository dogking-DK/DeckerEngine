---
module: foundation-core
created_at: "2026-09-22T09:41:24+08:00"
updated_at: "2026-09-22T10:15:52+08:00"
status: accepted
---

# Foundation Core：错误、日志和稳定 ID

## 目标与本次范围

实现 Roadmap M1 的第一项任务：可恢复错误、模块日志和持久身份基础。
数学、文件 IO、Scene 和命令协议不在本次实现范围。
以 [架构](architecture.md) 和 [Roadmap](../roadmap.md) 为依据，
初始实施见 [0002](../development/0002-foundation-core.md)，
stduuid 迁移见 [0003](../development/0003-stduuid-migration.md)。

## 边界、目录和 target

- `engine/foundation/core/include/dk/core/` 提供公开头，src 提供实现。
- `dk_core / dk::core`：版本、Error/Result、StableId。PRIVATE 链接 stduuid；
  第三方类型只出现在实现文件中，不改变公开头的强类型接口。
- `dk_logging / dk::logging`：Logger；PUBLIC 链接 dk::core 和 fmt::fmt
  （格式模板出现在公开头中），PRIVATE 链接 spdlog::spdlog。
- 不引入 SDL、Vulkan、Scene 或 Editor；不直接暴露 spdlog 对象或注册表。
- 移除直接的 Windows/bcrypt 实现与链接。生成、解析、格式化、哈希由
  vcpkg 基线锁定的 stduuid 1.2.3 提供；使用默认功能，不启用 system-gen。

## 错误与 Result

`ErrorCode` 使用明确整数值：invalid_argument=1、invalid_state=2、
not_found=3、io_error=4、not_supported=5、internal_error=6。
`error_code_name()` 提供稳定名称，未知枚举值返回 unknown。

`Error` 保存 code、message 与由内到外追加的 context。
`with_context()` 返回追加上下文后的副本，保留原始错误，
便于调用方通过 `std::unexpected` 继续传播。
`Result<T>` 是 `std::expected<T, Error>` 的别名，支持 void 和 move-only 数据；
不引入传播宏、另一套结果容器或异常层级。

可预期的参数、系统和日志 IO 失败返回 Result。
std::bad_alloc、调用方自定义格式化器抛出的异常等不伪装成可恢复业务错误，
也不声称全部 API noexcept；未来应用/协议边界负责统一兜底。
assert 只用于程序内部不变量，不能替代外部参数验证；
当前实现不依赖 assert 才能生效的校验。

## 稳定 ID

`StableId<Tag>` 是 16 字节值类型。首批类型别名为 EntityId、AssetId、SceneId，
仅定义身份类型，不建立对上层模块的依赖。

公开操作：

- 默认构造得到 nil；`is_nil()` 显式判断。上层创建实际实体/资产时必须拒绝 nil。
- `generate() -> Result<StableId>`：由
  `uuids::basic_uuid_random_generator<std::random_device>` 生成 UUIDv4。
  random_device 和 generator 为每线程独立实例，初始化失败返回错误；
  不自行编码版本位、不退化为时钟/递增计数。
- `parse(string_view) -> Result<StableId>`：只接受 36 字符
  `8-4-4-4-12` 十六进制布局，不接受前后空白、花括号或隐式截断；
  大小写均可读，输出统一小写。
- `to_string()`、`bytes()`、同类型比较/排序与 std::hash。
  二进制字节顺序与文本顺序一致，不借用 Windows GUID 内存布局。
- 解析允许 nil 及其他 UUID 版本的已有字节值；只有 generate 保证生成 v4/标准 variant。
- 不提供 EntityId/AssetId 之间的隐式转换或比较。

身份稳定来自保存和重载同一值，不来自文件路径或运行时句柄。
随机唯一性不是数学上的无碰撞保证，上层注册/导入仍应检测重复。
哈希只用于进程内容器，不作为持久协议或密码学摘要。

实现保留 16 字节存储以维持 constexpr nil、bytes() 和强类型 API；
通过 stduuid 的 uuid/as_bytes 在私有实现中转换。
解析前仅检查长度和四个分隔符位置，再交给 uuid::from_string 检查/解析十六进制。
stduuid 原生解析器接受更宽松的无连字符/花括号格式，项目继续拒绝这些输入，
避免修改已经约定的场景身份格式。输出使用 uuids::to_string，hash 使用库的 std::hash。
容器 hash 值可能因此改变，文本和字节持久化格式不变。
random_device 作为随机源，不把 UUID 接口定义为安全凭证 API。

## 日志 API 与生命周期

`Logger::create(LogConfig) -> Result<std::unique_ptr<Logger>>` 创建实例；
Logger 不可复制/移动，实例所有权显式。独立实例互不注册到全局 spdlog，
销毁一个实例不影响另一个实例。

LogConfig 包含 minimum_level（默认 info）、stderr_enabled（默认 true）
和可选文件路径（空路径表示关闭文件输出）。
至少开启一个输出；文件采用追加模式，不静默创建父目录。
使用 std::ofstream 的原生 filesystem::path 和 spdlog ostream sink，
保留 Windows Unicode 路径；流启用失败异常以检测写入/flush 错误。

`write(level, module, message) -> Result<void>` 接受原始文本，
不会将消息中的花括号作为格式串。
`log(level, module, fmt::format_string<...>, ...) -> Result<void>`
提供编译期校验的格式化。无效级别、空模块或含换行/NUL 的模块名返回 invalid_argument；
off 仅用于关闭配置输出，不作为消息级别。
`enabled()` 查询过滤状态；过滤级别不会产生输出。
`flush() -> Result<void>` 让调用方显式检查落盘失败。
日志包含时间、级别和模块。多个实例不能同时写同一文件。

写入与 flush 使用 spdlog 的同步、多线程 sink；运行时配置不修改，
允许多个线程在实例存活期间调用。销毁前由拥有者停止/join 所有使用线程。
不做异步日志、全局单例、动态重配置或跨进程文件锁。
析构尽力 flush，失败向 stderr 输出固定诊断且不抛异常；
需要确认落盘的调用者应显式检查 flush。
spdlog 报告的写入/刷新错误经实例错误处理器转回 Result。
无有效控制台句柄时 stderr sink 遵循平台行为；需要确认落盘时配置文件并检查 flush。

## 构建和测试配置

- 新增 DK_BUILD_LOGGING、DK_BUILD_UNIT_TESTS 选项（默认开启）。
  DK_BUILD_TESTS=OFF 时不构建/安装单元测试。
- 启用日志时 vcpkg 自动选择 foundation；启用单元测试时自动选择 tests，
  均在 project() 前完成 feature 选择。用户额外的依赖组仍保留。
- stduuid 为 manifest 的基础依赖；不依赖 foundation/tests 等可选 feature。
- windows-bootstrap 显式关闭日志/单元测试，通过 vcpkg 安装 stduuid，
  构建最小 core 和版本探针；新二进制目录 windows-bootstrap-stduuid 避免复用旧的无 toolchain 缓存。
  windows-dev/Ninja 默认包含日志和 Catch2 测试。
- runner 在日志启用时通过 dk::logging 记录非法参数，帮助/版本的 stdout 保持兼容。
- Catch2/CTest 分别在 Debug/Release 验证错误传播、ID 格式和类型隔离、日志输出及生命周期。
  独立子进程探针分离捕获 stdout/stderr，检查协议输出未被日志污染。
- 文件日志测试保留在构建目录的 test-artifacts 下以便失败排查，不写入源码目录。

## 实施步骤和验收

1. 先建立本设计与 0002 记录，再添加 Error/Result、StableId、Logger。
2. 接入 target、选项/feature 和 Catch2；把 runner 的错误日志接入公共 API。
3. 验证成功/失败 Result 和上下文；合法/非法 ID、nil、hash、格式/版本位、
   生成往返及多线程生成；编译期验证跨类型不可混用。
4. 验证日志过滤、格式化/原始文本、Unicode 文件路径、初始化失败、
   无输出配置、显式 flush、析构后内容、独立实例和并发写入；
   子进程确认 stdout/stderr 分离。
5. 执行 Windows 开发 Debug/Release 与 bootstrap 兼容验证；
   实际链接 fmt/spdlog/Catch2 后记录工具链兼容性。
6. 更新 Roadmap 为 M1 部分完成，数学与 IO 仍待开发。
7. stduuid 迁移保持公开接口和原测试，补充库互操作/严格输入格式回归，
   重新执行开发及最小构建的 Debug/Release 验证。

## 参考

- [spdlog 官方用法](https://github.com/gabime/spdlog)
- [Catch2 CMake 集成](https://catch2-temp.readthedocs.io/en/latest/cmake-integration.html)
- [RFC 9562：UUID 布局](https://www.rfc-editor.org/rfc/rfc9562.html)
- [stduuid 官方仓库](https://github.com/mariusbancila/stduuid)
