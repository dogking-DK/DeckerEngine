---
id: "0013"
created_at: "2026-09-22T13:12:54+08:00"
updated_at: "2026-09-22T13:26:36+08:00"
status: completed
design_refs:
  - ../design/commands.md
---

# 0013 命令注册与能力发现

基线 55bb8fd，用户授权开发 M3 全部五节。先写 [commands](../design/commands.md)，
本节建立独立命令层，后续逐节连接场景服务、事务、Runtime 和 stdio。

## 实际变更

[commands](../../engine/framework/commands/include/dk/commands/CommandRegistry.hpp) 注册表持有描述和 handler，
提供排序发现、完整 schema、参数预检和结果契约检查；业务错误追加命令上下文，普通异常转换为内部错误。
Schema 递归校验嵌套对象/数组、未知字段、nullable、整数 token、Unicode 长度和有限数值；
严格解析拒绝重复键、尾随文本、非法 UTF-8 和结构/字节超限。enum 注册使用有序集合检查重复。
新增 framework 选项和仅 JSON 的 commands feature；默认开发启用，bootstrap 保持隔离。
README 的独立 Foundation 命令显式关闭 FRAMEWORK，新增 commands 最小配置说明。

## 验证记录

- `cmake --preset windows-dev`，`cmake --build --preset windows-debug/windows-release`，
  对应 `ctest --preset windows-debug/windows-release`：各 134 注册，133 通过、1 个既有符号链接权限跳过。
- README 的 windows-commands-only 配置，Debug/Release 构建（警告视作错误）和 CTest 各 15/15；
  vcpkg 仅 stduuid、JSON、Catch2 及构建辅助依赖。
- 新增 5 项命令行为测试覆盖发现/注册/嵌套 schema/handler 故障/严格解析。
- 尚未接入场景服务、进程传输或并发；本节不声称任意 handler 的副作用可回滚。

## 遗留与下一步

M3.2 场景服务与编辑操作。

## 修改记录

- 2026-09-22T13:12:54+08:00：读取既有流程和架构，建立设计与记录。
- 2026-09-22T13:26:36+08:00：完成独立命令层及双配置验证，准备本地提交。
