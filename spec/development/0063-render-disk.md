---
id: "0063"
created_at: "2026-10-03T17:17:15+08:00"
updated_at: "2026-10-03T17:37:32+08:00"
status: completed
design_refs:
  - ../design/render-disk.md
  - ../design/render-pipeline.md
  - ../design/assets-importers.md
  - ../design/render-resources.md
  - ../design/architecture.md
---

# 0063 M7.3 磁盘场景与资产集成

## 目标与设计依据

按 [render-disk](../design/render-disk.md) 接通 M2 Scene、M4 glTF/CPU artifact 与 M7 离屏管线。
默认测试输入为用户指定 Sponza，明确预览省略的光照信息并保留 alpha mask。

## 实际变更

- 新增 [render/disk](../../engine/render/disk) 与 DK_BUILD_RENDER_DISK。DiskScene 只读加载
  M2 Project/Scene，按引用去重读取 glTF 或 M4 CPU artifact，检查 mesh 身份、meta、载荷预算和可绘制材质。
  返回独立 CPU 快照；upload 新建候选缓存，逐包等待完成后返回，失败保留调用者原缓存与旧帧。
- GltfImportRequest 增加显式 unlit_preview。严格导入/编译/缓存默认值不变；预览校验但省略
  TANGENT 与不参与无光照计算的纹理，并返回诊断，仍拒绝 emissiveTexture/未知属性/扩展等。
- ScenePipeline 增加 mask depth fragment，与颜色 fragment 共用同一 alpha 采样/factor/cutoff；
  opaque 保留仅顶点深度管线，blend 继续拒绝。Graph 深度 Pass 增加纹理声明。
- 新增 [dk-render-demo](../../examples/render)、默认 [Project](../../projects/demo/project.json)
  和 [Sponza Scene](../../projects/demo/scenes/sponza.scene.json)，按源唯一节点保留 0.008 缩放。
  示例使用固定透视相机、无光照预览，等待帧完成后原子写 PPM 和独立图计划。
- 新增自制 [磁盘夹具](../../tests/fixtures/render-disk/README.md)、CPU/真实 GPU 回归，
  更新构建/Render 指南、模块设计、测试入口、默认素材说明及 Roadmap，无新增依赖或 Runtime 命令。
- 工作区原有 GraphExecution.hpp 的 clear_depth 声明删除与现有实现不匹配；本阶段恢复该必要声明。
  该文件最终与 HEAD 相同，不改变 M7.2 已实现行为。

## 验证记录

环境：Windows、VS 2026/MSVC、windows-graphics Debug，NVIDIA GeForce RTX 4070 Laptop GPU，驱动 596.49。
GPU 三个探针均要求 Khronos validation 和同步验证，仅进程内设置
DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1 兼容旧 AMD 隐式层，结束后恢复环境。

实际配置及定向命令：

```powershell
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_disk_tests','dk_import_tests','dk_render_pipeline_probe','dk_render_disk_probe','dk_render_demo') `
    -TestRegex '^dk\.(render\.disk\.|import\.)' `
    -Reason 'M7.3 disk candidates, persistent identity and importer profile contracts; build GPU fixture and Sponza example'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_render_pipeline_probe','dk_render_disk_probe','dk_render_demo') `
    -TestRegex '^dk\.render\.(pipeline_gpu_validation|disk_gpu_validation|sponza_validation)$' `
    -Reason 'M7.3 alpha mask GPU pixels, disk and CPU artifact parity, failure lifetime and local Sponza image'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
    -Target @('dk_asset_pipeline_tests','dk_asset_cache_tests') `
    -TestRegex '^dk\.(pipeline\.compiler publishes portable CPU artifact|cache\.cache hit skips importing)' `
    -Reason 'GltfImportRequest profile defaults must preserve strict compiler identity and existing cache hit behavior'
```

- 配置通过：out/m73-configure.log。
- 初次 out/verify/20261003-172329-fdb4c2c0 构建失败：新空对象 CPU 测试错误地尝试默认构造
  SubmissionQueue；移除该无效构造，将空 DiskScene.upload 拒绝检查放入 GPU 探针。失败时未运行测试。
- out/verify/20261003-172755-7b9fce77：15/15 CPU 测试通过，0 跳过。
  其中 4 个磁盘用例覆盖多实例去重、父级变换、保存/重载/旧快照、meta 单位缩放与身份、M4 artifact、
  缺图/坏 glTF/坏 meta/错误身份/坏摘要/未恢复操作/blend、数量/载荷限制及 closing。
  11 个导入用例验证严格路径保持拒绝行为，预览输出明确诊断，切线数量非法与 emissiveTexture 仍拒绝。
- out/verify/20261003-173123-4ef2d561：3/3 GPU 测试通过，0 跳过。
  磁盘夹具核对 mask 孔洞透出后方蓝色、factor alpha 与 cutoff 相等时保留、实例、M4 artifact 与直接
  导入图像完全一致；M2 修改保存/重载后 revision/图像更新，旧帧保持；第二包 submit 失败不返回
  部分缓存、第一包提交已完成且所有候选回收；在途卸载和帧释放最终 VMA/Memory 归零。
  原 M7.2 五组图像/预算/提交/关闭回归通过；blend 拒绝取代原 mask 拒绝。
  默认 Sponza 实际导入 192496 顶点、786801 索引的源网格，输出 103 primitives/draws、25 纹理，
  SceneId=9c44d3b5-254e-49b1-b29c-d29ca7ba038b、revision=1、frame=1、960x540。
  三个探针均 errors=0、warnings=0、liveAllocations=0。
- out/verify/20261003-173328-b7267665：严格 compiler 身份重导入、cache hit 两个直接回归 2/2 通过。
  合计 17 项 CPU + 3 项 GPU 通过，无失败/跳过。
- 默认输出 out/build/windows-graphics/tests/integration/render-sponza.ppm 和 render-sponza.txt；
  转换同目录 render-sponza.png 做可视检查，确认庭院、布帘、植被镂空和纹理朝向。
  与 out/default-test-assets-copy.json 再次逐文件 SHA-256 比对，82 个默认素材未被导入/渲染修改。
- scripts/check-spec.ps1 通过：129 Markdown、1299 本地链接及元数据/表格/索引/测试入口/JSON 检查；
  git diff --check 通过。

未运行：Release、全量/独立 CPU-only/窗口/跨 GPU/性能验收；其他 Runtime 功能没有新依赖。
PPM 与图计划各自原子发布，二者不是跨文件事务；本阶段不提供 capture 文件终态语义。

## 偏差与决策

首版同步磁盘加载/资产上传准备，异步帧渲染；capture Jobs 留在 M7.4。
显式 unlit_preview 只适用于直接候选，不改变严格编译器/缓存策略；HDR/PBR/天空盒尚未接入。

## 遗留问题与下一步

本阶段已完成；下一项 M7.4 截图任务与自动化验收。
限制：预览省略的光照输入有明确诊断；多 mesh/复杂 glTF、PBR、HDR/天空盒、透明混合仍未支持。
磁盘输入不承诺跨文件并发一致性，累计载荷上限不包含解析/元数据临时内存，也不扩大既有任意 OOM 保证。

## 修改记录

- 2026-10-03T17:17:15+08:00：创建专项设计与记录。
- 2026-10-03T17:37:32+08:00：实现、默认 Sponza 图像及定向失败/兼容性验收完成。
