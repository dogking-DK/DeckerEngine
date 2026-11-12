---
id: "0167"
created_at: "2026-11-12T12:32:27+08:00"
updated_at: "2026-11-12T13:30:34+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/render-pathtracing.md
---

# 0167 M23.9 原生稀疏体素 Classic 路径

## 目标与设计依据

实施 [M23.9](../roadmap.md#m23实体体素渲染) 与 [Classic 契约](../design/render-voxels.md#m239-classic-路径实施契约)。
复用现有积分器与共享 HDDA，使体素参与主射线、阴影与间接反弹；保持 M23.10/11/12 的后续边界。

## 状态与失败保护

CPU 场景为拥有型不可变快照；GPU 上传/BLAS/TLAS/映射先构建候选，成功 submit 后发布。
内容/材质/变换参与几何指纹与完整比较；失败保留旧场景及已完成累积，在途资源由提交保活。

## 实现

- `SparseGrid::surface_blocks` 枚举非零常量 tile 和非空 leaf；layout v1 保留 8/128/4096 尺度，按实心侧 cell 确定表面所属块，不展开大 tile。
- CPU Classic 消费冻结的网格、palette、entity/asset 变换。`PathTraceHit` 区分 triangle/voxel，位置/法线/材质适配继续使用原 NEE、MIS、RR、Lambert/GGX 与灯光分布。
- GPU 打包 Nano 子区间、实例、块和调色板；同一 CPU grid 的 Nano 数据共享上传，当前每个非空实例各建一个块 AABB BLAS，和 mesh 共同进入 TLAS。四个 hit groups 对应 triangle/voxel 的 radiance/visibility；20-byte procedural 属性携带 cell/face/value。
- intersection 直接复用共享 PNanoVDB/HDDA。内部块入口吸附和起点所有权跳过保持跨块连续表面；平行内部块面的射线按半开块归属。变换误差相关的 cell-space 偏移避免小尺度旋转实例自遮挡。
- 首命中用 signed 21-bit cell 保位编码，读回得到类型化命中；固定射线和渐进相机都覆盖。占用、palette、实例及布局变化更新几何身份，历史按既有 scene/view 生命周期重置。
- CPU、GPU 各入口明确拒绝未实现的 voxel ReSTIR/信号和 media 组合；失败不替换当前完整累积。保持无 CUDA、无新增/升级依赖，关闭 VDB 时仍可使用 mesh-only CPU 路径。
- 预算测试暴露 MSVC Debug allocator-only vector 构造的 `noexcept` 终止路径。场景的 texture/voxel 容器改用可传播异常的空 list 构造，palette/块使用带计数构造并归属 Memory；候选失败可正常返回并回收。

## 验证计划

CPU 独立外露面与三角化对照；required GPU 数值/统计、混合间接光、边界/自相交、渐进与失效、错误组合拒绝；旧 mesh 回归。

## 验证记录

专项测试：`dk_voxel_path_tests`（块枚举、独立表面、内容/能力与预算）；`dk_pathtracing_probe voxel_numeric/voxel_lighting/voxel_state`（required RT/validation/sync）。

独立参考枚举每个实心 cell 的外露面并三角化，不调用生产 sparse query、DDA/HDDA 或 block 枚举。覆盖负坐标、inactive tile、8/128/4096 tile/leaf 接缝、内部起点、区间与边角；GPU 另覆盖 1e-5/1/1000 尺度、负/非均匀缩放、旋转、剪切、多实例共享及独立 Nano 子区间。Lambert/GGX 混合两向间接光以 4096 spp 与三角化 CPU 均值/二阶矩对照，并验证 RR、环境/太阳/mesh 面积灯。

环境：Windows x64 / Debug，NVIDIA GeForce RTX 4070 Laptop GPU，Khronos validation 与 synchronization validation 必需；仅禁用无关 AMD implicit layer。各轮准确 target/regex、输出及失败信息保存在 `out/verify/*/summary.json`、`tests.log` 和 `results.xml`。

| 验证记录（out/verify） | 实际结果 |
| --- | --- |
| `20261112-125954-c27dccb6` | 初轮专项 6/6 通过：3 CPU、3 required GPU |
| `20261112-130113-1a549ae8` | 76 项扩展定向回归：73 通过、2 失败、1 超时；13 个 GPU 全通过。两个失败分别为 Debug 分配终止与旧容量测试溢出，见后续修复复验；单散射长测触及 240 秒限制 |
| `20261112-132216-05f00138` | 最终受影响范围 14/14 通过：新增 4 CPU 专项、既有分配/解析/尺度/间接光 4 项，以及 voxel 三项、mesh 解析/GGX/分配的 6 项 GPU；含内部块面平行射线、候选预算耗尽与恢复 |
| `20261112-132417-a294ca72` | 旧容量边界用例修正后 1/1 通过 |
| `20261112-132145-2bb90142` | VDB 关闭的独立 CPU 配置，最终构建及 5/5 定向兼容性检查通过 |
| `m239-volume-long` | 保持原有 16 组独立 N/4N、16384/65536 spp 和全部断言；独立运行 277.17 秒，277 条断言通过，exit 0；N/4N 标量相对差 0.000758338。未修改 CTest 时限或数值断言 |

最终 77 个不同 CTest 有通过证据（63 CPU、14 required GPU），另 1 个 CPU 长测独立运行通过，合计 78 项，无跳过；其中新 M23.9 4 CPU/3 GPU 均在最终代码通过。14 个 GPU 检查均为零验证错误/警告，资源关闭检查通过；旧 mesh 解析/GGX 四反弹与 RR、纹理 mask、ReSTIR、signals、空气、渐进图像，以及共享 compute 的 Slang/SPIR-V、数值和两投影离屏均通过。长测仍保留 CTest 超时记录，其通过证据来自不改变样本/断言的独立执行。

混合光照固定样本红通道示例（GPU / 独立三角化 CPU）：Lambert 首命中 voxel `0.259650 / 0.259650`、首命中 mesh `0.327103 / 0.327103`；GGX 对应 `0.184805 / 0.184805`、`0.271791 / 0.271779`。测试同时检查 RGB 均值/二阶矩、四反弹相对单反弹的额外贡献，以及 RR/阴影/面积灯。

主要复验命令：

```powershell
$env:VCPKG_ROOT = 'C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Target dk_voxel_path_tests,dk_pathtracing_probe -TestRegex '^(dk\.voxels\.path_cpu\.|dk\.pathtracing\.voxel_(numeric|lighting|state)_gpu_required$)' -Reason 'M23.9 native Classic acceptance' -Parallel 4
./scripts/verify.ps1 -BuildDir out/build/windows-path-media-cpu -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(allocation failures|black constant|empty degenerate|path rebind|scene snapshots) ' -Reason 'M23.9 optional VDB disabled compatibility' -Parallel 2
```

已定位并修复的迭代失败：voxel-only Graph copy 资源被裁剪、实例偏移被 float 往返吞掉、块入口舍入导致 HDDA 不前进、MSVC Debug 容器预算失败终止。随机表面参考排除恰落 float 三角化边界的起点，边/角/裁剪起点改用单独解析断言；没有用放宽照明容差掩盖这些问题。

工具/测试选择修正：首轮误写不存在的 target 导致构建未运行；第二轮 regex 过宽，在未进入 GPU 前停止，不计验收。测试中 Submission 比较和非默认构造容器的 count 构造各有一次编译失败，均修正后复验。关闭 VDB 的重复长时统计回归停止后，以 5 项直接兼容性用例复验，不计被停止轮为通过。
既有容量用例把 `SIZE_MAX + 1` 回绕成零后误当超上限；当前实现允许无纹理场景使用零纹理预算。修正为可表示的 triangle 硬上限 + 1，并增加零纹理预算合法断言，没有收紧生产接口来迁就测试。

文档检查通过：7 份 Markdown、744 条本地链接，以及元数据、表格、索引、测试入口和 JSON 清单；`git diff --check` 通过。

## 实际限制与下一步

本阶段只交付原生实体表面 Classic 路径。ReSTIR/信号身份、既有介质组合、Editor/capture 路径装配、联合性能交付分别留在 M23.10/11/12/13；M23 父阶段保持开放。
未运行 Release、跨厂商 GPU、上层 GUI/capture 或完整性能基准。GPU 数值域、region 工作和内存额度见设计，不宣称任意坐标/变换都可表达；块 BLAS 仍按实例创建，未实现 AS 增量更新或跨实例 BLAS 去重。
每个验收阶段独立本地提交；本次未尝试远端推送。

## 修改记录

- 2026-11-12T12:32:27+08:00：确定 block layout v1、共享命中/材质和提交边界。
- 2026-11-12T13:29:40+08:00：完成 Classic 混合几何、独立参考、真实 GPU/兼容性与失败保护验收，关闭 M23.9；下一阶段为 M23.10。
