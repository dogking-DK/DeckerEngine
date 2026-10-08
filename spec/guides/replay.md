---
created_at: "2026-10-08T16:49:00+08:00"
updated_at: "2026-10-08T16:57:18+08:00"
---

# 记录和重放自动化实验

[返回项目入口](../../README.md)。本功能基于 [Python SDK](python.md)，把已物化命令、
版本/输入文件摘要、seed、执行上下文和逻辑状态核验保存到 DeckerRecording v1 JSON。
重放使用相同 Commands/Services，不执行记录文件中的 Python 源码。

## 准备两个相同的起始工程

录制与重放分别使用独占、尚未加载场景的 runner/editor，以及同一初始工程的两个副本。
录制会修改内存，并可能保存场景/截图；因此必须在录制前保留初始副本。
下面命令在仓库根执行（输出目录需不存在）：

```powershell
$env:PYTHONPATH = (Resolve-Path sdk/python).Path
$runner = (Resolve-Path out/build/windows-graphics/bin/Debug/dk-run.exe).Path
$ctl = (Resolve-Path out/build/windows-graphics/bin/Debug/dk-ctl.exe).Path
$build = (& $runner --version) + ' / ' + (git rev-parse HEAD)
New-Item -ItemType Directory -Force out/replay-demo | Out-Null
Copy-Item -LiteralPath tests/fixtures/render-disk -Destination out/replay-demo/source -Recurse
Copy-Item -LiteralPath tests/fixtures/render-disk -Destination out/replay-demo/target -Recurse
# 在单独的终端运行并保持服务；不要事先 scene.load。
& $runner --project-root out/replay-demo/source --pipe record-demo
```

在另一个已设置 PYTHONPATH、$ctl 和相同 $build 的终端录制：

```powershell
python examples/automation/record_experiment.py --pipe record-demo --ctl $ctl --project-root out/replay-demo/source --recording out/replay-demo/experiment.json --engine-build $build --seed 42 --capture
```

[record_experiment.py](../../examples/automation/record_experiment.py) 加载工程、用局部随机数生成变换，
原子批量改名/变换、可选截图、保存 Scene，最后发布记录文件。记录必须位于工程根外。
示例使用固定的新输出名 experiment.ppm，初始工程不得已有同名文件。相机适合该小夹具。
CPU-only windows-dev/windows-scripting 可省略 --capture。
可用 [Luau 示例](../../examples/scripting/create-scene.luau) 创建初始工程，完成保存后再复制用于实验。

用另一个终端启动重放宿主，再从调用终端执行 CLI：

```powershell
# 服务终端
& $runner --project-root out/replay-demo/target --pipe replay-demo
# 调用终端
python -m decker.replay out/replay-demo/experiment.json --pipe replay-demo --ctl $ctl --project-root out/replay-demo/target --engine-build $build --timeout 300
```

成功退出 0，stdout 一个 JSON（completed、final_sha256、artifacts）；失败退出 1，包含
phase、step（从 0 起）、completed 和诊断。completed 是已经匹配记录的步骤数，含预期业务拒绝。
CLI 用法错误由 argparse 输出到 stderr 并退出 2。服务不会自动退出；需要时显式调用 runtime.shutdown。
SDK 不扫描端点，也不自动替换活动文档。

## Python 接口

```python
from decker import Client, Command, RpcError, guard
from decker.replay import Recorder, replay
from decker.replay_format import ReplayError, load_recording

client = Client("record-demo", ctl_path)
rec = Recorder.start(client, source_root, manifest="project.json", seed=42,
                     engine_build=build_id, context={"experiment": "translation", "units": "metres"})
query = rec.call("scene.query").value
state = query["state"]
created = rec.call("entity.create", {"guard": guard(state)}).value
state = created["state"]
rec.call("entity.set_name", {"guard": guard(state), "id": created["created_id"], "name": "Recorded"})
rec.save(record_path)  # 此例只保存实验记录，没有隐式 scene.save

record = load_recording(record_path)
report = replay(Client("replay-demo", ctl_path), record, target_root,
                engine_build=build_id, timeout=300)
```

Recorder.call 默认总预算 30 秒，capture 默认 60 秒，start/save 默认 60 秒；replay 默认 300 秒，
均可在 0.001–86400 秒内指定。每个底层 RPC 还受 Client.timeout 限制。
文件扫描/系统 IO/进程回收不是硬实时可抢占操作。
Recorder.rng 是私有 random.Random(seed)，可用于生成实验参数；缺省 entity.create ID 也在发送前由它生成。
重放使用保存的具体参数，不重新执行随机脚本，也不设置引擎或未来物理求解器的全局 RNG。

