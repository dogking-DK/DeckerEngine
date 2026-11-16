---
id: "0172"
created_at: "2026-11-16T13:58:36+08:00"
updated_at: "2026-11-16T15:25:58+08:00"
status: completed
design_refs:
  - ../design/assets-importers.md
  - ../design/render-voxels.md
---

# 0172 OpenVDB 网格体素化与 Sponza 性能验证

## 目标与设计依据

按用户要求先使用 OpenVDB 接口实现 Sponza/glTF 网格体素化，再测量转换和真实 GPU 性能；依据 [assets-importers](../design/assets-importers.md)、[render-voxels](../design/render-voxels.md)。M23.1–13 保留原关闭状态，本项是后续资产制作扩展。

## 实际变更

- `MeshVoxelizer` 复用 OpenVDB 12.0.1 `meshToVolume<FloatGrid>` 无符号距离和最近面索引，把单 mesh 局部三角形变成有厚度的表面壳；保持开放薄片和建筑空腔，不填实内部。
- 按 UV0 双线性采样基础颜色、sRGB 转线性、乘因子；MASK 过滤，BLEND 明确拒绝；线性 RGB 每通道 4 bit 调色板，保留金属/粗糙因子。
- `dk-assetc voxelize` 输出新目录：VDB、描述、meta/NanoVDB 缓存、project/scene 和统计。候选在唯一短 sibling staging 内完成并编译验证，rename 发布；失败只清理自己创建的 staging，不改源或已有输出。Windows 临时 sharing 拒绝仅进行有界 20×25 ms 重试。
- CPU 接口只返回拥有型字节/JSON，采用现有无 CUDA 依赖；MSVC `/bigobj` 解决新增 OpenVDB 模板在 Debug 的节数限制。
- 增加四项 CPU 测试和 CLI 进程测试；文件基准复用生产光栅/原生路径，新增三分辨率三轮脚本、CPU 分段耗时/进程峰值、GPU 上传/AS/帧时间与释放检查。
- 真实 256 Sponza 暴露共享 HDDA 两个数值推进问题：同 t 跨轴反复丢失吸附平面，以及独立除法把仍前向的平面舍入到前一 t。保留已吸附坐标；仅当前向坐标证明仍前进时，按当前可表示 t 跨面。保留预算、后退错误和诊断，无任意 epsilon。

## 验证记录

[性能报告与归档](../benchmarks/2026-11-16-sponza-voxels.md)保存最终三轮及 validation，早期失败测量不混入最终数据。Windows x64 / MSVC Release / RTX 4070 Laptop GPU；后台 Editor。

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target @('dk_assetc','dk_voxel_asset_tests') -TestRegex '^dk\.(voxels\.assets\.mesh voxelizer|assetc\.)' -Reason 'CPU voxelization and file publication contracts'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target @('dk_voxel_joint_benchmark','dk_voxel_gpu_probe','dk_pathtracing_probe','dk_editor_app','dk_run','dk_ctl','dk_voxel_asset_process_probe') -TestRegex '^dk\.(voxels\.(gpu_numeric_required|path_consumers_gpu_required)|pathtracing\.voxel_(numeric|lighting|state)_gpu_required)$' -Reason 'Forward-plane regression and all affected GPU consumers'
python scripts/benchmark-mesh-voxels.py --assetc out/build/windows-voxel-editor/bin/Release/dk-assetc.exe --benchmark out/build/windows-voxel-editor/bin/Release/dk-voxel-joint-benchmark.exe --output out/sponza-voxel-benchmark-verified
```

通过的证据：

- `20261116-143137-07723e22`：4 项 mesh voxelizer 与 `dk.assetc.voxelize_process` 通过；该 scope 的旧 mesh GUI 失败归 [0173](0173-editor-view-settings.md) 后续修复。旧 `dk.assetc.process` 在 `20261116-141910-023ac2f3` 中通过。
- `20261116-151444-9919a3fe`：上述最终 GPU scope 全部 5 项通过，包含 compute 的 6144 条独立参考射线、1296 读值，以及原生数值/光照/状态、GUI/capture 四 DI 和介质消费端。
- 两个最小解析盒回归分别在去掉对应修复时复现 status 3 / 4；恢复最终源码后均通过。构造的大偏移等时刻射线会被 CPU DDA 数值保护拒绝，测试独立采用解析单位盒交点，不以被拒绝的 CPU 查询作为通过参考。
- Sponza 64/128/256 各 3 个独立 Release 转换及 GPU 进程；每个覆盖 640×480、1280×720 与透视/正交，32 spp 预热、每视图 11 spp，最终统计各含 24 个稳态样本。128 短测及 256 完整长测另开 required validation，通过且 0 错误/警告。
- 无 source 修改、同分辨率三轮 NanoVDB 内容一致、重复光栅图像稳定、路径输出有限、GPU/CPU owned 资源回收均通过。

实际失败与处理：Debug 初编译 C1128 → `/bigobj`；输出 staging/cache 路径过长与临时文件占用 → 短独占目录与有界重试；Lambert 文件基准拒绝金属材质 → 按生成调色板采用 metallic_roughness；旧 HDDA 两类错误 → 最小失败回归与最终 GPU 验收。初次不同 scope 的路径截图 sharing、设备失败和 temporal 图像不一致不计通过；checkpoint 仅重试 PermissionError，完整重建所有消费端后最终同一判据通过。

文档定向检查通过：14 个 Markdown、1177 个本地链接、元数据/表格/索引/测试入口/JSON 清单；`git diff --check` 通过。未运行全仓回归。

## 偏差与决策

依赖版本/baseline 未变；无 CUDA、mesh 三角化渲染替代或新的 Runtime Command。性能发现的共享 HDDA 缺陷必须修复后才能交付文件输入，因此纳入本记录。UI 布局单独记录/提交 [0173](0173-editor-view-settings.md)。

## 遗留问题与下一步

本项完成。当前沿用单 mesh 局部坐标，不展开 glTF node 层级；不烘焙 normal/emissive/metallic-roughness 贴图。最近三角形的 MASK 重叠与薄层是有限分辨率近似；未实现封闭实体填充、LOD/流式、GPU 体素化或 GUI 一键转换。16–512 是允许范围，本次真实 Sponza 性能只覆盖 64/128/256。
OpenVDB 树/候选预算在调用后检查，进程峰值采样不是 scratch 硬上限。仅本机 Windows GPU 验收；不宣称其他平台、任意 glTF 场景或全仓测试已通过。

## 修改记录

- 2026-11-16T13:58:36+08:00：创建设计与记录。
- 2026-11-16T15:22:33+08:00：完成转换/CLI/HDDA 修复、定向验收及真实 Sponza 性能交付。
