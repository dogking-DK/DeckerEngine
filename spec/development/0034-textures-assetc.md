---
id: "0034"
created_at: "2026-09-28T12:40:00+08:00"
updated_at: "2026-09-28T16:00:00+08:00"
status: completed
design_refs:
  - ../design/assets-importers.md
  - ../design/assets-runtime.md
---

# 0034 M4.2.2 纹理与离线工具

接续 5fa3771，完成用户授权的 M4.2 后半段。先固定 [纹理/产物/工具契约](../design/assets-importers.md#m422-实施契约)，
接入已核验的 stb 2024-07-29#1，完成 PNG/JPEG、材质/采样关联、CPU 产物 v1 与 dk-assetc。
不实现缓存、Jobs、业务命令或 Project 自动登记。

## 实际交付

- `TextureData` 保存 RGBA8、sRGB/顶行顺序、sampler、来源与 texture/N 身份。
  单个 stb 实现单元限制 PNG/JPEG 内存解码，拒绝 16 位、坏图、MIME 不符、缺失 UV0 和超预算；
  纹理输出去重按 texture selector，不把不同 sampler 的同一 image 合并。
- [AssetCompiler](../../engine/assets/runtime/include/dk/assets/AssetCompiler.hpp) 同步导入候选、复用旧 meta ID/settings，
  先在独占临时目录写入/读回 CPU 产物，再比对输入/旧 meta，独占目录改名发布，最后原子提交 meta。
  普通失败清理本次拥有且未变的文件；外部改写/未知内容不删除，清理失败附原错误与路径。
  语义未变的旧 meta 保留原字节和时间；旧映射已缺失、坏 meta、已有输出、未恢复 M4.1 操作均拒绝。
- [CpuArtifact](../../engine/assets/runtime/include/dk/assets/CpuArtifact.hpp) 固定 v1 manifest + little-endian data.bin，
  包含几何、材质、图片、全部 ID 关联、来源/输入摘要/诊断；复用 XXH3-128，严格验证版本、摘要、
  连续 offset/count、形状/预算/有限数、边界和引用后返回拥有型 CPU 数据。
- [dk-assetc](../../tools/assetc/src/main.cpp) 使用同一入口，Unicode 原生参数、stdout JSON、stderr 诊断，
  返回 0/1/2/3；自身装配 MemorySystem/Assets 域/线程 scratch。不依赖 Scene、Framework、窗口或 renderer。
  runtime+importers 同时开启时才提供编译/产物 API 与工具，runtime 的导入 API 依赖以 PUBLIC importers 表达。
- [自制夹具](../../tests/fixtures/assets/generate.py) 可再生成 PNG、16 位 PNG、JPEG、外部 glTF 与内嵌 GLB，
  图片无外部版权内容；JPEG 生成使用本机 Pillow 11.1.0，正常构建/测试只读已版本化夹具，不依赖 Python/Pillow。
  定向测试包含源/图片改变、meta 改变、目标冲突、发布故障和未知文件保留，真实进程覆盖完整导入与重导入。

## 依赖和设计边界

2026-09-28 实时核对 [官方 stb port](https://github.com/microsoft/vcpkg/blob/master/ports/stb/vcpkg.json)
与固定 `33d78c1ed898a06938f31312167c7abefd229455` 的 port：均为 2024-07-29#1（stb_image 2.30），
许可证 MIT OR CC-PDDC。asset-importers feature 增加 stb，find_package(Stb) / SYSTEM PRIVATE include，
固定 builtin-baseline 保持不变。依赖安装、编译成功，无其他库升级。

同工程写入需由调用者串行化，路径/摘要复查不提供跨进程锁或敌意并发文件系统隔离。
meta 为最终持久身份；进程中断可留下孤立临时/完整产物，本阶段没有多文件断电事务或自动恢复孤立目录。
CPU 输出上限 256 MiB，图片单边 8192/单张 64 MiB/总 RGBA 128 MiB，manifest 16 MiB；
这些是输入/输出上限，不声称覆盖全部第三方临时分配或进程 RSS。OOM/ContextError 保持基础设施异常语义。
没有缓存命中/current 索引、Jobs、Ready 或 GPU 能力；M4.3 复用本格式继续实现。

## 实际验证

Windows / VS 2026 / MSVC 19.51 / x64-windows / Debug，使用版本化 verify.ps1，未执行全量或 Release。

| 范围 | 结果 | 证据（仓库内 out/verify） |
| --- | --- | --- |
| 新增工具初次编译及旧网格回归 | 7/7 通过 | 20260928-125359-6834b375 |
| 首次新增测试发现 | 构建通过；JSON 发现失败，测试未运行 | 20260928-125827-74357615 |
| 默认配置纹理/产物/提交/真实 CLI | 18/18 通过，无跳过 | 20260928-130210-d78dc7e4 |
| 最终 JSON 摘要/源预算/改动产物清理后，复验直接受影响管线与 CLI | 8/8 通过，无跳过 | 20260928-130449-e9da8c32 |
| 独立 assetc：Scene/Framework/Logging/Examples/Runner OFF，/WX ON | 18/18 通过，无跳过 | 20260928-130448-7448e355 |

发现失败原因是 mimalloc-redirect DLL 在 Catch 列举测试时向 stderr 写入初始化顺序诊断，
verify.ps1 原先把它与 stdout JSON 合并。已修正脚本为 discovery.json 和 discovery-stderr.log 分别留存，
保留原诊断，不过滤/吞掉错误、不改变退出码或测试选择。所选程序功能测试全部通过，mimalloc 仍为现有
MI_OVERRIDE=OFF 包；未在本任务更改 allocator 依赖/链接政策。该现象不影响 CLI 的 stdout JSON。

复现命令和独立配置见 [README](../guides/assets.md#cpu-导入与离线工具m42)。默认目标为
`dk_import_tests, dk_asset_pipeline_tests, dk_assetc`，筛选 `^dk\.(import|pipeline|assetc)\.`。
README 复制夹具的示例实际执行通过，产生 meta 与完整 CPU 产物（out/assetc-demo-573d92a6848742bdacc7871b96dc8bd9）。
定向文档检查：11 个 Markdown / 337 个本地链接、时间戳/编号/清单检查通过；git diff --check 通过。

## 下一项

M4.2.1 已独立提交 5fa3771，本记录为 M4.2.2，M4.2 所有子节完成。
下一项 **M4.3.1 内容键与产物发布**，下一可用开发编号 **0035**；M4 整体仍未完成。
