---
id: "0028"
created_at: "2026-09-24T10:10:29+08:00"
updated_at: "2026-09-24T10:22:21+08:00"
status: completed
design_refs:
  - ../design/foundation-core.md
  - ../design/assets-types.md
  - ../design/commands.md
  - ../design/foundation-profiling.md
---

# 0028 magic_enum 与枚举字符串迁移

基于 9f1526b，初始工作区干净；本次为用户要求的跨模块依赖与转换迁移，不推进 M1.7.6。
应用 spec-workflow、build-verify、command-development；先更新相关设计，再实现与定向验收。

2026-09-24 10:09 +08:00 核验 vcpkg 官方 master `f907dc21e0e8699955b002d0fe7673de5db55fab`，
magic-enum 最新为 0.9.8（port 修订 0），与工程固定基线 33d78c1e 的条目相同；保留原基线，
只新增 magic-enum 基础依赖，不连带升级其他三方库。使用目标为 magic_enum::magic_enum，均为 PRIVATE。

检查清单：ErrorCode 名称、AssetKind 名称/解析及 schema、CommandEffect 名称、HeapCategory/PoolKind
Tracy 标签改用反射。ContextError::what 的人类说明、LogLevel 到 spdlog 以及 DomainCategory 到
HeapCategory 的跨枚举语义映射保留；task bool、JSON schema 关键字、组件字段描述不是枚举名称表。

## 实际修改

- [依赖清单](../../vcpkg.json) 增加 magic-enum；Core、AssetTypes、Commands 与启用时的 Profiling
  各自 find_package 并 PRIVATE 链接。公开头文件只使用标准库类型，没有全局 include 或间接传递要求。
- [Error.cpp](../../engine/foundation/core/src/Error.cpp)、
  [CommandRegistry.cpp](../../engine/framework/commands/src/CommandRegistry.cpp) 使用 enum_name，
  未知值分别继续返回 unknown/invalid；整数错误码和拒绝非法 effect 的行为不变。
- [AssetReference.cpp](../../engine/assets/types/src/AssetReference.cpp) 使用 enum_name/enum_cast/enum_names；
  新增只读静态 span `asset_kind_names()`，
  [SceneOperations.cpp](../../engine/framework/operations/src/SceneOperations.cpp) 的参数和结果 schema 共用此列表。
  严格大小写、非法输入错误码/消息、未知枚举空名称保持不变。
- [Profiling Memory.cpp](../../engine/foundation/profiling/src/Memory.cpp) 用 consteval 拼接反射名称与协议前后缀，
  静态数组持有 Tracy 指针；hook 中无字符串分配，保留分类地址身份、未知分类回退和计数/锁规则。
  scratch 曲线与 CPU zone 名称是功能标签，继续保持字面量。
- 同步 [Core](../design/foundation-core.md)、[资产类型](../design/assets-types.md)、
  [命令](../design/commands.md)、[Profiling](../design/foundation-profiling.md) 设计，
  [依赖说明](../third-party-libraries.md)、[发现参考](../commands/discovery.md) 与 README/基础配置提示。
  定向扫描 engine/apps 后，保留的 switch 仅为上述非名称转换；没有将人类说明替换成标识符。

## 验证结果

Windows x64 / VS 2026 / MSVC 19.51，沿用已有构建配置；两个目录均实际安装 magic-enum 0.9.8。
测试期望使用固定协议值，不从反射生成期望；补齐全部错误码、三种资产种类和四种 effect，
以及非法数值、大小写、空白、数字文本、嵌入 NUL、静态列表生命周期与 schema 对齐。

```powershell
& ./scripts/verify.ps1 -Target @('dk_core_tests','dk_commands_tests','dk_scene_tests','dk_service_tests','dk_protocol_tests','dk_run','dk_profiling_disabled_test') -TestRegex '^(dk\.core\.error names|dk\.commands\.(effect reflection|discovery is sorted|registration rejects)|dk\.scene\.(asset kind reflection|project protocol|project rejects|scene asset references|scene files roundtrip|scene invalid schemas)|dk\.services\.(asset kind discovery|asset edits)|dk\.protocol\.RPC distinguishes|dk\.profiling\.disabled_no_side_effects$)' -Reason 'magic_enum name parsing schema and protocol compatibility plus disabled profiling header'
& ./scripts/verify.ps1 -BuildDir out/build/windows-profiling -Configuration RelWithDebInfo -Target @('dk_memory_probe','dk_pool_probe','dk_profiling_disabled_test') -TestRegex '^(dk\.memory\.(probe|pool_probe)|dk\.profiling\.disabled_no_side_effects)$' -Reason 'Reflected Tracy heap and pool labels require ON compilation and real capture probes'
& ./scripts/capture-profiling.ps1 -Mode memory -Port 18093
& ./scripts/capture-profiling.ps1 -Mode pool -Port 18094
```

| 检查 | 实际结果 | 本地证据（out 不入 Git） |
| --- | --- | --- |
| Debug 定向构建/测试 | 14/14 通过，0 失败/跳过 | `out/verify/20260924-101621-8fafa823` |
| Tracy ON RelWithDebInfo 探针/禁用头文件 | 3/3 通过，0 失败/跳过 | `out/verify/20260924-101659-5abbc736` |
| heap 真实采集与 inspector | 36 次申请全部配对，原分类名称不变，结束 live=0 | `out/profiling/20260924-101844-b94be3e5` |
| pool 真实采集与 inspector | 21 次上游申请配对，八条曲线名称/峰值/结束值通过 | `out/profiling/20260924-101851-38a72924` |
| 本次 runner commands.list/describe | 22 条命令、4 种 effect；scene.new/entity.set_assets/entity.get 的资产 schema 仍为原三值 | `out/enum-reflection-discovery` |

`check-spec.ps1 -Path` 限定本次 10 个 Markdown，231 个本地链接、时间戳、开发编号与 JSON 清单检查通过；
`git diff --check` 通过。依赖安装、编译、测试和真实采集本轮均一次通过，无失败或跳过。

未执行全量引擎回归、独立 bootstrap、其他平台、Tracy GUI 或性能基准；未对未触及的
CPU-only/Memory-OFF 组合重复采集。Tracy ON 单独构建是因反射标签实现处于条件编译分支，
不是例行双配置。扫描范围内未遇到需自定义枚举名、范围或同值别名的转换；未来扩展需重新核对。

## 交接

M1.7.6 仍为下一阶段，本记录不代表其实现；下一可用开发编号为 0029。
官方来源：[固定 port](https://github.com/microsoft/vcpkg/blob/f907dc21e0e8699955b002d0fe7673de5db55fab/ports/magic-enum/vcpkg.json)、
[magic_enum 0.9.8 限制](https://github.com/Neargye/magic_enum/blob/v0.9.8/doc/limitations.md)。
