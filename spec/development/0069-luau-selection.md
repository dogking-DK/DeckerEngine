---
id: "0069"
created_at: "2026-10-08T14:42:00+08:00"
updated_at: "2026-10-08T14:44:00+08:00"
status: completed
design_refs:
  - ../design/architecture.md
  - ../design/project-foundation.md
---

# 0069 内嵌脚本方案由 Lua 改为 Luau

## 目标与设计依据

按用户要求替换内嵌脚本选型。当前 engine/scripting 只有占位目录，没有 Lua 运行时、
绑定或脚本文件；本次调整 [架构](../design/architecture.md)、[构建策略](../design/project-foundation.md)、
预留依赖及 M9 规划，不将 M9.1 标为完成。

## 实际变更

- vcpkg.json 的 scripting feature 移除 lua/sol2，改为 luau；vcpkg-configuration.json
  为 luau 固定官方 Git registry 的 reference/baseline，其余包保持原 builtin-baseline。
- 占位目录 engine/scripting/lua 改为 engine/scripting/luau；没有需要迁移的运行时代码或脚本。
- 架构和 M9 路线统一采用 Luau 官方 Compiler/VM，源码扩展名 `.luau`，未来只加载宿主编译的字节码；
  受控命令、错误恢复和运行限制继续按 M9.1/M9.2 实现，Python SDK 计划不变。
- 同步构建指南、三方库版本/状态及开发索引。历史记录 0021/0068 保留当时的 Lua 版本和交接描述，
  当前选型以架构、Roadmap 和本记录为准。

## 验证记录

实时读取官方 registry HEAD `2750401336fb7c95f6619657a46a7e798661341c` 的 Luau port：
0.741，port #0，MIT。现有 builtin-baseline 对应 Luau 0.739；网页缓存中的 master 版本为 0.740，
采用固定 commit 的实时响应为准。证据保存在 `out/luau-migration/registry-head.json` 和 `luau-port.json`。

在仓库根执行（VS2026 检测到 MSVC 14.51.36231，x64-windows）：

```powershell
& "$env:VCPKG_ROOT/vcpkg.exe" install --dry-run --triplet x64-windows --x-feature=scripting --x-no-default-features --x-install-root=out/luau-migration/vcpkg_installed
& "$env:VCPKG_ROOT/vcpkg.exe" install --dry-run --triplet x64-windows --x-no-default-features --x-install-root=out/luau-migration/default_installed
./scripts/check-spec.ps1
git diff --check
```

- scripting dry-run 通过：Luau 0.741（官方 port tree `0e4dc3aa2ddb251a1f656073da80cde78bbfb002`）、
  magic-enum 0.9.8、stduuid 1.2.3，以及 vcpkg-cmake 2025-08-07 / vcpkg-cmake-config 2026-07-21；
  未解析 lua、sol2 或 Luau tool。日志 `out/luau-migration/scripting-dry-run.log`。
- 默认 dry-run 通过：仅保留上述两个基础包与两个构建辅助包，无 Luau；
  日志 `out/luau-migration/default-dry-run.log`。
- 文档检查通过：145 个 Markdown、1456 个本地链接，元数据、表格、索引、测试入口及 JSON 清单通过；
  新 vcpkg-configuration.json 另经 JSON 解析和实际 vcpkg registry 解析通过。`git diff --check` 通过。
- 未运行 C++ 构建、引擎单元/GPU 测试或 Luau 安装/运行：此次只有预留依赖、目录和文档变化，
  没有 C++ 消费者，dry-run 不代表运行时兼容性验收。

## 偏差与决策

只对 Luau 使用官方 Git registry 的固定 reference/baseline；其他包仍用原 builtin-baseline，
不因替换一个尚未集成的库升级已验收的 Vulkan/SDL 等依赖。不添加旧版本 override。
首次官方 HTTP 请求发生 EOF，curl 读取 Luau port 成功；完整远端 baseline 下载超时/连接重置，
最终通过 vcpkg 自身 registry 解析验证所需条目，没有因网络问题回退版本。
该配置遵循 [vcpkg registry 规则](https://learn.microsoft.com/en-us/vcpkg/reference/vcpkg-configuration-json)：
省略 default-registry 时其余包继续使用 builtin-baseline；下次统一升级到包含 Luau 的基线时移除单包配置。

## 遗留问题与下一步

M9.1 后续实现 Luau 编译/VM 生命周期、受控命令绑定及脚本错误恢复；M9.2 验收预算、取消与能力限制。
本次不安装或链接 Luau，不声称 VM 运行、静态类型分析或安全隔离已经实现。

## 修改记录

- 2026-10-08T14:42:00+08:00：核验当前占位范围与官方 port，更新架构和依赖策略。
- 2026-10-08T14:44:00+08:00：完成依赖/目录/规划替换，两种依赖解析及文档检查通过。
