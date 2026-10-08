---
id: "0072"
created_at: "2026-10-08T16:00:00+08:00"
updated_at: "2026-10-08T16:22:26+08:00"
status: completed
design_refs:
  - ../design/automation-python.md
  - ../design/automation-client.md
  - ../design/architecture.md
---

# M9.3 Python 自动化客户端

## 目标与设计依据

按 [Python 设计](../design/automation-python.md) 封装 dk-ctl，完成超时、错误、批量、
任务等待与截图产物链路。范围止于 M9.3，不包含记录/重放。

## 实际变更

- [SDK](../../sdk/python/decker/client.py) 提供 Client/Command，复用 dk-ctl 的发现、命令、
  显式 ticket 重试及 Task envelope；Windows 隐藏子进程、UTF-8 无 BOM 临时参数文件，
  shell=false、截止时终止并回收自身子进程，临时文件随调用回收。无自动服务启动/关闭。
- [数据与错误](../../sdk/python/decker/models.py) 保留 Reply/TaskId/Ticket、JSON-RPC 错误
  和 execution 状态。普通 batch 先校验/快照本地输入，顺序调用并共享期限，失败保留
  completed/index/cause；transaction 直接映射原场景事务，不改 guard 或业务语义。
- wait_job 区分 queued/running/succeeded/failed/cancelled，短 jobs.wait 与整体期限结合，
  超时不取消；capture 保留 submission，在完成时核对所有版本/帧/尺寸/输出身份。
  Capture.collect 校验工程根内 P6 文件，返回路径/尺寸/字节数/SHA256；不隐式保存。
- 输入检查覆盖 JSON 类型、有限数值、int64/uint64、深度/节点/大小和重复字符串的逻辑字节；
  输出检查覆盖重复键、非有限数值、ticket/id/Task envelope、Job 状态及截图身份。
- 新增 [11 项单元测试](../../sdk/python/tests/test_client.py)、[真实进程验收](../../tests/integration/PythonClientTest.py)
  和 [批量截图示例](../../examples/automation/batch_capture.py)。[CTest](../../tests/integration/CMakeLists.txt)
  在找到 Python 3.11+ 时注册 dk.python.unit/cpu，启用截图后注册 capture_gpu；缺失解释器明确提示。
- 同步 [指南](../guides/python.md)、README、构建/IPC/脚本指南、设计索引、测试选择表和 Roadmap。
  无 C++ 命令/协议修改，无新增三方库或 vcpkg baseline 调整；SDK 按源码分发。

## 验证记录

环境：Windows x64、VS2026/MSVC 14.51、Python 3.13，Debug；复用 windows-graphics
与 CPU-only windows-dev。GPU 验收使用 required Vulkan validation，不把缺失设备计为通过。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.' -Reason 'M9.3 Python client unit errors deadlines CPU IPC batch restart and GPU capture artifacts'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.' -Reason 'M9.3 rerun after fixture path and known driver warning fixes; validate strict replies and runnable example'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.cpu$' -Reason 'M9.3 Python automation works with CPU-only Runtime and conditional test registration'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.unit$' -Reason 'M9.3 final client review: bound repeated-string serialization and reject non-finite response numbers'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_run','dk_ctl') -TestRegex '^dk\.python\.unit$' -Reason 'M9.3 final invalid-timeout boundary regression including arbitrarily large Python integers'
./scripts/check-spec.ps1
./scripts/check-spec.ps1 -Path @('sdk/python/README.md')
git diff --check
```

| 日志目录（out/verify 下） | 实际结果 |
| --- | --- |
| 20261008-160701-977ec3e9 | 构建成功；unit 通过，cpu/GPU 夹具断言失败。CPU 缓存临时路径过长；GPU 已生成正确图像，但把已知 AMD loader 警告视为失败 |
| 20261008-161542-c13da4b0 | 修正夹具后 3/3 CTest 通过、0 跳过；含 11 个 unittest 与 CPU/GPU 真实流程及可执行示例 |
| 20261008-161716-33bfaca7 | CPU-only 配置 cpu 1/1 通过、0 跳过 |
| 20261008-162028-199a8ce3 | 输入逻辑字节预算/非有限响应数值改进后，unit 1/1（11 个 unittest）通过 |
| 20261008-162207-fbc05e65 | 超大整数 timeout 输入边界回归，unit 1/1（11 个 unittest）通过、0 跳过 |

真实 CPU 验证 Unicode 名称/路径、显式重试不重复创建、旧 session 拒绝、过期 guard、
事务中途失败回滚、普通 batch 部分提交/停止、TaskId 查询、保存重启重载、uint64 guard 和资产 import 作业等待。
单元验证真实睡眠子进程被截止并回收，参数文件清理、结果未知、错误元数据、部分结果、
等待失败/取消/超时、无自动取消、截图版本不符及文件越界/截断拒绝。

GPU 流程绘制夹具 3 个 draw，64×64 PPM 有前景和多种颜色；原图与批量参数修改后图像摘要不同，
revision 前进、文件完整，每张 12301 字节。缺失纹理的失败 Job 保留 submission 和已有产物，
磁盘 Scene 未被隐式保存。另在同一服务实际运行示例，生成 256×256 唯一命名截图及 JSON 元数据。
产物位于 `out/build/windows-graphics/test-artifacts/Debug/python/中文 space-8c52487821de`，
artifacts.json 记录两张验收图：first 的 SHA256 为
`3d8263318a7966f6d28897c129860193fb21293b8b73ed13b0ec0f98c405ec01`，edited 为
`8471b19962755f1ec55d4333dfbcf966955197b98ff927d4f876c0a579229270`。

构建无编译警告。GPU 宿主报告可选 AMD switchable layer 使用 API 1.3、低于应用 1.4 的已知 loader 警告；
夹具仅允许这一条精确消息并打印计数，其余诊断继续失败，未关闭验证层。
文档检查通过 152 个 Markdown、1547 个本地链接及元数据/索引/测试入口/JSON 清单；
SDK README 单独检查 1 个 Markdown 和 2 个本地链接通过。diff 空白检查通过，未运行项目列于下文。

## 偏差与决策

复用轻量客户端和原协议，无新增依赖；使用源码 PYTHONPATH 分发，不引入包构建工具。
初次 CPU 夹具目录叠加测试路径与 32 位随机后缀，缓存原子保存临时路径触发 Windows MAX_PATH；
改到 build/test-artifacts 并缩短随机后缀，保留中文/空格覆盖。未顺带修改引擎长路径支持。
默认 5 秒单次调用与 30/60 秒等待/截图分开；总期限不会延长服务端已有 IO 限制。
Python 子进程超时拿不到响应时保守报告 unknown，不能凭本地进程终止推断远端没执行。

## 遗留问题与下一步

M9.3 已验收，下一项 M9.4 记录与重放，M9 父阶段保持进行中。
未运行 Release、完整引擎回归、GUI 专项或其他 Python/OS 版本：C++ 引擎/协议未修改，
已覆盖 CPU-only 和 GPU runner 的受影响链路；声明 Python 3.11+，本次实际解释器为 3.13。
不承诺远程传输、pip 发布、异步 Python API、强制取消引擎命令/驱动、文件系统沙箱，
同名文件后续覆盖也不能由 SDK 保证原截图身份；示例使用唯一输出名。

## 修改记录

- 2026-10-08T16:00:00+08:00：创建设计与开发记录。
- 2026-10-08T16:22:26+08:00：完成 SDK、示例、定向验收与文档，记录首轮失败及修正依据。
