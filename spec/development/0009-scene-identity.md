---
id: "0009"
created_at: "2026-09-22T12:05:49+08:00"
updated_at: "2026-09-22T12:16:00+08:00"
status: completed
design_refs:
  - ../design/scene.md
---

# 0009 flecs 场景文档与实体身份

## 目标与设计依据

M1.6 已在 070d224 完成。按照连续 M2 授权进入 M2.1，
先写 [SceneDocument 设计](../design/scene.md)，本节止于 ECS 所有权与稳定身份。

## 实际变更

已接入 [SceneDocument](../../engine/scene/include/dk/scene/SceneDocument.hpp) 的私有 flecs world、
身份索引、实体操作和 revision/dirty 基础。新增 6 项消费者单元测试，验证实际 ECS 数据与索引一致、
ID 去重、失败不改变状态、跨文档隔离、排序和重复生命周期。flecs 4.1.4 由 vcpkg scene feature 提供，
默认开发预设启用 DK_BUILD_SCENE，独立 Foundation 命令显式关闭。
组件/层级和保存确认留到后续子阶段。

## 验证记录

Windows/MSVC 19.51：`cmake --preset windows-dev`、Debug/Release build/test 预设全部通过；
各注册 107 项，106 项通过，1 项既有符号链接权限跳过。

```powershell
cmake --preset windows-dev -B out/build/windows-scene-only -DDK_BUILD_MATH=OFF -DDK_BUILD_IO=OFF -DDK_BUILD_LOGGING=OFF -DDK_BUILD_EXAMPLES=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-scene-only --config Debug
ctest --test-dir out/build/windows-scene-only -C Debug --output-on-failure
cmake --build out/build/windows-scene-only --config Release
ctest --test-dir out/build/windows-scene-only -C Release --output-on-failure
```

独立配置只安装 stduuid/flecs/Catch2 及构建辅助包，各 16/16 通过，无警告。
非 Windows 平台和资源耗尽路径未实测；不承诺 flecs 致命错误后的恢复。

## 偏差与决策

Scene 为显式模块开关，开发预设启用；独立 Foundation 配置显式关闭。

## 遗留问题与下一步

完成本节后继续 M2.2 组件与变换层级，不等待逐节确认。

## 修改记录

- 2026-09-22T12:05:49+08:00：先行设计与建立记录。
- 2026-09-22T12:16:00+08:00：完成实现、两组 Debug/Release 验证和阶段交接。
