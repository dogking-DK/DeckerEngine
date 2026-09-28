---
id: "0040"
created_at: "2026-09-28T15:13:00+08:00"
updated_at: "2026-09-28T15:29:44+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/foundation-jobs.md
  - ../design/runtime.md
  - ../design/automation-protocol.md
---

# M4.4.4 CPU 资产交付验收

固定 [资产夹具](../../tests/fixtures/assets) 验证真实 stdio 的登记、导入、load/Ready、
依赖改变后重建、普通失败恢复、取消/卸载、Scene 引用、shutdown/EOF 和重启身份。
核对持久 AssetId 保留、目录/JobId 会话重建、退出不隐式保存 Scene；仅 CPU，不链接窗口/GPU。
独立关闭 Scene/Framework 构建验证 Jobs+资产基础层，再检查关闭 Memory/Jobs 的原 M3 runner 配置。

M4.4.1–4 全部完成，M4.4 与 M4 可关闭。下一项 M5.1，下一开发编号 0041。

## 完成内容

- 扩展 [RuntimeAssetsTest.ps1](../../tests/integration/RuntimeAssetsTest.ps1)：先登记身份，导入 mesh/material/texture，
  stdin 保持打开且不发送下一请求仍发布；load Ready、同大小/mtime 内容改变导致新 key、缺失依赖失败后恢复。
  检查接受取消不更改 current，终态后取消幂等；确定性 queued/running/完成前取消由 Jobs/AsyncAssets 闸门测试覆盖。
- 三次真实进程验证 AssetId 与所有子输出 ID 保留，旧 JobId/目录 guard 失效；Scene 引用可重载，
  未保存编辑不因 shutdown 落盘；EOF 含未结束最后一行与后台工作、shutdown 含超出 reader 容量的后续输入均可回收。
- 验收补充修复：新建 Scene 保存到活动资产清单前合入已登记映射；更新工程 name/scene 时保持目录 guard。
  对应单元与进程回归通过，避免先覆盖资产映射再报告快照冲突。
- 作业摘要/错误截断释放超额 capacity，UTF-8 截断保持完整字符；诊断分配异常同样上报宿主 fatal。
- 关闭将所有活动项转换为 cancelled 时也按完成顺序执行终态保留上限，避免关闭后历史超额。

## 验证与复现

所有下列检查均为 Debug，无跳过；未执行全量、Release 或新的 Tracy 性能采集。

| 配置与目标 | 筛选/结果 | 证据目录 |
| --- | --- | --- |
| windows-dev：dk_run、dk_asset_command_tests、dk_service_tests、dk_asset_tests、dk_protocol_tests、dk_jobs_tests | Jobs、资产命令、Scene/资产服务、协议、四项进程，36/36 | out/verify/20260928-152101-f1ba2ba4 |
| windows-assetc-only，/WX，Scene/Framework/Logging/Runner OFF：dk_jobs_tests、dk_async_asset_tests、dk_asset_cache_tests、dk_asset_pipeline_tests、dk_assetc | Jobs/async_assets/cache/pipeline/assetc，35/35 | out/verify/20260928-151812-348db31f |
| 同独立配置：dk_jobs_tests | 最后 UTF-8 边界变更单独重验，1/1 | out/verify/20260928-152224-a88de359 |
| windows-dev：dk_jobs_tests、dk_run | 最后关闭保留上限变更后全部 Jobs 与 CPU 进程闭环，11/11 | out/verify/20260928-152735-a20b71c3 |
| windows-runtime-cpu，/WX，Memory/Jobs/Assets OFF：dk_run | 原 M3 stdio 与 batch 进程，3/3 | out/verify/20260928-151930-6f95b61d |

脚本入口为 [verify.ps1](../../scripts/verify.ps1)，记录 summary.json/build.log/tests.log/results.xml；
确切目标/筛选/配置在对应 summary 中。真实进程例子：

```powershell
& ./scripts/verify.ps1 -Target @('dk_run') -TestRegex '^dk\.runtime\.assets_stdio$' -Reason 'M4 CPU delivery'
```

dumpbin /dependents 核对 dk-run 依赖 flecs、xxHash、fastgltf、mimalloc 和系统 CRT，无窗口/GPU 库；
mimalloc-debug.dll 不再依赖 redirect DLL（0039 overlay 生效）。
文档/链接/时间/编号和 git diff --check 在提交前核验。

## 限制

一个 worker，协作取消只发生在阶段边界；三方解码和可完成 IO 可能延迟退出，不承诺硬截止。
meta/current 不提供跨文件断电原子事务；现有 Windows IO 长路径限制保持。
CPU Ready 与未来 GPU 上传分离；不提供文件监视、自动重建、网络连接或后台 JSON-RPC 推送。
