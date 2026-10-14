---
id: "0117"
created_at: "2026-10-14T18:13:17+08:00"
updated_at: "2026-10-14T18:31:22+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/graphics-resources.md
---

# 0117 M16.8 每视图渐进累积与额度

依据[渐进会话契约](../design/render-pathtracing.md#m168-渐进会话实施契约)，完成双视图、
GPU逐样本累积、分块、reset/poll、失败/取消保护及共享资源额度。

## 实际变更

- [PathTraceRenderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)新增configure_view、
  submit_batch、poll、reset/cancel/release_view、status/completed_frame与memory_stats。
  两视图共享一个渐进批次；tile串行，整批成功才发布。配置、GPU提交和完成发布是三个独立提交点。
  相机/场景版本/积分设置/session/gesture重置；曝光、调度及frame序号保留历史；降低target报告实际spp。
- [累积shader](../../engine/render/pathtracing/shaders/path-accumulate.slang)以全局pixel/sample计数续接Kahan sum和moment，
  保存整数count与首样本诊断，128字节/像素。每tile仅读回4字节错误标志；完整线性读回为显式异步请求。
  GPU completed owner可以直接供后续显示读取，未引入每批全帧CPU读回上传。
- [共享资源额度](../../engine/graphics/device/include/dk/graphics/Resources.hpp)按驱动memory requirements先预留，
  VMA失败回滚，实际资源owner销毁后归还；factory、AS storage/instances/scratch与SBT继承额度。
  在途工作与外部旧帧继续计费，report含owned当前/峰值/host/device字节及全设备VMA allocation/block。
- 逐tile工作上限2^24射线，整目标2^32，batch 1..16，tile 1..64按max_rays缩小；
  GPU相机上限2048，CPU仍64。GPU不以CPU暴力triangle_tests额度限制硬件遍历。
- [新增探针](../../tests/integration/PathProgressiveCases.hpp)与现有GPU探针/CMake、设计/指南/索引/Roadmap同步。
  无命令变更、依赖升级或Scene格式变化。

## 验证记录

windows-graphics/RelWithDebInfo、MSVC 14.51、RTX 4070 Laptop，required RT及Khronos validation/sync。
沿0114临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1并在finally恢复。

- out/verify/20261014-181830-a0c6d173：构建通过，最初测试筛选无匹配，未作为通过证据。
- 182115-c96a477c：5项失败，Graph拒绝status原子读写同时标记full overwrite；改为保留已初始化内容的读写访问。
- 182217-32116db5：修正后4组渐进和原analytic共5/5通过。
- 182445-48e3f179：完整定向范围47/47通过，0失败/0跳过：25项CPU、20组路径GPU、资源和RT底座各1组。
  22组GPU均errors=0、warnings=0、liveAllocations=0；路径探针结束也断言VMA allocationCount=0。
  完整LastTest.log复制到该证据目录，避免长输出被results.xml截断。
- 182948-088fb19c：最终审查禁止复制额度身份，并在完成发布前再次检查取消；重新构建后5组渐进及资源回归6/6通过，0失败/0跳过。此前其余检查未受影响。
- 文档检查9份Markdown、元数据/表格/索引/测试入口和本地链接通过；git diff --check通过。
- progressive_images覆盖19spp的1/3/7/16批量、非整除tile、Lambert/GGX及CPU小图，
  同设备分块/批次与一次GPU计算逐位相同；空天空65536spp连续累积相同；2048×2边界完整。
- state/failure/lost覆盖双视图上限、曝光/目标/暂停、相机/投影/尺寸/seed/session/gesture/资产更新、
  迟到reset/release同ID、后续tile注入提交失败与重试、timeout、两种stop与显式cancel、满命令槽poll、
  device loss终止不发布旧候选。合成device loss前先等待真实硬件空闲，避免测试破坏在途实际资源。
- budget覆盖1MiB准入、实际pending/外部owner寿命、AS scratch/upload回收、工作额度自动分块和超限拒绝。
  本用例观测owned current=298384、peak=885392、host-visible=256、device-local=298128字节；
  全设备VMA allocation=298656、block=100663296字节。它们仅代表此小夹具，不是生产显存/性能基线。

复现命令（GPU环境变量按上段临时设置/恢复）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe,dk_pathtracing_reference_tests,dk_graphics_resource_probe,dk_raytracing_probe `
  -TestRegex '^dk\.(pathtracing\.(.*_gpu_required|[a-z]+ |GGX CPU)|graphics\.gpu_resources_validation$|raytracing\.required_validation$)' `
  -Reason 'M16.8全路径积分回归与受影响资源/AS寿命验证'
```

## 限制与交接

默认1GiB冻结为VMA allocation准入上限，driver隐藏pipeline/descriptor开销不计入，VMA池block是全设备共享统计。
128字节历史使2048²单张占512MiB；双张加场景超过1GiB，最大尺寸不能保证续批或显式读回同时可用，超额明确拒绝。
本次验收2048×2边界，未声称2048²、1080p交互性能或跨设备逐位一致。保守以GPU场景owner身份重置，重新prepare相同内容也会重采样。
未运行全引擎/Debug双配置、Editor或capture服务测试；不属于本阶段。下一项M16.9接worker、tone/显示交接与UI。
原有spec/README.md及未跟踪文档迁移指南的工作区变更保留，不纳入本次提交。
