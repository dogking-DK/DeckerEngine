---
id: "0010"
created_at: "2026-09-22T12:17:16+08:00"
updated_at: "2026-09-22T12:27:09+08:00"
status: completed
design_refs:
  - ../design/scene.md
---

# 0010 组件与变换层级

M2.1 基线 cccbe94。按 [Scene 设计](../design/scene.md) 的 M2.2 扩展实现组件、
显式属性描述、层级变换和原子编辑验证。本节不加入序列化和资产加载。

## 实际变更

实现 [Components.hpp](../../engine/scene/include/dk/scene/Components.hpp) 的稳定组件/属性描述、
EntityData 副本与 SceneDocument 名称/TRS/父级写入口。采用双精度局部 TRS 和派生仿射 world，
拓扑队列保留剪切并避免递归。变换/父级修改先计算候选，成功后交换不可变 ECS 负载；
循环、缺失父级、溢出均保留旧值与 revision。仅允许删除叶节点，重挂保留 local。
名称严格检查 UTF-8、NUL 和长度；无操作不递增 revision。
Scene PUBLIC 依赖数学，缺少数学时 CMake 明确报错；未加入额外三方库。

## 验证记录

默认 windows-debug/windows-release build/test 各 112 项通过、1 项既有符号链接权限跳过。
独立 windows-scene-only 配置将 DK_BUILD_MATH 从 OFF 改为 ON，其余保留
关闭日志/IO/示例/runner、DK_VCPKG_FEATURES 为空、DK_WARNINGS_AS_ERRORS=ON；
Debug/Release build/test 各 64/64 通过。
新增 6 项测试覆盖属性描述、UTF-8、副本隔离、传播/剪切、重挂/解绑、循环、
非叶删除、变换异常回退、零/负缩放和 160 层链。无编译警告。
Git diff 和文档链接/元数据检查通过；未验证其他平台及资源耗尽。

## 遗留与下一步

M2.3 工程格式与资产引用。层级编辑复制并重算全体，O(N+E)；
M2 基线不提供增量更新、任意 ECS 访问或保留世界变换的重挂。

## 修改记录

- 2026-09-22T12:27:09+08:00：实现与两组 Debug/Release 验证完成。

- 2026-09-22T12:17:16+08:00：设计扩展并创建开发记录。
