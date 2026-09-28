---
id: "0032"
created_at: "2026-09-28T11:07:00+08:00"
updated_at: "2026-09-28T11:34:33+08:00"
status: completed
design_refs:
  - ../design/assets-runtime.md
  - ../design/project-format.md
---

# 0032 M4.1.2 登记提交与受控改名

基线 4be8946，初始工作区干净。先细化 [提交与恢复契约](../design/assets-runtime.md#m412-提交与恢复契约)，
实现有记录的多文件补偿、Project/目录发布、同目录改名及重启恢复。新增内部 XXH3-128 摘要。
不涉及导入、缓存、业务命令或 Jobs。采用 spec-workflow、state-contracts、build-verify。

## 实现

- [Catalog.hpp](../../engine/assets/runtime/include/dk/assets/Catalog.hpp)、[Catalog.cpp](../../engine/assets/runtime/src/Catalog.cpp)：
  登记提交、私有构造的改名候选/提交、恢复闸门；持久成功后无分配交换记录并发布 next guard。
  no-op 保留原字节/Project 和 revision，仍核验过期输入/清单及路径冲突。目标文件/目录记录/sidecar 占用均拒绝。
- [Persistence.hpp](../../engine/assets/runtime/include/dk/assets/Persistence.hpp)、[AssetPersistence.cpp](../../engine/assets/runtime/src/AssetPersistence.cpp)：
  限长严格操作记录 v1、XXH3 指纹、Windows 普通路径校验、原子文件替换、禁止覆盖的改名、逆序补偿和显式恢复。
  恢复先检查全部状态，保留外部变化、未知算法/文件和损坏记录；错误同时保留主错误、回滚错误及记录路径。
  journal 写入后的异常关闭写闸门；恢复成功后旧实例仍需重新打开。文件被外部改变时不猜测身份或删除冲突内容。
- [ContentDigest.hpp](../../engine/assets/runtime/src/ContentDigest.hpp)、[ContentDigest.cpp](../../engine/assets/runtime/src/ContentDigest.cpp)：
  内部 16 字节 canonical 大端摘要、32 位小写 hex、one-shot 与 RAII streaming、64 KiB 文件分块、IO 错误传播及 CPU zone。
  不暴露 xxHash 类型，不新增通用空 target；标准/三方临时分配未宣称全部由 Memory 接管。
- [AssetService.hpp](../../engine/framework/services/include/dk/services/AssetService.hpp)、[AssetService.cpp](../../engine/framework/services/src/AssetService.cpp)：
  从已保存 Project 打开独立所有者，构造完整有效 Project 后提交，最后交换 Project/清单字节。
  保护 Scene 所有路径，保留全部子资产 ID、Scene 内容/revision/dirty；不持有或编辑 Scene 历史。
  未将新服务自动装配入 Runtime/SceneService，宿主仍显式绑定 Assets 域。
- [测试](../../tests/unit/AssetPersistenceTests.cpp)、[服务测试](../../tests/unit/AssetServiceTests.cpp)、
  [摘要测试](../../tests/unit/AssetDigestTests.cpp) 和 [跨进程夹具](../../tests/integration/AssetRecoveryProbe.cpp)：
  14 个新增单元用例（内部含故障点/损坏记录组合）及 8 个独立进程重启用例。
  进程在 source/meta/manifest/finish 前 `_Exit(73)`，下一进程检查拒绝打开、显式回滚、精确字节恢复和可重新打开。

## 状态与提交点

操作记录固定为 `.decker/asset-operations/pending.json`，源/target 指纹与 meta/manifest 前后 image 可完整诊断。
单写者下按 record → source（改名）→ meta → manifest → 校验 → 删除 record 顺序提交。
删除 record 后只执行不分配的目录/Project swap；没有可能报普通文件错误的后置步骤。
删除前失败按 manifest → meta → source → record 回滚；不承诺多文件原子性。
旧 SceneService 的 Project 不会被隐式改写，调用者应管理自己的只读快照和借用引用寿命。

## 实际验证

Windows x64，Debug，/W4 /WX；仅验证资产提交链路及直接受影响候选，不执行全引擎/双配置矩阵。

| 检查 | 结果/证据（仓库 out/verify 下） |
| --- | --- |
| windows-dev 配置，安装 xxhash 0.8.4 | 通过，原包及 baseline 不变 |
| 首轮新增测试 | `20260928-112027-93e51b30`：19 通过、1 失败；官方向量夹具的 PRIME64 末位抄写错误 |
| 修正官方向量 + 既有目录/Project 候选 | `20260928-112149-5af545fb`：13/13 通过（1 摘要 + 12 既有回归） |
| 新增路径所有权检查后提交/服务/重启 | `20260928-112327-51d67d14`：20/20 通过，含全部 8 个真实跨进程恢复 |
| Scene/Framework 关闭的 assets-only | `20260928-112503-146d3dee`：10/10 新增底层摘要/持久化测试通过 |
| no-op 路径冲突补充及服务回归 | `20260928-112907-fbd796b5`：5/5 通过 |
| 文档与差异检查 | check-spec.ps1（本次文档/测试选择参考）及 git diff --check 通过 |

编译阶段曾发现遗漏 profiling PRIVATE 链接、Catch2 三元表达式括号和夹具 Memory 错误类型不匹配，均已修复。
两次 verify 构建失败（`111903-cc1d5f4d`、`111942-ccfa98c1`）均停止于编译，没有使用旧程序运行测试。
哈希已知向量来自 xxHash 0.8.4 官方 [sanity_test_vectors.h](https://github.com/Cyan4973/xxHash/blob/v0.8.4/tests/sanity_test_vectors.h)，
长度 0/4/16/32/130/1578，另有跨 64 KiB 边界的二进制文件/空文件/缺失文件验证。
故障测试覆盖 5 个提交步骤 × 登记/改名、4 个回滚步骤 × 登记/改名、真实 Windows sharing violation、
既有 sidecar 原始格式回滚、外部内容冲突、9 类坏记录、OOM 异常恢复、过期 guard/manifest、目标抢占及 ID/Scene 保持。

复现命令（细分筛选及实际名单保存在对应 summary.json）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_asset_tests','dk_asset_recovery_probe') -TestRegex '^dk\.assets\.(content digest|asset |restart_)' -Reason 'M4.1.2 摘要、提交与重启恢复'
& ./scripts/verify.ps1 -BuildDir out/build/windows-assets-only -Target dk_asset_tests -TestRegex '^dk\.assets\.(content digest|asset )' -Reason 'M4.1.2 独立资产底层'
```

## 依赖核验

2026-09-28 直接请求 vcpkg 官方 master 的 ports/xxhash/vcpkg.json：0.8.4，无 port-version（#0），
最近 port 提交 00be06124721b0fa2fb451982e707f81b3b06e5a。Web 文本缓存仍返回旧 0.8.3，
以官方实时响应及固定 baseline 内容复核为准。不启用 xxhsum。
`git show 33d78c1ed898a06938f31312167c7abefd229455:ports/xxhash/vcpkg.json` 与实时结果一致，
故保持 baseline；assets feature 新增 xxhash（default-features=false），runtime PRIVATE 链接 `xxHash::xxhash`。

## 限制与交接

仅 Windows 本地盘同步单写者；拒绝 reparse point、短名称/硬链接别名和跨目录/扩展名变化。
不承诺任意断电恢复、恶意篡改防护或外部并发写隔离。未知或冲突文件必须人工确认，恢复函数不删除它们。
没有实现 glTF 解码、缓存、Jobs、业务命令、真实 Tracy capture 或 Release/其他平台验证；这些不属于本节验收。
M4.1.1/2 均通过，M4.1 关闭。下一项 **M4.2.1 CPU 数据与网格导入**，下一开发编号 0033。