| 支持入口 | 行为 |
| --- | --- |
| call(entity.* / scene.query / history.*) | 保存参数、结果或可恢复业务错误，逐步核验状态 |
| transaction(guard, Command 列表) | 同 scene.transaction；子项只允许实体内存编辑，失败原子性由引擎保证 |
| call(scene.save / project.save) | 保存当前 scene / 原 manifest，记录并核验实际文件摘要 |
| capture(params) | 提交、等待成功、核对版本/尺寸和 PPM 文件，记录产物摘要 |
| save(path) | 核验末态和文件，原子发布完整实验 JSON；随后 Recorder 关闭 |

不记录中途 scene.new/load、资产导入/注册/改名、任意 TaskId/JobId 查询、取消或 runtime.shutdown。
使用普通 Client 调用这些命令；录制期间不得并发修改同一个宿主。支持的业务错误仍抛 RpcError，
捕获后可以继续录制；未知结果、截图等待失败或观察到状态/文件漂移会使 Recorder 失效，不能发布完成记录。
记录文件写入失败保留旧文件和已执行的引擎状态，修正输出位置后可再次 save。

## 记录内容和核验范围

- v1 格式/SDK 记录协议版本、IPC 协议、Runtime capabilities 与命令 schema 摘要。
- 调用者提供的 engine_build、uint64 seed、JSON context、Python/OS 与原工程根信息。
- 工程所有普通文件的相对路径、大小和 SHA256，排除根 `.decker` 派生缓存；包括外部纹理/缓冲。
- 每步具体参数、成功值或错误 code/engine_name、逻辑后状态摘要；初始/最终完整逻辑快照。
- 保存文件和截图产物元数据；截图记录原始摘要，重放报告实际摘要，供调用者比较。

engine_build 是调用者提供的构建标识，应包括实际宿主版本和源码提交/构建编号；SDK 无法仅凭
该字符串认证远端二进制。重放同时检查服务能力与 schema，不把这些检查当作二进制签名。
记录 JSON 的 SHA256 用于损坏检查，可重新计算，不构成签名或来源认证。

默认最多 1024 步、16MiB 记录；输入最多 4096 文件、512MiB。不自动复制资产或迁移格式。
拒绝输入变化、缺失/额外文件、符号链接/目录连接和越界路径；所以服务 stdout/stderr 日志与实验
记录应放在工程根外。截图只使用新的非输入文件名；父目录须预先存在。
存在未完成资产操作日志时拒绝录制/重放，须先完成已有资产恢复。
初始工程 manifest/scene 必须已保存；save 只允许写入已知 scene/manifest，其他初始输入保持不变。

逻辑快照遍历全部实体分页，核对同一文档/revision 和数量，比较 scene_id、revision、dirty、
实体 ID/名称/局部 TRS/父级/资产引用。新建实体 ID 被固定，重放只替换已知 guard.document_id，
保留原 revision 与故意失效的 guard；不复用 TaskId/JobId。world_matrix 不进入逻辑快照摘要。
遇到预期业务错误，核对错误类别与后状态；路径诊断文本不要求两个工程根相同。

任一步结果或状态不符立即停止，不跳过、不回滚、不自动重试或恢复中断的实验。
ReplayError 的 phase/step/completed 及 Python 异常 cause 可定位已经发生的操作；失败步也可能已提交。
超时不会取消后台 Job，原始 SDK 异常保留在 cause。记录不是崩溃恢复日志，也不提供跨资源事务。
独占使用仍是调用者责任：文件摘要与逐步检查不是跨进程锁或敌对文件系统沙箱。
截图成功只验证身份、尺寸、终态和产物结构，不承诺跨 GPU/驱动像素或模拟的完全确定性。

## 定向验证

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_run','dk_ctl') -TestRegex '^dk\.(python\.unit|replay\.cpu)$' -Reason '记录格式/失败保护与 Luau 工程跨进程重放'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.replay\.gpu$' -Reason 'GPU 截图记录重放与产物核验'
```

实际结果见 [0073](../development/0073-automation-replay.md)，当前约定见 [设计](../design/automation-replay.md)。
