---
id: "0033"
created_at: "2026-09-28T12:24:00+08:00"
updated_at: "2026-09-28T12:39:21+08:00"
status: completed
design_refs:
  - ../design/assets-importers.md
  - ../design/assets-runtime.md
---

# 0033 M4.2.1 CPU 数据与网格导入

基线 cd46345，工作区干净。用户授权整个 M4.2，按 M4.2.1/2 分别验收和本地提交。
先固定 [导入契约](../design/assets-importers.md#m421-实施契约)，实现 data/importers、快照输入、
严格静态 mesh/材质子集、ID 映射与定向边界测试。纹理和离线工具在下一编号记录。

2026-09-28 实时请求 vcpkg 官方 master 的 fastgltf/stb/simdjson port，并与固定 baseline 的
git show 比较：fastgltf 0.9.0、stb 2024-07-29#1、simdjson 4.6.11 一致，保留 baseline。
M4.2.1 接入 fastgltf，stb 留在 M4.2.2；没有修改其他依赖。

实现 [CpuData.hpp](../../engine/assets/data/include/dk/assets/CpuData.hpp) 与
[import_gltf](../../engine/assets/importers/include/dk/assets/GltfImporter.hpp)，创建真实 data/importers target。
输入快照与输出均为拥有型 dk 容器，解析后不保存 fastgltf view；临时 selector 索引使用 ScratchScope。
所有文件只读。已有 output 身份被复用，消失的 selector 返回 not_found；新身份只属于候选。
一个 mesh、多 primitive、FLOAT 位置/法线/UV、三种 uint 索引及默认索引、材质数值、局部 bounds 已实现。
nodes 不烘焙，输出诊断；纹理明确 not_supported，留给已授权的 M4.2.2。

fastgltf 不自动读取外部文件，URI 先解码和限制，再经 IO 读取；GLB 容器结构先检查。
源/依赖/累计输入、顶点/索引和输出数值载荷预算在读取和分配前检查；容器/JSON 对象管理开销不计为数值载荷。
导入调用要求宿主绑定持久域及带 scratch 的 ThreadContext，缺少配置抛 ContextError；OOM 同样不被伪装为格式错误。

## 验证

- `scripts/verify.ps1 -Target dk_import_tests -TestRegex '^dk\.import\.'`：
  `out/verify/20260928-123551-5e0484fb`，7/7 通过。
- 独立 `windows-import-only`（AssetRuntime/Scene/Framework/日志/Runner 关闭，Math/Memory/IO 开启）：
  `out/verify/20260928-123628-d8357808`，7/7 通过。
- 自制生成夹具验证 offset/stride、多 primitive、3 种索引与无索引、GLB、数值/材质/ID，
  16 类格式/范围/特性失败、7 类预算失败、9 类 URI 拒绝、Unicode 百分号编码兄弟路径、缺失依赖与上下文退出后的数据寿命。
- 首轮配置在新模块 CMake 尚未落盘时停止（依赖安装成功）；随后已完整配置。
  编译曾发现 dk::String/std::string 比较及 fastgltf 0.9 已弃用 LoadGLBBuffers 选项，修正为 string_view / Options::None。
  首轮运行因夹具 ThreadContext 没有 scratch 全部失败，已修正宿主配置后重跑；未使用旧二进制提供通过证据。
- 只运行目标 Debug，未运行全工程/Release/Tracy capture；纹理与 assetc 待 M4.2.2。

文档/JSON/本地链接与 git diff --check 在提交前检查。M4.2.1 完成，继续 M4.2.2，记录编号 0034。
