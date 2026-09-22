---
id: "0008"
created_at: "2026-09-22T11:52:43+08:00"
updated_at: "2026-09-22T12:02:27+08:00"
status: completed
design_refs:
  - ../design/foundation-integration.md
  - ../design/project-foundation.md
---

# 0008 Foundation CPU 集成验收

## 目标与设计依据

完成 M1.6，先写 [集成设计](../design/foundation-integration.md)。基线 f6a90e5，工作区干净。
用户要求后续 Git 提交正文更详细，并授权本节完成后连续推进 M2.1–M2.4，无需逐节确认。

## 实际变更

新增 [CPU 示例](../../examples/foundation/src/main.cpp) 与 examples 两层 CMake，
通过公開 Foundation 接口生成稳定 ID、组合父子变换、保存完整仿射矩阵、重载并核验点逆变换。
示例格式使用逐元素 double 文本和严格 from_chars 解码，保留剪切；不转储 Eigen 内存。
新增 DK_BUILD_EXAMPLES 开关和 bootstrap 关闭项；集成测试不再以 runner 存在为总开关。
新增 [15 项跨进程测试](../../tests/integration/FoundationDemoTest.cmake)，
覆盖 Unicode 路径、覆盖保存、损坏格式、读上限、路径/参数错误与 stdout/stderr 分离。
将详细提交信息约定落入 AGENTS.md，方便后续开发保持一致。

## 验证记录

Windows x64 / NTFS，MSVC 19.51、CMake 4.2.1，沿用锁定 vcpkg 基线。

实际执行：

- cmake --build --preset windows-debug；ctest --preset windows-debug：
  构建通过，101 项中 100 通过、1 项既有符号链接权限跳过，0 失败。
- cmake --build --preset windows-release；ctest --preset windows-release：结果相同。
- cmake --preset windows-dev -B out/build/windows-foundation -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_BUILD_UNIT_TESTS=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON。
- cmake --build out/build/windows-foundation --config Debug/Release（分别执行）；
  ctest --test-dir out/build/windows-foundation -C Debug/Release --output-on-failure（分别执行）：
  两组均构建成功、15/15 通过，无跳过。依赖仅 stduuid 1.2.3、Eigen 5.0.1 与 vcpkg CMake 辅助包。
- cmake --preset windows-bootstrap；cmake --build --preset windows-bootstrap-debug；
  ctest --preset windows-bootstrap-debug：配置/构建成功，1/1 通过，示例关闭。

所有测试产物位于构建目录，每个进程案例创建独立随机测试工程。
损坏文件加载后哈希不变；成功 save/readback 与独立 load 的 ID/世界点输出一致，
二次 save 创建新身份并覆盖旧示例，无原子保存临时残留。

## 偏差与决策

无实现失败或范围偏移。不把示例格式作为正式场景协议；
M2 的 flecs、组件、工程/场景格式后续分别设计实现。
符号链接用例权限跳过不计通过，Linux/macOS、断电恢复与外部目录竞态未验证。

## 遗留问题与下一步

M1.6 已完成并关闭 M1。直接进入 M2.1，使用 0009；保持每节独立编号记录与详细本地提交。

## 修改记录

- 2026-09-22T11:52:43+08:00：先行设计与建立记录。
- 2026-09-22T12:02:27+08:00：完成独立示例、进程验收与构建边界验证，同步 M1 完成和连续 M2 授权。
