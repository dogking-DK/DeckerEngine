---
id: "0037"
created_at: "2026-09-28T14:29:43+08:00"
updated_at: "2026-09-28T14:36:48+08:00"
status: completed
design_refs:
  - ../design/foundation-jobs.md
  - ../design/foundation-memory.md
---

# M4.4.1 CPU 工作队列

依据 [Jobs 设计](../design/foundation-jobs.md) 实现独立单 worker、有界输入/完成/终态、
JobId、路由捕获、协作取消、owner 确认发布与关闭。无资产/JSON/Scene 依赖。

验证计划：确定性闸门覆盖 queued/running 取消、等待超时、拒绝容量、终态淘汰、
回调锁外执行、worker 异常向宿主传播、跨系统持久结果与 join 回收。
已实现 `dk_jobs`、独立 `DK_BUILD_JOBS` 开关及 windows-dev 装配。队列存储和记录使用专属持久资源，worker 捕获路由并在安全点清空本线程上下文；终态只留有界元数据。后续小节见 [M4 安排](0020-m4-development-plan.md)。

## 验证结果

- windows-dev Debug：dk_jobs_tests / ^dk\.jobs\.，8/8 通过，无跳过。
- 证据：out/verify/20260928-143603-61f07d63/summary.json。
- 覆盖 owner 发布、排队/运行取消、三个容量约束、终态淘汰、wait 超时/关闭唤醒、跨系统路由、退休系统拒绝、worker/publication 异常。
- 未运行全量、Release 或 Tracy 采集；未新增第三方依赖。下一项 M4.4.2。
