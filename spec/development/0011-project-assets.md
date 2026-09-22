---
id: "0011"
created_at: "2026-09-22T12:28:05+08:00"
updated_at: "2026-09-22T12:36:42+08:00"
status: completed
design_refs:
  - ../design/assets-types.md
  - ../design/project-format.md
  - ../design/scene.md
---

# 0011 工程格式与资产引用

基线 17b94da，按 M2.3 设计建立最小工程协议、资产类型和引用解析；
[资产设计](../design/assets-types.md) 与 [工程设计](../design/project-format.md) 已先行。
本节不实现完整资产加载链。

## 实际变更

新增 dk::asset_types（AssetKind、AssetReference）与 Scene 的资产列表、描述及编辑校验。
[Project.hpp](../../engine/scene/include/dk/scene/Project.hpp) 提供 v1 JSON 编解码、
显式工程根、只读资产索引和普通文件诊断；UTF-8 名称、规范相对路径、版本/字段、
重复对象键/ID、输入大小和深度全部检查。资产引用失败包含实体/资产上下文。
scene feature 新增 nlohmann-json 3.12.0，Scene PUBLIC 引用 IO/asset_types，JSON 保持 PRIVATE。
独立配置继续不引入 fmt/spdlog、窗口或 GPU。新增测试辅助目录使用独立 UUID 和 Unicode 路径。

## 验证记录

默认 windows-debug/windows-release build/test 各注册 119 项，118 项通过，1 项既有符号链接权限跳过。
windows-scene-only 将 DK_BUILD_IO=ON，保留 math=ON、日志/示例/runner=OFF、
DK_VCPKG_FEATURES 为空与警告即错误；Debug/Release build/test 各注册 96 项，95 通过、同一权限跳过。
6 项新测试覆盖协议往返、确定输出、重复键/ID、非法版本/字段/路径、UTF-8、大小/深度、
真实资源解析和缺失/种类/目录诊断，失败保持场景 revision。编译无警告，diff/文档检查通过。
其他操作系统未测试。

## 遗留与下一步

M2.4 场景 JSON、两遍加载、一致快照和安全重载。

## 修改记录

- 2026-09-22T12:36:42+08:00：实现与两组 Debug/Release 验证完成。

- 2026-09-22T12:28:05+08:00：设计先行并建立记录。
