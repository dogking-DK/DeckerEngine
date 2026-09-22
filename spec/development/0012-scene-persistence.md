---
id: "0012"
created_at: "2026-09-22T12:39:16+08:00"
updated_at: "2026-09-22T13:06:18+08:00"
status: completed
design_refs:
  - ../design/scene.md
  - ../design/project-format.md
  - ../design/foundation-io.md
---

# 0012 场景序列化与安全重载

M2.3 基线 e6d12bc。依据 [Scene 设计](../design/scene.md) 的 M2.4 扩展，
实现一致快照、版本化 JSON、两遍恢复、原子保存和失败保留旧状态。
同步 [IO 设计](../design/foundation-io.md) 的临时文件验证扩展。

## 实际变更

新增 [SceneSnapshot.hpp](../../engine/scene/include/dk/scene/SceneSnapshot.hpp) 与
[SceneIO.hpp](../../engine/scene/include/dk/scene/SceneIO.hpp)：不可变实体副本、文档实例令牌、
场景/组件 v1 JSON、先登记 ID 再解析父级的候选文档恢复、资产校验和安全重载。
内存导入 dirty=true；文件加载恢复 revision 并建立干净基线。
失败不替换 live 文档，成功重载交换所有者；旧文档引用/指针失效，调用方重新查询实体。

保存支持当前/旧快照，成功后按保存 revision 确认 dirty；拒绝跨文档实例快照，
即使相同 SceneId 也不能误确认。旧快照覆盖后即使此前干净，也重新保持 dirty。
Foundation atomic writer 增加可选只读验证器，在关闭临时文件后、替换前逐字节读回校验
和协议解析；验证错误或异常清理临时文件，旧文件保持不变。工程清单同样支持安全保存。

新增 [CPU 场景示例](../../examples/scene/src/main.cpp) 和跨进程 CTest，生成父子和资产引用，
分别保存 scene.json/project.json，再由独立进程加载。无需窗口、GPU、日志或 Catch2。
新增 9 项持久化测试（累计 27 项 Scene），JSON 样本变异覆盖版本、schema、循环、孤儿、
资源缺失、超限、共享冲突和 revision 上限。同步架构、模块设计、README 和 Roadmap。

## 验证记录

Windows 11 / MSVC 19.51 / CMake 4.2.1，依赖版本沿用前节锁定基线。

| 配置 | Debug | Release |
| --- | --- | --- |
| windows-dev | 128 通过，1 权限跳过 | 128 通过，1 权限跳过 |
| windows-scene-only（无日志/runner/示例，警告即错误） | 104 通过，1 权限跳过 | 104 通过，1 权限跳过 |
| windows-scene-cpu（无日志/runner/Catch2，警告即错误） | 16/16 通过 | 16/16 通过 |

命令使用 `cmake --build --preset windows-debug/windows-release` 与对应 `ctest --preset`；
独立配置使用 `cmake --build out/build/<配置> --config Debug/Release`，
以及 `ctest --test-dir out/build/<配置> -C Debug/Release --output-on-failure`。
完整配置参数见 [README](../../README.md) 的两个 Scene 配置入口。
bootstrap 重新自动配置仅安装 stduuid，Debug build 和 1/1 版本测试通过。
跳过项均为既有 Windows 符号链接创建权限不足，未出现新的跳过或最终失败。
文档相对链接、时间/编号/JSON 清单及 git diff --check 检查通过。

开发过程中已解决的失败：测试目录显式 find_package 修复 JSON imported target 作用域；
Windows.h 的 max 宏冲突通过 NOMINMAX 处理。加强旋转精度测试发现原样 TRS 写回被重复
归一化而误增 revision，修复精确无操作判断和导入已单位化四元数的末位保持；
32 组非平凡旋转的字段、文本往返与 revision 验证由失败转为通过。
上述修复后重新执行全量 Debug/Release 与独立配置验收。

## 遗留与下一步

完成 M2 后停止于本次授权边界；下一里程碑为 M3.1 命令注册与能力发现。
当前仅验收 Windows 本地文件；不包含跨平台原子替换、断电恢复、并发编辑、
多文件事务、版本迁移或资产内容解码。层级编辑以 O(N+E) 全体重算为基线，
最多 10000 实体和 16 MiB JSON；flecs 致命错误与资源耗尽未实测。

## 修改记录

- 2026-09-22T12:39:16+08:00：扩展设计并建立记录。
- 2026-09-22T13:04:44+08:00：实现、精度修复、故障回归及三组 Debug/Release 验收完成。
