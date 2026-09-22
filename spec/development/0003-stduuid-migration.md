---
id: "0003"
created_at: "2026-09-22T10:15:52+08:00"
updated_at: "2026-09-22T10:25:07+08:00"
status: completed
design_refs:
  - ../design/foundation-core.md
  - ../design/project-foundation.md
---

# 0003 StableId 迁移到 stduuid

## 目标与设计依据

按用户要求改用 stduuid，先更新 [Core 设计](../design/foundation-core.md)
和 [工程基础设计](../design/project-foundation.md)，再替换实现。
保留 Strong ID、Result、nil、bytes、比较及文本格式契约。

## 实际变更

- [StableId.cpp](../../engine/foundation/core/src/StableId.cpp) 使用 stduuid
  生成 UUIDv4、解析、输出文本和计算容器哈希，移除手写十六进制编解码、
  FNV 哈希及平台专用 BCrypt 实现。
- [StableId.hpp](../../engine/foundation/core/include/dk/core/StableId.hpp)
  保留 16 字节存储、constexpr nil、强类型接口；hash 委托到私有实现。
  EntityId / AssetId / SceneId 不暴露第三方类型。
- [vcpkg.json](../../vcpkg.json) 增加必需依赖 stduuid；
  基线仍为 62159a45e18f3a9ac0548628dcaf74fcb60c6ff9，实际解析为 1.2.3。
  [Core CMake](../../engine/foundation/core/CMakeLists.txt) 查找并 PRIVATE 链接
  stduuid，移除 bcrypt 链接，不启用 system-gen。
- [CMakePresets.json](../../CMakePresets.json) 中最小预设开启 vcpkg，
  不选额外 feature，继续关闭日志和单元测试。
  使用 out/build/windows-bootstrap-stduuid 目录，避免复用原无 toolchain 缓存；
  构建/测试预设名称保持不变。[Vcpkg.cmake](../../cmake/Vcpkg.cmake)
  同步缺少依赖时的提示；关闭 vcpkg 仍需调用者提供 stduuid CMake package。
- [ID 测试](../../tests/unit/StableIdTests.cpp) 新增库互操作与严格格式回归，
  包含非 NUL 结尾 string_view、字节/文本/hash 一致性，以及库接受而 dk 拒绝的宽松格式。
  [测试 CMake](../../tests/unit/CMakeLists.txt) 显式声明该测试的 stduuid 依赖。
- 同步 README、两份设计、设计/开发索引和 Roadmap；下一功能仍为数学/Transform，
  下一可用记录编号更新为 0004。
- 此前 0002 改动仍未提交，本次保留并在其上增量修改；不改写历史开发记录。

## 验证记录

环境：Windows x64、Visual Studio 18 2026、MSVC 19.51.36257.0、
CMake 4.2.1；vcpkg 检测到的依赖工具链为 MSVC 19.44.35207。
开发和最小构建均开启 DK_WARNINGS_AS_ERRORS。

| 命令 | 结果 |
| --- | --- |
| `cmake --preset windows-dev -DDK_WARNINGS_AS_ERRORS=ON` | 通过；安装 stduuid 1.2.3，保留既有依赖 |
| `cmake --build --preset windows-debug` + `ctest --preset windows-debug` | 修正测试依赖后通过，18/18 |
| `cmake --build --preset windows-release` + `ctest --preset windows-release` | 通过，18/18 |
| `cmake --preset windows-bootstrap -DDK_WARNINGS_AS_ERRORS=ON` | 新目录配置通过；仅安装 stduuid 和 vcpkg CMake 辅助包 |
| `cmake --build --preset windows-bootstrap-debug` + `ctest --preset windows-bootstrap-debug` | 通过，1/1 |
| `cmake --build --preset windows-bootstrap-release` + `ctest --preset windows-bootstrap-release` | 通过，1/1 |

收尾检查通过：2 份 JSON 可解析、105 个本地文档链接有效、7 组元数据时间顺序正确；
git diff --check 无空白错误，engine/tests 不再含 BCrypt 或 FNV 实现引用。

首次 Debug 编译时互操作测试未找到 uuid.h：Core 目录发现的 imported target
在兄弟测试目录不可见，单纯写库名未携带 include 路径。
测试目录补充 find_package(stduuid CONFIG REQUIRED) 后重新构建、测试通过，
没有通过全局 include 路径绕过依赖声明。

18 项包括 16 个 Catch2 行为用例、日志流分离与版本探针；
保留原有 UUIDv4/variant 位、nil、版本兼容、非法文本、类型隔离、容器往返、
1024 次生成及四线程共 512 次生成测试。
这些检查验证行为与并发使用，不构成随机无碰撞或随机源失效路径的证明。

## 偏差与决策

stduuid 原生解析支持宽松格式，封装维持原来的 36 字符和固定连字符位置。
不启用 system-gen，使用库的随机 UUIDv4 生成器与每线程 random_device。
容器 hash 迁移到库实现，不影响持久化 ID 字节/文本。
原无第三方 bootstrap 改为最小 stduuid 依赖构建，并使用新构建目录避开旧缓存。

## 遗留问题与下一步

本次迁移已完成。Linux/macOS、外部提供 stduuid 且关闭 vcpkg 的配置未实测；
未验证全桌面依赖组。下一功能任务为数学/变换设计与实现。

## 修改记录

- 2026-09-22T10:15:52+08:00：先更新设计并创建开发记录。
- 2026-09-22T10:22:33+08:00：完成 stduuid 迁移、CMake 依赖修正与四组构建/测试验证，同步文档。
- 2026-09-22T10:25:07+08:00：完成文档链接、JSON、时间元数据和差异检查。
