---
id: "0114"
created_at: "2026-10-14T14:15:00+08:00"
updated_at: "2026-10-14T16:20:00+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/architecture.md
---

# 0114 M16.6 GPU无历史Lambert路径追踪

按[M16.6契约](../design/render-pathtracing.md#m166-gpu无历史lambert实施契约)新增独立GPU renderer，
复用CPU不可变输入与冻结模型，经Graph构建AS、trace并读回HDR/诊断。先共享纯输入校验，再实现GPU表与shader、
异步frame/完成发布，最后做required RT数值/统计及失败寿命验收；不接入材质扩展、历史累积或编辑器。

## 实际变更

- [GPU公开接口](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)提供PathTraceRenderer、
  PathTraceGpuScene、PathTraceFrame和不可变PathTraceGpuImage。新增默认OFF的DK_BUILD_RENDER_PATHTRACING，
  PUBLIC依赖reference/Device、PRIVATE依赖Graph/Shader，自动提供reference与必要纯数据target。
  未改三方库baseline、Runtime命令或Scene持久化格式。
- [renderer](../../engine/render/pathtracing/src/PathTraceRenderer.cpp)从冻结CPU世界三角形生成顶点、平面法线、
  Lambert/发光和光源CDF表。单BLAS/恒等TLAS经Graph upload→BLAS→TLAS提交，空场景不建立非法AS。
  float退化、非有限参数和无法表示的正面积/权重在提交前拒绝；source保留来源映射和指纹，资产变化不覆写旧代。
- [raygen/hit/miss/any-hit](../../engine/render/pathtracing/shaders/pathtrace.slang)实现CPU v1计数样本、
  主射线与visibility两类静态SBT、多次Lambert散射、三类光源NEE、面积到立体角PDF、power MIS和RR。
  单/双面和负缩放按世界绕序，次级射线不继承相机far；位置误差尺度偏移及双端阴影偏移，禁止亮点截断。
  主射线按缩放向量计算长度，CPU提前检查float裁剪区间。补偿求和输出均值/二阶矩、射线数和首样本命中诊断。
  [空场景compute](../../engine/render/pathtracing/shaders/pathtrace-empty.slang)只输出常量环境，沿用Graph及读回契约。
- prepare_scene/submit_view/submit_rays不隐式等待，Graph submit成功后仅移动预分配owner/票据。
  frame.read使用零超时完成查询，通过后完整校验并发布CPU图像；未完成返回空optional，取消/分配/非法数值不返回半帧。
  pending保活AS、管线、表和输出；外部提前释放及timeout不取消GPU工作。CPU参考共享纯输入校验，算法未改。
- [required探针](../../tests/integration/PathTraceProbe.cpp)新增五组测试；同步CMake/测试选择表、
  [使用指南](../guides/pathtracing-gpu.md)、设计/架构/入口和Roadmap。

## 验证与证据

环境：现有windows-graphics/RelWithDebInfo，MSVC 14.51，NVIDIA GeForce RTX 4070 Laptop GPU。
`cmake --preset windows-graphics -DDK_BUILD_RENDER_PATHTRACING=ON`通过，vcpkg复用既有固定版本，无依赖升级。
GPU沿用0110–0113核验的旧AMD隐式层处理：验证进程临时设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，
finally恢复；未修改持久环境或注册表。Khronos validation必须启用，CTest设置同步验证，两者缺失均失败，不计77跳过。
日志位于out/verify各目录的summary.json、build.log、tests.log、results.xml。

| 运行编号（20261014） | 范围 | 结果 |
| --- | --- | --- |
| 142218-ba530b66 | 新GPU模块/探针和CPU测试构建 | 测试局部变量遮蔽触发/WX，构建失败；修复命名，未运行旧二进制 |
| 142336-76c80f00 | 3项CPU参考、GPU解析/几何 | 3通过、1失败；GPU数值/同步和零残留分配已通过，但夹具ExecutionScope尚未退出，Memory关闭断言失败；调整作用域 |
| 142548-374cc52c | 全部16项CPU参考、4组GPU | 20通过、0失败、0跳过；GPU错误/警告/残留分配均0 |
| 143200-2ce0d81f | 补充float准入和Memory失败 | 探针丢弃nodiscard结果触发/WX，构建失败；显式处理后重跑 |
| 143410-9b6cf10e | 5组GPU含跨尺度阴影/分配恢复 | 4通过、1失败；病态相机已被RenderView提前拒绝，修正测试对准入层级的预期 |
| 143606-aebb24a5 | 相机缩放计算、图像/寿命/Memory恢复 | 3通过、0失败、0跳过；GPU诊断和残留分配均0 |
| 143843-b7b58d94 | HDR报告区分图像与统计协议 | images用例单独重跑，1通过、0失败、0跳过；GPU诊断和残留分配均0 |

增量共有21个不同CTest条目通过（16 CPU+5 required GPU），不将重复运行相加。
后续仅重跑受修改影响的GPU组，未固定全量/双配置回归。

主要复现命令（GPU部分在上述临时环境内执行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe,dk_pathtracing_reference_tests `
  -TestRegex '^dk\.pathtracing\.(.* |.*_gpu_required$)' -Reason 'M16.6 GPU验收与CPU冻结模型回归'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\..*_gpu_required$' `
  -Reason 'M16.6 float准入、跨尺度阴影、Memory失败与恢复'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(images|lifetime|allocations)_gpu_required$' `
  -Reason 'M16.6 最终相机与发布失败保护'
```

冻结门槛实测，统计组均为8个seed（101+7919*i）：

| 检查 | 结果 |
| --- | --- |
| 黑场/背景/发光/BSDF白炉/太阳与遮挡 | 确定值绝对误差≤1e-4；空场景无AS，secondary不继承camera far |
| 几何与跨尺度阴影 | 镜像/非均匀缩放/剪切，1e-6/1/1e4尺度；非边界命中来源、距离、重心/法线及面积灯能量通过 |
| 面积灯独立二维积分 | 参考0.28734861；16384spp GPU均值0.287082002，65536spp为0.287229713，N/4N相差约0.0514%＜0.5% |
| 面积灯CPU/GPU | N与4N的nRMSE约1.80e-6/2.56e-6；BSDF-only均值0.288179584；均通过2%及5标准误差门槛 |
| 64/256/1024spp收敛 | seed集合MSE为4.15571e-5 / 9.61123e-6 / 5.17725e-6，持续下降 |
| 多反弹颜色与RR | 无RR解析(1.12,0.22,0.105)误差≤1e-4；RR1/3红色均值1.12168884/1.12057362，通过2%与置信门槛 |
| 混合光源 | 环境/太阳/面积灯两两及三类混合，MIS与BSDF-only分别对照独立能量与CPU通过 |
| 小图参考与GPU | 4×4、8 seed、4096/16384spp，CPU N/4N nRMSE=0.00123593672（0.124%＜0.5%）；CPU/GPU=3.75550e-6（0.000376%＜2%） |
| 连续样本与诊断 | 100+156分段与256一次结果误差≤1e-6，同输入重复逐位一致；相机首样本命中匹配；高32-bit seed、first_pixel=17和first_sample=29对照CPU通过 |
| 失败与寿命 | 预算、取消、缺shader、错queue、float退化/小面积、media未确认、scene/view不匹配拒绝；真实AS/trace submit失败不推进票据；旧HDR与provenance不变 |
| 分配与关闭 | 耗尽16MiB renderer Memory并逐步释放，初始/中途scene/frame/read失败、恢复成功、closing拒绝新工作；timeout/pending外部释放/queue.close完成读回，最终VMA与Memory分配为0 |

CPU reference测试程序经dumpbin /dependents检查，仅flecs、mimalloc及Windows/MSVC运行库，无Vulkan/Slang DLL。
HDR原始产物为`out/build/windows-graphics/tests/integration/pathtrace-hdr.json`；linear_rgb对应seed42/256spp，
comparison单独记录8 seed/4096与16384spp参考协议，避免把两组样本混为同一误差报告。
产物经ConvertFrom-Json解析并校验16个RGB、图像256spp及comparison的8 seed/N/4N字段通过。
`scripts/check-spec.ps1 -Path`覆盖本次12份Markdown，758个本地链接、元数据、表格、索引及测试入口检查通过；
`git -c core.safecrlf=false diff --check`通过。

## 限制与下一步

提交前复核：核对上述summary.json与CTest原始结果、最终源码修改时间及HDR产物；
最终各组均有有效通过证据，后续源码未变更，沿用21项定向结果而不重复运行。
修正架构文档仍将AS/SBT及路径追踪shader称为规划的旧描述；本次收尾仅更新文档并重跑文档与diff检查。

仍为≤64×64/4096固定射线的小场景无历史正确性参考，GPU沿用CPU工作额度；一条invocation循环此像素样本，
不承诺实时性能或交互响应。GPU显存配额、tile、历史计数与视图会话留待M16.8。
世界三角形合并为单BLAS，不做资产BLAS缓存/实例复用、UPDATE/refit或跨队列AS；任意病态/共享边几何不承诺精确决胜。
只有opaque无纹理Lambert和发光，材质扩展从M16.7.1开始。CPU double与GPU float不承诺逐位相同，硬件遍历统计不可得。
未运行全引擎、其他GPU/平台、Debug/Release矩阵、编辑器/capture、完整资产/PBR或性能基线；无新依赖/命令。
Roadmap标记M16.6完成，下一项M16.7.1为纹理/alpha-mask/双面语义及固定descriptor数组能力。
