---
id: "0068"
created_at: "2026-10-08T13:40:00+08:00"
updated_at: "2026-10-08T13:57:00+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/editor-interaction.md
  - ../design/render-capture.md
  - ../design/render-pipeline.md
  - ../design/automation-transport.md
  - ../design/architecture.md
---

# 0068 M8.4 编辑器与外部操作一致性

## 目标与设计依据

按 [编辑器设计](../design/editor.md) 的跨入口契约完成 M8.4 / 交付 C：
GUI、外部命令、指定版本截图与保存后 runner 重现同一已提交场景。

## 实际变更

- [ScenePipeline](../../engine/render/pipeline/include/dk/render/ScenePipeline.hpp) 提供共用
  `unlit_preview_settings()`，Viewport 和 CaptureService 使用同一背景色 (0.04,0.08,0.16)、
  exposure=1；修复此前视口 (0.025,0.035,0.05) 与截图背景不同的问题。通用 RenderSettings 黑色默认值保留。
- [ConsistencyDriver](../../engine/editor/src/ConsistencyDriver.cpp) 使用真实 ImGui 输入完成选中、名称/TRS
  Apply、草稿冲突/Revert、Undo/Redo、Save/Reload；每次窗口 GPU 提交完成后发布固定检查点。
  报告包含场景版本、全部实体、Inspector/选择/历史、已发布相机/尺寸与实际上传的视口像素。
- `dk-editor --consistency-smoke` 仅允许带一次性标记的夹具，必须提供 pipe、fixture-camera、screenshot，
  拒绝混用其他 smoke/frames。固定同步文件只推进测试步骤，180 秒超时；普通会话不额外保留 CPU 像素副本。
- [进程验收](../../tests/integration/EditorConsistencyTest.ps1) 通过独立 dk-ctl 子进程修改同一 Runtime，
  逐阶段比较 GUI/RPC 全部实体及指定版本离屏图像；另测重复 ticket 原 TaskId、旧 guard 拒绝、
  已提交旧版本截图不受后续编辑影响、草稿不进入截图、操作不隐式保存和 Reload 的旧会话拒绝。
  关闭编辑器后独立 runner 加载保存结果，比较全部状态及图像。
- 新增 `dk.editor.consistency_gpu_validation`（gpu、串行、240 秒），同步测试选择表、
  [编辑器指南](../guides/editor.md)、[截图命令参考](../commands/render.md)、设计与 Roadmap。
  没有新增业务命令、修改命令 schema 或引入/升级依赖；CPU-only 模块边界不变。

## 验证记录

VS2026 x64 Debug，`out/build/windows-editor`；NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49。
Vulkan/Khronos 与同步验证开启。仅在验证进程环境设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，
finally 恢复原值，以避开本机已定位的 AMD 隐式层冲突。

```powershell
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.editor\.consistency_gpu_validation$' -Reason 'M8.4 real GUI IPC capture runner consistency acceptance'
./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_editor_app','dk_run','dk_ctl','dk_ipc_tests') -TestRegex '^dk\.editor\.(workbench|interaction)_gpu_validation$|^dk\.runtime\.capture_gpu_validation$|^dk\.ipc\.(IPC |Named pipe |Disconnected |Pipe )' -Reason 'M8.4 shared preview settings and acceptance integration: original editor capture and IPC failure regressions'
```

| 检查 | 证据 | 结果 |
| --- | --- | --- |
| M8.4 跨入口验收 | `out/verify/20261008-135302-9c3cbc55` | 1/1 通过，20.12 秒，无跳过 |
| 原工作台、交互、截图与 IPC | `out/verify/20261008-135414-37e87fc2` | 12/12 通过，无跳过；3 个 GPU 集成测试及 9 个 IPC 超时/断连/重放/分帧/队列用例 |
| 命令发现与文档契约 | 本轮 runner 执行 commands.list/describe，`out/m84-command-contract` | render.capture 存在、external/非撤销，guard/output 必填，相机/尺寸/profile 与参考一致 |
| 工作台视觉核验 | 下述 workbench.ppm 无损转 PNG 后查看 | rev 6 / Saved、4 个实体、3 draws，外部修改名称正确，无 STALE PREVIEW |

验收产物位于 `out/build/windows-editor/test-artifacts/Debug/editor-consistency-99a1a91feca0454a8c2c30cebda9b42e/`：
`acceptance.json`、参数/回复、stdout/stderr、工作台截图，以及 `project/.dk-consistency/` 中的 9 个检查点和 capture PPM。
视口 796×446；revision 顺序为 1、2、3、3、4、5、6、6、6，runner 重载仍为 6，document_id 按会话更换。
9 个窗口检查点均与离屏图像 SHA256 相同；额外旧版本 capture 对应 revision 2，runner 图像与最终视口相同。
最终 RGB8 PPM SHA256 为 `2EEAFCDDF4E3B6AD2D50533D9E18A7C4B240CC622F2000767B6A44D23CF40086`。
GUI 草稿冲突是预期的一次业务错误，未覆盖远端内容；所有阶段 Vulkan validation errors=0、warnings=0，
编辑器退出 liveAllocations=0；capture 子任务全部成功且无诊断。

初轮 `out/verify/20261008-134712-209cc28d` 因新增 helper 重复定义失败；
第二轮 `out/verify/20261008-134851-ef1c5edc` 因测试中 string/JSON 的 C++ 重写比较候选失败。
两次均在构建阶段停止、未运行测试；删除重复定义并显式读取 JSON 字符串后通过上述最终验证。

`scripts/check-spec.ps1` 通过：144 个 Markdown、1448 个本地链接及元数据、表格、索引、测试入口和 JSON 清单。
`git diff --check` 通过。

## 偏差与决策

使用现有 disposable smoke 机制与固定 checkpoint/continue 同步；不增加测试用业务命令。
原始视口 RGB8 用于像素比较，工作台截图用于呈现证据；相机/尺寸显式记录，不比较含 UI 的不同窗口图像。

## 遗留问题与下一步

M8.4、M8 和交付 C 完成；下一项为 M9.1 Lua 命令绑定。
本次未运行 Release、全量测试、Sponza 大场景一致性、跨 GPU/驱动或非 Windows 验证。
精确像素结果只适用于固定资产、同设备/驱动和相同视图；资产文件仍按 capture 执行时读取，
会话相机不写入 Scene，Gizmo preview/未应用草稿不属于已提交截图。

## 修改记录

- 2026-10-08T13:40:00+08:00：核对设计与既有实现，建立跨入口契约和验收记录。
- 2026-10-08T13:57:00+08:00：完成共享预览设置、真实窗口/进程验收及定向回归，关闭 M8/交付 C。
