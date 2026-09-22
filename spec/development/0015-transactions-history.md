---
id: "0015"
created_at: "2026-09-22T13:42:16+08:00"
updated_at: "2026-09-22T13:50:31+08:00"
status: completed
design_refs:
  - ../design/application-services.md
  - ../design/scene.md
---

# 0015 事务与撤销重做

基线 3dbd67c，连续开发 M3.3；先扩展 [事务设计](../design/application-services.md)
和 [内存快照提交](../design/scene.md)。

## 实际变更

SceneSnapshot 增加内容比较/逻辑载荷计算；SceneDocument::stage 创建 revision=0 的独立世界，
apply_snapshot 预构建候选内容并交换，保留原快照来源、递增真实 revision，失败/同内容保持状态。
SceneService 的单条/批量编辑统一先暂存后提交，历史 before/after 快照独立于文件 IO。
历史最多 64 单元/32 MiB 逻辑载荷，可设更小限额；超额淘汰旧 undo，单项超额修改前拒绝。
新增 scene.transaction 与 history.status/undo/redo，复用实体命令 schema/解码器，禁止批内外部操作和嵌套 guard。

## 验证记录

新增事务/历史测试覆盖单次 revision、失败回滚、稳定 ID/层级恢复、redo 分支与 no-op、预算限制、
new/load 清空、资源文件缺失时撤销、保存 dirty、快照来源兼容和 uint64 revision 溢出保护。
`cmake --build --preset windows-debug/windows-release` 和相应 CTest 最终各 144 注册、143 通过、
1 个既有符号链接权限跳过；MSVC 警告视作错误。新增 6 个行为测试，文档链接/元数据检查与 diff --check 通过。

## 遗留与下一步

M3.4 CPU Runtime 与 CLI 批处理。
历史容量为逻辑载荷预算，未声称进程峰值内存限制；真实 OOM 或进程终止不提供持久化事务保证。
