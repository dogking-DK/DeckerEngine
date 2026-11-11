---
id: "0160"
created_at: "2026-11-11T16:40:00+08:00"
updated_at: "2026-11-11T17:42:27+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/assets-importers.md
  - ../design/geometry-query.md
---

# 0160 M23.2 CPU 体素参考与稀疏转换

## 目标与设计依据

实现[体素设计](../design/render-voxels.md)的 M23.2：稠密正确性参考、无 CUDA OpenVDB/NanoVDB 依赖、CPU 稀疏转换/查询。
不提前实施 GPU/Slang、完整文件资产协议或 Editor；范围和状态见 [Roadmap](../roadmap.md#m23实体体素渲染)。

## 实际变更

新增 `dk_geometry_voxels` 稠密拥有型网格/逐格遍历、`dk_geometry_voxels_sparse` NanoVDB CPU 层级查询，
以及 `dk_asset_voxel_importers` OpenVDB 稀疏块构建/转换；使用已有 Geometry Ray 与 Math 仿射变换。
公开接口不含第三方类型；不可变快照共享保活，NanoVDB 输出分配器在分配前校验精确字节额度。
查询返回 cell/face/value/t/世界法线及访问区域数，实例逆变换保留射线参数。
新增两个 CPU 预设、独立外露面 oracle、PNanoVDB C32 读值对照、tile/大跨度/变换/失败/并发用例。
公开接口、有限域/半格约定、默认额度与 scratch 限制已合入主题设计；同步 Roadmap、模块索引、依赖说明、构建指南和测试选择表。

## 锁定来源与构建边界

- 官方 port 核验提交 `f38855d514df2b21c2e911a6c6865703abb8da15`，OpenVDB 最新 12.0.1#0。
  原基线 `33d78c1ed898a06938f31312167c7abefd229455` 已有相同版本，保持不变；其他工程依赖和 tools/profiling 清单不升级。
- `cmake/vcpkg-ports/openvdb` 保留同源官方补丁与 Apache-2.0 许可证，移除 nanovdb feature 的 CUDA 依赖，
  关闭 `NANOVDB_USE_CUDA`、intrinsics、examples 和上游 unit tests；只选择 core/nanovdb，不启用 AX/Python/GUI/tools。
  工程的 Math/Core/Geometry 公共接口不暴露 VDB；NanoVDB 查询模块只消费私有头文件，不链接 OpenVDB。
- 上游归档 `AcademySoftwareFoundation/openvdb` 的 `v12.0.1.tar.gz` SHA512：
  `67b859bf77c53e68116faa7915bb6a5a50a8cff10435762890e13348625e8aebdb6661b722017632471648afe31e2f9d4cd2e18456c728192bfd0accd70a40ef`。
  实际下载哈希与 port 一致；日常配置不追踪 HEAD。
- 安装头 NanoVDB/PNanoVDB 均为 **32.7.0**（ABI major 32），与 OpenVDB 产品版本分别记录。

| 安装头（nanovdb/ 下） | SHA256 |
| --- | --- |
| NanoVDB.h | `c83c7f3b1699dd009446194e04ffcf5e737104345c0c5c903f697cde201ae808` |
| PNanoVDB.h | `eed582ab74878225c05bb5e3f08c92ca5a06cfd6075490f873607086f21fe7b9` |
| tools/CreateNanoGrid.h | `f047b8e0a5bf89849281cfa90e7ec34af3668dacb366112c6de5831e727c8db7` |

传递依赖按固定 port 核对：TBB 2023.1.0/Apache-2.0、hwloc 2.11.2/BSD-2-Clause、Boost 1.92.0/BSL-1.0，
Imath 3.2.2、OpenEXR 3.4.15、Blosc 1.21.6 为 BSD-3-Clause；压缩库包括 zlib 1.3.2#2/Zlib、
zstd 1.5.7/BSD-3-Clause OR GPL-2.0-only、lz4 1.10.0/BSD-2-Clause、bzip2 1.0.8#6/bzip2-1.0.6、liblzma 5.8.4/0BSD。
snappy 1.2.2#2 的 port 未声明 SPDX，已检查其安装 copyright 中的三条款 BSD 源库许可及测试数据附加说明，未使用该测试数据。
核对数据留在 `out/agent-notes/voxel-dependency-audit.json`；安装 copyright 保留随库分发。

## 验证记录

环境为 Windows x64、VS 2026 / MSVC 19.51.36260、CMake 4.2.1、vcpkg 2026-10-29 工具。
本阶段工程验收使用 Debug、`DK_WARNINGS_AS_ERRORS=ON`（/W4 /WX）；依赖由 x64-windows port 生成 Debug/Release，不等同于引擎双配置验收。

稠密独立预设已验证（Debug，MSVC /W4 /WX）：

```powershell
cmake --preset windows-voxels-dense -DVCPKG_INSTALLED_DIR=D:/resource/code/DeckerEngine/out/build/windows-dev/vcpkg_installed -DVCPKG_MANIFEST_INSTALL=OFF -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels-dense -Target @('dk_voxel_tests','dk_geometry_tests','dk_math_tests','dk_core_tests') -TestRegex '^dk\.(voxels|geometry|math|core)\.' -Reason 'M23.2 稠密参考与直接回归' -Parallel 4
```

复用与原固定基线一致的已安装 CPU 依赖（Eigen 5.0.1、Catch2 3.16.0、stduuid 1.2.3、magic-enum 0.9.8），
不并发安装或更改该前缀。结果：64 通过、0 失败、0 跳过；证据 `out/verify/20261111-171039-a924d5a3/summary.json`。
其中 3072 条随机射线跨空/实/空心/棋盘/随机/单格六类夹具，与独立外露面 oracle 对照；边界和仿射另外解析验收。

稀疏独立预设已配置、构建并运行：

```powershell
cmake --preset windows-voxels -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels -Target @('dk_voxel_tests','dk_voxel_sparse_tests') -TestRegex '^dk\.voxels\.' -Reason 'M23.2 CPU 体素验收' -Parallel 4
```

结果：**13 通过、0 失败、0 跳过**；`out/verify/20261111-173655-12ee3262/summary.json`。
其中 4 项稠密测试与前述 64 项重叠，不把两次运行相加当作唯一测试数。

| 验收项 | 实际结果 |
| --- | --- |
| 六类小网格三方对照 | 6144 条固定种子射线，对照稠密 DDA 与独立面 oracle 的 hit/t/cell/face/value；逐格检查源 active 任意组合的规范化结果 |
| 背景/tile/leaf | 零背景、非零 inactive 与零 active、负坐标、8/128/4096 层级；半格与非均匀/负缩放/shear/实例组合，t 和世界法线一致 |
| 大常量 tile | 两个 4096³ tile，非叶节点/叶节点均为 0，NanoVDB 小于 8 KiB；内部射线访问不超过 2 区域，材质交界不制造表面 |
| 大跨度空域 | x=±1048576 的两单格，只有 2 个 leaf、产物小于 1 MiB；跨空域查询小于 1024 次访问，限额 1 明确失败 |
| 边界与拒绝 | 内外起点、空心、平行、边角同时跨轴、裁剪；非法值/坐标/维度/背景/类型/SDF/fog/非线性和奇异变换、构建/输出/访问额度拒绝 |
| 原子结果与寿命 | 失败不修改输入 active 状态或已发布快照；复制共享保活，替换后旧快照可用，双线程只读查询通过 |
| PNanoVDB portable | C32 读相同 buffer 的 background、负坐标、各层 tile/leaf；包含 16777217 和 INT32_MAX，保持 Int32 精确值；这不是 Slang 验收 |
| 无 CUDA/GPU 依赖 | OpenVDB cache 的 CUDA/AX/Python/examples/tests OFF；工程 Graphics/Render/Scene/Editor/IO OFF；安装清单无 CUDA/Vulkan/Slang，dumpbin 检查测试 exe 与 7 个部署 DLL 无这些导入 |

DLL 证据为 `out/agent-notes/voxel-dll-imports.log`；7 个部署 DLL 为 OpenVDB、TBB、Imath、Blosc、lz4、zlib、zstd。
上游附带 CUDA 头文件不构成 CUDA target/SDK 依赖。独立稠密预设不选择/查找 VDB，不链接它们。
最终一次 OpenVDB 及剩余 18 个依赖安装用时 4.3 min（此前已构建部分依赖，不能当作完整冷构建基线）；
专项最终 verify 用时约 37 s，包含增量编译和测试，不作为渲染性能结论。

文档检查通过：`scripts/check-spec.ps1 -Path @(本次 13 份 Markdown)` 验证 1030 个本地链接、元数据、表格、索引、测试入口和 JSON manifests；`git diff --check` 通过。

### 已处理的失败与未运行项

- 首次尝试统一切到新 registry 时，本机缺版本数据库，确认无本地修改后快进并 bootstrap；
  随后确认原基线已含最新 OpenVDB，撤销工程 baseline 更新并中止该次安装，保留 `out/agent-notes/vdb-latest-baseline-cancelled.log`。
- 7-Zip TLS、Boost UUID/Atomic DNS 下载失败，使用相同官方归档恢复缓存，未绕过哈希；Atomic 另经公共 DNS 只读查询后用 curl `--resolve` 下载，未修改系统 DNS。
- 旧 windows-math-only 配置缺 magic-enum 而失败，未用旧二进制验收；改用专用稠密目录和已匹配基线的 CPU 依赖。
- 首次稀疏配置中 OpenVDB IMPORTED target 不可见于兄弟测试目录，改为 `find_package(... GLOBAL)`；
  上游 FindOpenVDB 在 CMake 4.2 发出 CMP0167 开发警告，配置/构建成功，未修改上游 Find 模块。
- 四次专项构建在测试前失败：弃用的 `GridHandle::size()`、NanoVDB `toGridType` 的 C4702、
  MSVC 旧 `__cplusplus` 分支选到已移除的 result_of/StatsMode 命名空间、Invoke/Reduce 模板 C4702。
  改用 bufferSize/tools::StatsMode、仅 NanoVDB 消费者 `/Zc:__cplusplus` 和 SDK include 范围的 C4702 pragma；没有关闭工程 /WX 或修改上游头。
  失败摘要在 `out/verify/20261111-173254-49841738`、`20261111-173359-a3033da3`、`20261111-173449-209b6d42`、`20261111-173554-e17fb050`；最终结果见上文。
- 未运行引擎 Release/全量、GPU/Slang、Editor 或文件资产验收：本次没有这些实现改动，不把 CPU portable 成功算作后续阶段通过。

## 偏差与决策

官方 nanovdb feature 强制 CUDA，采用同源最小 overlay 提供无 CUDA CPU/header 接入。
原 builtin-baseline 33d78c1ed898a06938f31312167c7abefd229455 已固定最新 OpenVDB 12.0.1#0，故保留该基线，
避免扩大到 Slang/Vulkan/Editor/Tracy 等无关升级；tools/profiling 清单也无需改变。overlay 来源固定为最新官方 port 提交，源码版本/摘要不变。
稀疏和稠密共享表面边界契约与推进框架，但独立外露面 oracle 不调用遍历；额外解析 tile/空域用例检查是否仍按稠密方式访问。

## 遗留问题与下一步

M23.2 完成；下一阶段为 M23.3 NanoVDB 上传、PNanoVDB/Slang compute 与共享 HDDA。
M23.5 仍须处理正式 `.vdb` 文件/资产协议与解压峰值；当前程序化输入和私有 GridBase 桥不能冒充完整导入器。
树/产物额度不是进程 RSS 硬上限，OpenVDB/TBB scratch 未接管；GPU 光栅、原生 RT 和 Slang 双门禁均未实现。

## 修改记录

- 2026-11-11T16:40:00+08:00：创建 M23.2 实施记录。
- 2026-11-11T17:40:00+08:00：完成 CPU 实施、依赖及数值/失败/隔离验收，同步设计与下一阶段。
