---
id: "0061"
created_at: "2026-10-03T15:06:44+08:00"
updated_at: "2026-10-03T15:34:09+08:00"
status: completed
design_refs:
  - ../design/render-data.md
  - ../design/render-resources.md
  - ../design/assets-importers.md
  - ../design/architecture.md
---

# 0061 M7.1 场景提取与 GPU 资源

## 目标与设计依据

按 [Render 数据](../design/render-data.md) 和 [GPU 资源](../design/render-resources.md)
完成 M7.1，只推进场景只读提取和 GPU 资产生命周期；绘制管线留在 M7.2。

## 实际变更

- [render/data](../../engine/render/data) 新增 dk::render_data：从 SceneSnapshot 迭代提取
  世界矩阵、稳定实体 ID 和资产引用，保留 revision，不持有 ECS 或源快照。
  RenderView 显式绑定相机、投影、像素尺寸、frame 和不可变场景版本。
- [render/resources](../../engine/render/resources) 新增 dk::render_resources：
  GpuAssets 对 CpuAsset 完整校验后构建 GpuMesh、sRGB 纹理/view/sampler 和材质副本。
  每 primitive 使用固定 32 字节顶点和 uint32 索引，缺失属性有默认值和存在标志。
- 上传通过 Graph external staging → copy Pass → final access 执行，无 Render 层直接 Vulkan API 调用。
  先准备所有候选/发布槽，成功提交后无分配地替换当前包；失败保留旧代际。
  明确 submitted 与完成的区别，GpuAsset::wait 使用所属队列票据。
- find 返回共享快照；unload/clear/缓存销毁撤销缓存引用，旧快照与 pending 提交独立保活。
  新上传不改写旧资源，允许 GPU 包装器晚于 queue 销毁。
- [asset_data](../../engine/assets/data/CMakeLists.txt) 可由 Render Resources 独立启用，
  不强制 IO/导入器；原 IO 前置检查移动至 [importers](../../engine/assets/importers/CMakeLists.txt)。
  两个新构建选项默认 OFF，windows-graphics 启用，无依赖版本/feature 变动。
- 新增 [RenderDataTests](../../tests/unit/RenderDataTests.cpp)、
  [GpuAssetTests](../../tests/unit/GpuAssetTests.cpp)、
  [RenderResourcesProbe](../../tests/integration/RenderResourcesProbe.cpp)，
  同步设计/开发索引、架构/资产边界、测试选择表、Roadmap、README 和 [使用指南](../guides/render.md)。

## 验证记录

Windows x64 Debug、VS 2026/MSVC、DK_WARNINGS_AS_ERRORS=ON。
`cmake --preset windows-graphics` 配置成功；最终依赖检查拆分后的配置输出保存在
`out/m71-configure.log`。未新增或升级三方依赖。

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_data_tests','dk_gpu_asset_tests') `
    -TestRegex '^dk\.render\.(data|assets)\.' `
    -Reason 'M7.1 immutable extraction, view validation and GPU asset CPU contracts'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target dk_render_resources_probe -TestRegex '^dk\.render\.gpu_validation$' `
    -Reason 'M7.1 Graph upload byte roundtrip, reload/unload, failure publication and GPU lifetime'
& ./scripts/check-spec.ps1
git diff --check
```

实际定向运行：

| 证据目录（out/verify 下） | 范围 | 结果 |
| --- | --- | --- |
| 20261003-151843-5983ef30 | 3 个 Render Data + 2 个 GPU Asset CPU 测试 | 5/5 通过，0 跳过 |
| 20261003-152213-827f952b | 首轮 GPU 同步验证 | 1/1 通过，0 跳过 |
| 20261003-152456-17c2c9f2 | 最终 GpuMesh 接口、2 个 Asset CPU 测试及 queue 析构寿命 | 3/3 通过，0 跳过 |
| 20261003-153042-608dbf2a | 加入无纹理程序化分支后的最终 GPU 探针 | 1/1 通过，0 跳过 |

最终两个跟进命令的 Target/TestRegex/Reason 记录在各 summary.json；源码未再改变的
Render Data CPU 结果沿用首轮，不将重跑累加为独立测试数量。本阶段覆盖 5 个 CPU 测试
（输入校验含 18 个拒绝场景）和 1 个 GPU 集成测试。

GPU 环境：NVIDIA GeForce RTX 4070 Laptop GPU，driver 596.49，Khronos required validation +
synchronization validation。沿用 [0048](0048-vulkan-14-baseline.md) 的进程级
`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1` 兼容处理，命令 finally 恢复原值，保留 Khronos 层。
最终输出：`mesh/texture roundtrips=3 pending=0 allocations=0`，
`errors=0 warnings=0 liveAllocations=0`。

验证内容：

- 场景完整仿射/剪切和零缩放、引用顺序、版本隔离、源对象释放、View 相机/投影/尺寸拒绝，
  Memory closing 只读和逐步预算释放后的失败/部分构造/成功回退。
- 多 primitive/texture 全字节读回、缺失属性、sRGB/采样器/材质及状态导出；
  后续读回直接排在上传之后，无预先 host wait；无纹理/无材质包也通过。
- 超时、异队列票据拒绝；重载保留旧字节；pending 卸载/输入释放/缓存销毁；
  queue.close 和实际 queue 析构后的快照寿命。
- 输入错误、对象创建失败、native submit 失败、缓存预算逐步失败，旧 find/代际不变，
  失败候选 GPU/Memory 无泄漏；提交钩子关闭缓存 heap 后首次插入仍能发布，证明提交后无需分配。
- 文档检查发现开发索引新行前空行和一处历史记录链接，均已修正；最终 125 篇 Markdown、1248 个本地链接、
  元数据/表格/索引/测试入口/JSON 检查通过，diff whitespace 检查通过。

未运行：全量回归、Release、窗口测试、CPU runner/资产磁盘管线回归、独立最小配置和其他 GPU/平台。
本轮未更改这些执行链路；模块独立性由实际 CMake 依赖表达，未宣称额外平台验收。

## 偏差与决策

- CPU 资产包是上传单位；不推测 scene material override，不引入磁盘加载或后台作业。
  CPU Ready 不等于 GPU 完成，调用者明确持有 GpuAsset 并等待其 ticket。
- GpuMesh 是借用视图，所有权归 GpuAsset；此细化已同步模块设计。
- 首版缓存线性查找、包内元数据二次去重校验；不做跨包去重、自动淘汰或 mipmap 生成。
  CPU Error 文本和外部库的进程级 OOM 不承诺完全可恢复。

## 遗留问题与下一步

M7.1 范围内无未解决阻塞。M7 仍进行中，下一项为 **M7.2 最小渲染管线**：
先设计 render-pipeline，再把 opaque/depth/tone mapping 接入 Graph，建立程序化场景图像回归。
M7.3 磁盘集成与 M7.4 capture 命令尚未实现。

## 修改记录

- 2026-10-03T15:06:44+08:00：创建设计和本记录。
- 2026-10-03T15:32:47+08:00：完成提取、GPU 上传缓存、失败/寿命验收和文档同步。
