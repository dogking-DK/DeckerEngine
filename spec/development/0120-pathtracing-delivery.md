---
id: "0120"
created_at: "2026-10-15T11:15:38+08:00"
updated_at: "2026-10-15T11:41:57+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/foundation-profiling.md
---

# 0120 M16.11 路径追踪质量与性能交付

依据[交付协议](../design/render-pathtracing.md#m1611-质量与性能交付协议)先固定门槛，再完成多seed收敛、CPU/GPU线性质量、动态重置及三轮性能/显存原始证据。
M16.11与交付F完成；性能和有限采样质量候选的未达结果如实保留，详见[实测报告](../benchmarks/2026-10-15-pathtracing.md)。

## 实现与边界

- [PathBenchmark](../../tests/integration/PathBenchmark.cpp)和[夹具/采集辅助](../../tests/integration/PathBenchmarkSupport.hpp)：程序化面积灯Lambert、GGX盒及只读磁盘mask。
  固定4×4/4反弹/8seed，CPU N/4N=16384/65536，GPU同seed高样本及64/256/1024渐进；相机/reset/pending取消恢复对照新视图。
- [PathTraceRenderer](../../engine/render/pathtracing/include/dk/render/PathTraceRenderer.hpp)仅增加只读pending_submission观察接口，允许独占benchmark worker等待并读取当前tile profile。
  空闲返回空票据；取消后的票据仍可等待最终回收。不改变状态提交点、调度、失败保护或默认profiling开关。
- 性能进程关闭validation并显式开timestamp；1080p/8反弹/每批1 spp/64像素tile，首批加六批稳态。
  冷设备/编译、上传/BLAS/TLAS、trace+accumulate联合区间、tone/全图读回、CPU record-submit/wait-poll、取消与显存均有原始数据。
- [采集脚本](../../scripts/benchmark-pathtracing.py)每次独立目录、质量和三个独立性能进程，保存输入/版本/源码/二进制SHA256及失败状态。
  质量复用要求原manifest通过且原始报告/源码/二进制摘要全部一致；不支持改门槛、删慢轮或静默降分辨率。
- [离线复核](../../scripts/verify-pathtracing-evidence.py)检查压缩/原始字节摘要、三轮完整性及输入，重算后与summary逐项比较。
  [拒绝测试](../../tests/integration/PathBenchmarkTest.py)覆盖缺seed/缺tile、参考不收敛、CPU/GPU误差、非有限值、进阶退化、验证层误开、重复提交、缺timestamp、额度/泄漏、缺轮和损坏文件。
- 新target和CTest已接入[集成构建](../../tests/integration/CMakeLists.txt)，同步测试选择表、设计索引、Roadmap和[复现指南](../guides/pathtracing-benchmark.md)。
  无依赖升级、没有新命令或磁盘格式变更。用户已有spec/README.md和未跟踪迁移指南保持原状，不纳入本次提交。

## 验证与证据

Windows-editor / RelWithDebInfo，RTX4070 Laptop / 596.49；质量required RT/Khronos同步验证，性能关闭validation。
测试子进程设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，CPU-only另用windows-dev，同一配置没有全引擎/双配置回归。

| 证据目录（out/verify下） | 检查 | 结果 |
| --- | --- | --- |
| 20261015-112447-dafe11b0 | 新benchmark质量、报告拒绝 | 2/2通过 |
| 20261015-112555-625dfada | CPU-only解析/独立积分、相机、纹理、预算/取消/内存 | 26/26通过 |
| 20261015-112732-326a0d77 | GPU数值/纹理/GGX/渐进、失败/寿命/额度、真实GUI与capture | 22/22通过 |
| 20261015-113352-c05b6e27 | 新增归档重算与缺失/损坏拒绝后复核聚合 | 1/1通过，包含4个Python测试方法 |
| 20261015-113800-a2e4bdbc | 最终dk-pathtracing-benchmark命名后的二进制质量及报告检查 | 2/2通过 |
| 20261015-114121-6fb5bb8e | 最终三轮归档重算和缺失/损坏拒绝 | 1/1通过 |

共50个不同CTest通过，均0失败/0跳过；表中因归档和程序命名收尾而重跑的检查不计为新用例。
quality原始目录为out/build/windows-editor/test-artifacts/RelWithDebInfo/path-benchmark-quality/20261015-113816-1baaac82。
三轮独立性能目录为out/benchmarks/pathtracing/20261015-113919-a114dd0c；完整raw字节以gzip纳入[归档](../benchmarks/2026-10-15-pathtracing/archive.json)，避免Git换行转换影响SHA256。
离线重算3个质量输入、3轮完整性能通过；最终二进制和全部被测源码SHA256再次核对一致。没有仅引用上阶段二进制作为本阶段通过证据。
GUI产物pathtrace-d38d455e76b045f0bc3ce79ee0d8068a的三个检查点各比较320896像素，最大绝对误差均0；capture产物path-capture-4171c62e9e144f5eaabcbd863f8eb712验证脚本、进度和取消/文件保护。
check-spec通过8份Markdown、653条本地链接、元数据/表格/索引/测试入口和JSON；git diff --check通过。

复现命令（完整性能也可省略quality-report重新生成质量）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_pathtracing_benchmark -TestRegex '^dk\.pathtracing\.benchmark_' -Reason 'M16.11固定质量及报告校验'
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Configuration RelWithDebInfo `
  -Target dk_pathtracing_reference_tests -TestRegex '^dk\.pathtracing\.(reference |CPU |GGX CPU|texture CPU|[a-z]+ )' `
  -Reason 'M16.11 CPU-only参考'
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe,dk_editor_app,dk_ctl,dk_run `
  -TestRegex '^dk\.(pathtracing\.(progressive_.*|analytic|statistics|images|lifetime|allocations|ggx_.*|texture_.*|capture)_gpu_required|editor\.pathtracing_gpu_validation)$' `
  -Reason 'M16.11 GPU与GUI/capture闭环'
python -X utf8 scripts/benchmark-pathtracing.py `
  --executable out/build/windows-editor/bin/RelWithDebInfo/dk-pathtracing-benchmark.exe `
  --output out/benchmarks/pathtracing `
  --quality-report out/build/windows-editor/test-artifacts/RelWithDebInfo/path-benchmark-quality/20261015-113816-1baaac82/quality.json
python -X utf8 scripts/verify-pathtracing-evidence.py spec/benchmarks/2026-10-15-pathtracing
```

## 结论与限制

CPU参考N/4N最差0.1693%，小于0.5%；CPU/GPU最差0.000516%，小于2%；三输入的渐进集合误差随档位下降。
2%质量候选：Lambert在1024 spp、磁盘在64 spp达到；GGX在1024 spp仍为2.2824%，达到时间保留null。
三轮1080p稳态wall p50中位数为131.958 / 140.246 / 114.073ms，均未达16.67ms；GPU总区间分别22.281 / 33.773 / 10.746ms。
owner峰值506.258MiB低于1GiB，最终CPU/VMA存活分配为0；取消接受与GPU最终回收分别计时。
质量只代表固定小图和既有解析用例；未验证1080p全图质量、跨设备、完整PBR/介质路径、去噪或60fps。
4×4/8seed质量时间不外推1080p；独占worker时间不是GUI owner延迟；GPU trace和accumulate不能拆成独立计时。
后续按510次提交/等待、路径积分、累积内存和显式读回热点另行规划，维持已有质量门槛。

最终命名只调整CMake OUTPUT_NAME为dk-pathtracing-benchmark，未改渲染行为；为绑定最终二进制重新采集完整质量与三轮性能，初版数据保留在out/benchmarks/pathtracing/20261015-112641-d1ccad19，未用于最终报告。
