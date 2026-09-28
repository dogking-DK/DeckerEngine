---
id: "0038"
created_at: "2026-09-28T14:38:00+08:00"
updated_at: "2026-09-28T14:45:27+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/foundation-jobs.md
  - ../design/assets-importers.md
---

# M4.4.2 异步资产状态与主线程发布

按 [资产设计](../design/assets-runtime.md) 拆分缓存 prepare/publish，保留同步 assetc 行为，
引入 AsyncAssets 的会话、generation、Loading/Ready/Failed 与只读拥有句柄。
取消/卸载/旧会话结果不得更新 meta/current/Ready；worker 不持有服务或 Scene 引用。

计划定向验证异步准备不发布、接受取消后的磁盘与 Ready 不变、旧代晚到、卸载后句柄、
缓存直接受影响回归和独立 CPU 构建。已完成下列定向验证。

## 实现与验证

- AssetCache 拆分 prepare/publish；同步 compile_cached_asset 复用新路径。Prepared 析构清理拥有的未提交产物，保留外来改动。
- 新增 AsyncAssets 与可控 prepare 注入点；Jobs 开启时构建，独立同步 assetc 不依赖 Jobs。
- 缓存/产物 19/19 通过：out/verify/20260928-144145-1b1a7983。
- 异步资产/导入器 15/15 通过：out/verify/20260928-144357-1ed81f31；无跳过。
- dk_assetc 同步目标已重建。独立 CPU 配置及真实进程闭环留至 M4.4.4 一次执行；未跑全量、Release 或性能采集。
- 原有 current 提交失败可保留合法 meta 与完整孤立产物，仍不承诺跨文件原子事务。下一项 M4.4.3。
