---
id: "0118"
created_at: "2026-10-15T09:00:17+08:00"
updated_at: "2026-10-15T09:29:41+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/render-pathtracing.md
  - ../design/graphics-resources.md
---

# 0118 M16.9 编辑器路径追踪预览

依据[编辑器接入契约](../design/editor.md#m169-编辑器路径追踪接入契约)，完成后台渐进预览、
GPU显示/完成图像交接、显式模式和参数、共享队列同步以及真实GUI/IPC验收。

## 实际变更

- [PathPreview](../../engine/editor/src/PathPreview.cpp)拥有独立线程、同设备队列0视图、冻结DiskScene和renderer。
  一个待处理输入/一个进行中输入/一个待领取结果；新请求取消旧批次，轮询收完旧提交后重配，不阻塞GUI等待路径完成。
  会话/Refresh重载资产，revision/手势重建场景；相机/尺寸/积分设置重置，曝光/目标/暂停保留完成样本。
  配置成功后才记录Reset计数，避免被合并或失败输入吞掉；迟到结果只回收，空闲/暂停降低轮询频率。
- [PathViewport](../../engine/editor/src/PathViewport.cpp)直接import完成图像，在前次GUI提交结束后替换ImGui descriptor。
  [PathPanel](../../engine/editor/src/PathPanel.cpp)提供Raster/Path trace、目标/seed/反弹/环境/太阳/曝光、进度/暂停/重置，
  设置不进入Scene/Undo。旧图和旧图样本数保留但标记STALE，不能用于新输入拾取；相机/Gizmo继续走既有控件。
- [GPU tone](../../engine/render/pathtracing/shaders/pathtrace-tone.slang)从完成HDR历史生成RGBA8 UNORM，
  曝光为EV，Reinhard后按目标仅编码一次sRGB；显式存储格式和Graph color/mip/layer范围。
  正常显示没有全帧CPU读回/回传，display图像/参数继承renderer预算；只有验收显式读回签名。
- [Device](../../engine/graphics/device/include/dk/graphics/Device.hpp)共享按原生队列区分的gate，
  submit/present/queue-idle/Tracy校时遵守同一串行入口，各SubmissionQueue账本仍单线程独占。
  ImGui动态字体更新也含native submit/idle，GuiRenderer在gate内提前执行，避免隐藏的同队列竞争。
  AllocationBudget的预留/归还/统计加锁，支持导出图像在GUI线程最终释放。
- windows-editor默认启用路径模块；窗口请求RT和纹理索引if_available，无能力/无模块显示原因。
  空气/云需要Surface-only acknowledgement。最小化、隐藏、切Simulation暂停后续路径工作；
  正常或异常退出均先取消、排空并join worker，再释放显示/设备。
- CLI仅新增内部验收模式--pathtrace-smoke；未新增业务命令、Scene格式或依赖版本。
  设计/指南/索引/测试选择表/Roadmap已同步，M16.10/M16.11未推进。

## 验证记录

windows-editor/RelWithDebInfo、MSVC 14.51、RTX 4070 Laptop、required RT和Khronos同步validation。
GPU进程使用DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1避开已知旧AMD隐式层；仅作用验证shell子进程。

- out/verify/20261015-091001-344c2b4e、091318-905c0b5e、091416-ea310bb6：中途注册测试源、
  C++数组推导和readback头文件问题导致构建失败，测试未运行；修正后继续。
- 091508-110181fc、091615-21f982b8：显示shader缺显式RGBA8格式、Graph图像范围未声明，2项运行失败；
  补齐契约后GPU逐像素显示数值通过。
- 091707-8f30712e：14个GUI检查点和IPC均达成，但同步验证抓到ImGui字体上传的QueueWaitIdle竞争；
  此轮GUI不计通过。加入字体更新gate后091941-4629f8bf的2/2通过。
- 092120-d9a3c26e：定向10/10通过，0失败/0跳过，覆盖5组渐进、图像交接、2组资源和2组Editor。
  暂停后提高target到3而completed保持2；曝光保持generation，Reset清零，恢复同seed得到相同图像。
- 092356-162317df：显式关闭路径模块后Editor构建与光栅工作台1/1通过，未以关闭模块回避路径测试。
- 092558-e0722049：恢复windows-editor默认ON，最终worker命令槽/Reset提交点和界面元数据整理后，
  路径GUI与原交互2/2通过，0失败/0跳过；最终编辑器二进制包含路径模块。
- GUI检查点覆盖模式、暂停、曝光、Reset、恢复、RMB相机、真实Gizmo、IPC修改、resize、最小化恢复、
  Raster往返、保存/新DocumentId重载及64spp未完成时关闭。场景revision/Undo与渲染输入一致。
  结果与像素签名、IPC往返耗时、工作台PPM保存在test-artifacts/RelWithDebInfo/pathtrace-*中。
  GPU和GUI成功运行均零validation错误/警告、零存活Memory；GPU探针同时验证VMA资源释放。
- GPU显示对线性参考测试EV=-2/0/2及线性/sRGB两种目标，RGBA8误差不超过1级，不改变generation/spp。
  暂停/取消/重置/迟到/预算/设备丢失仍通过已有M16.8探针。
- check-spec通过11份Markdown、692条本地链接、元数据/索引/测试入口与JSON；git diff --check通过。
- 已查看实际工作台截图并修正窄面板环境颜色控件宽度、旧图样本计数和路径模式的draw标签。

复现主体范围：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_editor_app,dk_ctl,dk_run,dk_pathtracing_probe,dk_image_transfer_probe,dk_graphics_resource_probe `
  -TestRegex '^dk\.(pathtracing\.progressive_.*_gpu_required|graphics\.(image_transfer_validation|gpu_.*)|editor\.(pathtracing|interaction)_gpu_validation)$' `
  -Reason 'M16.9路径预览、共享队列与资源、原编辑器交互回归'
```

## 限制与交接

继承4096三角形、64纹理/源纹理64MiB、1GiB GPU分配额度；DiskScene为64资产/512MiB CPU额度，输入快照32MiB。
默认Sponza完整场景超出路径几何上限；应使用受支持的小场景。仅表面光照，没有路径体积云/空气、去噪或路径capture。
共享队列0已实际验证，不宣称GPU抢占、零主线程呈现/字体同步、跨设备结果逐位一致或实时性能达标。
未运行全引擎/Debug双配置、无RT实机或M16.11三轮性能基线；Tracy校时门锁路径未在profiling配置执行。
下一项M16.10添加capture、Job进度/取消与来源报告。原spec/README.md及未跟踪文档迁移指南保留，不纳入提交。
