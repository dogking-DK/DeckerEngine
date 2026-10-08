---
id: "0067"
created_at: "2026-10-08T12:30:00+08:00"
updated_at: "2026-10-08T13:07:24+08:00"
status: completed
design_refs:
  - ../design/automation-transport.md
  - ../design/automation-client.md
  - ../design/automation-protocol.md
  - ../design/runtime.md
  - ../design/editor.md
  - ../design/architecture.md
  - ../design/application-services.md
  - ../design/project-foundation.md
---

# 0067 M8.3 IPC 协议与 dk-ctl

## 目标与设计依据

按 [传输设计](../design/automation-transport.md) 与 [客户端设计](../design/automation-client.md)
完成 Windows Named Pipe、轻量客户端、超时/断连/重复请求语义。

## 实际变更

- 拆分 [protocol](../../engine/automation/protocol)、[server](../../engine/automation/server)、
  [transport](../../engine/automation/transport)、[client](../../engine/automation/client)。纯协议注入 RpcEndpoint，
  Runtime 适配、既有 JSON Lines/stdio 移到 server；原 stdio/batch 格式兼容。
- Named Pipe 使用当前用户 DACL、拒绝远端、独占端点、长度前缀、有界队列、overlapped IO 与可取消退出。
  worker 只收发字节帧，owner pump 解析及执行；已入队请求不会因客户端断连被当作未执行。
- session + 单调预留 ticket，最多 256 条/32 MiB 逻辑载荷。重复返回原结果/TaskId；
  不同内容冲突，淘汰/未预留/重启明确拒绝。请求在分派前绑定，异常仍保留已消费状态。
- 新增 [dk-ctl](../../apps/ctl/src/main.cpp)：发现、UTF-8 参数文件、单一截止时间、显式重试、
  not_sent/unknown/received 与 0/1/2/3 退出码；不自动重试或替换 guard。
- runner/editor 的 `--pipe NAME` 显式启用同一服务器；编辑器保持草稿旧 guard，刷新快照/历史。
  SceneReadSnapshot 增加 manifest，使远端 project.save/load 后 Reload 跟随当前清单；换文档重置选择。
  shutdown 回复后限时排空管道再回收线程/窗口/GPU。
- 增加 windows-client 预设、DK_BUILD_CTL、测试选择表、[使用指南](../guides/ipc.md) 与 [协议/CLI 参考](../commands/ipc.md)。无新增或升级三方库。

## 验证记录

所有构建使用 VS2026 x64 Debug，按定向范围执行，没有全量/双配置回归。

| 检查 | 命令/证据 | 结果 |
| --- | --- | --- |
| 协议拆分及原 batch/stdio | `verify.ps1 -Target dk_protocol_tests,dk_run,...`，筛选 `^dk\.protocol\.` 与 `^dk\.runtime\.(batch_\|stdio_interactive)`；`out/verify/20261008-124804-a8349eb0` | 既有协议/进程 9 项通过；同轮新增 IPC 有 2 项失败，见下述修复 |
| 最终 CPU IPC | `verify.ps1 -Target @('dk_ipc_tests','dk_run','dk_ctl') -TestRegex '^dk\.ipc\.'`；`out/verify/20261008-130055-3884c451` | 11/11 通过；ticket 重放/冲突/淘汰/重启、字节预算、分帧/截断、超时/断连、owner 分派、队列、关闭、2 MiB 响应/非法回复及真实 runner/ctl |
| JSON 共用解析及模型 | `verify.ps1 -Target @('dk_ipc_tests','dk_commands_tests','dk_run','dk_ctl','dk_editor_tests')`，筛选 IPC、commands、workspace IPC；`out/verify/20261008-125847-6e278ce3` | 19/19 通过，其中 Commands 7 项、IPC 11 项、编辑器 IPC 模型 1 项 |
| 编辑器模型/窗口接入回归 | windows-editor，targets `dk_editor_app,dk_editor_tests,dk_run,dk_ctl`，筛选 editor 的 workspace/runtime/camera/interaction/workbench 及 IPC editor；`out/verify/20261008-125406-5e8d2550` | 12/12 通过；真实 IPC 窗口和工作台 GPU validation 均通过，无跳过 |
| 最终双宿主进程复核 | windows-editor，targets `dk_editor_app,dk_run,dk_ctl`，筛选 `^dk\.ipc\.(editor_gpu_validation\|runner_client)$`；`out/verify/20261008-130302-f5c49aa5` | 2/2 通过，覆盖最终管道/客户端及 CLI 参数和 BOM 校验 |
| 独立轻量客户端 | `cmake --preset windows-client`；`verify.ps1 -BuildDir out/build/windows-client -Target dk_ctl -TestRegex '^dk\.ipc\.client_help$'`；`out/verify/20261008-130127-2c08753d` | 配置/构建成功，1/1 通过；缓存及生成 target 检查无 Scene/Runtime/Renderer/Vulkan/SDL，依赖仅已有 stduuid、magic-enum、nlohmann-json |

GPU 验证只在运行进程环境中设 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，finally 恢复原值。
真实 CLI 进程覆盖 Unicode 根/参数路径和实体名称、显式 guard/冲突、Undo/Redo、保存、重启读取、
原 TaskId 重试、新 session 拒绝，以及退出码和 BOM 参数拒绝。

初轮构建 `20261008-124529-32e09eeb` 因 RpcRuntime 匿名 namespace 未闭合失败，未运行测试；已修复。
第二轮 `20261008-124804-a8349eb0` 为 14 通过/2 失败：
一项测试误把 tasks.list 的数组返回值当对象，另一项发现客户端先开后关触发 ConnectNamedPipe ERROR_NO_DATA。
修正测试断言，并把该断连作为可恢复连接状态；后续 IPC 全部通过。

`scripts/check-spec.ps1` 通过：143 个 Markdown、1430 个本地链接、元数据/表格/索引/测试入口和 JSON 清单。
检查期间发现历史记录 0016 的 JsonLines 链接随移动失效，仅更新其源码链接并注明迁移，不改写历史验收。

## 偏差与决策

采用服务预留单调 ticket 和 session，淘汰后明确拒绝重试，避免有限缓存丢失去重记录后再次执行。
复用现有 JSON-RPC/命令校验，不增加业务命令；原 stdio 兼容性需要回归。

## 遗留问题与下一步

M8.3 完成，下一项 M8.4：GUI/外部/截图完整 revision 一致性、保存后 runner 图像重现与交付 C。
本次未做 Release、全量 GPU、Sponza 大场景交互、Unix socket 或网络远程控制验证。
同一 Windows 用户是信任边界；缓存预算是逻辑载荷，不代表进程内存硬上限。
5 秒服务端等待超时不取消已接受的命令；票据淘汰后的未知结果只能通过业务查询核对。

## 修改记录

- 2026-10-08T12:30:00+08:00：建立设计和开发记录。
- 2026-10-08T13:04:00+08:00：完成模块拆分、IPC/客户端与宿主接入，记录定向验证及修复。
