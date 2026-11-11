---
id: "0163"
created_at: "2026-11-11T19:45:00+08:00"
updated_at: "2026-11-11T20:49:52+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/assets-types.md
  - ../design/assets-runtime.md
  - ../design/assets-importers.md
  - ../design/render-resources.md
  - ../design/render-pipeline.md
  - ../design/scene.md
---

# 0163 M23.5 体素文件资产与版本缓存

## 目标与设计依据

完成 [M23.5](../roadmap.md#m23实体体素渲染)，先冻结[资产实施契约](../design/render-voxels.md#m235-资产实施契约)，再接文件/缓存/Scene 和 GPU 资源。
复用既有身份、内容缓存及提交/取消边界；M23.6 的 Editor/服务设置/capture 闭环、M23.8/9 原生路径不提前实施。

## 实际变更

- `.dkvoxel` 描述显式声明命名 `.vdb`、实体材质索引、有限域、sample map 策略与 palette。
  OpenVDB File/Stream 都从拥有型字节解码；仅选择 UNKNOWN Int32Grid、零背景，规范化 active/inactive/tile，导入时应用 unit_scale 一次。
  控制输入、解码树、规范化树、Nano 输出和转换存活载荷；拒绝类型/语义/域/palette/格式/变换/预算错误。
- AssetKind/命令共享枚举增加 voxel；meta 为 voxel-vdb v1 / 唯一 voxel/0。
  现有编译/缓存/异步流程使用 root_asset_id，支持登记、同目录同扩展名改名和恢复；Scene/Project 继续仅保存 ID/kind/path/TRS。
  `.dkvoxel` 新建 meta 必须显式请求 voxel/0，既有默认 mesh/0 不变；已有 meta 省略 outputs 时仍完整采用。
- 独立 DeckerVoxelAsset v1 manifest/data 记录域、double corner map、palette、依赖摘要、完整布局版本及构建签名。
  Nano loader 在 accessor 前验证字节区域、BFS 节点数组、相对 child 地址、唯一坐标归属、非零值/active/domain、map 与完整 checksum。
  根表按 OpenVDB 的有符号坐标顺序校验，不能按打包后的无符号 key 排序。全零内部节点使用 GridStats 空 bbox 哨兵，
  按父节点地址/值验证，不能误读为有效 origin。保持根 tile 压缩，不展开稀疏域。
- Cache 键覆盖描述/VDB 全字节、配置、palette、单位缩放、固定库版本和 NanoVDB/PNanoVDB 头 SHA256；版本标签限 256 字节。
  失配布局/摘要/未知版本不复用，重建保留 AssetId 与旧快照。prepare/publish 之间输入变化拒绝，取消不发布。
- VoxelAssets 以 AssetId 管理不可变 GPU grid/palette/generation；上传成功后无分配发布，失败保留 current。
  默认最多 64 当前资产，每个 grid cache 512 MiB/两代；旧代释放并完成后可再替换。
  ScenePipeline::render_voxel_assets 解析 Scene 引用，复用 HDDA/深度/HDR/tone；缺资源/mesh-only/DiskScene/原生 PathTraceScene 明确拒绝 voxel。
  在途 Frame 保留 GPU 网格与 palette，卸载、缓存退出及 Memory closing 不破坏已提交帧。
- CMake 增加 CPU-only 与 GPU 资产预设和测试入口；既有依赖版本、baseline、无 CUDA overlay 保持。
  完整 runner 重编译暴露既有 RenderOperations 局部变量遮蔽，仅重命名 ReSTIR 临时变量消除 /W4 /WX 阻塞。
  资产命令测试的过时全局命令总数改为准确核对 8 个 assets + 3 个 jobs 命令；不依赖其他模块可选命令数。

## 验证记录

Windows x64、VS 2026/MSVC 19.51、Debug `/W4 /WX`；沿用 Vulkan SDK 1.4.363.0、Slang 2026.18、OpenVDB 12.0.1#0、NanoVDB/PNanoVDB 32.7.0。
通过 `scripts/verify.ps1` 显式选择 target/regex。按各测试最后一次结果汇总 **117 项通过，0 失败，0 跳过**；这是多次定向运行的去重结果，不是全量或单次 117 项运行。
索引保存在 `out/agent-notes/m235-acceptance-index.json`，原始 build/tests/results.xml 保留于下表目录。

| 范围 | 命令范围与结果 | 证据（out/verify） |
| --- | --- | --- |
| CPU 稀疏、glTF 编译/缓存、资产、Scene 直接回归 | windows-voxel-assets；dk_voxel_asset_tests、dk_voxel_sparse_tests、dk_voxel_asset_process_probe、dk_asset_tests、dk_asset_cache_tests、dk_asset_pipeline_tests、dk_scene_tests；初轮 86/93，7 个失败见下文及复验 | 20261111-202010-13d1e046 |
| 体素格式与四种 kind 中间复验 | 5/9；剩余 Unicode 探针、登记扩展名及测试路径问题已修复 | 20261111-202306-8421ebb6 |
| 文件/缓存/格式/预算/Scene/异步/登记改名与资产回归 | dk_voxel_asset_tests、dk_voxel_asset_process_probe、dk_asset_tests、dk_scene_tests；实际筛选见表后；35/36，剩余缓存损坏夹具复验通过 | 20261111-202707-b0db7f86 |
| 缓存布局失效最终检查 | dk_voxel_asset_tests；`^dk\.voxels\.assets\.voxel cache content changes`；1/1 | 20261111-203116-d356c69b |
| 同大小同 mtime 内容变化 + 独立进程及载荷统计 | dk_voxel_asset_tests、dk_voxel_asset_process_probe；`^dk\.voxels\.(assets_process$\|assets\.voxel cache content changes)`；2/2 | 20261111-203645-99973d1e |
| M23.4 合成直接回归 | dk_voxel_raster_probe；两项 raster_*_required 均通过；同轮新资产门禁初次失败后单独修正 | 20261111-201626-dff94d1a |
| 新文件资产真实 GPU | windows-voxel-assets-gpu；dk_voxel_asset_gpu_probe；`^dk\.voxels\.assets_gpu_required$`；1/1 | 20261111-202144-e0197c83 |
| 无 VDB 构建、旧异步/命令/服务/DiskScene/mesh GPU | windows-graphics；dk_run、dk_service_tests、dk_asset_command_tests、dk_async_asset_tests、dk_render_disk_tests、dk_render_pipeline_probe、dk_render_pathtracing；17/18，旧全局命令计数断言复验通过 | 20261111-202545-8d45fd8d |
| 命令 schema 最终检查 | dk_asset_command_tests；`^dk\.asset_commands\.asset command discovery`；1/1 | 20261111-203134-0225f021 |
| 全零内部节点最终复验 | dk_voxel_asset_tests、dk_voxel_asset_process_probe；`^dk\.voxels\.assets[._]`；10/10，包含全部 Nano loader 消费者 | 20261111-204636-0425f816 |
| 最终 loader 真实 GPU 消费者 | dk_voxel_asset_gpu_probe；`^dk\.voxels\.assets_gpu_required$`；1/1，零验证错误/警告/遗留分配 | 20261111-204652-09e8e7e4 |

CPU 主筛选为 `^dk\.(voxels\.(assets[._]|sparse\.)|assets\.|cache\.|pipeline\.|scene\.)`；
后续范围为 `^dk\.(voxels\.assets[._]|assets\.|scene\.asset kind reflection)`。
旧链路筛选为 `^dk\.(asset_commands\.|async_assets\.|services\..*(asset|schema)|render\.disk\.|render\.pipeline_gpu_validation$)`。
最终 loader 复验新增全零但仍分配节点的稀疏分支，并重新执行全部体素资产 CPU 与 GPU 消费者。
测试覆盖标准/流式 VDB、非零 inactive tile/零 active、负坐标/空树/root tile、命名 grid/缩放、坏 Nano 地址/校验和/布局、
描述与 VDB 内容失效、布局损坏重建、发布步骤注错、输入中途变化、已接受取消、旧快照与 closing、Unicode 独立进程及旧文件回归。

真实 GPU：**NVIDIA GeForce RTX 4070 Laptop GPU / 596.49**，RT 未启用，validation required + 同步检查；
新资产门禁记录 `validation errors=0 warnings=0 live=0`，VMA allocationCount=0，提交槽均回收。
核对文件→cache→Scene→AssetId 的实际像素/线性 palette，失败上传不消耗 generation，旧版本/代际额度/卸载/超时/在途及 closing 保活。
M23.4 两投影/双向遮挡/方向光/曝光与旧 mesh 的真实 GPU 检查直接复用，没有将 CPU-only 成功当作 GPU 成功。

CPU-only 探针及 12 个部署 DLL 经 dumpbin 审计，无 CUDA/Vulkan/Slang 导入，vcpkg 安装清单同样没有这些包；
证据 `out/agent-notes/m235-cpu-dependency-audit.json`。
独立进程夹具实际记录：源树 **611704 B**、规范化树 **611704 B**、Nano **611520 B**、全部解码树 **1224664 B**、
转换存活载荷 **2488656 B**。这些是树/字节载荷统计，**不是进程 RSS、allocator scratch 或驱动峰值**。

本次构建的 dk-run 在独立 out 工程只读调用 commands.list/describe，49 条命令，assets.register/catalog/status、entity.set_assets、scene.new 的 kind schema 均含 voxel；
effect/undoable 保持既有值，证据 `out/agent-notes/m235-command-describe.jsonl`。

中间失败均保留：测试辅助类型/宏编译错误、版本签名超过原长度限、负坐标 root key 排序、旧枚举非法值断言、
登记扩展名遗漏、Windows 窄参数/路径转换及原子损坏夹具长路径、过时命令总数断言；已修复并只重跑直接受影响检查。
文档检查通过：17 个 Markdown、1118 个本地链接、元数据/表格/索引/测试入口/JSON；`git diff --check` 通过。
一次受限环境的 MSBuild 子进程启动循环被精确终止，使用已有构建工具权限重启；不使用旧二进制冒充成功。
未运行 Release、全量、其他平台、GUI/capture 体素或原生 RT/intersection 验收；后二者属于后续阶段。

## 偏差与决策

OpenVDB 解码树预算为解码后检查，内部临时分配没有硬限额；保留该限制，不声称整个进程峰值受控。
Scene/Project 增加严格 kind，字段结构不变；旧 reader 未知种类拒绝，未做空转版本升级。
meta/current 沿用分步提交，current 写失败保持旧 current，但可留下已提交 meta 和完整孤立产物，错误 context 显式报告。
GPU 额度按每个缓存实例计，不是跨资产、卸载重建及外部所有者的全局显存配额；同一 CPU 网格快照可复用 GPU buffer。

## 遗留问题与下一步

M23.5 完成；推进 M23.6 的光栅侧服务设置、Editor/拾取/Gizmo/独立 capture 与使用示例。
原生 GPU 路径追踪和 intersection/Slang 门禁仍待 M23.8/9；大规模性能/峰值基线归 M23.7。

## 修改记录

- 2026-11-11T19:45:00+08:00：先定义源/产物/缓存、严格 Nano 读取与状态边界。
- 2026-11-11T20:41:10+08:00：完成实现、CPU/真实 GPU/旧链路定向验收和文档同步。
