---
id: "0087"
created_at: "2026-10-10T14:49:00+08:00"
updated_at: "2026-10-10T14:52:00+08:00"
status: completed
design_refs:
  - ../design/platform.md
  - ../design/editor.md
---

# 0087 编辑器后台窗口与自动化免打扰

## 目标与设计依据

用户要求测试时编辑器不频繁弹到顶层打断工作，普通启动仍自动弹出。
先更新[窗口设计](../design/platform.md)及[编辑器设计](../design/editor.md)：增加本次进程可选的`--background`，
从创建到最小化恢复均避免自动激活，所有编辑器测试入口显式使用该参数。
保留已有用户文档改动，不纳入本任务提交。

## 决策

当前没有ALWAYS_ON_TOP，打断来源是SDL默认创建显示和Windows恢复时自动激活。
核对本机SDL 3.4.16实现，单独设置[SDL显示激活hint](https://wiki.libsdl.org/SDL3/SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN)不覆盖恢复。
采用每窗口后台策略，Windows先隐藏创建，再用NOACTIVATE放到后方；恢复使用
[SetWindowPlacement](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwindowplacement)的不激活状态。
不修改全局hint，不让普通交互窗口永久不可聚焦；隐藏请求及SDL事件/实际尺寸继续有效。

## 验证计划

平台探针检查后台创建、resize/minimize/restore、可见/非置顶/可聚焦、前台激活事件为零及资源回收。
沿用模拟视口真实窗口smoke验证后台仍可渲染、注入交互、截图、最小化恢复和关闭；选择一项IPC路径验证后台宿主。
只用已有windows-editor/RelWithDebInfo，不全量或双配置；普通前台启动不在本次桌面弹窗试验中执行，核对默认false及原路径。

## 实施与验证

- [Window](../../engine/platform/src/Window.cpp)增加每窗口background状态和Win32显示/恢复适配；
  隐藏创建后用SetWindowPos/NOACTIVATE显示于后方，最小化/恢复采用SW_SHOWMINNOACTIVE/SW_SHOWNOACTIVATE。
  SDL事件继续反映实际可见/最小化和尺寸；user32仅通过Platform的PRIVATE依赖表达。
- [Workbench](../../engine/editor/src/Workbench.cpp)、[选项](../../engine/editor/include/dk/editor/Workbench.hpp)和
  [CLI](../../apps/editor/src/main.cpp)传递`--background`，默认false；help及[使用指南](../guides/editor.md)同步。
- 7个编辑器启动位置统一传参：工作台/交互/模拟三个PowerShell smoke、编辑一致性、IPC、模拟一致性及响应基准；
  三规模模拟任务复用响应基准host，自动获得相同行为。AGENTS补充自动化使用后台选项的约定。
- [PlatformProbe](../../tests/integration/PlatformProbe.cpp)通过WinEvent前台事件观察和实际窗口属性/层级验证，
  覆盖3轮可见窗口生命周期、hidden优先、可手动聚焦、正常窗口不继承政策、线程限制和Memory回收。

首轮执行：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_platform_probe','dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.(platform\.windows|editor\.simulation_gpu_validation|ipc\.editor_gpu_validation)$' -Reason '验证后台窗口创建/恢复不抢焦点，真实布料GUI交互和IPC后台宿主；普通前台启动不弹窗测试'
```

`out/verify/20261010-144841-cfb335d7`构建成功，2通过、1失败、0跳过：

| 检查 | 结果 |
| --- | --- |
| platform.windows | 0.27秒，3轮后台可见/可聚焦/位于前台窗口后方，activation_events=0，Memory存活分配0 |
| editor.simulation_gpu_validation | 16.14秒，CPU/GPU控制与持续相机输入、截图、最小化恢复、Stop/restart和活动关闭通过；2204个窗口帧；Vulkan errors=0、liveAllocations=0，既有精确Loader提示1条 |
| ipc.editor_gpu_validation | 2.88秒，退出时被既有AMD隐式层版本警告判失败；不是后台创建或IPC交互失败 |

按[既有指南](../guides/editor.md)仅在重跑进程环境设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，
保留Khronos validation，结束恢复原环境；不修改程序的正常验证/退出政策。仅重跑失败项：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo -Target @('dk_editor_app','dk_run','dk_ctl') -TestRegex '^dk\.ipc\.editor_gpu_validation$' -Reason '仅重跑受已知AMD隐式层警告影响的后台IPC检查，按既有指南关闭该层，保留Khronos验证'
```

`out/verify/20261010-145011-7037d8e9`：1通过、0失败/跳过，2.65秒；后台IPC启动/编辑/关闭和零验证告警、零存活分配检查通过。
首轮有效结果未重复运行，合计3个不同的定向检查全部取得通过结果。
完整首轮CTest输出保留在该轮`ctest-full.log`；GUI截图和导出位于
`out/build/windows-editor/test-artifacts/RelWithDebInfo/editor-simulation-89a053ddfd274b40b343d93f29d4f3b8`。
另运行`dk-editor --help`确认参数文案；文档检查及git diff --check通过。

## 限制与下一步

本次Windows后台显示已完成。普通启动保持原SDL路径，没有为了验证默认弹出行为而再次抢占用户桌面。
未运行全量/双配置、完整响应性基准、其他操作系统或所有仅追加相同启动参数的夹具。
后台可见窗口由系统管理遮挡，性能基准今后在该启动方式下采集，不能直接当成旧前台采集条件。
