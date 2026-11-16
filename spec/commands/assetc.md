---
created_at: "2026-11-16T14:20:00+08:00"
updated_at: "2026-11-16T15:25:58+08:00"
---

# dk-assetc 网格体素化

独立 CPU 离线 CLI，不属于 Runtime Commands，不要求活动文档或 guard，不提供 Undo。
已有 import/cache/cache-clean 见[资产指南](../guides/assets.md)。

## voxelize

```text
dk-assetc voxelize --project-root ROOT --source REL --output NEW_REL_DIR [--resolution 128] [--unit-scale 1] [--colors texture|material]
```

要求构建启用 `DK_BUILD_VOXEL_VDB` 与现有 assetc/glTF 导入器；无 Vulkan、CUDA 或窗口依赖。
`--project-root`、`--source`、`--output` 必填。源为工程内 glTF/GLB，沿用单 mesh 局部坐标、UV0 和受控依赖读取限制；
node 放置不展开。`--unit-scale` 为有限正数，应用一次。`--resolution` 为最长轴格数 16–512，默认 128，保持立方体素。
`--colors texture` 默认烘焙基础颜色贴图与因子，线性 RGB 每通道 4 bit；material 只用因子，两者均采样 Alpha MASK。
基础颜色固定使用双线性采样和源 wrap；不声明保留 glTF min/mag/mipmap 过滤。
最近三角形的 unsigned distance 不超过 sqrt(3)/2 格即为表面壳，不填充封闭内部；薄片和邻近层会受分辨率影响。
Alpha BLEND 拒绝；不烘焙法线、发光和金属粗糙贴图，保留金属度/粗糙度因子。

输出目录必须尚不存在，父目录必须存在。先计算候选，在同父目录 staging 写入全部文件并完成 NanoVDB 编译验证，
最后 rename 发布。失败不改写源或已有输出；进程中断可能留下 `.vxl-*`，不承诺断电耐久性或并发隔离。
输出包含 grid.vdb、solid.dkvoxel、meta/缓存、project.json、scene.json 和 conversion.json。
JSON stdout 给出分阶段耗时、格数、调色板、树/NanoVDB 字节及诊断，错误走 stderr。

退出码 0 成功，1 转换/输出失败（包括非法分辨率、预算、已有输出、不支持数据），2 CLI 参数结构错误，3 基础设施失败。
默认限制一百万三角形/顶点、16 Mi 候选体素、256 MiB 树、8192 个调色板项、64 MiB VDB、2 MiB 描述。
树和候选数量在 OpenVDB 调用结束后检查，不能限制该调用的内部瞬时分配。

```powershell
$bin = "$PWD/out/build/windows-voxel-editor/bin/Release"
& "$bin/dk-assetc.exe" voxelize --project-root "$PWD" --source projects/demo/assets/gltf/Sponza/glTF/Sponza.gltf --output out/sponza-voxels --resolution 128 --unit-scale 0.008
& "$bin/dk-editor.exe" --root "$PWD/out/sponza-voxels" --manifest project.json
```

Hierarchy 选择实体后 Frame；使用与验证见[体素指南](../guides/voxels.md#gltf-网格体素化)、[0172](../development/0172-mesh-voxelization.md)。
