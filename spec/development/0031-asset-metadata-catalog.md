---
id: "0031"
created_at: "2026-09-28T10:29:00+08:00"
updated_at: "2026-09-28T16:00:00+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/assets-types.md
  - ../design/project-format.md
---

# 0031 M4.1.1 元数据与身份目录

基线 73de1ec，初始工作区干净。按 spec-workflow、state-contracts、build-verify，
先细化 [M4.1.1 契约](../design/assets-runtime.md#m411-实施契约)，再实现 meta v1、只读目录和 Project 候选适配。
本阶段不写 sidecar/Project、不发布目录 revision；多文件提交、恢复记录和改名留在 M4.1.2。

## 实现范围

复用固定 nlohmann-json、Memory 和 StableId，不新增/升级依赖。
持久结果拥有内存资源，严格 JSON/路径/数量验证，显式首次登记与旧单 mesh ID 采用，重复及增量映射保留 ID。
验证有效候选、失败/过期输入不修改旧状态，以及旧 Project v1 无 meta 兼容；默认只运行定向 Debug 测试。

## 实现与验证进展

已实现 runtime codec/catalog、services 的只读 Project 适配和独立构建开关，新增 18 个定向用例。
首轮 Debug 新用例与旧 Project 直接兼容测试共 25/25 通过（`out/verify/20260928-103901-48fa6e84`）。
随后配置 Scene/Framework 关闭的独立资产构建时发现测试 target 缺少本目录作用域的 JSON imported target，
已补 `find_package(nlohmann_json)`；这是测试 CMake 依赖声明缺漏，不通过链接 Scene 绕过。

实际文件：

- [Metadata.hpp](../../engine/assets/runtime/include/dk/assets/Metadata.hpp)、[Metadata.cpp](../../engine/assets/runtime/src/Metadata.cpp)：
  严格 meta v1 编解码、有限正数 settings、root/selector/ID 一致性、2 MiB/16 层/10000 输出预算和稳定排序。
- [Catalog.hpp](../../engine/assets/runtime/include/dk/assets/Catalog.hpp)、[Catalog.cpp](../../engine/assets/runtime/src/Catalog.cpp)：
  独立只读快照、会话/revision guard、源/meta 读取、显式首次登记/旧 mesh 采用、增量映射和提交前只读检查。
  未请求的旧 output 保留；跨源 ID、kind、同源旧记录以及 Windows ordinal 大小写别名冲突均拒绝。
- [AssetRegistration.hpp](../../engine/framework/services/include/dk/services/AssetRegistration.hpp)、
  [适配实现](../../engine/framework/services/src/AssetRegistration.cpp)：将旧 Project 转为目录，核对快照再构造新 Project；
  经 Project::create 完整验证，源路径/名称/scene 保持既有协议，不暴露可变 Project 索引。
- [CMake 开关](../../cmake/Options.cmake)、assets/runtime 与 asset_services target、assets vcpkg feature：
  复用 nlohmann-json 3.12.0#2 / mimalloc 3.5.3 / stduuid；builtin-baseline 未变。
  windows-dev/profiling 开启可选模块；README 的 Memory/IO 关闭配置显式禁用新模块。
- [codec 测试](../../tests/unit/AssetMetadataTests.cpp)、[目录测试](../../tests/unit/AssetCatalogTests.cpp)、
  [Project 适配测试](../../tests/unit/AssetRegistrationTests.cpp)：18 个新用例；验证资源持有、预算异常、
  原文件/目录/Project/Scene 不变、Unicode 路径、实际 sidecar 字节变化/删除、非法输入和数量边界。

## 接口决策与失败保证

完整登记提交的输入契约体现为只读 `RegistrationCandidate`（base/next guard、原 meta 字节/不存在标记、
新 meta/完整记录）和 `ProjectRegistrationCandidate`（经验证的新 Project）。没有导出可被误用的空 commit 实现。
M4.1.2 需要消费这些值并再次核验，持久提交、操作记录和发布完成后才更新目录 revision；当前初始 revision 始终为 0。
语义 no-op 候选 next guard 不增加 revision；新候选未落盘前由调用者持有，重复 prepare 不提供随机 ID 去重缓存。

缺失 meta 默认 not_found；只有显式 create_or_adopt 可新建或采用唯一同源 mesh。坏 meta 不自动重建，
多条旧记录不能猜测 selector。目录 guard/身份冲突及 sidecar 字节/存在性变化为 conflict；源缺失为 not_found，
结构/参数错误为 invalid_argument，未知 meta/importer 版本为 not_supported；IO 错误保留路径上下文。
新持久容器持有 Memory 资源；ContextError/bad_alloc 沿用 Memory 异常契约，所有准备步骤无输入状态或磁盘提交点。
IO/JSON DOM、查找临时容器和旧 Project 边界复用标准存储，未建设通用 JSON/哈希模块，也未变更旧 Project 实现。

## 验证结果

Windows x64 / MSVC 19.51.36257.0，Debug，`/W4 /WX`。相关配置/验证命令已写入
[README](../guides/assets.md#资产元数据登记与改名m41)，下列证据均由 scripts/verify.ps1 产生：

| 检查 | 结果 | 证据目录 |
| --- | --- | --- |
| dk_asset_tests + dk_scene_tests，按 README 的新资产与 Project 兼容筛选 | 25/25 通过：18 个新用例 + 7 个 Project 兼容用例 | `out/verify/20260928-103901-48fa6e84` |
| 关闭 Scene/Framework/math/logging 的独立 dk_asset_tests，`^dk\.assets\.` | 15/15 底层用例通过 | `out/verify/20260928-104244-7e5c906c` |
| 最终 catalog/Project 适配及严格标量版本类型重验 | 13/13 通过 | `out/verify/20260928-104500-6dc3144a` |
| 独立配置补充源删除、sidecar 删除/变化、路径/数量预算断言 | 3/3 通过 | `out/verify/20260928-104516-d83097dc` |
| ASSET_RUNTIME=OFF 的 bootstrap，dk_run / `^dk\.bootstrap\.version$` | 1/1 通过，无 Memory/Scene/JSON 依赖 | `out/verify/20260928-105121-2776b2a2` |

重复验证不累加为新增用例数；25 项功能/兼容用例以及 bootstrap 启动检查覆盖本次范围。
边界补充只重跑受影响检查，未再次运行已有效的 10000-output 编解码大用例。
独立配置首次生成失败的 JSON imported target 问题已修复，后续同配置生成、构建和测试均通过。
收尾 `check-spec.ps1 -Path` 检查 12 个 Markdown、313 个本地链接、时间戳、开发编号及 JSON 清单，通过；
`git diff --check` 通过。独立资产安装树没有 flecs/Eigen，bootstrap 安装树没有 JSON/Memory，核对与实际链接边界一致。

## 限制与下一步

本节没有写 meta/清单、持久提交/恢复/改名、源内容解析、CPU Ready/缓存、Jobs 或新命令。
路径约束沿用 IO 的词法边界，不保证子符号链接隔离、并发文件快照、多进程写入或物理别名归一化。
只读 validate 之后仍可能被外部修改，不能替代未来提交步骤中的再次验证。
未运行全引擎回归、Release、profiling capture、其他平台、sanitizer 或真实 glTF 导入；无相应完成声明。
README 的 C++ 片段在既有宿主入口内使用，完整行为由 ProjectRegistrationTests 执行；其他旧模块独立构建示例未逐一重跑。

M4.1.1 完成；M4.1 和 M4 仍进行中。下一项为 **M4.1.2 登记提交与受控改名**，
需固定操作记录/恢复与关闭边界并接入 XXH3-128，下一可用开发编号 **0032**（开工前重查）。
