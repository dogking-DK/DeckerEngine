---
id: "0121"
created_at: "2026-10-16T09:19:01+08:00"
updated_at: "2026-10-16T09:34:39+08:00"
status: completed
design_refs:
  - ../design/editor.md
  - ../design/render-pathtracing.md
  - ../design/render-pipeline.md
  - ../design/render-clouds.md
---

# 0121 默认编辑器路径追踪与大气修复

依据[编辑器](../design/editor.md)、[路径追踪](../design/render-pathtracing.md)和[管线](../design/render-pipeline.md)契约，
修复M16.9/M14.6正常打开默认工程无法预览的问题。

默认Sponza有262267个三角形、103个primitive、25张base color纹理（约96MiB RGBA8），路径worker却采用严格导入和4096三角形/64MiB纹理上限；
大气接收体检查因部分几何低于地面而拒绝整帧。原测试仅使用小型、抬离地面的夹具，未覆盖默认启动。

## 实际变更

- [PathPreview](../../engine/editor/src/PathPreview.cpp)采用与光栅相同的预览导入，显式选择100万三角形、4096 primitive、64张纹理/128MiB源像素额度。
  CPU参考默认4096三角形/64MiB及逐射线工作额度仍独立保留；GPU实际分配仍限制1GiB，不改变capture的严格导入。
  材质仍支持base color/mask/双面和金属粗糙度常量，省略的纹理与切线在面板说明。
- 场景重建校验CPU候选后排空旧提交，退休旧history/GPU场景，再准备新场景，避免默认场景两份GPU资源叠加超额。
  GUI旧显示图仍可保留并标过期；失败后Retry重新构建，不恢复已退休的累积。
- [ScenePipeline](../../engine/render/pipeline/src/ScenePipeline.cpp)只在完整大气/云合成允许跨地面的包围盒；已有着色器按高度夹到地表并以行星交点遮挡地下几何。
  日光独立入口继续拒绝地下接收体。云影显式ground_occluded参数只放宽地下角点，保留云底上界、原投影UV、世界坐标等检查。
- 视口增加Settings入口，方便大气开启时找到surface-only确认。内部`--desktop-render-smoke`使用正常相机、窗口与生产if_available选卡，
  显式检查RT可用并通过真实ImGui输入操作默认工程副本。记录路径、大气、带云预设和返回路径的实际像素/版本。
  [CPU边界](../../tests/unit/PathTraceReferenceTests.cpp)、[地面数值](../../tests/integration/AerialPerspectiveProbe.cpp)、
  [默认工程回归](../../tests/integration/EditorDesktopRenderTest.py)补上原夹具缺失的范围。

## 验证

Windows/windows-editor/Release，沿用现有依赖与工具链；测试显式使用`--background`、Khronos同步validation，
验证子进程临时设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`避开已知旧AMD隐式层，不改变普通启动环境。
所有构建/测试通过`scripts/verify.ps1`记录，以下目录相对out/verify。

| 证据目录 | 结果 |
| --- | --- |
| 20261016-092238-6e9d7eca | 沙箱拒绝访问外部vcpkg，构建失败，未运行测试；获得工具授权后使用同一配置继续。 |
| 20261016-092254-df2cd094 | 2项CPU通过；默认工程首张路径和大气图成功，返回路径时双份GPU资源超过1GiB，GUI测试失败。 |
| 20261016-092513-1bf82ced | 退休旧GPU资源后3/3通过：CPU额度/旧边界和默认工程完整往返，零跳过。 |
| 20261016-092621-5d64ad4a | 5/5通过：环境worker、独立日光失败保护、大气地面数值、原环境GUI/capture和路径GUI/capture；零跳过。 |
| 20261016-092903-c5347e86 | 默认工程使用生产if_available选卡复验1/1通过，零跳过。 |
| 20261016-093101-96b7ad84 | 6/6通过：云影CPU投影/设置、GPU数值/表面/失败保护及默认工程四阶段往返，零跳过。 |
| 20261016-093324-3e160e6b | 最终2/2通过：按大气/云模块注册的新默认工程测试，以及原云GUI/capture一致性，零跳过。 |

地面数值用真空/零太阳与自发光平面跨地表，断言地上片段保留原HDR、地下片段严格为零；
默认工程已实际显示736×436路径图、736×405大气图，往返后重新得到完整样本，退出零validation错误/警告和存活分配。
带云预设完成32帧预热并发布有效图像，随后路径重建成功。GPU为NVIDIA GeForce RTX 4070 Laptop GPU/596.49。
累计14个独立CTest名称最终通过（不重复统计复验），0跳过；已查看真实工作台、大气和带云视口截图。
默认场景完整证据位于out/build/windows-editor/test-artifacts/Release/desktop-render-13b77aa4082e4776a27c9a38d8591e07，
含四阶段JSON/PPM、workbench.ppm和report.json。最后复验的新路径另见对应tests.log。
文档检查通过8份Markdown、204条本地链接及元数据/索引/测试入口/JSON；git diff --check通过。

最终实际命令（其他定向范围和target由上述各summary.json完整记录）：

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration Release `
  -Target @('dk_editor_app','dk_run','dk_ctl') `
  -TestRegex '^dk\.editor\.(desktop_render|cloud_consistency)_gpu_validation$' `
  -Reason 'Final conditional smoke registration and cloud GUI/capture parity with ground-clipped shadow validation'
```

## 范围与限制

本次不新增完整PBR纹理、体积路径追踪、去噪或默认Sponza的严格capture支持，不降低三角形/贴图质量以绕过预算。
保留1GiB实际预算，其他更大输入仍可能被拒绝。默认场景回归验证可用性及切换，不作为64spp质量/性能基线。
未运行全量、Debug/RelWithDebInfo或跨设备矩阵；本次更新Release二进制。
既有spec/README.md及迁移指南修改不属于本任务，不纳入提交。
