---
id: "0016"
created_at: "2026-09-22T13:50:31+08:00"
updated_at: "2026-09-22T14:05:00+08:00"
status: completed
design_refs:
  - ../design/runtime.md
  - ../design/automation-protocol.md
---

# 0016 CPU Runtime 与 CLI 批处理

基线 0b35813。先设计 [Runtime](../design/runtime.md) 与 [协议](../design/automation-protocol.md)，
串联已有命令/服务/事务，扩展 dk-run 的 CPU 入口。

## 实际变更

新增 [Runtime](../../engine/framework/runtime/include/dk/runtime/Runtime.hpp) 生命周期装配与重入保护，
[JSON-RPC](../../engine/automation/protocol/include/dk/automation/JsonRpc.hpp) 和
[JSON Lines](../../engine/automation/transport/include/dk/automation/JsonLines.hpp) 传输分别独立 target。
dk-run 使用原生 Windows 宽字符路径，保留版本入口，新增批处理/显式 auto-guard；诊断直接写 stderr，
不依赖 logging。支持通知/混合协议 batch、业务错误映射、每行排空与即时 flush。
新增可运行的 [创建示例](../../examples/automation/create-scene.jsonl) / [加载示例](../../examples/automation/load-scene.jsonl)。

## 验证记录

- 编译时修正 optional<Json>/Json 三元表达式歧义和测试中的 JSON 数组构造调用。
- 首次进程错误流测试发现 CMake 将 JSON 错误消息中的分号当成列表分隔符；修正测试转义后通过，协议通知本身正常。
- `cmake --build --preset windows-debug/windows-release` 和对应 CTest：各 150 注册，149 通过、1 个既有符号链接权限跳过。
- README 的 windows-runtime-cpu 配置只装配 stduuid/Eigen/flecs/JSON，关闭日志/示例/Catch2；Debug/Release 各 3/3。
- 最小 bootstrap Debug/Release 各 1/1，原版本入口保持兼容。
- 4 项协议单测和 2 项进程测试覆盖 ID/参数/错误/通知/batch、超长行排空、末行无换行、流失败、
  父子实体/变换保存后第二进程加载相同状态，以及 Unicode/空格工程路径和 stdout/stderr 分离。
- 实际运行 examples/automation 两份 JSONL：第二进程返回 2 实体、revision=1、dirty=false。
- 文档链接/元数据和 git diff --check 通过；未运行窗口/GPU 或非 Windows 验证。

## 遗留与下一步

M3.5 持续 stdio、同步任务状态和交付 A。
