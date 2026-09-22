---
id: "0017"
created_at: "2026-09-22T14:05:59+08:00"
updated_at: "2026-09-22T14:17:51+08:00"
status: completed
design_refs:
  - ../design/runtime.md
  - ../design/automation-protocol.md
---

# 0017 持续 stdio 与交付 A

基线 75279d3；先补充 [Runtime 任务/关闭设计](../design/runtime.md) 和
[协议终态契约](../design/automation-protocol.md)，完成 M3 最后一节。

## 实际变更

Runtime 分派生成 TaskId，返回同步终态并保留最多 256 条元数据；新增 capabilities、tasks.list/get、shutdown。
JSON-RPC 成功与业务错误携带任务 ID/状态，协议级错误不生成任务；result.value 保持原命令结果位置。
JSON Lines 复用批处理实现，stdio 要求显式 guard，正常 EOF/关闭返回 0、可恢复错误继续；
shutdown 刷新当前响应后停止，协议 batch 后续命令被拒绝。Windows stdin/stdout 设为二进制字节流。
runner 新增 --stdio 参数互斥检查；JSONL 文件固定 LF，README 增加交互和任务说明。
新增真实 [stdio 子进程测试](../../tests/integration/RuntimeStdioTest.ps1)，CTest 自动找到 PowerShell/pwsh；
无窗口启动，finally 清理测试进程，超时即失败，不留下后台服务。

## 验证记录

- 首轮默认 Debug/Release 各 152 注册，151 通过、1 个既有符号链接权限跳过（加入进程测试前）。
- 新任务测试覆盖 succeeded/failed、元数据、256 项淘汰、未知任务、无场景错误以及编辑后的 revision。
- EOF/通知 shutdown/同 batch 后续命令拒绝有单元测试，已停止 Runtime 不再分派。
- 直接运行 PowerShell 交互脚本通过：stdin 持续打开时即时响应、事务/冲突/任务、Unicode、撤销重做、
  重复键、通知/混合 batch、保存、shutdown、第二进程重载和错误后 EOF=0；stderr 保持为空。
- 最终 `cmake --build --preset windows-debug/windows-release` 及对应 CTest 各 153 注册、152 通过、
  1 个既有符号链接权限跳过；MSVC 警告视作错误。
- README 的 windows-runtime-cpu 配置 Debug/Release 各 4/4：版本、持续 stdio、批处理重载和错误流；
  无日志/示例/Catch2/窗口/GPU。windows-bootstrap-stduuid 双配置各 1/1。
- 同一 stdio 脚本在 pwsh 和 Windows 自带 powershell.exe 均通过，不要求额外安装 PowerShell 7。
- 文档元数据/本地链接、开发编号、JSON 清单检查和 git diff --check 通过。

## 交付结果与实际限制

M3.1–M3.5 按设计、编号记录、测试和独立详细本地提交完成，交付 A 已验收：
无窗口/GPU 从空进程创建父子场景、编辑/事务/撤销、保存、重启加载，stdout 为结构化 JSON。
任务均为同步终态，最近 256 条仅内存保留；没有异步 wait/cancel、网络传输或图形功能。
场景/清单分别保存，外部文件效果不可撤销；历史预算为逻辑载荷，协议输出失败不等于修改未执行。
只验证当前 Windows 本地文件范围，其他平台和崩溃/断电恢复不在本次验收内。

## 遗留与下一步

本次范围止于 M3.5。完成交付 A 后，下一阶段为 M4.1；不自动开始 M4。
