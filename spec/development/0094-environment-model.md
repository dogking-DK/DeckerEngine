---
id: "0094"
created_at: "2026-10-12T15:00:00+08:00"
updated_at: "2026-10-12T15:16:00+08:00"
status: completed
design_refs:
  - ../design/render-atmosphere.md
  - ../design/scene.md
  - ../design/application-services.md
  - ../design/render-data.md
---

# 0094 M14.1 CPU环境模型与持久化

## 目标与设计依据

完成[Roadmap](../roadmap.md) M14.1；以[大气CPU契约](../design/render-atmosphere.md)为依据，
实现单位/坐标/单太阳参数、版本迁移、环境命令、事务/历史和不可变Render提取。
GPU底座、积分、LUT、着色与面板属于后续小阶段。

## 实际变更

- [Environment.hpp](../../engine/scene/include/dk/scene/Environment.hpp)及实现新增纯CPU固定大小值类型：
  EnvironmentDescription、AtmosphereParameters、SunParameters；默认disabled，double米制/逆米系数。
  统一校验有限值、支持范围、Mie散射≤消光、太阳方向规范化；提供带符号球面高度与太阳盘能量换算。
  冻结线性sRGB/D65场景归一化辐照度标度，不把数值当作lux或原始光谱能量。
- [SceneDocument](../../engine/scene/src/SceneDocument.cpp)保存环境，先校验候选再无分配赋值提交；
  snapshot/same_content/logical_bytes/stage/apply_snapshot包含环境，保留保存来源、revision和dirty规则。
- [SceneIO](../../engine/scene/src/SceneIO.cpp)写DeckerScene v2，环境自带version=1；严格读v1时补disabled。
  未知字段/版本、缺失字段和非法值拒绝，迁移保留持久ID与revision，Project仍为v1。
- [SceneOperations](../../engine/framework/operations/src/SceneOperations.cpp)新增environment.query/set；
  SetEnvironment加入SceneEdit，复用guard、候选文档、预算、一次提交及undo/redo。
  scene.transaction可混用环境与实体；无变化保留redo、dirty和revision。
  环境线协议在Scene与Operations各自私有codec中维护，以跨进程query与持久JSON等值测试核对；
  不为复用JSON而让Scene公开依赖Commands或JSON头。
- [RenderScene](../../engine/render/data/src/RenderScene.cpp)复制不可变环境，与提取版本绑定；
  旧代、临时变换预览、源文档释放和heap关闭后值保持稳定，无GPU资源。
- 新增[环境命令参考](../commands/environment.md)、[可执行CPU示例](../../examples/automation/environment.jsonl)、
  [参数/迁移测试](../../tests/unit/EnvironmentTests.cpp)及[真实runner进程测试](../../tests/integration/EnvironmentProcessTest.py)；
  同步Services/Render Data测试、CMake、选择表、设计/开发索引与Roadmap。
  未新增三方依赖或GPU构建开关。

## 验证记录

环境：Windows x64，MSVC 14.51 / Visual Studio 2026，Debug；复用windows-dev和windows-graphics。
Scene/Services/runner在关闭全部Graphics/Render模块的windows-dev构建；Render Data仅运行CPU测试。

实际命令（仓库根PowerShell）：

```powershell
& ./scripts/verify.ps1 -Target @('dk_scene_tests','dk_service_tests','dk_run') -TestRegex '^dk\.(scene\.|services\.|environment\.)' -Reason 'M14.1 environment values, scene migration, transaction history and runner process roundtrip'
& ./scripts/verify.ps1 -Target @('dk_service_tests','dk_run') -TestRegex '^dk\.(services\.|environment\.|runtime\.batch_roundtrip)' -Reason 'Validate corrected environment integer schema registration and dependent service/process paths'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_render_data_tests -TestRegex '^dk\.render\.data\.' -Reason 'M14.1 immutable environment extraction, preview and lifetime CPU regression'
```

