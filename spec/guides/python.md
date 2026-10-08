---
created_at: "2026-10-08T16:10:00+08:00"
updated_at: "2026-10-08T16:10:00+08:00"
---

# Python 自动化

[返回项目入口](../../README.md)。[SDK](../../sdk/python) 使用 Python 3.11+ 标准库封装
Windows `dk-ctl`，连接现有 runner/editor。无需 pip 依赖，不嵌入 Python，不负责启动或
关闭引擎。C++ 命令、schema、guard 和错误码仍以 [命令参考](../commands/README.md) 为准。

## 配置与运行

从仓库根执行。以下示例复制小型渲染夹具到新工程目录；截图需要 Vulkan 与
windows-graphics 构建，单纯编辑可用 windows-dev。环境要求见 [构建指南](build.md)。

```powershell
cmake --preset windows-graphics
cmake --build out/build/windows-graphics --config Debug --target dk_run dk_ctl
$root = Join-Path $PWD ('out/python-demo-' + [guid]::NewGuid().ToString('N').Substring(0,12))
Copy-Item -LiteralPath tests/fixtures/render-disk -Destination $root -Recurse
Write-Output $root
# 第一个终端持续运行；第二个终端将 $root 设为同一路径。
./out/build/windows-graphics/bin/Debug/dk-run.exe --project-root $root --pipe python-demo
```

第二个终端仍在仓库根，设置源码导入路径、加载场景，再运行示例：

```powershell
$env:PYTHONPATH = (Resolve-Path sdk/python).Path
$ctl = (Resolve-Path out/build/windows-graphics/bin/Debug/dk-ctl.exe).Path
# $root 设为第一个终端输出的工程绝对路径。
python -c "from decker import Client; import sys; Client('python-demo', sys.argv[1]).call('scene.load', {'manifest':'project.json'})" $ctl
python examples/automation/batch_capture.py --pipe python-demo --ctl $ctl --project-root $root
```

[batch_capture.py](../../examples/automation/batch_capture.py) 对当前页最多 8 个实体一次事务改名，
提交唯一文件名截图、等待 Job 完成并输出 JSON（版本/frame、文件绝对路径、尺寸、字节数、SHA256）。
示例相机适合夹具；实际工程须提供合适相机。截图为 P6 PPM。
如需保留编辑，显式调用 scene.save，再调用 runtime.shutdown。
编辑器可开放管道供同一个 Client 使用，见 [IPC 指南](ipc.md)。

## 调用、错误与重试

```python
from decker import Client, RpcError, TransportError, guard

client = Client("python-demo", "out/build/windows-graphics/bin/Debug/dk-ctl.exe", timeout=5)
hello = client.hello()
state = client.call("scene.query").value["state"]
params = {"guard": guard(state)}
reply = client.call("entity.create", params)
print(reply.value, reply.task_id, reply.ticket)
print(client.task(reply.task_id))  # 同步 Task 已终态，不是后台 JobId

# 显式重试原 ticket、方法、参数；服务返回原 TaskId，不重复编辑。
same = client.call("entity.create", params, retry=reply.ticket)
```

call 返回 Reply；value 是 response.result.value，raw 保留 dk-ctl 完整输出。
参数文件使用 UTF-8 无 BOM，不经 shell；Python int 保留 int64/uint64 精度。
NaN、无穷、非字符串 key、非 JSON 类型、越界整数、深度/节点/大小超限在启动前拒绝。
输入错误使用 ValueError；文件/进程/协议与业务错误使用 ClientError 子类。

| 异常 | 可恢复上下文 |
| --- | --- |
| RpcError | code、data（engine_name/context/可用的 TaskId）、task_id、ticket、raw；execution=received |
| TransportError / CallTimeout | status、execution、可用的 ticket/raw、stderr；超时不代表未执行 |
| BatchError | completed（成功 Reply 元组）、failed_index（从 0 起）、cause |
| JobError | 终态 job，区分 failed/cancelled；不是提交 Task 失败 |
| WaitTimeout | job_id、last_job；原始调用超时保留在异常 cause |
| ArtifactError | 本地文件不存在、越界或格式/尺寸/长度不匹配 |

