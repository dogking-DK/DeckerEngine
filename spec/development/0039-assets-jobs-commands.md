---
id: "0039"
created_at: "2026-09-28T14:47:00+08:00"
updated_at: "2026-09-28T15:11:42+08:00"
status: completed
design_refs:
  - ../design/runtime.md
  - ../design/application-services.md
  - ../design/foundation-jobs.md
  - ../design/automation-protocol.md
---

# M4.4.3 服务、命令和事件驱动 stdio

实现 Runtime 装配、AsyncAssetService、资产/作业 Operations、目录 guard 与命令参考，
使用有界 reader 队列和 RuntimeEvents 让主线程在 stdin 打开时消费完成项。
关闭必须先刷新响应，再取消读取/工作、join、释放拥有资源。

验证计划：先用现有 stdio 进程验证新 reader 的 EOF/shutdown/错误恢复，再验证新命令发现/schema、
目录与 Scene 状态隔离、资产提交与 Ready 区别以及 jobs.wait/cancel。已完成下列检查。

## 实现与证据

- 新增 AsyncAssetService/AssetOperations，33 条命令；open/catalog 独立目录会话、guard、CPU 状态和有界 jobs.wait。Runtime 自动管理内存路由与队列生命周期。
- SceneService 同清单映射同步保留文档/历史；project.save 刷新等价资产清单快照，防止下次登记误冲突。
- Windows 有界 reader + RuntimeEvents，重复 CancelSynchronousIo 覆盖进入 ReadFile 的竞态，退出先唤醒满队列再 join。
- 初始 reader 真实 stdio 验证通过：out/verify/20260928-144831-a8ff56bf。
- 最终直接受影响的协议、资产状态、服务、命令和四个进程用例 22/22 通过、无跳过：out/verify/20260928-150929-f514d8f1。含实际 runner 的 commands.list/describe 和 stdin 空闲完成发布。
- Memory 上下文 14 项 + context_probe 通过：out/verify/20260928-150307-a81a572c；该轮总 20/21，唯一失败为测试误用 entity.id，改为 created_id 后已在最终轮通过。
- 集成暴露官方 mimalloc port 的 MI_WIN_REDIRECT=ON（即使 MI_OVERRIDE=OFF）。同版本 overlay 仅关闭 redirect；debug 缓存确认两项 OFF，真实进程 stderr 为空。基线/版本/源码哈希不变。
- 曾因重复 CMake target、测试夹具路径超过 Windows IO 限额而失败；均已修复重跑。夹具使用短唯一工程根，未声称支持超长 Windows 路径。
- 未运行全量、Release、Tracy 或完整 M4 重启链路；下一项 M4.4.4。