- `out/verify/20261012-150705-9dac8974`：Scene 33项全通过；该轮共46项，34通过、12失败。
  失败定位为环境version schema遗漏项目必需的`x-dk-integer-token`，导致注册表初始化失败；
  改为schema::integer()后重跑所有Services和进程路径。Scene代码未再变化，保留其已通过结果。
- `out/verify/20261012-150915-c1df9f99`：Services 12项、环境真实进程1项、原有batch往返1项，全14项通过，0失败/跳过。
  真实commands.list核实45条命令，query/set effect/undoable和describe的参数/结果对象一致。
  执行完整示例、跨进程v2重载、规范化值无变化写入、v1迁移后保存v2，身份/revision/dirty正确。
- `out/verify/20261012-151124-d068e919`：Render Data 5项全部通过，0失败/跳过。
  新旧环境快照、preview、源对象寿命、heap关闭与既有预算失败回归通过。
- 汇总52个不同测试通过（含9个新增测试），无未解决失败；数值断言包括1mm/1m/100km/顶外和切向位移
  高度绝对误差<1e-6m、巨大/次正规太阳方向幂等、32次保存/解析值稳定、太阳盘积分恢复法向辐照度。
  非法disabled参数、v1/v2严格字段、失败重载保留对象、旧快照保存、跨来源拒绝、revision耗尽、
  guard冲突、环境/实体混合回滚、历史预算拒绝和redo保留均有覆盖。
- 检查windows-dev runner的AdditionalDependencies及MSVC dumpbin /dependents：仅CPU引擎/资产、
  flecs、xxhash、fastgltf、mimalloc、系统和CRT依赖，没有Vulkan/Slang/SDL。
- 文档check-spec和git diff --check通过；最终命令及结果见下方修改记录。

构建中先修复一个MSVC警告即错误（环境与四元数局部变量normalized重名）。
沙箱内vcpkg缓存写入和MSBuild FileTracker曾被拒绝访问；获准环境重跑正常。
这些失败未使用旧二进制作为成功证据。

未运行Release、完整引擎测试、GUI/GPU图像和性能采样：本阶段是CPU状态/协议变更，无GPU实现；
不能以此记录声称天空/日光/LUT已经可用。真实OOM注入、flecs致命错误和进程断电不在验证范围。

## 偏差与决策

环境采用完整候选替换；云、时间、曝光、质量和ViewId未提前写入持久格式。
首版支持范围明确且disabled也校验；方向保留8 epsilon内的单位系数，避免反复序列化漂移。
太阳能量使用可解释的场景归一化RGB标度，物理积分和GPU相对坐标转换在后续阶段实现。

## 遗留问题与下一步

M14.1范围无遗留；下一项M14.2：3D image/view、必要float格式、深度采样、Slang/Graph访问与屏障。
当前ScenePipeline仍沿用原unlit路径；编辑器环境面板与Python/Luau专门用例留到M14.6。
保留用户已有spec/README.md和spec/guides/ai-documentation-workflow.md改动，不纳入提交。

## 修改记录

- 2026-10-12T15:00:00+08:00：先细化设计和字段/版本/提交点，创建开发记录。
- 2026-10-12T15:15:23+08:00：实现与52项定向验证完成，登记失败修复和CPU依赖证据，关闭M14.1；后续M14阶段仍待实现。

- 2026-10-12T15:16:00+08:00：最终文档检查通过14份Markdown/661个本地链接及元数据、表格、索引、测试入口、JSON清单；git diff --check通过。

最终文档命令：

```powershell
& ./scripts/check-spec.ps1 -Path @(
  'README.md','.agents/skills/decker-build-verify/references/test-selection.md',
  'spec/design/architecture.md','spec/design/render-atmosphere.md','spec/design/scene.md',
  'spec/design/application-services.md','spec/design/render-data.md','spec/design/README.md',
  'spec/commands/environment.md','spec/commands/README.md','spec/commands/scene.md',
  'spec/development/0094-environment-model.md','spec/development/README.md','spec/roadmap.md'
)
git diff --check
```
