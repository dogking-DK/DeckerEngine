---
id: "0064"
created_at: "2026-10-03T21:21:47+08:00"
updated_at: "2026-10-03T21:50:44+08:00"
status: completed
design_refs:
  - ../design/render-capture.md
  - ../design/runtime.md
  - ../design/application-services.md
  - ../design/render-disk.md
  - ../design/architecture.md
---

# 0064 M7.4 截图任务与自动化验收

## 目标与设计依据

按 [截图设计](../design/render-capture.md) 接入 render.capture 与 jobs 查询/等待/取消，
固定活动内存场景版本，完成无窗口自动化交付 B。

## 实际变更

- 新增 [CaptureService](../../engine/framework/services/include/dk/services/CaptureService.hpp)、
  [RenderOperations](../../engine/framework/operations/src/RenderOperations.cpp) 与可选 DK_BUILD_RENDER_CAPTURE。
  接受 guard/output/尺寸/相机/profile/validation，固定 document_id、scene_id、revision、会话帧号。
- SceneService::read_snapshot 复制活动内存场景与 Project 映射；DiskScene::load_snapshot 读取其资产并转发 stop_token，
  不重新加载磁盘 Scene。后续编辑与场景替换不改写已提交输入。
- 独立有界 worker 创建/回收 GPU，完成上传、绘制、等待、读回和 PPM 编码；owner 原子替换输出。
  接受取消则不发布，失败保留旧文件；关闭取消并 join。TaskId 与 JobId 的原有区分不变。
- [JobOperations](../../engine/framework/operations/src/JobOperations.cpp) 复用 jobs.get/wait/cancel 注册；
  GPU Runtime 按 JobId 路由资产和截图。wait/idle 同时泵两种完成，不重入 dispatch。
- 新增 [stdio 示例](../../examples/render/capture.ps1)、[命令参考](../commands/render.md) 和
  [进程验收](../../tests/integration/RuntimeCaptureTest.ps1)。默认项目 captures 生成目录被忽略。
  windows-dev 仍关闭 GPU，windows-graphics 条件启用截图，无新三方库或 baseline 变更。

## 验证记录

全部使用 Windows x64/MSVC Debug；定向运行，没有全量或 Release 矩阵。

1. `cmake --preset windows-graphics` 首次因 RenderServices 未显式 find_package(JSON) 生成失败；补齐查找后成功。
2. `verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_protocol_tests','dk_asset_command_tests','dk_render_disk_tests','dk_run') -TestRegex '^dk\.(protocol\.capture|render\.disk\.|asset_commands\.)'`：
   **11 passed / 0 failed / 0 skipped**，证据 `out/verify/20261003-213134-81946778`。
   包括3组截图CPU行为、5组DiskScene行为及3组资产命令回归；GENERATE 覆盖多种非法参数。
3. `verify.ps1 -BuildDir out/build/windows-graphics -Target dk_run -TestRegex '^dk\.runtime\.(capture_gpu_validation|assets_stdio)$'`：
   原资产 stdio 通过；截图测试脚本复用了会覆盖 project.json 的夹具准备语句，scene.load 提前失败。
   删除误覆盖，保留真实映射后只重跑 capture_gpu_validation：**1 passed / 0 failed / 0 skipped**，
   证据 `out/verify/20261003-213625-ec0c5af2`；原资产通过证据 `out/verify/20261003-213447-b618bd67`。
   新 runner 实际查询 commands.list/describe，导出 capture/jobs schema。
   同快照图像逐字节相同，未保存变换图像不同；确认 revision/frame、空闲发布、导入失败与输出锁定失败保护、取消、EOF/shutdown。
4. `verify.ps1 -BuildDir out/build/windows-dev -Target @('dk_run','dk_asset_command_tests') -TestRegex '^dk\.(asset_commands\.|runtime\.assets_stdio$|bootstrap\.version$)'`：
   CPU-only **5 passed / 0 failed / 0 skipped**，证据 `out/verify/20261003-213805-25add576`；
   CMakeCache 的全部 Render 选项 OFF，配置不选择 Vulkan/Slang 依赖。
5. `./examples/render/capture.ps1 -RequireValidation`：新命令通过默认 Sponza，960×540、103 draws、revision=1、frame=1。
   stdout 元数据保存 `out/m7-4-sponza-capture.json`；生成 `projects/demo/captures/sponza.ppm`。
   SHA256=`516C5FDEDF418ACBCE9A30F36B5267312F0356C06F4FB30010DAFC2344123474`，与 M7.3 独立示例产物完全相同。
   检查尺寸/像素范围，并转换 `out/m7-4-sponza-capture.png` 人工查看；PNG 只是验收预览，不是 Runtime 新格式。

GPU 使用 NVIDIA RTX 4070 Laptop、driver596.49/Vulkan1.4.329，Khronos validation required，stderr 无 warning/error。
沿用进程局部 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，finally 恢复；未关闭 Khronos 或同步验证。
文档检查最初发现索引新增行被空行隔开，修正后 `scripts/check-spec.ps1` 通过：132 Markdown、1332 本地链接，含元数据/表格/索引/测试入口/JSON；`git diff --check` 通过。
最终 runner 帮助标题去掉 CPU 限定后重新构建，bootstrap.version **1 passed**，证据 `out/verify/20261003-214828-5aece9bf`。
通过两配置真实 runner 的 commands.list 与 runtime.capabilities/commands.describe，确认 graphics=34 条且 render_capture=true、CPU-only=33 条且 false；
导出到 `out/m7-4-discovery-windows-graphics.json` / `out/m7-4-discovery-windows-dev.json`。

## 偏差与决策

复用既有 jobs.*，没有新增语义重复的 render.status/wait；截图结果用 kind="capture" 区分。
不扩展 schema 的 oneOf 子集，采用可选字段并集并在文档明确完整返回变体。
独立 worker 每次创建 Device/队列/管线，避免跨线程共享 GPU 及与资产队列的持久化相互阻塞。

## 遗留问题与下一步

下一项 M8.1 编辑器工作台。本阶段只支持 PPM、无光照 opaque/mask；不支持 PNG 输出、PBR、alpha blend、HDR/天空盒。
每个作业独立重建 GPU/管线，未实现设备与 GPU 缓存的跨截图复用；优先保持线程归属和退出正确。
资产文件读取不是提交时的字节快照，不保证外部修改隔离；输出原子替换不承诺 OOM/进程中断跨资源事务。
Linux/macOS、无设备机器 skip 分支和实际 device-lost 驱动故障未运行。

## 修改记录

- 2026-10-03T21:21:47+08:00：创建记录与设计。

- 2026-10-03T21:50:44+08:00：完成实现、验收、文档与 Roadmap；准备独立本地提交。