execution 沿用 [IPC 语义](ipc.md#超时断连和重试)：not_sent 未尝试业务发送，unknown
可能已执行，received 已收响应。SDK 不自动重试或刷新 guard；未知结果先查询实际状态，
有可用 ticket 才能尝试同一请求。服务重启或票据淘汰会拒绝旧 ticket。
Python 自身期限到达时终止并回收自己的 dk-ctl，通常拿不到 ticket，保守报告 unknown。
这不会停止已运行的引擎命令、撤销修改或取消后台 Job。

单次 call/hello 和普通 batch 的 timeout 以秒计，默认 5，范围 0.001–60；batch
共享总截止时间。wait_job/capture 的总 timeout 范围 0.001–86400，默认分别 30/60。
使用单调时钟；进程启动、系统文件 IO 和 kill 后回收本身不保证硬实时上限。
wait_job 每个 jobs.wait 受 Client.timeout 和剩余时间限制，服务端等待最多 1000ms；
任一调用超时即可结束当前等待，保留异常原因，并可再次 wait_job。

## 顺序批量与事务

```python
from decker import Command

state = client.call("scene.query").value["state"]
client.batch([
    Command("scene.save", {"guard": guard(state)}),
    Command("project.save", {"guard": guard(state), "manifest": "project.json"}),
])
```

batch 接收 1–128 条 Command，先校验本地参数，再逐个发送。首个业务/传输错误停止；
已完成的保存/编辑保留，未开始项不发送。以上两个文件保存不构成跨文件事务。
多次普通编辑需要各自正确的 guard，SDK 不替换过期 guard。需要原子修改时：

```python
query = client.call("scene.query").value
commands = [Command("entity.set_name", {"id": e["id"], "name": "实验实体"})
            for e in query["entities"]]
if commands:
    result = client.transaction(guard(query["state"]), commands)
```

transaction 映射 scene.transaction，内部仅允许场景内存编辑，子参数不得含 guard；
失败保持场景/历史不变，成功只形成一次 revision 和撤销单元。见 [事务参考](../commands/scene.md#scenetransaction)。

## 后台作业、截图与产物

```python
from uuid import uuid4

state = client.call("scene.query").value["state"]
capture = client.capture({
    "guard": guard(state), "output": f"capture-{uuid4().hex}.ppm",
    "width": 64, "height": 64,
    "camera": {"eye": [0, 0, -2], "target": [0, 0, 0], "fov_y": 60},
}, timeout=60)
artifact = capture.collect(project_root)  # 与宿主使用同一个本地工程根
print(artifact.path, artifact.sha256)
```

capture 合并提交与等待总期限，验证成功 Job 的 document_id/scene_id/revision/frame/
尺寸/output 与提交一致。等待失败时异常的 submission 保留接受任务时的 Reply，
其 value.job_id 可交给 client.wait_job(job_id) 再等，或显式调用 jobs.cancel。
仅提交时可直接 call render.capture；资产导入 JobId 也用同一个 wait_job。
未知/已淘汰 JobId 返回 RpcError，不会被视为成功。

collect 读取现有文件、校验 P6 头和长度并计算摘要。输出父目录须已存在；根目录解析
阻止越界，但不作为敌对文件系统沙箱。使用唯一输出名避免被其他任务覆盖；其他程序
修改同名文件后，尺寸和摘要校验不能证明它仍是原截图。
目前只支持 Windows 本机管道和 PPM，CPU-only 宿主不具备截图命令。

## 验证

CMake 找到 Python 3.11+ 时注册 Python CTest；未找到时明确提示。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.' -Reason 'Python 参数错误/超时/批量/重载与实际截图产物'
```

CPU-only 配置筛选 `^dk\.python\.(unit|cpu)$`。GPU 验收要求 Vulkan 验证层，
环境不可用会失败，不会把跳过当通过。实际证据见 [0072](../development/0072-python-automation.md)。
