---
id: "0073"
created_at: "2026-10-08T16:34:00+08:00"
updated_at: "2026-10-08T17:03:11+08:00"
status: completed
design_refs:
  - ../design/automation-replay.md
  - ../design/automation-python.md
  - ../design/architecture.md
---

# M9.4 自动化记录与重放

## 目标与设计依据

按 [记录/重放设计](../design/automation-replay.md) 保存版本、输入、seed 和上下文，
通过相同服务重放并逐步核验逻辑状态。完成后验收 M9，保留非确定 GPU 与失败边界。

## 实际变更

- [执行层](../../sdk/python/decker/replay.py) 新增 Recorder、replay、ReplayReport 和
  `python -m decker.replay`。从初始 Project 副本/无活动 Scene 的独占宿主开始；
  录制实体/事务/查询/历史/保存及高层 capture，不复用旧 TaskId/JobId 或自动修正 revision。
- [格式层](../../sdk/python/decker/replay_format.py) 实现 DeckerRecording v1、有界严格 JSON、
  文件/内容摘要和原子发布。记录协议/schema、调用者构建标识、seed/context/环境、输入清单、
  初始/末态、步骤结果与文件产物；不更改引擎协议、命令或三方依赖。
- 全工程普通文件参与输入核验（排除根 .decker 派生缓存），含 glTF 外部纹理/缓冲；
  拒绝未完成资产操作日志、重解析点、缺失/新增/变化输入和输出覆盖输入。
  record JSON 最多16MiB/1024步，输入最多4096文件/512MiB，快照遍历全部实体分页。
- seed 仅初始化私有 Python RNG，entity.create 缺省 ID 在发送前物化，事务内也一样；
  跨进程仅映射原 guard.document_id，持久 ID/revision 保留。每步核对结果和逻辑状态摘要。
  普通业务拒绝可记录并继续；未知结果/等待失败使 Recorder 失效，不能写出完成文件。
- scene.save/project.save 校验实际文件摘要；capture 验证 Job 与 PPM 元数据并保留原/重放 SHA256，
  不承诺跨设备像素相等。日志文件原子替换是记录提交点，不补偿之前引擎或文件副作用。
- 新增 [7项记录单元测试](../../sdk/python/tests/test_replay.py)、[跨进程 CPU/GPU 验收](../../tests/replay/ReplayTest.py)
  和 [可执行录制示例](../../examples/automation/record_experiment.py)。CTest 注册 dk.replay.cpu/gpu，
  CPU 分支在启用 Luau 时运行现有脚本生成并保存起始工程。同步索引、选择表与[指南](../guides/replay.md)。

## 验证记录

