---
id: "0014"
created_at: "2026-09-22T13:28:00+08:00"
updated_at: "2026-09-22T13:41:30+08:00"
status: completed
design_refs:
  - ../design/application-services.md
  - ../design/foundation-core.md
---

# 0014 场景服务与编辑操作

基线 9bcb6cb；按 M3 连续授权推进 M3.2，先写 [设计](../design/application-services.md)。

## 实际变更

[SceneService](../../engine/framework/services/include/dk/services/SceneService.hpp) 拥有文档/工程和会话 ID，
[SceneOperations](../../engine/framework/operations/include/dk/operations/SceneOperations.hpp) 注册 12 个场景/实体命令。
编辑、保存和文档替换检查会话/revision，冲突返回 Core conflict=7；加载失败保持原文档。
实体查询输出完整仿射 world_matrix；scene.query 按 ID 分页（默认 128，最大 256）。
资产编辑先解析真实资源，场景/工程清单分别安全保存，清单拒绝直接覆盖场景路径。

集成发现 nlohmann-json 跨 signed/unsigned 比较将 uint64 最大 revision 误判为负值，
已在命令 schema 添加精确整数和浮点边界比较，并同步 enum 深层相等检查（最多 256 项）。
变更先更新 [commands 设计](../design/commands.md)，新增专门边界测试。

## 验证记录

- 初次 MSVC 编译暴露 variant 模板分支不可达警告、Catch2 INFO 三元表达式优先级，均已修正。
- 首次场景测试暴露 uint64 schema 上下界误判；补充诊断和回归后 Debug 全部通过。
- 场景命令测试覆盖父子变换、Unicode、分页、保存重载、ABA guard、非法层级/资源、失败状态保持和 no-op。
- `cmake --build --preset windows-debug/windows-release` 与相应 CTest：各 138 注册，137 通过、1 个既有符号链接权限跳过。
- 独立 commands Debug/Release：各 16/16，保留无 Scene/IO/Eigen 的构建边界。

## 遗留与下一步

M3.3 事务与撤销重做。
本节尚无历史记录；new/load 和保存不声明可撤销。路径行为继承 Foundation，未验证其他平台。
