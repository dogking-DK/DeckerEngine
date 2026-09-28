---
id: "0035"
created_at: "2026-09-28T13:54:46+08:00"
updated_at: "2026-09-28T14:08:22+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/assets-importers.md
---

# 0035 M4.3.1 内容键与产物发布

接续 8179f2c，完成用户指定 M4.3 的第一子节。先固定
[缓存键/目录/提交契约](../design/assets-runtime.md#m431-实施契约)，再实现类型化 key、
兼容检查、复用 CPU 产物、current 原子发布、缓存命中和 assetc cache。
复用现有 xxHash/fastgltf/stb 与固定 baseline，无依赖新增或升级。
第二子节 M4.3.2 的清理与失效验收另建 0036，分别本地提交。

## 实现与状态约定

- [CacheFormat.cpp](../../engine/assets/runtime/src/CacheFormat.cpp) 实现有 tag/长度的确定性编码、
  排序无关的输出/依赖描述、严格版本/格式/摘要验证；独立 Python struct 编码和 XXH3 验证固定向量。
- [AssetCache.cpp](../../engine/assets/runtime/src/AssetCache.cpp) 从实际 importer 快照生成 key/产物，
  复用 M4.2 CPU v1 读回校验器。命中核验字节并跳过 importer；source/设置/meta 映射不匹配则重建。
- 新 build 只写独占临时目录，发布不可变目录，再提交合法 meta，最后原子提交 current；故障点覆盖各阶段。
  meta 已提交而 current 失败时保留合法身份和孤立完整产物，错误明确说明；不会宣称 Ready 或成功命中。
  同工程单写者、OOM/ContextError 为基础设施异常、无多文件断电原子性，均与设计一致。
- 新增公共 AssetCache.hpp、assetc cache、目标 dk_asset_cache_tests 与跨进程测试，未改变原 import --output。
  缓存复用原有固定依赖，构建条件仍为 runtime+importers；无新业务命令、Scene/Framework 依赖或协议版本变更。

## 验证

Windows x64 / MSVC 19.51 / Debug，均由 scripts/verify.ps1 执行：

| 范围 | 结果 | out/verify 证据 |
| --- | --- | --- |
| 首次编译与既有 pipeline/assetc | 8/8 通过 | 20260928-140145-bf366fb0 |
| key/命中/提交故障/输入冲突与 assetc 两种真进程 | 6/6 通过 | 20260928-140431-5d1b1b30 |
| 加入独立 golden key 后定向复验 | 1/1 通过 | 20260928-140622-56ae9b8d |

无失败、无跳过。key 测试逐项改变 13 个版本/内容/身份/设置输入，打乱顺序仍相同，并区分字符串字段边界。
故障测试覆盖首次/已有身份与 validate_inputs/publish/metadata/current 的组合，断言索引字节和 meta 阶段状态。
真实新进程验证 Unicode、同输入命中、设置失效和旧产物保留；既有 DLL stderr 提示未改变 stdout JSON。
未执行全量、Release、GPU 或 Tracy capture；失效/清理的完整矩阵属于下一子节。

下一项 M4.3.2，M4.3 尚未整体完成。定向文档检查 7 个文件/243 个本地链接与时间戳/编号/JSON 清单通过，git diff --check 通过。
