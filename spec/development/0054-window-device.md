---
id: "0054"
created_at: "2026-10-02T21:40:00+08:00"
updated_at: "2026-10-02T21:56:00+08:00"
status: completed
design_refs:
  - ../design/platform.md
  - ../design/graphics-presentation.md
  - ../design/graphics-device.md
---

# 0054 M5.6.1 窗口与设备接入

## 范围

依据 [Platform](../design/platform.md) 与 [Presentation](../design/graphics-presentation.md)，
接入 SDL3 窗口、事件与 surface-aware 设备选择。独立验收窗口变化、设备呈现能力与销毁顺序。

## 依赖决策

2026-10-02 核验[官方 SDL3 port](https://raw.githubusercontent.com/microsoft/vcpkg/master/ports/sdl3/vcpkg.json)
仍为 3.4.16#1，与已固定 builtin-baseline 一致，保留 baseline，不升级其他包。
从 editor 预留中提取独立 platform feature，实际消费 SDL3，不带入 ImGui。
本机 vulkaninfo 证实 RTX 4070 Laptop 支持 EXT/KHR swapchain_maintenance1，采用 EXT 名称兼容既有驱动。

## 实际变更与验证

- 新增 Window 共享句柄、SDL video lease、实际像素状态及全窗口事件路由；内部桥接拥有的 surface，Device 持有窗口 token。
- AdapterInfo 与纯选卡策略扩展 present family、swapchain/maintenance1；headless 不请求 WSI。
- 新增 windows-presentation 预设、独立 platform feature 和两个桌面探针。
- 首轮构建因 tests 目录缺少 SDL3 imported target 的 find_package 失败；修正作用域后重跑。
- `out/verify/20261002-215343-25651a02`：Windows x64 Debug 18/18，通过、零跳过。
  `verify.ps1 -BuildDir out/build/windows-presentation -Target @('dk_platform_probe','dk_present_device_probe','dk_device_tests')`
  `-TestRegex '^dk\.(platform\.|presentation\.device_validation$|device\.unit\.)'`。
  16 项 Device 策略/故障用例、窗口实际 resize/minimize/restore/事件/线程/共享寿命、3 次 surface-aware device 创建销毁。
- RTX 4070 Laptop / NVIDIA 596.49 / API 1.4.329，required Khronos validation 零警告/错误，Memory 活分配为零。
  沿用 0048 的进程内 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复；未关闭 Khronos validation。
- 文档检查与 diff 检查另于本阶段提交前运行。未运行 Release/其他平台；交换链功能在 M5.6.2 实现。

## 下一步

M5.6.2 外部图像、交换链与帧同步。
