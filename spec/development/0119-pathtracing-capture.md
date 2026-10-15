---
id: "0119"
created_at: "2026-10-15T10:06:53+08:00"
updated_at: "2026-10-15T10:26:24+08:00"
status: completed
design_refs:
  - ../design/render-capture.md
  - ../design/render-pathtracing.md
  - ../design/foundation-jobs.md
---

# 0119 M16.10 路径追踪 capture 与自动化

完成render.capture显式路径后端、从0精确到目标spp、Job进度/取消、单文件线性报告、输入来源及Python/Luau例子。
沿用owner唯一原子发布点，取消/失败保留旧输出；GUI已完成帧与同输入capture在线性域逐像素一致。

## 实际变更

- [CaptureService](../../engine/framework/services/src/CaptureService.cpp)在提交时验证后端/格式/参数和有界报告预算，
  冻结Scene/Project/相机/设置，不修改revision/dirty/Undo。renderer默认raster，pathtrace需显式v1对象和目标。
- [PathCapture](../../engine/framework/services/src/PathCapture.cpp)独立worker设备/renderer/视图从空历史开始，
  metallic_roughness/model3/sampler1/MIS，每批1spp/tile64，固定seed/反弹/RR/常量环境/太阳/surface_only；
  与Editor共用渐进积分和GPU tone。渲染中按tile轮询取消，回收在途工作后返回，owner最后一次原子替换PPM或JSON。
- [JobQueue](../../engine/foundation/jobs/include/dk/jobs/JobQueue.hpp)新增有界ProgressWork/reporter，旧Work兼容；
  mutex保护phase/unit/completed/submitted/total，计数单调且不超目标，取消/终态不接纳迟到更新。
  [jobs命令](../../engine/framework/operations/src/JobOperations.cpp)统一返回progress；没有报告的旧任务为null。
- [命令注册](../../engine/framework/operations/src/RenderOperations.cpp)同步schema/发现，拒绝路径与光栅专用参数混用。
  runtime.capabilities.pathtrace_capture报告编译能力，不初始化设备；未编译/无RT明确拒绝，不降级。
- 单线性JSON记录曝光前RGB、GPU显示RGBA8、完整来源、样本区间、device/driver/build、额度/峰值，
  ≤524288像素/128MiB，队列预约256字节/像素且总192MiB。PPM单独任务，无跨文件事务。
  wall_ms覆盖worker启动到报告组装，不含序列化/owner发布；GPU分项为null，没有伪造性能数据。
- [PathTraceScene](../../engine/render/pathtracing/src/PathTraceScene.cpp)输入摘要v2移除导入临时纹理UUID，
  改以确定性绑定槽、内容、采样、UV和来源计算；保留Scene/实体/mesh身份与revision，重复导入可复现。
  shader闭包编译前后及输出准备完成前核对，执行期改变即失败。
- Editor仅在现有验收模式显式异步读回完成HDR帧，随呈现图像一起交接；普通GUI路径保持GPU显示。
  [GUI测试](../../tests/integration/EditorPathTraceTest.py)在初始、曝光重置恢复、移动相机三检查点提交真实capture比较。
- [Python](../../examples/automation/pathtrace_capture.py)及[Luau](../../examples/scripting/pathtrace-capture.luau)调用同一服务，
  [集成测试](../../tests/integration/PathCaptureTest.py)执行真实脚本、发现和文件保护；设计/命令/指南/测试表/Roadmap同步。
  无依赖升级，不修改Scene磁盘格式。

## 验证与证据

windows-editor/RelWithDebInfo、MSVC14.51、RTX4070 Laptop、required RT/Khronos同步validation；
验证子进程设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，避免旧AMD隐式层，不修改全局环境。

- out/verify/20261015-101030-eff1f3e1：初次构建缺ByteBuffer/Readback完整声明，失败且测试未运行；补齐头文件。
- 101231-79ccacee：17/17通过，涵盖Jobs、命令/schema/guard/预算、取消关闭、无GPU的导入失败和能力字段。
- 101514-5019319f：旧光栅capture和GUI通过，路径capture因同内容输入摘要不同失败；定位为导入器临时纹理UUID，
  修改摘要算法并增加CPU回归，未放宽验收断言。
- 101819-26972dc5：6项CPU纹理/摘要、required路径capture和GUI共8/8通过。
- 101932-a17b41cf：windows-dev CPU-only新构建2/2通过；实际新runner的capabilities/list/describe也核验，
  pathtrace_capture=false、render.capture未注册，证据out/test-artifacts/m1610-cpu-capabilities/evidence.json。
  首次手动查询遗漏--stdio被CLI拒绝，补齐后成功，未使用旧二进制作为证据。
- 102114-cd815d9b：最终20/20通过，0失败/0跳过，包括11个Jobs、5个Capture单元、能力边界、
  原云序列、required路径capture及GUI。当前Editor/runner二进制含路径模块。
- 最终capture产物位于test-artifacts/RelWithDebInfo/path-capture-e4b980ffb7f547e2a089826984bffc42：
  3spp从0精确完成，完整uint64最大seed保留；冻结票据后编辑场景不改变旧结果，重复图像/摘要一致；
  曝光改变只影响RGBA8，PPM字节与报告显示数据一致；运行进度可观察。
  queued取消、running取消、无surface_only的失败、在途shutdown均保持旧文件；发布后cancel返回false。
  Python/Luau输出同样的线性RGB和显示像素；stderr为空，无Vulkan错误/警告。
- GUI三种输入各比较320896像素，预设最大绝对误差门槛1e-5，实际三组均0；仍通过原14个GUI/IPC检查点，
  含拖动、resize、最小化、重载和未完成关闭；退出零validation错误/警告、零Memory存活。
- check-spec通过14份Markdown、723条本地链接、元数据/表格/索引/测试入口与JSON；首次索引新增行被空行隔开，修正后通过。git diff --check通过。

复现最终功能范围：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_run,dk_ctl,dk_editor_app,dk_protocol_tests,dk_jobs_tests `
  -TestRegex '^dk\.(jobs\.|protocol\.(capture |runtime GPU)|pathtracing\.capture_gpu_required$|clouds\.sequence_gpu_validation$|editor\.pathtracing_gpu_validation$)' `
  -Reason 'M16.10固定capture、进度/取消、来源、脚本、GUI线性及原序列回归'
```

## 限制与交接

路径仍限4096三角形/64纹理/源纹理64MiB/renderer额度1GiB，命令反弹1..16、目标1..65536并受工作量约束。
空气/云仅接受显式surface_only确认；没有介质路径积分、去噪、PNG/EXR或跨驱动逐位保证。
资产内容仍在worker开始时读盘，不声称接受请求时冻结全部外部文件；摘要是诊断而非加密验证。
没有无RT实机测试、全引擎/双配置回归或M16.11三轮质量性能交付；下一项M16.11。
原spec/README.md及未跟踪的文档迁移指南保留，不纳入本阶段提交。
