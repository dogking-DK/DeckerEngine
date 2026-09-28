---
id: "0036"
created_at: "2026-09-28T14:09:03+08:00"
updated_at: "2026-09-28T14:22:33+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/assets-importers.md
---

# 0036 M4.3.2 失效、重建与显式清理

接续 6e8582b，按用户指定完成 M4.3 后半段。先补充
[有界清理与失败契约](../design/assets-runtime.md#m432-实施契约)，再实现清理 API/CLI，
补齐源/图片/参数/删除/损坏/未知版本、失败索引保护与跨进程验收。
复用现有依赖，不升级 baseline；缓存为可重建数据，不修改源/meta/工程清单/恢复操作目录。

## 实际实现与边界

- [CacheClean.cpp](../../engine/assets/runtime/src/CacheClean.cpp) 先有界枚举全部 current 与 key/build，
  任意坏索引、未恢复操作或全局扫描超限都在删除前拒绝。只对完整兼容、三文件集合精确、
  无硬链接/reparse、未被任何 current 引用的 build 形成候选。
- 删除前再次核对索引集合/字节和候选摘要，逐个删除验证过的文件及空 build 目录。
  不递归删除，不清理 key 桶/未知/损坏/额外文件/tmp；保留诊断。已返回的拥有型 CPU 值不受磁盘清理影响。
  API/CLI 提供 removed/retained/skipped/failed 和诊断，逐文件 IO 失败可部分完成；不会伪造全量成功。
- `dk-assetc cache-clean --project-root ROOT` 使用同一管线；正常/部分清理输出 JSON，部分失败退出 1 并补 stderr。
  参数错退出 2，前置失败退出 1 且只写 stderr；原 import/cache 调用保持兼容。
- 失效矩阵验证源 buffer/材质/图片/URI、设置变化，以及缓存删除、坏 JSON/数据/摘要、未知算法/实现/产物版本。
  重建更换物理 build 目录，保留持久 ID；同输入 key 相同，未知旧条目不被覆盖。
  current 为首版缓存查找入口，没有 current 时重新导入，不做历史 key 搜索或 LRU。

新增 API/测试/CLI 不引入新的三方依赖。更新运行时/导入器/架构设计、Roadmap、使用说明和开发索引。
同步单写者、meta 与 current 两阶段提交、无多文件断电事务的边界沿用 0035，未接入 Jobs/Ready/业务命令。

## 实际验证

Windows x64 / VS2026 / MSVC 19.51 / Debug，均使用版本化 scripts/verify.ps1：

| 范围 | 结果 | out/verify 证据 |
| --- | --- | --- |
| 默认缓存/失效/清理与两个 assetc 真进程 | 13/13 通过 | 20260928-141451-bc6e060d |
| 新增硬链接删除保护后定向复验 | 1/1 通过 | 20260928-141649-ed7c3a9f |
| 独立 assetc-only，Scene/Framework/Logging/Runner OFF，/WX ON | 14/14 通过 | 20260928-141646-e6eaa659 |

全部无失败/跳过。覆盖 13 种条目/索引损坏删除场景、4 种内容依赖变化、5 类失败源/身份/恢复记录状态；
清理覆盖正常回收、坏索引/预算/并发 current、未知文件/版本/摘要、硬链接、逐文件注入失败。
跨进程覆盖冷构建→命中→参数失效→坏缓存重建→清理→继续命中；stdout JSON 可解析，ID 稳定。
未跑全量、Release、GPU 或 Tracy；未声明性能收益、跨进程锁或断电恢复。原有 mimalloc DLL 的 stderr 诊断仍保留。

README 示例实际通过：out/asset-cache-demo-e77b115f9f974031bde513fcd9b28949，冷构建 false、重复命中 true、改设置 false，ID 不变，清理 removed=1/retained=1。
文档检查 10 个 Markdown/347 个本地链接、时间戳/开发编号/JSON 清单全部通过；git diff --check 通过。

## 下一项

M4.3.1 已提交 6e8582b，M4.3.2 完成后关闭整个 M4.3。
下一项 **M4.4.1 CPU 工作队列**，下一可用开发编号 **0037**；M4 整体仍进行中。