环境：Windows x64、VS2026/MSVC14.51、Python3.13，Debug。
windows-scripting 用于 Luau 起始工程和 CPU 重放，windows-graphics 用于真实 Vulkan capture。
所有构建和执行过的测试均通过；无跳过。未改变依赖/功能开关或复用构建失败后的旧二进制。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.unit$' -Reason 'M9.4 recording codec fingerprints atomic publication and failure lifecycle plus SDK regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_run','dk_ctl') -TestRegex '^dk\.replay\.cpu$' -Reason 'M9.4 CPU replay seeded by M9.1 Luau scene including identities errors saves and divergence'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.replay\.gpu$' -Reason 'M9.4 real GPU capture recording replay artifacts and logical state verification'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_run','dk_ctl') -TestRegex '^dk\.(python\.unit|replay\.cpu)$' -Reason 'M9.4 final snapshot format validation SDK regression and runnable recording example replay'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.replay\.gpu$' -Reason 'M9.4 final format validation with real asset references and captured outputs'
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.unit$' -Reason 'M9.4 input preflight rejects unfinished asset persistence journals'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.replay\.gpu$' -Reason 'M9.4 execute documented recording example with capture and replay its artifact'
./scripts/check-spec.ps1
./scripts/check-spec.ps1 -Path @('sdk/python/README.md')
git diff --check
```

| 日志目录（out/verify 下） | 结果 |
| --- | --- |
| 20261008-164227-8b0a5d9c | Python unit 1/1通过，18个 unittest，含原11个 SDK 回归 |
| 20261008-164524-c218c2b7 | Luau 脚本生成工程后的 CPU 重放 1/1通过 |
| 20261008-164525-53fa6722 | GPU 截图记录与重放 1/1通过 |
| 20261008-165204-303acc1e | 完整快照格式检查后 unit+CPU 2/2通过，含可执行录制示例及其重放 |
| 20261008-165234-f3baa19c | 最终格式下的真实资产引用/截图重放 1/1通过 |
| 20261008-165518-b0a3e0de | 未完成资产日志拒绝回归，unit 1/1通过，18个 unittest |
| 20261008-170117-e1294f21 | GPU 1/1通过；另实际执行带 --capture 的录制示例，128x128产物及4步记录重放成功 |

CPU 实际录制10步，GPU为11步，覆盖初始工程、seed、事务、新实体显式 ID、父级、故意过期 guard、
中途失败事务、undo/redo、scene/project 保存；重放后的 Scene 文件与录制一致，DocumentId 不同但
持久实体 ID/逻辑摘要一致。输入字节或命令契约变化在加载前拒绝；修改第1号步骤参数并重算记录摘要，
仍在该步骤后检测逻辑差异，completed=1，已提交修改保留，后续保存/截图未执行。
业务失败、取消/超时/未知结果、分页变化、字段/版本/路径拒绝、原子日志替换失败保留旧文件由单元覆盖。

最终 CPU 产物位于 `out/build/windows-scripting/test-artifacts/Debug/replay/重放-43a72f488e`，
包含 Luau 创建的初始工程、experiment.json、CLI报告及示例 demo.json。
GPU 产物位于 `out/build/windows-graphics/test-artifacts/Debug/replay/重放-7293695431`：
report.json 的 completed=11，逻辑摘要
`7cb88e0cbe91388e2296da823093fbd009e62de0b42c970a109e3d922fb53ce5`；
64×64 PPM 为12301字节，原/重放摘要本机相同：
`a1e865009680a315496dd34754b43ec9b7d1db3b83646d46f4cdbdaa2d0161e7`。
这只是本机观察，不转化为跨 GPU 确定性保证。
GPU 要求 validation=required；仅记录精确的已知 AMD switchable layer API1.3/应用1.4 loader 警告，
其他诊断仍使验收失败。没有关闭验证层。

文档检查通过155个 Markdown、1582个本地链接及元数据/索引/测试入口/JSON清单；
SDK README另检查1文档/3链接通过。diff空白检查通过，未运行项见下文。

## 偏差与决策

复用 Python 标准库/原命令；以初始 Project 副本作为稳定起点，不尝试序列化任意运行时内部状态。
仅凭 engine_build 字符串无法认证宿主二进制，故明确为调用者 provenance，并同时实查能力/schema；
未为此增加一个声称能验证源码构建的 Runtime 字段。内容摘要也不是数字签名。
资产持久化有 `.decker/asset-operations/pending.json` 恢复日志，因此除排除派生缓存外，
显式拒绝待恢复工程，避免将未完成的资产操作当稳定初始条件。
正常业务失败记录错误类别与后状态，路径/message 作为诊断不要求两个根目录相同。

## 遗留问题与下一步

M9.4 已验收；结合 [0070](0070-luau-command-bindings.md) 的脚本保存/恢复、
[0071](0071-luau-execution-limits.md) 的预算/取消/退出、[0072](0072-python-automation.md) 的客户端与截图证据，
M9.1–4 全部完成，下一项 M10.1 模拟世界与固定步长。本次新测 Luau→Python→CPU 重放与 GPU重放，
未机械重跑未改变的所有 M9.1–3 用例。
未运行 Release、完整引擎回归、GUI 专项、其他操作系统/Python版本、跨GPU/驱动矩阵或掉电测试；
引擎C++未改动，本次范围不需要这些验证。暂不提供任意命令录制、文档中途切换、资产改名/导入重放、
崩溃恢复日志、自动回滚/重试或强制取消已发送引擎工作。使用者须独占宿主并保存初始输入副本。

## 修改记录

- 2026-10-08T16:34:00+08:00：创建设计与记录。
- 2026-10-08T16:58:00+08:00：完成 SDK、格式/文件保护、示例、CPU/GPU 验收；M9.4 与 M9 完成。
- 2026-10-08T17:03:11+08:00：补充使用指南的截图录制示例实际重放验证，文档检查通过。
