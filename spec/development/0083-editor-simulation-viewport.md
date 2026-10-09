---
id: "0083"
created_at: "2026-10-09T20:17:00+08:00"
updated_at: "2026-10-09T20:52:43+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/editor-interaction.md
  - ../design/physics-api.md
  - ../design/render-simulation.md
  - ../design/graphics-resources.md
---

# 0083 M12.2 实时模拟视口与取景

## 目标与方案

接入CPU/GPU实时布片、独立模拟相机与自动取景，保持暂停稳定及缩放/最小化/Stop/关闭资源安全。
设计先更新[编辑器](../design/editor.md)、[交互](../design/editor-interaction.md)、
[服务](../design/physics-api.md)、[可视化](../design/render-simulation.md)与[资源交接](../design/graphics-resources.md)。
后台在批次边界响应有界预览请求，GPU图像直接交给GUI采样；无默认粒子/图像读回，不改变严格步数。
保留用户原有spec/README.md及spec/guides/ai-documentation-workflow.md，不纳入本阶段提交。

## 验证计划

使用现有windows-editor/RelWithDebInfo；资源交接定向测试、有限任务预览边界、真实CPU/GPU GUI相机/生命周期，
并回归原编辑视图交互。只运行目标及直接受影响链路，不重跑性能矩阵或全量/双配置。
记录真实截图、同步验证和文档检查；失败定位后仅复跑受影响项。

## 实施与结果

- Graphics增加单次ImageTransfer：生产队列确认图像完成并封存，消费队列核对设备/族后建立本地ledger，
  提交显式等待保活的源timeline；生产QueueState可先销毁。失败不消费transfer或发布资源状态。
- ClothRenderer增加CPU位置Graph上传/绘制；GPU零步图直接读现有求解结果。图像默认无readback。
- AsyncSimulation增加最新视图/结果邮箱、约30Hz采样与250ms请求租期，暂停时按步数/视图失效；
  预览错误独立于模拟fault。Runtime/Workspace仅转发带run_id的宿主预览接口，不新增JSON命令。
- Viewport直接导入后台图像并由ImGui采样，核对run/视图序号；独立模拟相机支持自动取景及导航，
  Edit/Simulation切换、Stop恢复与描述符/图像释放沿用GUI提交完成边界。
- 新增图像交接真实GPU探针、预览合并/暂停/错误隔离任务测试；扩展现有窗口smoke验证CPU/GPU连续图像、
  暂停稳定、相机、缩放/最小化恢复、Stop/重启及活动预览关闭。像素读取仅为显式测试诊断。

首次定向verify：out/verify/20261009-202715-2b162267，CMake因新增源文件自动配置时无权访问工作区外
既有vcpkg目录，构建/测试均未执行。使用相同范围授权重试20261009-202735-0382ffe6：
构建成功，17/17通过、0失败/跳过，测试耗时13.85秒。命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target @('dk_editor_app','dk_simulation_tests','dk_image_transfer_probe','dk_run') `
  -TestRegex '^dk\.(simulation\.finite task |graphics\.image_transfer_validation$|editor\.(simulation_gpu_validation|interaction_gpu_validation)$)' `
  -Reason 'M12.2 有界预览、图像交接及真实视口/编辑交互'
```

14个有限任务边界测试通过，包括预览合并、精确步数和显示错误隔离；图像交接探针通过三轮跨队列像素比较、
非法/重复/异设备拒绝、消费提交失败、生产队列提前销毁和在途释放。真实模拟窗口完成CPU 21阶段、GPU 25阶段，
最后保持GPU实验运行并关闭；原编辑交互22阶段及保存后runner重载通过。GPU同步验证0错误，
仅已知精确AMD Loader Message单列；图像探针live=0，两项编辑器liveAllocations=0。

实际截图：out/build/windows-editor/test-artifacts/RelWithDebInfo/
editor-simulation-e4f851c3a78645bb89df5e5de3f66fd8/simulation-panel.ppm；无损转PNG后已查看布片、地面、
相机控件、步数和左右面板均可见。最终审查将预览错误放在图像上方，并使Retry保留旧图；
GPU探针仅在设备/能力不可用时返回跳过，其他创建设备错误返回失败。

- 20261009-203250-f91dabaa：只重建dk_editor_app/dk_image_transfer_probe，筛选图像交接及模拟窗口，2/2通过。
- 20261009-203541-403a0098：补齐公开SimulationPreview头文件的条件PUBLIC graphics_device依赖，
  只构建dk_editor_app并验证simulation_gpu_validation，1/1通过；不改变CPU-only条件或依赖版本。
- 最终调度审查发现持续改变相机视图也必须受运行时33ms预览间隔限制，否则连续视图更新可能挤占求解批次。
  统一视图/步数更新的运行时频率上限，新增后端主动连续换视图仍能完成精确16步的回归。
  20261009-203821-b8fed8ea：dk_editor_app/dk_simulation_tests，
  `^dk\.(simulation\.finite task preview |editor\.simulation_gpu_validation$)`，3/3通过，0失败/跳过。
  其余代码未受影响，保留已有结果；共18个不同的定向检查取得通过结果，没有将重复运行累加为覆盖数。

文档初查9个Markdown、137个本地链接通过；最终Roadmap/状态更新后10个Markdown、356个本地链接及
元数据/表格/索引/测试入口/JSON清单通过，git diff --check通过。
最终窗口证据位于editor-simulation-2406c50b1a424ad38c58a73b05fdbae7/simulation-panel.ppm
（同上述test-artifacts/RelWithDebInfo目录），0验证错误、0存活分配。
M12.2完成，M12保持进行中，下一项M12.3。

## 限制与未运行项

只验收本机Windows、windows-editor/RelWithDebInfo；未执行全量、独立CPU-only配置或M11性能/响应矩阵。
有限任务仍按计算能力推进，预览有界采样不等于墙钟限速。实时预览需要共享设备双队列；
没有增加通用多队列调度或跨队列族转移，不承诺跨设备固定帧率。
M12.3指标采样/导出UI、粒子拖拽和模拟结果写回编辑场景均不属于本阶段。
